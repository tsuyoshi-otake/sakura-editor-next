/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/CWorkbenchRuntime.h"

#include "config/ConfigurationFileSourceController.h"
#include "config/ConfigurationFileWatchController.h"
#include "config/JsoncConfigurationSource.h"
// The grant path must not append past the bound the codec will refuse to encode,
// so the persistence limit is read from the codec that owns it rather than
// restated here as a second number that could drift.
#include "config/TrustedFoldersCodec.h"
#include <sakura/filesystem/FileSystemFactory.h>
#include <sakura/uri/UriIdentity.h>
#include "workbench/layout/WorkbenchIds.h"
#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"
#include "workbench/workspace/WorkspaceArtifactDocumentSourceController.h"
#include "workbench/workspace/WorkspaceResourceDescriptors.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <utility>

namespace workbench {
namespace {

using config::ConfigurationFileSourceControllerResult;
using config::ConfigurationSource;
using config::ConfigurationTarget;
using config::EConfigurationFileSourceControllerStatus;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
	using config::EWorkspaceContextOutcome;
	using config::EWorkspaceKind;
	using workspace::EWorkspaceArtifactDocumentSourceStatus;
using platform::uri::Uri;
using platform::uri::UriIdentityService;

constexpr std::string_view kProfileDocumentKey = "profile.settings";
constexpr std::string_view kWorkspaceDocumentPrefix = "workspace.folder.settings/";
constexpr std::string_view kWorkspaceSettingsSourceId = "workspace.settings";

//! The file-source controller deliberately owns parse/apply/CAS tracking as one
//! operation. This boundary makes a read that completes after Stop terminally
//! cancelled before the controller can begin its post-read work.
class StopAwareFileService final : public platform::filesystem::IFileService {
public:
	StopAwareFileService(
		std::unique_ptr<platform::filesystem::IFileService> inner,
		std::shared_ptr<std::atomic_bool> stopRequested)
		: m_inner(std::move(inner))
		, m_stopRequested(std::move(stopRequested))
	{
	}

	platform::filesystem::FileResult<platform::filesystem::FileStat> Stat(const Uri& resource) override
	{
		return IsStopped()
			? platform::filesystem::FileResult<platform::filesystem::FileStat>::Failure(platform::filesystem::EFileResultStatus::Cancelled)
			: m_inner->Stat(resource);
	}

	platform::filesystem::FileResult<std::vector<platform::filesystem::DirectoryEntry>> Enumerate(const Uri& resource) override
	{
		return IsStopped()
			? platform::filesystem::FileResult<std::vector<platform::filesystem::DirectoryEntry>>::Failure(platform::filesystem::EFileResultStatus::Cancelled)
			: m_inner->Enumerate(resource);
	}

	platform::filesystem::FileResult<platform::filesystem::FileBytes> Read(
		const Uri& resource, const platform::filesystem::FileReadOptions& options) override
	{
		if (IsStopped()) return platform::filesystem::FileResult<platform::filesystem::FileBytes>::Failure(
			platform::filesystem::EFileResultStatus::Cancelled);
		auto result = m_inner->Read(resource, options);
		return IsStopped()
			? platform::filesystem::FileResult<platform::filesystem::FileBytes>::Failure(platform::filesystem::EFileResultStatus::Cancelled)
			: result;
	}

	platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot> ReadVersioned(
		const Uri& resource, const platform::filesystem::FileReadOptions& options) override
	{
		if (IsStopped()) return platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot>::Failure(
			platform::filesystem::EFileResultStatus::Cancelled);
		auto result = m_inner->ReadVersioned(resource, options);
		return IsStopped()
			? platform::filesystem::FileResult<platform::filesystem::FileContentSnapshot>::Failure(platform::filesystem::EFileResultStatus::Cancelled)
			: result;
	}

	platform::filesystem::FileConditionalReplaceResult ConditionalAtomicReplace(
		const Uri& resource,
		const platform::filesystem::FileBytes& bytes,
		const platform::filesystem::FileConditionalReplaceOptions& options) override
	{
		if (IsStopped()) return platform::filesystem::FileConditionalReplaceResult::Failure();
		auto result = m_inner->ConditionalAtomicReplace(resource, bytes, options);
		return IsStopped() ? platform::filesystem::FileConditionalReplaceResult::Failure() : result;
	}

	platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>> Watch(
		const Uri& resource, const platform::filesystem::FileWatchOptions& options) override
	{
		return IsStopped()
			? platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>>::Failure(platform::filesystem::EFileResultStatus::Cancelled)
			: m_inner->Watch(resource, options);
	}

private:
	[[nodiscard]] bool IsStopped() const noexcept
	{
		return m_stopRequested && m_stopRequested->load(std::memory_order_acquire);
	}

	std::unique_ptr<platform::filesystem::IFileService> m_inner;
	std::shared_ptr<std::atomic_bool> m_stopRequested;
};

thread_local std::vector<const void*> g_activeListenerGates;

std::wstring WorkspaceDocumentIdentity(
	const config::WorkspaceContextSnapshot& workspace,
	const Uri& folder)
{
	const auto folderIdentity = UriIdentityService::MakeComparisonKey(folder);
	// Folder documents keep their own stable URI identity. A multi-folder
	// workspace adds only its immutable configuration resource, never folder
	// order or the current membership list, so controller CAS tracking survives
	// add/remove/reorder transitions.
	std::wstring result = workspace.kind == EWorkspaceKind::Workspace && workspace.workspaceConfigUri
		? UriIdentityService::MakeComparisonKey(*workspace.workspaceConfigUri)
		: folderIdentity;
	result = workspace.kind == EWorkspaceKind::Workspace
		? std::wstring(L"workspace:") + result
		: std::wstring(L"folder:") + result;
	result += L"|folder:";
	result += std::to_wstring(folderIdentity.size());
	result.push_back(L':');
	result += folderIdentity;
	return result;
}

bool HasSameFolders(
	const config::WorkspaceContextSnapshot& workspace,
	const std::vector<workspace::WorkspaceConfigurationFolder>& folders) noexcept
{
	if (workspace.folders.size() != folders.size()) return false;
	for (std::size_t index = 0; index < folders.size(); ++index) {
		if (workspace.folders[index].displayName != folders[index].displayName
			|| !UriIdentityService::IsEqual(workspace.folders[index].uri, folders[index].uri)) return false;
	}
	return true;
}

void RememberWorkspaceSettingsRevisions(
	config::JsoncConfigurationSourceRevisions& revisions,
	const config::ConfigurationBatchResult& result,
	const ConfigurationSource& source)
{
	config::JsoncConfigurationSourceRevisions accepted;
	for (const auto& sourceRevision : result.revisions) {
		if (sourceRevision.source.scope == EConfigurationScope::LanguageOverride
			&& sourceRevision.source.target.languageId) {
			accepted.languageRevisions[*sourceRevision.source.target.languageId] = sourceRevision.revision;
		} else if (sourceRevision.source.scope == source.scope
			&& sourceRevision.source.sourceId == source.sourceId) {
			accepted.baseRevision = sourceRevision.revision;
		}
	}
	revisions = std::move(accepted);
}

bool IsConfigurationSuccess(EConfigurationOutcome outcome) noexcept
{
	return outcome == EConfigurationOutcome::Applied
		|| outcome == EConfigurationOutcome::NoChange
		|| outcome == EConfigurationOutcome::Replayed;
}

bool HasSameConfigurationShape(
	const config::WorkspaceContextSnapshot& left,
	const config::WorkspaceContextSnapshot& right) noexcept
{
	if (left.kind != right.kind || left.workspaceIdentityKey != right.workspaceIdentityKey
		|| left.workspaceConfigUri.has_value() != right.workspaceConfigUri.has_value()
		|| left.folders.size() != right.folders.size()) {
		return false;
	}
	if (left.workspaceConfigUri
		&& !UriIdentityService::IsEqual(*left.workspaceConfigUri, *right.workspaceConfigUri)) {
		return false;
	}
	for (std::size_t index = 0; index < left.folders.size(); ++index) {
		if (!UriIdentityService::IsEqual(left.folders[index].uri, right.folders[index].uri)) {
			return false;
		}
	}
	return true;
}

bool HasSameArtifactTopology(
	const config::WorkspaceContextSnapshot& left,
	const config::WorkspaceContextSnapshot& right) noexcept
{
	if (left.kind != right.kind || left.workspaceIdentityKey != right.workspaceIdentityKey
		|| left.workspaceConfigUri.has_value() != right.workspaceConfigUri.has_value()
		|| left.folders.size() != right.folders.size()) {
		return false;
	}
	if (left.workspaceConfigUri
		&& !UriIdentityService::IsEqual(*left.workspaceConfigUri, *right.workspaceConfigUri)) {
		return false;
	}
	// Folder order affects Explorer presentation and settings reload order, but
	// it is not artifact source identity. Avoid clearing/re-reading `.vscode`
	// documents solely because the same multi-root members were reordered.
	return std::is_permutation(
		left.folders.begin(), left.folders.end(),
		right.folders.begin(), right.folders.end(),
		[](const auto& first, const auto& second) {
			return UriIdentityService::IsEqual(first.uri, second.uri);
		});
}

EWorkbenchRuntimeDiagnosticCode DiagnosticCodeFor(const ConfigurationFileSourceControllerResult& result)
{
	switch (result.status) {
	case EConfigurationFileSourceControllerStatus::ReadFailed:
		return EWorkbenchRuntimeDiagnosticCode::ReadFailed;
	case EConfigurationFileSourceControllerStatus::ParseFailed:
		return EWorkbenchRuntimeDiagnosticCode::ParseFailed;
	default:
		return EWorkbenchRuntimeDiagnosticCode::ApplyFailed;
	}
}

std::string DiagnosticMessageFor(const ConfigurationFileSourceControllerResult& result)
{
	if (result.status == EConfigurationFileSourceControllerStatus::ParseFailed && result.jsoncDiagnostic) {
		return result.jsoncDiagnostic->message;
	}
	return result.diagnostic.empty() ? "configuration source did not reach an accepted terminal state" : result.diagnostic;
}

bool IsWorkspaceMutationSuccess(EWorkspaceContextOutcome outcome) noexcept
{
	return outcome == EWorkspaceContextOutcome::Succeeded || outcome == EWorkspaceContextOutcome::NotApplicable;
}

} // namespace

CWorkbenchRuntime::CWorkbenchRuntime(
	WorkbenchBootstrapContext bootstrap,
	std::vector<config::ConfigurationDescriptor> descriptors)
	: CWorkbenchRuntime(std::move(bootstrap), std::move(descriptors), {})
{
}

CWorkbenchRuntime::CWorkbenchRuntime(
	WorkbenchBootstrapContext bootstrap,
	std::vector<config::ConfigurationDescriptor> descriptors,
	WorkbenchRuntimeDependencies dependencies)
	: m_bootstrap(std::move(bootstrap))
	, m_configuration(std::move(descriptors))
	, m_workspaceContext(m_bootstrap.WindowInstanceIdentity())
	, m_contributions()
	, m_layoutState(m_contributions.Snapshot())
	, m_layoutMementoStore(std::move(dependencies.layoutMementoStore))
	, m_statusbarVisibilityMementoStore(std::move(dependencies.statusbarVisibilityMementoStore))
	, m_trustedFoldersStore(std::move(dependencies.trustedFoldersStore))
	, m_fileService(std::move(dependencies.fileService))
	, m_recentlyOpenedWorkspaces(dependencies.recentlyOpenedWorkspaceStore
		? std::make_unique<recent::CRecentlyOpenedWorkspaceService>(std::move(dependencies.recentlyOpenedWorkspaceStore))
		: nullptr)
	, m_taskExecution(std::move(dependencies.taskExecutionSessionFactory))
	, m_markers(problems::MarkerServiceLimits {
		.maximumOwners = 128U,
	})
	, m_output(output::OutputServiceLimits {
		.maximumOwners = 128U,
		.maximumChannels = 128U,
		.maximumTextBytesPerChannel = 1U << 20,
		.maximumPayloadBytes = 64U << 10,
		.maximumLogEntriesPerChannel = 4'096U,
		.maximumSubscriptions = 256U,
		.maximumRememberedOperations = 512U,
		.maximumPendingNotifications = 512U,
	})
	, m_scm(scm::SourceControlServiceLimits {
		.maximumOwners = 128U,
		.maximumProviders = 128U,
		.maximumGroupsPerProvider = 128U,
		.maximumResourcesPerGroup = 4'096U,
		.maximumPayloadBytes = 1U << 20,
		.maximumSubscriptions = 256U,
	})
	, m_stopRequested(std::make_shared<std::atomic_bool>(false))
	, m_listenerGate(std::make_shared<ListenerGate>())
{
	m_listenerGate->owner = this;
	if (!m_fileService) {
		auto files = platform::filesystem::CreateWin32FileService();
		if (!files.Succeeded() || !files.value) {
			m_initializationFailure = files.diagnostic.empty()
				? "local filesystem provider registration failed"
				: std::string(files.diagnostic.begin(), files.diagnostic.end());
		} else {
			m_fileService = std::move(*files.value);
		}
	}
	if (m_fileService) {
		m_fileService = std::make_unique<StopAwareFileService>(std::move(m_fileService), m_stopRequested);
		m_workspaceEditing = std::make_unique<workspace::CWorkspaceEditingService>(*m_fileService);
		m_fileSources = std::make_unique<config::CConfigurationFileSourceController>(
			*m_fileService, m_configuration);
		m_settingsWriteback = std::make_unique<config::CSettingsWritebackCoordinator>(
			*m_fileService, *m_fileSources);
		m_fileWatches = std::make_unique<config::CConfigurationFileWatchController>(*m_fileService);
		m_workspaceArtifactSources = std::make_unique<workspace::CWorkspaceArtifactDocumentSourceController>(
			*m_fileService, m_workspaceArtifacts);
	}
	const auto gate = m_listenerGate;
	m_workspaceArtifactSubscription = m_workspaceArtifacts.Subscribe(
		[gate](const workspace::WorkspaceArtifactDocumentServiceSnapshot& snapshot) {
			const auto copied = snapshot;
			CWorkbenchRuntime::DispatchListener(gate, [copied](CWorkbenchRuntime& owner) {
				owner.OnWorkspaceArtifactsChanged(copied);
			});
		});
	if (!m_workspaceArtifactSubscription) {
		m_initializationFailure = "workspace artifact listener registration failed";
	}
	m_contributionSubscription = m_contributions.Subscribe(
		{ .ownerId = std::string(layout::ids::BuiltinOwner), .generation = 0 },
		[gate](const layout::WorkbenchContributionChange& change) {
			CWorkbenchRuntime::DispatchListener(gate, [&change](CWorkbenchRuntime& owner) {
				owner.OnContributionRegistryChanged(change);
			});
		});
	if (!m_contributionSubscription) {
		m_initializationFailure = "workbench contribution listener registration failed";
	}
}

recent::IRecentlyOpenedWorkspaceService* CWorkbenchRuntime::RecentlyOpenedWorkspaces() noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? m_recentlyOpenedWorkspaces.get() : nullptr;
}

CWorkbenchRuntime::~CWorkbenchRuntime()
{
	(void)Stop();
}

config::SettingsWritebackResult CWorkbenchRuntime::WriteSetting(const config::SettingsWritebackRequest& request)
{
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard stateLock(m_stateMutex);
		if (IsStopRequested() || (m_state != EWorkbenchRuntimeState::Ready && m_state != EWorkbenchRuntimeState::ReadyWithDiagnostics)) {
			return { .status = config::ESettingsWritebackStatus::Stopped, .diagnostic = "workbench settings owner is not ready" };
		}
	}
	if (!m_settingsWriteback) {
		return { .status = config::ESettingsWritebackStatus::Failed, .diagnostic = "workbench settings writeback owner is unavailable" };
	}
	auto result = m_settingsWriteback->Write(request);
	// A live edit -- most importantly to security.workspace.trust.banner itself,
	// but any Profile-scoped write can move it -- must change what the banner
	// Part shows immediately, not only after the next trust transition or restart.
	if (result.Succeeded()) {
		UpdateRestrictedModeBannerVisibility();
	}
	return result;
}

config::WorkspaceContextResult CWorkbenchRuntime::SwitchToFolderWorkspace(
	platform::uri::Uri folderUri, std::wstring displayName)
{
	const auto failed = [this](std::string reason) {
		return config::WorkspaceContextResult {
			.outcome = EWorkspaceContextOutcome::Failed,
			.reason = std::move(reason),
			.snapshot = m_workspaceContext.Snapshot(),
		};
	};
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard stateLock(m_stateMutex);
		if (!IsReadyForServiceAccessLocked()) return failed("workbench runtime is not ready");
	}
	const auto before = m_workspaceContext.Snapshot();
	auto operationId = NextWorkspaceOperationId();
	if (!operationId) return failed("workspace operation identifier space is exhausted");
	return m_workspaceContext.SetFolder({
		.operation = {
			.operationId = std::move(*operationId),
			.expectedRevision = before.revision,
		},
		.folderUri = std::move(folderUri),
		.displayName = std::move(displayName),
	});
}

workspace::WorkspaceEditingResult CWorkbenchRuntime::ReplaceCurrentWorkspaceFolders(
	const workspace::WorkspaceFoldersEditRequest& request)
{
	const auto failed = [](std::string diagnostic) {
		return workspace::WorkspaceEditingResult{ .diagnostic = std::move(diagnostic) };
	};
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	{
		std::lock_guard stateLock(m_stateMutex);
		if (!IsReadyForServiceAccessLocked()) return failed("workbench runtime is not ready");
	}
	if (!m_workspaceEditing) return failed("workspace editor is unavailable");
	const auto before = m_workspaceContext.Snapshot();
	if (before.kind != EWorkspaceKind::Workspace || !before.workspaceConfigUri
		|| !UriIdentityService::IsEqual(*before.workspaceConfigUri, request.source)
		|| !UriIdentityService::IsEqual(request.source, request.target)) {
		return failed("workspace edit does not target the active saved workspace");
	}
	{
		std::lock_guard sourceLock(m_sourceMutex);
		if (m_workspaceReloadActive || m_exactWorkspaceAcceptanceActive) {
			return failed("workspace semantic reload is already active");
		}
	}

	auto edited = m_workspaceEditing->ReplaceFolders(request);
	if (edited.outcome != workspace::EWorkspaceEditingOutcome::Succeeded) return edited;
	if (!edited.committedVersion || !edited.committedDocument) {
		return failed("workspace editor did not return exact committed state");
	}
	const auto expected = workspace::CWorkspaceConfigurationDocumentParser::Parse(
		*edited.committedDocument, request.target);
	if (!expected.Succeeded()) return failed("committed workspace document could not be parsed");

	{
		std::lock_guard sourceLock(m_sourceMutex);
		m_exactWorkspaceAcceptanceActive = true;
	}
	try {
		ReloadWorkspaceSettingsNow(before, &*edited.committedDocument);
	} catch (...) {
		std::lock_guard sourceLock(m_sourceMutex);
		m_exactWorkspaceAcceptanceActive = false;
		return failed("committed workspace document acceptance failed unexpectedly");
	}
	{
		std::lock_guard sourceLock(m_sourceMutex);
		m_exactWorkspaceAcceptanceActive = false;
		m_loadedWorkspaceRevision = m_workspaceContext.Snapshot().revision;
	}

	const auto acceptedContext = m_workspaceContext.Snapshot();
	const auto acceptedDocument = WorkspaceConfiguration();
	if (acceptedContext.kind != EWorkspaceKind::Workspace
		|| !acceptedContext.workspaceConfigUri
		|| !UriIdentityService::IsEqual(*acceptedContext.workspaceConfigUri, request.target)
		|| !HasSameFolders(acceptedContext, expected.document->folders)
		|| !acceptedDocument.resource || !acceptedDocument.document
		|| !UriIdentityService::IsEqual(*acceptedDocument.resource, request.target)
		|| !HasSameFolders(acceptedContext, acceptedDocument.document->folders)) {
		UpdateWorkspaceArtifacts(acceptedContext);
		RefreshFileWatching();
		return failed("committed workspace document was not accepted by the semantic workspace");
	}

	// The exact-acceptance guard suppressed the context listener. Reconcile the
	// two advisory consumers once, from the accepted semantic snapshot.
	UpdateWorkspaceArtifacts(acceptedContext);
	RefreshFileWatching();
	return edited;
}

workspace::WorkspaceConfigurationRuntimeSnapshot CWorkbenchRuntime::WorkspaceConfiguration() const
{
	std::lock_guard lock(m_stateMutex);
	return m_workspaceConfiguration;
}

bool CWorkbenchRuntime::IsReadyForServiceAccessLocked() const noexcept
{
	return !IsStopRequested()
		&& (m_state == EWorkbenchRuntimeState::Ready
		|| m_state == EWorkbenchRuntimeState::ReadyWithDiagnostics);
}

problems::MarkerService* CWorkbenchRuntime::Markers() noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_markers : nullptr;
}

const problems::MarkerService* CWorkbenchRuntime::Markers() const noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_markers : nullptr;
}

output::OutputService* CWorkbenchRuntime::Output() noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_output : nullptr;
}

const output::OutputService* CWorkbenchRuntime::Output() const noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_output : nullptr;
}

scm::SourceControlService* CWorkbenchRuntime::Scm() noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_scm : nullptr;
}

const scm::SourceControlService* CWorkbenchRuntime::Scm() const noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_scm : nullptr;
}

std::optional<tasks::FolderTaskCatalogSnapshot> CWorkbenchRuntime::TaskCatalogForFolder(
	const platform::uri::Uri& folderUri) const
{
	std::lock_guard lock(m_stateMutex);
	if (!IsReadyForServiceAccessLocked()) return std::nullopt;
	return m_taskCatalogs.SnapshotForFolder(folderUri);
}

tasks::TaskExecutionService* CWorkbenchRuntime::TaskExecution() noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_taskExecution : nullptr;
}

const tasks::TaskExecutionService* CWorkbenchRuntime::TaskExecution() const noexcept
{
	std::lock_guard lock(m_stateMutex);
	return IsReadyForServiceAccessLocked() ? &m_taskExecution : nullptr;
}

WorkbenchRuntimeSnapshot CWorkbenchRuntime::Snapshot() const
{
	std::lock_guard lock(m_stateMutex);
	WorkbenchRuntimeSnapshot snapshot;
	snapshot.state = m_state;
	snapshot.revision = m_revision;
	snapshot.diagnostics.reserve(m_diagnostics.size());
	for (const auto& [key, diagnostic] : m_diagnostics) {
		(void)key;
		snapshot.diagnostics.push_back(diagnostic);
	}
	return snapshot;
}

WorkbenchRuntimeResult CWorkbenchRuntime::CurrentResult(EWorkbenchRuntimeResultCode code) const
{
	return { code, Snapshot() };
}

bool CWorkbenchRuntime::IsStopRequested() const noexcept
{
	return m_stopRequested && m_stopRequested->load(std::memory_order_acquire);
}

bool CWorkbenchRuntime::IsExecutingListener() const noexcept
{
	return std::find(g_activeListenerGates.begin(), g_activeListenerGates.end(), m_listenerGate.get())
		!= g_activeListenerGates.end();
}

void CWorkbenchRuntime::DispatchListener(
	const std::shared_ptr<ListenerGate>& gate,
	const std::function<void(CWorkbenchRuntime&)>& callback) noexcept
{
	CWorkbenchRuntime* activeOwner = nullptr;
	{
		std::lock_guard lock(gate->mutex);
		if (gate->stopRequested || gate->owner == nullptr) return;
		try {
			g_activeListenerGates.push_back(gate.get());
		} catch (...) {
			return;
		}
		++gate->activeCallbacks;
		activeOwner = gate->owner;
	}
	try {
		callback(*activeOwner);
	} catch (...) {
		// Observer dispatch is a lifecycle boundary; the owning callback records
		// its own diagnostic where that remains meaningful.
	}

	bool finalizeAfterCallback = false;
	{
		std::lock_guard lock(gate->mutex);
		const auto active = std::find(g_activeListenerGates.rbegin(), g_activeListenerGates.rend(), gate.get());
		if (active != g_activeListenerGates.rend()) g_activeListenerGates.erase(std::next(active).base());
		finalizeAfterCallback = gate->stopRequested && gate->owner == activeOwner
			&& gate->activeCallbacks == 1;
	}
	// Keep this callback counted until the runtime lifecycle lock protects the
	// raw owner. A concurrent destructor therefore cannot drain the gate and
	// destroy the runtime in the gap between callback completion and finalization.
	std::unique_lock<std::recursive_mutex> lifecycleLock;
	if (finalizeAfterCallback) lifecycleLock = std::unique_lock(activeOwner->m_lifecycleMutex);
	{
		std::lock_guard lock(gate->mutex);
		if (gate->activeCallbacks != 0) --gate->activeCallbacks;
		if (gate->activeCallbacks == 0) gate->drained.notify_all();
	}
	if (finalizeAfterCallback) (void)activeOwner->CompleteStopAfterListeners();
}

WorkbenchRuntimeResult CWorkbenchRuntime::FailStart(
	EWorkbenchRuntimeDiagnosticCode code,
	std::string message)
{
	m_workspaceSubscription.Reset();
	// A failed bootstrap is terminal. The services may not have been exposed,
	// but their callback-draining Stop boundary must still be completed.
	(void)StopOwnedServices();
	{
		std::lock_guard lock(m_stateMutex);
		m_state = EWorkbenchRuntimeState::Failed;
		if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		try {
			m_diagnostics.insert_or_assign("bootstrap", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Bootstrap,
				.code = code,
				.message = std::move(message),
			});
		} catch (...) {
			// Failed remains a fully observable terminal state even when the
			// diagnostic allocation itself cannot be completed.
		}
	}
	return CurrentResult(EWorkbenchRuntimeResultCode::Failed);
}

void CWorkbenchRuntime::RefreshReadyStateLocked()
{
	if (m_state == EWorkbenchRuntimeState::Ready || m_state == EWorkbenchRuntimeState::ReadyWithDiagnostics) {
		m_state = m_diagnostics.empty() ? EWorkbenchRuntimeState::Ready : EWorkbenchRuntimeState::ReadyWithDiagnostics;
	}
}

bool CWorkbenchRuntime::HasTerminalState() const
{
	std::lock_guard lock(m_stateMutex);
	return m_state == EWorkbenchRuntimeState::Failed || m_state == EWorkbenchRuntimeState::Stopped;
}

void CWorkbenchRuntime::SetDiagnostic(std::string key, std::optional<WorkbenchRuntimeDiagnostic> diagnostic)
{
	std::lock_guard lock(m_stateMutex);
	if (m_state == EWorkbenchRuntimeState::Stopped || m_state == EWorkbenchRuntimeState::Failed) return;
	bool changed = false;
	if (diagnostic) {
		auto found = m_diagnostics.find(key);
		if (found == m_diagnostics.end() || found->second.source != diagnostic->source
			|| found->second.code != diagnostic->code || found->second.message != diagnostic->message) {
			m_diagnostics.insert_or_assign(std::move(key), std::move(*diagnostic));
			changed = true;
		}
	} else {
		changed = m_diagnostics.erase(key) != 0;
	}
	if (changed) {
		if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			m_state = EWorkbenchRuntimeState::Failed;
			return;
		}
		++m_revision;
	}
	RefreshReadyStateLocked();
}

std::optional<std::string> CWorkbenchRuntime::NextWorkspaceDocumentKey()
{
	if (m_nextWorkspaceDocument == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	return std::string(kWorkspaceDocumentPrefix) + std::to_string(m_nextWorkspaceDocument++);
}

std::optional<std::string> CWorkbenchRuntime::NextWorkspaceOperationId()
{
	if (m_nextWorkspaceOperation == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	return "sakura.workbench.workspace.v1/" + std::to_string(m_nextWorkspaceOperation++);
}

void CWorkbenchRuntime::RecordWorkspaceDocumentDiagnostic(
	std::string key, EWorkbenchRuntimeDiagnosticCode code, std::string message)
{
	SetDiagnostic(std::move(key), WorkbenchRuntimeDiagnostic {
		.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings,
		.code = code,
		.message = std::move(message),
	});
}

void CWorkbenchRuntime::SetWorkspaceConfigurationSnapshot(workspace::WorkspaceConfigurationRuntimeSnapshot snapshot)
{
	if (IsStopRequested()) return;
	std::lock_guard lock(m_stateMutex);
	if (m_state == EWorkbenchRuntimeState::Stopped || m_state == EWorkbenchRuntimeState::Failed) return;
	if (m_workspaceConfiguration.revision != std::numeric_limits<std::uint64_t>::max()) {
		snapshot.revision = m_workspaceConfiguration.revision + 1;
	}
	m_workspaceConfiguration = std::move(snapshot);
}

bool CWorkbenchRuntime::ApplyWorkspaceSettings(
	const config::WorkspaceContextSnapshot& snapshot,
	const std::optional<platform::serialization::JsoncValue::Object>& settings)
{
	if (IsStopRequested()) return false;
	if (!snapshot.workspaceConfigUri) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ApplyFailed,
			"workspace settings have no workspace configuration identity");
		return false;
	}
	if (m_workspaceSettingsActive && m_workspaceSettingsResource
		&& !UriIdentityService::IsEqual(*m_workspaceSettingsResource, *snapshot.workspaceConfigUri)) {
		ClearWorkspaceSettings();
		if (m_workspaceSettingsActive) return false;
	}
	auto operationId = NextWorkspaceOperationId();
	if (!operationId) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ApplyFailed,
			"workspace configuration operation identifier space is exhausted");
		return false;
	}
	ConfigurationTarget target;
	target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();
	target.workspaceUri = snapshot.workspaceConfigUri;
	const ConfigurationSource source {
		EConfigurationScope::Workspace, std::move(target), std::string(kWorkspaceSettingsSourceId), 0,
	};
	const platform::serialization::JsoncValue::Object empty;
	const auto applied = config::CJsoncConfigurationSource::ApplyObject(
		m_configuration, settings ? *settings : empty, source, std::move(*operationId),
		m_workspaceSettingsActive ? std::optional(m_workspaceSettingsRevisions) : std::nullopt);
	if (IsStopRequested()) return false;
	if (!applied.Parsed()) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ParseFailed,
			"workspace settings could not be adapted as configuration");
		return false;
	}
	if (!IsConfigurationSuccess(applied.result.outcome)) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ApplyFailed,
			"workspace settings replacement did not reach an accepted terminal state");
		return false;
	}
	RememberWorkspaceSettingsRevisions(m_workspaceSettingsRevisions, applied.result, source);
	m_workspaceSettingsActive = true;
	m_workspaceSettingsResource = snapshot.workspaceConfigUri;
	SetDiagnostic("workspace.settings", std::nullopt);
	return true;
}

void CWorkbenchRuntime::ClearWorkspaceSettings()
{
	if (IsStopRequested()) return;
	if (!m_workspaceSettingsActive || !m_workspaceSettingsResource) return;
	auto operationId = NextWorkspaceOperationId();
	if (!operationId) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ApplyFailed,
			"workspace configuration operation identifier space is exhausted");
		return;
	}
	ConfigurationTarget target;
	target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();
	target.workspaceUri = m_workspaceSettingsResource;
	const ConfigurationSource source {
		EConfigurationScope::Workspace, std::move(target), std::string(kWorkspaceSettingsSourceId), 0,
	};
	const platform::serialization::JsoncValue::Object empty;
	const auto applied = config::CJsoncConfigurationSource::ApplyObject(
		m_configuration, empty, source, std::move(*operationId), m_workspaceSettingsRevisions);
	if (!applied.Parsed() || !IsConfigurationSuccess(applied.result.outcome)) {
		RecordWorkspaceDocumentDiagnostic("workspace.settings", EWorkbenchRuntimeDiagnosticCode::ApplyFailed,
			"previous workspace settings could not be cleared");
		return;
	}
	m_workspaceSettingsRevisions = {};
	m_workspaceSettingsActive = false;
	m_workspaceSettingsResource.reset();
	SetDiagnostic("workspace.settings", std::nullopt);
}

void CWorkbenchRuntime::ClearWorkspaceFolderSettings()
{
	for (auto active = m_activeWorkspaceDocuments.begin(); active != m_activeWorkspaceDocuments.end();) {
		if (IsStopRequested()) return;
		const auto key = active->second;
		const auto removed = m_fileSources->Remove(key);
		RecordFileSourceResult(key, EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings, removed);
		if (removed.Succeeded()) {
			active = m_activeWorkspaceDocuments.erase(active);
		} else {
			++active;
		}
	}
	for (const auto& diagnosticKey : m_workspaceDiagnosticKeys) {
		if (std::none_of(m_activeWorkspaceDocuments.begin(), m_activeWorkspaceDocuments.end(),
			[&diagnosticKey](const auto& active) { return active.second == diagnosticKey; })) {
			SetDiagnostic(diagnosticKey, std::nullopt);
		}
	}
	m_workspaceDiagnosticKeys.clear();
	for (const auto& active : m_activeWorkspaceDocuments) m_workspaceDiagnosticKeys.insert(active.second);
}

void CWorkbenchRuntime::RecordFileSourceResult(
	std::string diagnosticKey,
	EWorkbenchRuntimeDiagnosticSource source,
	const ConfigurationFileSourceControllerResult& result)
{
	if (IsStopRequested()) return;
	if (result.Succeeded()) {
		SetDiagnostic(std::move(diagnosticKey), std::nullopt);
		return;
	}
	SetDiagnostic(std::move(diagnosticKey), WorkbenchRuntimeDiagnostic {
		.source = source,
		.code = DiagnosticCodeFor(result),
		.message = DiagnosticMessageFor(result),
	});
}

void CWorkbenchRuntime::RestoreInitialLayoutMemento()
{
	if (!m_layoutMementoStore) return;
	m_layoutPersistenceReady = false;
	const auto result = m_layoutMementoStore->Load();
	const auto recordFailure = [this](EWorkbenchRuntimeDiagnosticCode code, std::string message) {
		SetDiagnostic("layout.restore", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::Layout,
			.code = code,
			.message = std::move(message),
		});
	};

	switch (result.status) {
	case layout::EWorkbenchLayoutMementoLoadStatus::Loaded: {
		if (!result.snapshot) {
			recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
				"layout memento store returned no snapshot for a loaded result");
			return;
		}
		const auto hydrated = m_layoutState.HydrateInitialState(*result.snapshot);
		if (!hydrated.Succeeded()) {
			recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
				"stored layout memento could not be applied atomically");
			return;
		}
		m_layoutBaselineRevision = hydrated.snapshot.revision;
		m_layoutPersistenceReady = true;
		SetDiagnostic("layout.restore", std::nullopt);
		return;
	}
	case layout::EWorkbenchLayoutMementoLoadStatus::NotFound:
		m_layoutBaselineRevision = m_layoutState.Snapshot().revision;
		m_layoutPersistenceReady = true;
		SetDiagnostic("layout.restore", std::nullopt);
		return;
	case layout::EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
			"stored layout memento is invalid and was preserved without replacement");
		return;
	case layout::EWorkbenchLayoutMementoLoadStatus::Unavailable:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceUnavailable,
			"layout persistence was unavailable during startup");
		return;
	case layout::EWorkbenchLayoutMementoLoadStatus::Failed:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
			"layout memento restore failed");
		return;
	}
	recordFailure(EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
		"layout memento restore returned an unknown terminal state");
}

void CWorkbenchRuntime::RestoreTrustedFolders()
{
	m_trustedFolders.entries.clear();
	m_trustedFoldersPersistenceReady = false;
	// A runtime composed without the store is not degraded: it simply has no durable
	// trust list, so every folder resolves untrusted. That is the same answer a fresh
	// profile gives, and it must not be diagnosed as a failure.
	if (!m_trustedFoldersStore) return;

	const auto result = m_trustedFoldersStore->Load();
	const auto recordFailure = [this](EWorkbenchRuntimeDiagnosticCode code, std::string message) {
		SetDiagnostic("trust.restore", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceTrust,
			.code = code,
			.message = std::move(message),
		});
	};

	switch (result.status) {
	case config::ETrustedFoldersLoadStatus::Loaded:
		if (!result.snapshot) {
			recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustRestoreFailed,
				"trusted folders store returned no snapshot for a loaded result");
			return;
		}
		m_trustedFolders = *result.snapshot;
		m_trustedFoldersPersistenceReady = true;
		SetDiagnostic("trust.restore", std::nullopt);
		return;
	case config::ETrustedFoldersLoadStatus::NotFound:
		// No folder has ever been trusted in this profile. The empty list is the
		// correct record, and a later grant may write it.
		m_trustedFoldersPersistenceReady = true;
		SetDiagnostic("trust.restore", std::nullopt);
		return;
	case config::ETrustedFoldersLoadStatus::InvalidStoredList:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustRestoreFailed,
			"stored trusted folders list is invalid and was preserved without replacement");
		return;
	case config::ETrustedFoldersLoadStatus::Unavailable:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustPersistenceUnavailable,
			"trusted folders persistence was unavailable during startup");
		return;
	case config::ETrustedFoldersLoadStatus::Failed:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustRestoreFailed,
			"trusted folders restore failed");
		return;
	}
	recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustRestoreFailed,
		"trusted folders restore returned an unknown terminal state");
}

void CWorkbenchRuntime::RestoreStatusbarVisibilityMemento()
{
	m_statusbarPersistenceReady = false;
	if (!m_statusbarVisibilityMementoStore) return;
	const auto result = m_statusbarVisibilityMementoStore->Load();
	using LoadStatus = statusbar::EStatusbarMementoLoadStatus;
	switch (result.status) {
	case LoadStatus::Loaded:
	case LoadStatus::NotFound:
		if (!result.hiddenIds || !m_statusbarState.RestoreHiddenIds(*result.hiddenIds)) {
			SetDiagnostic("statusbar.restore", WorkbenchRuntimeDiagnostic{
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
				.message = "status bar visibility memento could not be applied",
			});
			return;
		}
		m_statusbarPersistenceReady = true;
		SetDiagnostic("statusbar.restore", std::nullopt);
		return;
	case LoadStatus::Unavailable:
		SetDiagnostic("statusbar.restore", WorkbenchRuntimeDiagnostic{
			.source = EWorkbenchRuntimeDiagnosticSource::Layout,
			.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceUnavailable,
			.message = "status bar visibility persistence was unavailable",
		});
		return;
	case LoadStatus::Invalid:
	case LoadStatus::Failed:
		SetDiagnostic("statusbar.restore", WorkbenchRuntimeDiagnostic{
			.source = EWorkbenchRuntimeDiagnosticSource::Layout,
			.code = EWorkbenchRuntimeDiagnosticCode::LayoutRestoreFailed,
			.message = "status bar visibility memento was invalid or failed",
		});
		return;
	}
}

statusbar::StatusbarMementoSaveResult CWorkbenchRuntime::PersistStatusbarVisibility()
{
	using SaveStatus = statusbar::EStatusbarMementoSaveStatus;
	if (!m_statusbarVisibilityMementoStore || !m_statusbarPersistenceReady) {
		return { SaveStatus::Unavailable, L"status bar visibility persistence is unavailable" };
	}
	const auto result = m_statusbarVisibilityMementoStore->Save(m_statusbarState.Snapshot().hiddenIds);
	if (result.Succeeded()) SetDiagnostic("statusbar.persist", std::nullopt);
	else SetDiagnostic("statusbar.persist", WorkbenchRuntimeDiagnostic{
		.source = EWorkbenchRuntimeDiagnosticSource::Layout,
		.code = result.status == SaveStatus::Conflict
			? EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceConflict
			: EWorkbenchRuntimeDiagnosticCode::LayoutPersistFailed,
		.message = "status bar visibility memento was not persisted",
	});
	return result;
}

void CWorkbenchRuntime::PersistFinalLayoutMemento() noexcept
{
	if (!m_layoutMementoStore || !m_layoutPersistenceReady) return;
	try {
		const auto snapshot = m_layoutState.MementoSnapshot();
		if (snapshot.revision == m_layoutBaselineRevision) return;
		const auto result = m_layoutMementoStore->Save(snapshot);
		switch (result.status) {
		case layout::EWorkbenchLayoutMementoSaveStatus::Persisted:
		case layout::EWorkbenchLayoutMementoSaveStatus::NotDirty:
			SetDiagnostic("layout.persist", std::nullopt);
			return;
		case layout::EWorkbenchLayoutMementoSaveStatus::Conflict:
			SetDiagnostic("layout.persist", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceConflict,
				.message = "layout memento was not overwritten after a storage conflict",
			});
			return;
		case layout::EWorkbenchLayoutMementoSaveStatus::Unavailable:
		case layout::EWorkbenchLayoutMementoSaveStatus::Stopped:
			SetDiagnostic("layout.persist", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistenceUnavailable,
				.message = "layout persistence became unavailable during shutdown",
			});
			return;
		case layout::EWorkbenchLayoutMementoSaveStatus::RetryExhausted:
		case layout::EWorkbenchLayoutMementoSaveStatus::Failed:
			SetDiagnostic("layout.persist", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistFailed,
				.message = "layout memento did not reach a durable terminal success",
			});
			return;
		}
		SetDiagnostic("layout.persist", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::Layout,
			.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistFailed,
			.message = "layout memento save returned an unknown terminal state",
		});
	} catch (...) {
		try {
			SetDiagnostic("layout.persist", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutPersistFailed,
				.message = "layout memento save raised an unexpected failure",
			});
		} catch (...) {
			// Stop still owns the terminal Stopped transition below.
		}
	}
}

bool CWorkbenchRuntime::ApplyBootstrapWorkspace()
{
	const auto& desired = m_bootstrap.Workspace();
	config::WorkspaceContextResult result;
	switch (desired.kind) {
	case EWorkspaceKind::Empty:
		result = { EWorkspaceContextOutcome::NotApplicable, 0, {}, false, m_workspaceContext.Snapshot(), std::nullopt };
		break;
	case EWorkspaceKind::Folder:
		if (desired.folders.size() != 1) return false;
		result = m_workspaceContext.SetFolder({
			.operation = { .operationId = "bootstrap.folder", .expectedRevision = 0 },
			.folderUri = desired.folders.front().uri,
			.displayName = desired.folders.front().displayName,
		});
		break;
	case EWorkspaceKind::Workspace:
		result = m_workspaceContext.SetWorkspace({
			.operation = { .operationId = "bootstrap.workspace", .expectedRevision = 0 },
			.workspaceConfigUri = desired.workspaceConfigUri,
			.folders = desired.folders,
		});
		break;
	}
	if (!IsWorkspaceMutationSuccess(result.outcome)) return false;
	const auto actual = m_workspaceContext.Snapshot();
	return actual.kind == desired.kind && actual.workspaceIdentityKey == desired.workspaceIdentityKey;
}

void CWorkbenchRuntime::ReloadProfileSettings()
{
	if (IsStopRequested()) return;
	ConfigurationTarget target;
	target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();
	const ConfigurationSource source {
		EConfigurationScope::Profile,
		std::move(target),
		"profile.settings",
		0,
	};
	const auto result = m_fileSources->Reload(
		kProfileDocumentKey, source, m_bootstrap.UserDataProfile().Resources().Settings());
	if (IsStopRequested()) return;
	RecordFileSourceResult("profile.settings", EWorkbenchRuntimeDiagnosticSource::ProfileSettings, result);
	// security.workspace.trust.banner is Profile-scoped (BuiltinConfigurationDescriptors.cpp),
	// so both the initial Start() load and a later file-watch-triggered reload of
	// this same document are places an edited banner policy must take effect
	// without waiting for a trust transition.
	UpdateRestrictedModeBannerVisibility();
}

void CWorkbenchRuntime::StartFileWatching()
{
	if (IsStopRequested() || !m_fileWatches) return;
	config::ConfigurationFileWatchRequest request {
		.profileSettings = m_bootstrap.UserDataProfile().Resources().Settings(),
	};
	const auto workspace = m_workspaceContext.Snapshot();
	request.workspaceConfiguration = workspace.workspaceConfigUri;
	request.workspaceFolders.reserve(workspace.folders.size());
	for (const auto& folder : workspace.folders) request.workspaceFolders.push_back(folder.uri);
	const auto started = m_fileWatches->Start(std::move(request), [this](config::EConfigurationFileWatchChange change) {
		OnConfigurationFileWatchChange(change);
	});
	if (!started.Succeeded()) {
		SetDiagnostic("settings.watch", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings,
			.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			.message = "external settings watch did not reach a started terminal state",
		});
	}
}

void CWorkbenchRuntime::RefreshFileWatching()
{
	if (IsStopRequested() || !m_fileWatches) return;
	config::ConfigurationFileWatchRequest request {
		.profileSettings = m_bootstrap.UserDataProfile().Resources().Settings(),
	};
	const auto workspace = m_workspaceContext.Snapshot();
	request.workspaceConfiguration = workspace.workspaceConfigUri;
	request.workspaceFolders.reserve(workspace.folders.size());
	for (const auto& folder : workspace.folders) request.workspaceFolders.push_back(folder.uri);
	const auto updated = m_fileWatches->Update(std::move(request));
	if (!updated.Succeeded()) {
		SetDiagnostic("settings.watch", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings,
			.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			.message = "external settings watch topology did not reach an updated terminal state",
		});
	}
}

void CWorkbenchRuntime::OnConfigurationFileWatchChange(config::EConfigurationFileWatchChange change) noexcept
{
	try {
		{
			std::lock_guard lock(m_stateMutex);
			if (IsStopRequested()
				|| (m_state != EWorkbenchRuntimeState::Ready && m_state != EWorkbenchRuntimeState::ReadyWithDiagnostics)) return;
		}
		if (change == config::EConfigurationFileWatchChange::ProfileSettings
			|| change == config::EConfigurationFileWatchChange::FullRescan) {
			ReloadProfileSettings();
		}
		if (change == config::EConfigurationFileWatchChange::WorkspaceFolderSettings
			|| change == config::EConfigurationFileWatchChange::WorkspaceConfiguration
			|| change == config::EConfigurationFileWatchChange::FullRescan) {
			ReloadWorkspaceSettings(m_workspaceContext.Snapshot());
			RefreshFileWatching();
		}
	} catch (...) {
		try {
			SetDiagnostic("settings.watch", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings,
				.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
				.message = "external settings refresh failed unexpectedly",
			});
		} catch (...) {
			// The lifecycle owner still transitions to Stopped if shutdown races this
			// advisory callback.
		}
	}
}

void CWorkbenchRuntime::ReloadWorkspaceSettings(const config::WorkspaceContextSnapshot& snapshot)
{
	if (IsStopRequested()) return;
	{
		std::lock_guard sourceLock(m_sourceMutex);
		if (m_loadedWorkspaceRevision && snapshot.revision < *m_loadedWorkspaceRevision) return;
		if (m_pendingWorkspaceSnapshot && snapshot.revision < m_pendingWorkspaceSnapshot->revision) return;
		m_pendingWorkspaceSnapshot = snapshot;
		if (m_workspaceReloadActive) return;
		m_workspaceReloadActive = true;
	}

	try {
		for (;;) {
			if (IsStopRequested() || HasTerminalState()) {
				std::lock_guard sourceLock(m_sourceMutex);
				m_pendingWorkspaceSnapshot.reset();
				m_workspaceReloadActive = false;
				return;
			}
			config::WorkspaceContextSnapshot pending;
			{
				std::lock_guard sourceLock(m_sourceMutex);
				if (!m_pendingWorkspaceSnapshot) {
					m_workspaceReloadActive = false;
					return;
				}
				pending = std::move(*m_pendingWorkspaceSnapshot);
				m_pendingWorkspaceSnapshot.reset();
			}
			ReloadWorkspaceSettingsNow(pending);
			if (IsStopRequested() || HasTerminalState()) continue;
			{
				std::lock_guard sourceLock(m_sourceMutex);
				// A successful .code-workspace parse can itself advance the semantic
				// context from the bootstrap's zero-folder snapshot. Keep the returned
				// context revision, so that transition is finalized once rather than
				// recursively reloading the same document after subscription.
				m_loadedWorkspaceRevision = m_workspaceContext.Snapshot().revision;
			}
		}
	} catch (...) {
		std::lock_guard sourceLock(m_sourceMutex);
		m_workspaceReloadActive = false;
		throw;
	}
}

void CWorkbenchRuntime::ReloadWorkspaceSettingsNow(const config::WorkspaceContextSnapshot& snapshot,
	const std::string* exactWorkspaceDocument)
{
	if (IsStopRequested()) return;
	struct DesiredDocument final {
		std::wstring identity;
		std::string key;
		ConfigurationSource source;
		Uri resource;
	};
	std::set<std::wstring, std::less<>> desiredIdentities;
	std::set<std::string, std::less<>> desiredDiagnosticKeys;
	std::vector<DesiredDocument> desiredDocuments;
	workspace::WorkspaceConfigurationRuntimeSnapshot workspaceConfiguration;
	config::WorkspaceContextSnapshot effectiveSnapshot = snapshot;
	bool resourceIdentityFailed = false;

	if (snapshot.kind == EWorkspaceKind::Workspace) {
		const bool workspaceFolderOwnerChanged = m_workspaceFolderOwnerIdentity
			&& *m_workspaceFolderOwnerIdentity != snapshot.workspaceIdentityKey;
		if (workspaceFolderOwnerChanged) {
			// Clean the old sources before reading the replacement document. If the
			// replacement cannot be read or parsed, it must not leave old workspace
			// folder settings or controller revisions applied to the new context.
			ClearWorkspaceFolderSettings();
			if (IsStopRequested()) return;
			m_workspaceFolderOwnerIdentity = snapshot.workspaceIdentityKey;
		}
		const auto restoreAcceptedWorkspaceModel = [this](const config::WorkspaceContextSnapshot& current) {
			const auto accepted = WorkspaceConfiguration();
			if (!current.workspaceConfigUri || !accepted.resource || !accepted.document
				|| !UriIdentityService::IsEqual(*current.workspaceConfigUri, *accepted.resource)
				|| HasSameFolders(current, accepted.document->folders)) return;
			auto operationId = NextWorkspaceOperationId();
			if (!operationId) {
				RecordWorkspaceDocumentDiagnostic("workspace.context", EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
					"workspace context operation identifier space is exhausted");
				return;
			}
			std::vector<config::WorkspaceFolderDescriptor> folders;
			folders.reserve(accepted.document->folders.size());
			for (const auto& folder : accepted.document->folders) folders.push_back({ folder.uri, folder.displayName });
			const auto restored = m_workspaceContext.SetWorkspace({
				.operation = { .operationId = std::move(*operationId), .expectedRevision = current.revision },
				.workspaceConfigUri = current.workspaceConfigUri,
				.folders = std::move(folders),
			});
			if (!IsWorkspaceMutationSuccess(restored.outcome)) {
				RecordWorkspaceDocumentDiagnostic("workspace.context", EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
					"last accepted workspace folders could not be restored");
			}
		};
		if (!snapshot.workspaceConfigUri) {
			RecordWorkspaceDocumentDiagnostic("workspace.configuration", EWorkbenchRuntimeDiagnosticCode::ReadFailed,
				"workspace configuration resource is unavailable");
			return;
		}
		// A different workspace identity must never retain the prior workspace
		// source while the replacement resource is unavailable or malformed. A
		// failed reload of the same resource, however, keeps its accepted model.
		if (m_workspaceSettingsActive && m_workspaceSettingsResource
			&& !UriIdentityService::IsEqual(*m_workspaceSettingsResource, *snapshot.workspaceConfigUri)) {
			ClearWorkspaceSettings();
			if (IsStopRequested() || m_workspaceSettingsActive) return;
			SetWorkspaceConfigurationSnapshot({});
		}
		std::string utf8;
		if (exactWorkspaceDocument != nullptr) {
			utf8 = *exactWorkspaceDocument;
		} else {
			const auto read = m_fileService->Read(*snapshot.workspaceConfigUri,
				{ .maximumBytes = config::CJsoncConfigurationSource::kMaximumInputBytes });
			if (!read.Succeeded() || !read.value) {
				if (IsStopRequested()) return;
				restoreAcceptedWorkspaceModel(snapshot);
				RecordWorkspaceDocumentDiagnostic("workspace.configuration", EWorkbenchRuntimeDiagnosticCode::ReadFailed,
					"workspace configuration document could not be read");
				return;
			}
			utf8.assign(read.value->begin(), read.value->end());
		}
		auto parsed = workspace::CWorkspaceConfigurationDocumentParser::Parse(utf8, *snapshot.workspaceConfigUri);
		if (IsStopRequested()) return;
		if (!parsed.Succeeded()) {
			restoreAcceptedWorkspaceModel(snapshot);
			RecordWorkspaceDocumentDiagnostic("workspace.configuration", EWorkbenchRuntimeDiagnosticCode::ParseFailed,
				"workspace configuration document could not be parsed");
			return;
		}
		for (const auto& diagnostic : parsed.diagnostics) {
			if (diagnostic.code == workspace::EWorkspaceConfigurationDiagnosticCode::DuplicateFolderUri) {
				RecordWorkspaceDocumentDiagnostic("workspace.folder.duplicate", EWorkbenchRuntimeDiagnosticCode::WorkspaceFolderDuplicate,
					diagnostic.message);
			}
		}
		if (std::none_of(parsed.diagnostics.begin(), parsed.diagnostics.end(), [](const auto& diagnostic) {
			return diagnostic.code == workspace::EWorkspaceConfigurationDiagnosticCode::DuplicateFolderUri;
		})) {
			SetDiagnostic("workspace.folder.duplicate", std::nullopt);
		}
		if (!HasSameFolders(snapshot, parsed.document->folders)) {
			auto operationId = NextWorkspaceOperationId();
			if (!operationId) {
				RecordWorkspaceDocumentDiagnostic("workspace.context", EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
					"workspace context operation identifier space is exhausted");
				return;
			}
			std::vector<config::WorkspaceFolderDescriptor> folders;
			folders.reserve(parsed.document->folders.size());
			for (const auto& folder : parsed.document->folders) folders.push_back({ folder.uri, folder.displayName });
			const auto updated = m_workspaceContext.SetWorkspace({
				.operation = { .operationId = std::move(*operationId), .expectedRevision = snapshot.revision },
				.workspaceConfigUri = snapshot.workspaceConfigUri,
				.folders = std::move(folders),
			});
			if (!IsWorkspaceMutationSuccess(updated.outcome)) {
				RecordWorkspaceDocumentDiagnostic("workspace.context", EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
					"parsed workspace folders could not become the semantic workspace context");
				return;
			}
			if (IsStopRequested()) return;
			effectiveSnapshot = updated.snapshot;
			SetDiagnostic("workspace.context", std::nullopt);
		}
		if (!ApplyWorkspaceSettings(effectiveSnapshot, parsed.document->settings)) {
			restoreAcceptedWorkspaceModel(effectiveSnapshot);
			return;
		}
		if (IsStopRequested()) return;
		workspaceConfiguration.resource = effectiveSnapshot.workspaceConfigUri;
		workspaceConfiguration.document = std::move(*parsed.document);
		workspaceConfiguration.folderResources.reserve(workspaceConfiguration.document->folders.size());
		for (const auto& folder : workspaceConfiguration.document->folders) {
			auto resources = workspace::DescribeWorkspaceFolderResources(folder.uri);
			if (!resources) {
				resourceIdentityFailed = true;
				continue;
			}
			workspaceConfiguration.folderResources.push_back({ folder, std::move(*resources) });
		}
		SetWorkspaceConfigurationSnapshot(std::move(workspaceConfiguration));
		SetDiagnostic("workspace.configuration", std::nullopt);
	} else {
		ClearWorkspaceSettings();
		ClearWorkspaceFolderSettings();
		if (IsStopRequested()) return;
		SetWorkspaceConfigurationSnapshot({});
		m_workspaceFolderOwnerIdentity.reset();
		SetDiagnostic("workspace.configuration", std::nullopt);
		SetDiagnostic("workspace.folder.duplicate", std::nullopt);
	}

	for (const auto& folder : effectiveSnapshot.folders) {
		if (IsStopRequested()) return;
		auto resources = workspace::DescribeWorkspaceFolderResources(folder.uri);
		if (!resources) {
			resourceIdentityFailed = true;
			continue;
		}
		const auto settings = std::find_if(resources->begin(), resources->end(), [](const auto& resource) {
			return resource.member == workspace::EWorkspaceFileMember::Settings;
		});
		if (settings == resources->end()) {
			resourceIdentityFailed = true;
			continue;
		}

		auto identity = WorkspaceDocumentIdentity(effectiveSnapshot, folder.uri);
		auto active = m_activeWorkspaceDocuments.find(identity);
		std::optional<std::string> key;
		if (active != m_activeWorkspaceDocuments.end()) {
			key = active->second;
		} else {
			key = NextWorkspaceDocumentKey();
		}
		if (!key) {
			resourceIdentityFailed = true;
			continue;
		}

		ConfigurationTarget target;
		target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();
		target.workspaceUri = effectiveSnapshot.kind == EWorkspaceKind::Workspace
			? effectiveSnapshot.workspaceConfigUri : std::optional<Uri>(folder.uri);
		target.folderUri = folder.uri;
		ConfigurationSource source {
			EConfigurationScope::Folder,
			std::move(target),
			"workspace.folder.settings",
			0,
		};
		desiredIdentities.insert(identity);
		desiredDiagnosticKeys.insert(*key);
		desiredDocuments.push_back(
			{ std::move(identity), std::move(*key), std::move(source), settings->resource });
	}
	if (resourceIdentityFailed) {
		SetDiagnostic("workspace.resource", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings,
			.code = EWorkbenchRuntimeDiagnosticCode::ReadFailed,
			.message = "workspace settings resource identity could not be derived",
		});
	} else {
		SetDiagnostic("workspace.resource", std::nullopt);
	}

	for (auto active = m_activeWorkspaceDocuments.begin(); active != m_activeWorkspaceDocuments.end();) {
		if (IsStopRequested() || HasTerminalState()) return;
		if (desiredIdentities.contains(active->first)) {
			++active;
			continue;
		}
		const auto key = active->second;
		const auto removed = m_fileSources->Remove(key);
		RecordFileSourceResult(key, EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings, removed);
		if (removed.Succeeded()) {
			active = m_activeWorkspaceDocuments.erase(active);
		} else {
			++active;
		}
	}
	for (const auto& desired : desiredDocuments) {
		if (IsStopRequested() || HasTerminalState()) return;
		const bool wasActive = m_activeWorkspaceDocuments.contains(desired.identity);
		const auto loaded = m_fileSources->Reload(desired.key, desired.source, desired.resource);
		RecordFileSourceResult(desired.key, EWorkbenchRuntimeDiagnosticSource::WorkspaceSettings, loaded);
		if (IsStopRequested() || HasTerminalState()) return;
		if (!wasActive && loaded.Succeeded()) {
			m_activeWorkspaceDocuments.emplace(desired.identity, desired.key);
		}
	}
	for (const auto& previousKey : m_workspaceDiagnosticKeys) {
		if (!desiredDiagnosticKeys.contains(previousKey)
			&& std::none_of(m_activeWorkspaceDocuments.begin(), m_activeWorkspaceDocuments.end(),
				[&previousKey](const auto& active) { return active.second == previousKey; })) {
			SetDiagnostic(previousKey, std::nullopt);
		}
	}
	m_workspaceDiagnosticKeys = std::move(desiredDiagnosticKeys);
	for (const auto& active : m_activeWorkspaceDocuments) {
		m_workspaceDiagnosticKeys.insert(active.second);
	}
	m_workspaceFolderOwnerIdentity = effectiveSnapshot.workspaceIdentityKey;
}

void CWorkbenchRuntime::RecordWorkspaceArtifactSourceResult(
	const workspace::WorkspaceArtifactDocumentSourceResult& result) noexcept
{
	try {
		const bool documentRejected = std::any_of(result.documents.begin(), result.documents.end(), [](const auto& document) {
			return !document.Succeeded();
		});
		if (result.Succeeded() && !documentRejected) {
			// Update only queues the next reload. Keep a prior reload diagnostic
			// until that reload has established a fresh aggregate outcome.
			if (result.status == EWorkspaceArtifactDocumentSourceStatus::Updated) return;
			SetDiagnostic("workspace.artifacts", std::nullopt);
			return;
		}
		SetDiagnostic("workspace.artifacts", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceArtifacts,
			.code = result.status == EWorkspaceArtifactDocumentSourceStatus::ReadFailed
				? EWorkbenchRuntimeDiagnosticCode::ReadFailed
				: (documentRejected ? EWorkbenchRuntimeDiagnosticCode::ParseFailed
					: EWorkbenchRuntimeDiagnosticCode::InternalFailure),
			.message = "workspace artifact source did not reach an accepted terminal state",
		});
	} catch (...) {
		// Artifact callbacks are advisory. Their controller owns neither runtime
		// shutdown nor the pure document service.
	}
}

void CWorkbenchRuntime::StartWorkspaceArtifacts(const config::WorkspaceContextSnapshot& snapshot)
{
	if (IsStopRequested() || !m_workspaceArtifactSources) return;
	if (snapshot.revision == std::numeric_limits<std::uint64_t>::max()) {
		SetDiagnostic("workspace.artifacts", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceArtifacts,
			.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			.message = "workspace artifact generation cannot advance from the semantic revision",
		});
		return;
	}
	workspace::WorkspaceArtifactDocumentSourceRequest request;
	request.generation = snapshot.revision + 1;
	request.workspaceConfiguration = snapshot.kind == EWorkspaceKind::Workspace
		? snapshot.workspaceConfigUri : std::nullopt;
	request.workspaceFolders.reserve(snapshot.folders.size());
	for (const auto& folder : snapshot.folders) request.workspaceFolders.push_back(folder.uri);
	const auto started = m_workspaceArtifactSources->Start(std::move(request), [this](const auto& result) {
		// This callback deliberately reports aggregate, path-free state only. It
		// must never request Stop because it runs on the controller dispatcher.
		RecordWorkspaceArtifactSourceResult(result);
	});
	if (started.Succeeded()) m_workspaceArtifactTopology = snapshot;
	RecordWorkspaceArtifactSourceResult(started);
}

void CWorkbenchRuntime::UpdateWorkspaceArtifacts(const config::WorkspaceContextSnapshot& snapshot)
{
	if (IsStopRequested() || !m_workspaceArtifactSources) return;
	if (!m_workspaceArtifactTopology) {
		StartWorkspaceArtifacts(snapshot);
		return;
	}
	if (HasSameArtifactTopology(*m_workspaceArtifactTopology, snapshot)) {
		m_workspaceArtifactTopology = snapshot;
		return;
	}
	if (snapshot.revision == std::numeric_limits<std::uint64_t>::max()) {
		SetDiagnostic("workspace.artifacts", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceArtifacts,
			.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			.message = "workspace artifact generation cannot advance from the semantic revision",
		});
		return;
	}
	workspace::WorkspaceArtifactDocumentSourceRequest request;
	request.generation = snapshot.revision + 1;
	request.workspaceConfiguration = snapshot.kind == EWorkspaceKind::Workspace
		? snapshot.workspaceConfigUri : std::nullopt;
	request.workspaceFolders.reserve(snapshot.folders.size());
	for (const auto& folder : snapshot.folders) request.workspaceFolders.push_back(folder.uri);
	const auto updated = m_workspaceArtifactSources->Update(std::move(request));
	if (updated.Succeeded()) m_workspaceArtifactTopology = snapshot;
	RecordWorkspaceArtifactSourceResult(updated);
}

void CWorkbenchRuntime::ReconcileTaskCatalogs(const config::WorkspaceContextSnapshot& snapshot) noexcept
{
	if (IsStopRequested()) return;
	try {
		const auto reconciled = m_taskCatalogs.Reconcile(snapshot, m_workspaceArtifacts);
		if (IsStopRequested()
			|| reconciled.status == tasks::EFolderTaskCatalogRegistryStatus::Stopped) return;

		// A typed catalog rejection is an expected terminal result: the
		// registry retains its last-good folder catalog while the artifact
		// source remains responsible for reporting JSON/schema diagnostics.
		// Promoting that result to a runtime diagnostic would expose
		// notification ordering (and could mask a later aggregate source
		// diagnostic) as workbench state.
		SetDiagnostic("workspace.tasks", std::nullopt);
	} catch (...) {
		try {
			SetDiagnostic("workspace.tasks", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceArtifacts,
				.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
				.message = "folder task catalog reconciliation failed unexpectedly",
			});
		} catch (...) {
			// Artifact reconciliation is advisory. Runtime Stop remains the sole
			// owner of the terminal lifecycle if diagnostics cannot be recorded.
		}
	}
}

void CWorkbenchRuntime::OnWorkspaceArtifactsChanged(
	const workspace::WorkspaceArtifactDocumentServiceSnapshot& snapshot) noexcept
{
	if (snapshot.stopped || IsStopRequested()) return;
	ReconcileTaskCatalogs(m_workspaceContext.Snapshot());
}

config::WorkspaceTrustSettings CWorkbenchRuntime::ReadWorkspaceTrustSettings() const
{
	// Trust policy is profile-owned and is never accepted from a workspace or folder
	// document, so the read target carries only the selected profile.
	ConfigurationTarget target;
	target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();

	config::WorkspaceTrustSettings settings;
	const auto read = m_configuration.ReadSnapshot(
		{ "security.workspace.trust.enabled", "security.workspace.trust.emptyWindow" }, target);
	// A failed read must never widen trust. The defaults deny it for every folder and
	// match VS Code for an empty window, so falling back to them is the safe answer.
	if (read.outcome != config::EConfigurationOutcome::Applied || !read.snapshot || read.snapshot->values.size() != 2) {
		return settings;
	}
	const auto readBoolean = [](const config::ConfigurationValue& value, bool fallback) {
		const auto* boolean = std::get_if<bool>(&value.Value());
		return boolean ? *boolean : fallback;
	};
	settings.enabled = readBoolean(read.snapshot->values[0], settings.enabled);
	settings.emptyWindow = readBoolean(read.snapshot->values[1], settings.emptyWindow);
	return settings;
}

void CWorkbenchRuntime::ResolveAndApplyWorkspaceTrust(const config::WorkspaceContextSnapshot& workspace)
{
	if (IsStopRequested()) return;

	config::WorkspaceTrustResolveRequest request;
	request.kind = workspace.kind;
	request.workspaceConfigUri = workspace.workspaceConfigUri;
	request.folderUris.reserve(workspace.folders.size());
	for (const auto& folder : workspace.folders) request.folderUris.push_back(folder.uri);
	request.settings = ReadWorkspaceTrustSettings();
	// The durable list read once at Start. It stays empty when no store is composed,
	// when the profile has never trusted anything, and when the stored bytes failed to
	// decode — in every one of those cases no folder resolves to Trusted, which is the
	// honest answer rather than a placeholder assuming trust.
	request.trustedEntries = m_trustedFolders.entries;

	const auto resolution = config::ResolveWorkspaceTrust(request);
	// Clearing on every settled resolution, not only after a repair, is what stops one
	// transient commit failure from pinning the runtime in ReadyWithDiagnostics forever.
	if (workspace.trust == resolution.state) {
		SetDiagnostic("workspace.trust", std::nullopt);
		// Nothing moved, but this is still a settled resolution -- in particular the
		// first one, at Start(), where the resting Unknown trust never differs from
		// itself and this branch is the only one that ever runs. The banner must
		// reflect that settled state rather than sit on the layout model's
		// visible-by-default construction value until some later trust transition.
		UpdateRestrictedModeBannerVisibility();
		return;
	}

	config::SetTrustRequest setTrust;
	// A fresh identifier per resolution, because the value carried differs between
	// resolutions and a reused identifier would be reported as an operation conflict.
	setTrust.operation.operationId =
		"workbench.trust.resolve." + std::to_string(m_trustResolutionCount.fetch_add(1) + 1);
	setTrust.trust = resolution.state;
	const auto result = m_workspaceContext.SetTrust(setTrust);
	if (IsWorkspaceMutationSuccess(result.outcome)) {
		SetDiagnostic("workspace.trust", std::nullopt);
		// Trust just moved, so every previously published restricted key's
		// withheld/admitted state must move with it now, not only the next time
		// CExtensionService rescans and republishes the same key set. The outcome
		// is deliberately discarded: workspace trust is already committed above,
		// and a failed re-apply here must not fail trust resolution or roll the
		// commit back -- the configuration service's own revision, and whatever
		// the next successful SetExtensionRestrictedConfigurations call applies,
		// remain the recovery signal for a restricted-policy commit that lags.
		(void)ApplyRestrictedConfigurationPolicy();
		// Same reasoning as ApplyRestrictedConfigurationPolicy just above: trust
		// moving is exactly the "commits a new trust value" case the banner must
		// react to without waiting for a later configuration change.
		UpdateRestrictedModeBannerVisibility();
		return;
	}
	SetDiagnostic("workspace.trust", WorkbenchRuntimeDiagnostic {
		.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceContext,
		.code = EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
		.message = "workspace trust could not be committed to the semantic workspace service",
	});
}

void CWorkbenchRuntime::UpdateRestrictedModeBannerVisibility()
{
	if (IsStopRequested()) return;

	// Only Trusted counts as trusted, exactly matching ApplyRestrictedConfigurationPolicy:
	// ResolveWorkspaceTrust never produces Untrusted on its own (config/CLAUDE.md's
	// Workspace Trust Resolution Checkpoint), so Unknown reaches here whenever trust
	// has been withheld rather than explicitly granted, and the banner must treat
	// that exactly like Untrusted.
	const bool restricted = m_workspaceContext.Snapshot().trust != config::EWorkspaceTrustState::Trusted;

	bool visible = false;
	if (restricted) {
		// Banner policy is profile-owned, like every other security.workspace.trust.*
		// setting (BuiltinConfigurationDescriptors.cpp), so the read target carries
		// only the selected profile, never a workspace/folder URI.
		ConfigurationTarget target;
		target.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();
		const auto read = m_configuration.ReadSnapshot({ "security.workspace.trust.banner" }, target);
		// The descriptor's own default ("untilDismissed") is also the safe fallback
		// for a read that failed outright: an unreadable setting must never resolve
		// to the banner silently disappearing.
		std::wstring banner = L"untilDismissed";
		if (read.outcome == config::EConfigurationOutcome::Applied && read.snapshot && read.snapshot->values.size() == 1) {
			if (const auto* string = std::get_if<std::wstring>(&read.snapshot->values[0].Value())) {
				banner = *string;
			}
		}
		if (banner == L"never") {
			visible = false;
		} else if (banner == L"always") {
			visible = true;
		} else {
			// "untilDismissed" -- and this is also where any other unrecognized
			// stored value lands -- is an explicit typed-unsupported boundary, not an
			// approximation. This runtime has no durable per-workspace dismissal
			// record yet (config/CLAUDE.md's Restricted Configurations checkpoint
			// tracks the banner Part and the dismissal store as still open work), so
			// "untilDismissed" behaves exactly like "always" until that store lands.
			// The alternative -- a session-only dismissal flag that forgets itself on
			// restart -- would silently misrepresent "untilDismissed" as working, which
			// the root CLAUDE.md's fake-capability rule forbids; failing closed to
			// "always shown" is the honest answer instead.
			visible = true;
		}
	}

	const auto revisionBeforeWrite = m_layoutState.Snapshot().revision;
	const auto result = m_layoutState.SetPartVisibility({
		.operation = { .operationId =
			"workbench.trust.banner." + std::to_string(m_bannerVisibilityUpdateCount.fetch_add(1) + 1) },
		.partId = std::string(layout::ids::part::Banner),
		.visible = visible,
	});
	// NotApplicable means the Part was already showing the answer just computed --
	// a normal, frequent outcome (e.g. a profile settings reload that did not touch
	// trust or the banner setting), not a failure.
	if (result.status == layout::EWorkbenchLayoutOperationStatus::Succeeded
		|| result.status == layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
		if (result.status == layout::EWorkbenchLayoutOperationStatus::Succeeded) {
			// This Part's visibility is derived, never chosen: it is recomputed from
			// trust and the banner setting on every startup, and WorkbenchLayoutState
			// Service::MementoSnapshot() therefore keeps it out of the durable payload
			// entirely. A revision produced by nothing but this derived write must not
			// make the memento look dirty, or every launch of a restricted window would
			// perform an O(N) full-file layout write that changes not one stored byte.
			// The CAS only advances the baseline when this write is the sole change
			// since it, so a real user change still to be persisted is never swallowed.
			auto expected = revisionBeforeWrite;
			(void)m_layoutBaselineRevision.compare_exchange_strong(expected, result.revision);
		}
		SetDiagnostic("workbench.trust.banner", std::nullopt);
		return;
	}
	SetDiagnostic("workbench.trust.banner", WorkbenchRuntimeDiagnostic {
		.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceTrust,
		.code = EWorkbenchRuntimeDiagnosticCode::BannerVisibilityFailed,
		.message = "restricted mode banner visibility could not be committed to the layout state",
	});
}

EConfigurationOutcome CWorkbenchRuntime::SetExtensionRestrictedConfigurations(std::vector<std::string> keys)
{
	{
		std::lock_guard lock(m_extensionRestrictedConfigurationsMutex);
		m_extensionRestrictedConfigurations = std::move(keys);
	}
	return ApplyRestrictedConfigurationPolicy();
}

EConfigurationOutcome CWorkbenchRuntime::ApplyRestrictedConfigurationPolicy()
{
	std::vector<std::string> keys;
	{
		// Held only long enough to copy the set. CExtensionService's worker thread
		// can replace it concurrently with this runtime's own thread reading it
		// here (see the member's declaration comment), and the configuration-service
		// call below must not run while holding this lock.
		std::lock_guard lock(m_extensionRestrictedConfigurationsMutex);
		keys = m_extensionRestrictedConfigurations;
	}

	config::RestrictedConfigurationPolicy policy;
	// Only Trusted counts as trusted. ResolveWorkspaceTrust never produces
	// Untrusted on its own (config/CLAUDE.md's Workspace Trust Resolution
	// Checkpoint), so Unknown reaches here exactly when trust has been withheld
	// rather than explicitly granted; treating it as trusted would admit a
	// restricted key's workspace-scoped value before the workspace was ever
	// actually resolved.
	policy.workspaceTrusted = WorkspaceContext().Snapshot().trust == config::EWorkspaceTrustState::Trusted;
	policy.restrictedKeys = std::move(keys);

	// evaluationTarget deliberately carries only the selected profile, not a
	// workspace/folder URI. Verified against LayerIdentity and its callers in
	// CConfigurationService.cpp: a Workspace-scope source is registered only
	// while a `.code-workspace` is open (ApplyWorkspaceSettings, this file),
	// keyed by target.workspaceUri = that workspace's own configuration URI, and
	// each Folder-scope source is registered once per folder, keyed by
	// (workspaceUri, folderUri) (the per-folder settings loop, this file). A
	// multi-root workspace can have more than one folder, so there is no single
	// folderUri -- and, for a bare Folder workspace with no `.code-workspace`,
	// no workspaceUri either -- that this runtime could put in one target
	// without guessing which root the resulting notification is about.
	//
	// This choice does not weaken enforcement: CollectProvenanceLocked withholds
	// a restricted key's Workspace/Folder-scope contribution against whatever
	// target a real GetValue/ReadSnapshot/Inspect caller supplies later, which is
	// entirely independent of evaluationTarget. evaluationTarget only decides
	// which ConfigurationChange notifications this call itself emits for
	// Subscribe listeners. The consequence of a profile-only target is therefore
	// narrow: those notifications never report a Workspace/Folder-scoped
	// restricted key's own before/after move (it always resolves the same
	// Default/Application/Profile-scope value on both sides), even though the
	// very next real read of that key correctly reflects the new trust decision.
	// This mirrors the same profile-only shape
	// CExtensionWorkbenchServiceBridge::BuildConfigurationSnapshot and
	// WriteGlobalConfiguration already use, for the identical reason.
	policy.evaluationTarget.profileId = m_bootstrap.UserDataProfile().SelectedProfileId();

	const auto result = m_configuration.ApplyRestrictedConfigurations(policy);
	return result.outcome;
}

std::vector<config::WorkspaceTrustEntry> CWorkbenchRuntime::BuildTrustGrantEntries(
	const config::WorkspaceContextSnapshot& workspace,
	EWorkspaceTrustGrantScope scope)
{
	std::vector<config::WorkspaceTrustEntry> entries;
	switch (scope) {
	case EWorkspaceTrustGrantScope::CurrentWorkspace:
		// A .code-workspace file is itself the trustable item, and the resolver treats
		// trusting it as covering every folder that workspace lists no matter where
		// those folders live. It therefore never needs descendant coverage, and giving
		// it any would silently trust the directory the file happens to sit in.
		if (workspace.kind == config::EWorkspaceKind::Workspace && workspace.workspaceConfigUri) {
			entries.push_back(config::WorkspaceTrustEntry{ *workspace.workspaceConfigUri, false });
			return entries;
		}
		entries.reserve(workspace.folders.size());
		for (const auto& folder : workspace.folders) {
			entries.push_back(config::WorkspaceTrustEntry{ folder.uri, true });
		}
		return entries;
	case EWorkspaceTrustGrantScope::ParentFolder:
		// Upstream offers the parent only for a single folder root. With several roots
		// the label names one folder while the grant would widen every root's parent at
		// once, which is not the decision the user was shown.
		if (workspace.kind != config::EWorkspaceKind::Folder || workspace.folders.size() != 1) return entries;
		if (auto parent = config::WorkspaceTrustParentFolder(workspace.folders.front().uri)) {
			entries.push_back(config::WorkspaceTrustEntry{ std::move(*parent), true });
		}
		return entries;
	}
	return entries;
}

WorkspaceTrustPromptModel CWorkbenchRuntime::WorkspaceTrustPrompt()
{
	WorkspaceTrustPromptModel model;
	if (IsStopRequested()) return model;

	const auto workspace = m_workspaceContext.Snapshot();
	model.state = workspace.trust;
	// A grant that cannot be written is refused, so a prompt that cannot see this
	// would offer a choice the runtime already knows it will reject.
	model.persistenceReady = m_trustedFoldersStore != nullptr && m_trustedFoldersPersistenceReady;

	for (const auto scope : { EWorkspaceTrustGrantScope::CurrentWorkspace, EWorkspaceTrustGrantScope::ParentFolder }) {
		const auto entries = BuildTrustGrantEntries(workspace, scope);
		if (entries.empty()) continue;
		model.options.push_back(WorkspaceTrustGrantOption {
			.scope = scope,
			.displayUri = entries.front().uri.ToString(),
			.resourceCount = entries.size(),
		});
	}
	return model;
}

WorkspaceTrustGrantResult CWorkbenchRuntime::GrantWorkspaceTrust(EWorkspaceTrustGrantScope scope)
{
	if (IsStopRequested()) {
		return { EWorkspaceTrustGrantStatus::Stopped, "the workbench runtime is stopping" };
	}
	if (m_trustedFoldersStore == nullptr || !m_trustedFoldersPersistenceReady) {
		// Refusing is the whole point. Applying the grant in memory only would report
		// trust this window cannot keep: the next launch would drop back to withheld
		// trust with nothing recorded to explain why.
		return { EWorkspaceTrustGrantStatus::PersistenceUnavailable,
			"the durable trusted folders list is unavailable, so trust was not granted" };
	}

	const auto workspace = m_workspaceContext.Snapshot();
	auto requested = BuildTrustGrantEntries(workspace, scope);
	if (requested.empty()) {
		return { EWorkspaceTrustGrantStatus::NotApplicable,
			"this workspace has no resource that the requested scope can trust" };
	}

	auto next = m_trustedFolders;
	for (auto& entry : requested) {
		// The codec accepts duplicates, so nothing would reject a repeated grant -- the
		// durable list would just grow by one entry every time the user confirms. An
		// existing entry that already covers the resource with at least the same reach
		// is the same decision, so it is left alone.
		const auto alreadyCovers = [&entry](const config::WorkspaceTrustEntry& existing) {
			return (existing.includesDescendants || !entry.includesDescendants)
				&& config::WorkspaceTrustEntryCovers(existing, entry.uri);
		};
		if (std::any_of(next.entries.begin(), next.entries.end(), alreadyCovers)) continue;
		next.entries.push_back(std::move(entry));
	}
	if (next.entries.size() == m_trustedFolders.entries.size()) {
		return { EWorkspaceTrustGrantStatus::AlreadyTrusted, {} };
	}
	if (next.entries.size() > config::kMaximumTrustedFolderEntries) {
		return { EWorkspaceTrustGrantStatus::Failed,
			"the trusted folders list is at its maximum size" };
	}

	const auto recordFailure = [this](EWorkbenchRuntimeDiagnosticCode code, std::string message) {
		SetDiagnostic("trust.persist", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceTrust,
			.code = code,
			.message = std::move(message),
		});
	};

	const auto saved = m_trustedFoldersStore->Save(next);
	switch (saved.status) {
	case config::ETrustedFoldersSaveStatus::Persisted:
	case config::ETrustedFoldersSaveStatus::NotDirty:
		break;
	case config::ETrustedFoldersSaveStatus::Conflict:
		// Another window committed a different list. Its bytes stay; this grant is
		// reported as a conflict rather than replayed over the newer state.
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustPersistenceConflict,
			"another window committed a different trusted folders list");
		return { EWorkspaceTrustGrantStatus::Conflict,
			"another window committed a different trusted folders list" };
	case config::ETrustedFoldersSaveStatus::Unavailable:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustPersistenceUnavailable,
			"trusted folders persistence was unavailable while granting trust");
		return { EWorkspaceTrustGrantStatus::PersistenceUnavailable,
			"trusted folders persistence was unavailable" };
	case config::ETrustedFoldersSaveStatus::Stopped:
		return { EWorkspaceTrustGrantStatus::Stopped, "trusted folders persistence is stopping" };
	case config::ETrustedFoldersSaveStatus::RetryExhausted:
	case config::ETrustedFoldersSaveStatus::Failed:
		recordFailure(EWorkbenchRuntimeDiagnosticCode::TrustPersistFailed,
			"the trusted folders list could not be written");
		return { EWorkspaceTrustGrantStatus::Failed, "the trusted folders list could not be written" };
	}

	// Only now, with the durable bytes committed, may this window act trusted. The
	// resolver is re-run rather than trusted directly, so one code path decides trust
	// and a grant that does not actually cover the workspace cannot fake it.
	m_trustedFolders = std::move(next);
	SetDiagnostic("trust.persist", std::nullopt);
	ResolveAndApplyWorkspaceTrust(m_workspaceContext.Snapshot());
	return { EWorkspaceTrustGrantStatus::Granted, {} };
}

void CWorkbenchRuntime::OnWorkspaceContextChanged(const config::WorkspaceContextChange& change) noexcept
{
	try {
		{
			std::lock_guard lock(m_stateMutex);
			if (IsStopRequested()
				|| (m_state != EWorkbenchRuntimeState::Ready && m_state != EWorkbenchRuntimeState::ReadyWithDiagnostics)) return;
		}
		// Trust follows the workspace shape, so it must be re-resolved even when the
		// configuration shape is unchanged and the reload below is skipped. Resolving
		// an unchanged value commits nothing, so this converges after one step and
		// never recurses through its own notification.
		ResolveAndApplyWorkspaceTrust(change.current);
		{
			std::lock_guard sourceLock(m_sourceMutex);
			if (m_exactWorkspaceAcceptanceActive) return;
		}
		if (HasSameConfigurationShape(change.previous, change.current)) return;
		ReloadWorkspaceSettings(change.current);
		// The settings reload can reconcile workspace-file folders, so artifacts
		// must follow the fresh semantic snapshot rather than the notification.
		UpdateWorkspaceArtifacts(m_workspaceContext.Snapshot());
		RefreshFileWatching();
	} catch (...) {
		try {
			SetDiagnostic("workspace.internal", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::WorkspaceContext,
				.code = EWorkbenchRuntimeDiagnosticCode::InternalFailure,
				.message = "workspace configuration refresh failed unexpectedly",
			});
		} catch (...) {
			std::lock_guard lock(m_stateMutex);
			m_state = EWorkbenchRuntimeState::Failed;
			if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		}
	}
}

void CWorkbenchRuntime::OnContributionRegistryChanged(const layout::WorkbenchContributionChange& change) noexcept
{
	try {
		{
			std::lock_guard lock(m_stateMutex);
			if (IsStopRequested() || m_state == EWorkbenchRuntimeState::Stopped || m_state == EWorkbenchRuntimeState::Failed) return;
		}
		const auto result = m_layoutState.Reconcile(m_contributions.Snapshot(), {
			.operation = { .operationId = "runtime.contributions.reconcile/" + std::to_string(change.revision) },
		});
		if (result.status == layout::EWorkbenchLayoutOperationStatus::Succeeded
			|| result.status == layout::EWorkbenchLayoutOperationStatus::NotApplicable) {
			SetDiagnostic("layout.contributions", std::nullopt);
			return;
		}
		SetDiagnostic("layout.contributions", WorkbenchRuntimeDiagnostic {
			.source = EWorkbenchRuntimeDiagnosticSource::Layout,
			.code = EWorkbenchRuntimeDiagnosticCode::LayoutReconcileFailed,
			.message = "workbench contribution layout reconciliation did not reach an accepted terminal state",
		});
	} catch (...) {
		try {
			SetDiagnostic("layout.contributions", WorkbenchRuntimeDiagnostic {
				.source = EWorkbenchRuntimeDiagnosticSource::Layout,
				.code = EWorkbenchRuntimeDiagnosticCode::LayoutReconcileFailed,
				.message = "workbench contribution layout reconciliation failed unexpectedly",
			});
		} catch (...) {
			std::lock_guard lock(m_stateMutex);
			m_state = EWorkbenchRuntimeState::Failed;
			if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		}
	}
}

WorkbenchRuntimeResult CWorkbenchRuntime::Start()
{
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	if (m_startActive) return CurrentResult(EWorkbenchRuntimeResultCode::Busy);
	if (IsStopRequested()) {
		const auto snapshot = Snapshot();
		return { snapshot.state == EWorkbenchRuntimeState::Stopped
			? EWorkbenchRuntimeResultCode::Stopped : EWorkbenchRuntimeResultCode::Busy, snapshot };
	}
	m_startActive = true;
	struct StartActivity final {
		bool& active;
		~StartActivity() { active = false; }
	} startActivity { m_startActive };
	std::optional<EWorkbenchRuntimeResultCode> earlyResult;
	{
		std::lock_guard lock(m_stateMutex);
		if (m_state == EWorkbenchRuntimeState::Ready || m_state == EWorkbenchRuntimeState::ReadyWithDiagnostics) {
			earlyResult = EWorkbenchRuntimeResultCode::AlreadyReady;
		}
		else if (m_state == EWorkbenchRuntimeState::Failed) {
			earlyResult = EWorkbenchRuntimeResultCode::Failed;
		}
		else if (m_state == EWorkbenchRuntimeState::Stopped) {
			earlyResult = EWorkbenchRuntimeResultCode::Stopped;
		}
	}
	if (earlyResult) return CurrentResult(*earlyResult);
	auto terminalResult = [this]() -> std::optional<WorkbenchRuntimeResult> {
		const auto snapshot = Snapshot();
		if (IsStopRequested()) {
			return WorkbenchRuntimeResult {
				snapshot.state == EWorkbenchRuntimeState::Stopped
					? EWorkbenchRuntimeResultCode::Stopped : EWorkbenchRuntimeResultCode::Busy,
				snapshot
			};
		}
		if (snapshot.state == EWorkbenchRuntimeState::Stopped) {
			return WorkbenchRuntimeResult { EWorkbenchRuntimeResultCode::Stopped, snapshot };
		}
		if (snapshot.state == EWorkbenchRuntimeState::Failed) {
			return WorkbenchRuntimeResult { EWorkbenchRuntimeResultCode::Failed, snapshot };
		}
		return std::nullopt;
	};

	if (!m_initializationFailure.empty() || !m_fileSources) {
		return FailStart(EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			m_initializationFailure.empty()
				? "configuration filesystem is unavailable"
				: m_initializationFailure);
	}

	try {
		RestoreInitialLayoutMemento();
		RestoreStatusbarVisibilityMemento();
		if (auto terminal = terminalResult()) return std::move(*terminal);

		// Recent history is non-critical and is intentionally fail-closed: a
		// malformed or unavailable control record never blocks a window, never
		// rewrites storage, and remains unavailable to command context until a
		// later successful store operation.
		if (m_recentlyOpenedWorkspaces) (void)m_recentlyOpenedWorkspaces->Load();

		if (!ApplyBootstrapWorkspace()) {
			return FailStart(EWorkbenchRuntimeDiagnosticCode::WorkspaceTransitionFailed,
				"immutable bootstrap workspace did not match the semantic workspace service");
		}
		if (auto terminal = terminalResult()) return std::move(*terminal);

		ReloadProfileSettings();
		if (auto terminal = terminalResult()) return std::move(*terminal);
		const auto initiallyLoadedWorkspace = m_workspaceContext.Snapshot();
		ReloadWorkspaceSettings(initiallyLoadedWorkspace);
		if (auto terminal = terminalResult()) return std::move(*terminal);

		// Trust resolves after the profile settings are loaded, because it reads
		// security.workspace.trust.*, and before anything can observe the context.
		// Bootstrap leaves trust unresolved rather than guessing it. The durable
		// trusted-folders list is read immediately before that resolution, and only
		// there: re-reading it per workspace change would let a concurrent write from
		// another window silently change this window's trust mid-session.
		RestoreTrustedFolders();
		if (auto terminal = terminalResult()) return std::move(*terminal);
		ResolveAndApplyWorkspaceTrust(m_workspaceContext.Snapshot());
		if (auto terminal = terminalResult()) return std::move(*terminal);

		const auto gate = m_listenerGate;
		m_workspaceSubscription = m_workspaceContext.Subscribe([gate](const config::WorkspaceContextChange& change) {
			CWorkbenchRuntime::DispatchListener(gate, [&change](CWorkbenchRuntime& owner) {
				owner.OnWorkspaceContextChanged(change);
			});
		});

		{
			std::lock_guard lock(m_stateMutex);
			if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
				m_state = EWorkbenchRuntimeState::Failed;
			} else {
				m_state = m_diagnostics.empty()
					? EWorkbenchRuntimeState::Ready
					: EWorkbenchRuntimeState::ReadyWithDiagnostics;
				++m_revision;
			}
		}
		if (auto terminal = terminalResult()) {
			m_workspaceSubscription.Reset();
			return std::move(*terminal);
		}

		// A context may have changed between the initial load and subscription.
		// Once Ready is observable, callbacks handle any later revision; this
		// reconciliation closes the earlier gap without guessing from a document.
		const auto currentWorkspace = m_workspaceContext.Snapshot();
		bool workspaceNeedsReconciliation = false;
		{
			std::lock_guard sourceLock(m_sourceMutex);
			workspaceNeedsReconciliation = !m_loadedWorkspaceRevision
				|| *m_loadedWorkspaceRevision != currentWorkspace.revision;
		}
		if (workspaceNeedsReconciliation) {
			ReloadWorkspaceSettings(currentWorkspace);
		}
		if (auto terminal = terminalResult()) return std::move(*terminal);
		// Reconciliation above is the final semantic workspace topology for this
		// bootstrap. Artifact reads must begin before the existing settings watches.
		const auto artifactWorkspace = m_workspaceContext.Snapshot();
		// Publish a known-empty slot for every explicit folder before filesystem
		// results arrive. Artifact notifications replace each slot atomically.
		ReconcileTaskCatalogs(artifactWorkspace);
		StartWorkspaceArtifacts(artifactWorkspace);
		if (auto terminal = terminalResult()) return std::move(*terminal);
		StartFileWatching();
		if (auto terminal = terminalResult()) return std::move(*terminal);

		const auto snapshot = Snapshot();
		return { snapshot.state == EWorkbenchRuntimeState::Ready
			? EWorkbenchRuntimeResultCode::Ready : EWorkbenchRuntimeResultCode::ReadyWithDiagnostics, snapshot };
	} catch (...) {
		if (auto terminal = terminalResult()) return std::move(*terminal);
		return FailStart(EWorkbenchRuntimeDiagnosticCode::InternalFailure,
			"workbench startup failed unexpectedly");
	}
}

WorkbenchRuntimeResult CWorkbenchRuntime::Stop() noexcept
{
	try {
		m_stopRequested->store(true, std::memory_order_release);
		{
			std::lock_guard gateLock(m_listenerGate->mutex);
			m_listenerGate->stopRequested = true;
		}
		if (IsExecutingListener()) return CurrentResult(EWorkbenchRuntimeResultCode::Busy);
		if (CompleteStopAfterListeners()) return CurrentResult(EWorkbenchRuntimeResultCode::Stopped);
		return CurrentResult(EWorkbenchRuntimeResultCode::Busy);
	} catch (...) {
		// Stop is a destructor-safe boundary, but an exception before terminal
		// publication must never fabricate a Stopped result. A later external
		// Stop coalesces with this pending request and retries finalization.
		return { EWorkbenchRuntimeResultCode::Busy, {} };
	}
}

bool CWorkbenchRuntime::CompleteStopAfterListeners() noexcept
{
	if (!IsStopRequested() || IsExecutingListener()) return false;
	try {
		if (m_settingsWriteback) (void)m_settingsWriteback->Stop();
		if (m_fileWatches) (void)m_fileWatches->Stop();
		{
			std::unique_lock gateLock(m_listenerGate->mutex);
			m_listenerGate->drained.wait(gateLock, [this] { return m_listenerGate->activeCallbacks == 0; });
		}
		std::lock_guard lifecycleLock(m_lifecycleMutex);
		{
			std::lock_guard lock(m_stateMutex);
			if (m_state == EWorkbenchRuntimeState::Stopped) return true;
		}
		// Start and topology updates are now excluded by both the listener gate
		// and lifecycle lock. First disconnect Task catalog updates, then quiesce
		// every running Task before stopping catalog and artifact ownership.
		if (m_workspaceArtifactSubscription) {
			m_workspaceArtifacts.Unsubscribe(*m_workspaceArtifactSubscription);
			m_workspaceArtifactSubscription.reset();
		}
		const auto taskStop = m_taskExecution.Stop();
		if (taskStop.status == tasks::ETaskExecutionOperationStatus::Deferred
			|| !m_taskExecution.Snapshot().stopped) return false;
		(void)m_taskCatalogs.Stop();
		// Join the controller before stopping its borrowed document service so no
		// post-stop Apply can race either boundary.
		if (m_workspaceArtifactSources) (void)m_workspaceArtifactSources->Stop();
		m_workspaceSubscription.Reset();
		(void)m_workspaceArtifacts.Stop();
		if (!StopOwnedServices()) return false;
		if (m_contributionSubscription) {
			m_contributions.Unsubscribe(*m_contributionSubscription);
			m_contributionSubscription.reset();
		}
		PersistFinalLayoutMemento();
		{
			std::lock_guard lock(m_stateMutex);
			m_state = EWorkbenchRuntimeState::Stopped;
			if (m_revision != std::numeric_limits<std::uint64_t>::max()) ++m_revision;
		}
		std::lock_guard gateLock(m_listenerGate->mutex);
		m_listenerGate->owner = nullptr;
		return true;
	} catch (...) {
		// Stop remains noexcept. A concurrent caller can retry finalization, and
		// destructor shutdown still retains ownership of the terminal transition.
		return false;
	}
}

bool CWorkbenchRuntime::StopOwnedServices() noexcept
{
	// Stop in reverse declaration/composition order. Keep both objects alive
	// until runtime destruction so a previously borrowed pointer observes the
	// service's typed Stopped result rather than becoming dangling mid-stop.
	const auto outputStop = m_output.Stop();
	const auto markerStop = m_markers.Stop();
	const auto scmStop = m_scm.Stop();
	return !outputStop.callbackDrainDeferred && !markerStop.callbackDrainDeferred &&
		(scmStop.status == scm::EScmOperationStatus::Succeeded || scmStop.status == scm::EScmOperationStatus::Stopped);
}

} // namespace workbench
