/*! @file
 * @brief Process-local owner for workbench configuration and workspace truth.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/CConfigurationService.h"
#include "config/CWorkspaceContextService.h"
#include "config/ITrustedFoldersStore.h"
#include "config/JsoncConfigurationSource.h"
#include "config/WorkspaceTrustPolicy.h"
#include "config/SettingsWritebackCoordinator.h"
#include <sakura/filesystem/IFileService.h>
#include "workbench/IWorkbenchRuntime.h"
#include "workbench/layout/WorkbenchContributionRegistry.h"
#include "workbench/layout/IWorkbenchLayoutMementoStore.h"
#include "workbench/layout/WorkbenchLayoutStateService.h"
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/recent/RecentlyOpenedWorkspaceService.h"
#include "workbench/scm/SourceControlService.h"
#include "workbench/statusbar/IStatusbarVisibilityMementoStore.h"
#include "workbench/statusbar/StatusbarViewModel.h"
#include "workbench/tasks/FolderTaskCatalogRegistry.h"
#include "workbench/tasks/TaskExecutionService.h"
#include "workbench/workspace/WorkspaceConfigurationTypes.h"
#include "workbench/workspace/WorkspaceArtifactDocumentService.h"
#include "workbench/workspace/WorkspaceEditingService.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace config {
class CConfigurationFileSourceController;
class CConfigurationFileWatchController;
enum class EConfigurationFileWatchChange : std::uint8_t;
struct ConfigurationFileSourceControllerResult;
}

namespace workbench {

namespace workspace {
class CWorkspaceArtifactDocumentSourceController;
struct WorkspaceArtifactDocumentSourceResult;
}

//! Tests may supply a scheme-aware file service. Production creates the Win32
//! `file:` provider when no service is injected.
struct WorkbenchRuntimeDependencies final {
	std::unique_ptr<platform::filesystem::IFileService> fileService;
	//! Optional production composition seam. The pure layout model never sees
	//! control IPC or a durable writer through this interface.
	std::unique_ptr<layout::IWorkbenchLayoutMementoStore> layoutMementoStore;
	std::unique_ptr<statusbar::IStatusbarVisibilityMementoStore> statusbarVisibilityMementoStore;
	//! Optional host adapter. Tests may leave this null to prove the runtime
	//! rejects execution without creating operating-system processes.
	std::shared_ptr<tasks::ITaskExecutionSessionFactory> taskExecutionSessionFactory;
	//! Profile/User persistence stays behind a control-process adapter. The
	//! runtime owns the UI-independent service, never the durable backend.
	std::unique_ptr<recent::IRecentlyOpenedWorkspaceStore> recentlyOpenedWorkspaceStore;
	//! The durable Trusted Folders and Workspaces list. Null is a real state: the
	//! runtime then resolves trust against an empty entry list, which is honest —
	//! no folder has been granted trust — rather than a degraded mode. It must
	//! never fall back to trusting anything.
	std::unique_ptr<config::ITrustedFoldersStore> trustedFoldersStore;
};

class CWorkbenchRuntime final : public IWorkbenchRuntime {
public:
	CWorkbenchRuntime(
		WorkbenchBootstrapContext bootstrap,
		std::vector<config::ConfigurationDescriptor> descriptors);
	CWorkbenchRuntime(
		WorkbenchBootstrapContext bootstrap,
		std::vector<config::ConfigurationDescriptor> descriptors,
		WorkbenchRuntimeDependencies dependencies);
	~CWorkbenchRuntime() override;

	CWorkbenchRuntime(const CWorkbenchRuntime&) = delete;
	CWorkbenchRuntime& operator=(const CWorkbenchRuntime&) = delete;

	[[nodiscard]] WorkbenchRuntimeResult Start();
	[[nodiscard]] WorkbenchRuntimeResult Stop() noexcept;
	//! The runtime is the sole production owner for settings writeback.  It
	//! serializes the versioned edit with the same file-source controller used by
	//! advisory watches, then resnapshots before exposing a terminal result.
	[[nodiscard]] config::SettingsWritebackResult WriteSetting(const config::SettingsWritebackRequest& request) override;

	[[nodiscard]] const WorkbenchBootstrapContext& Bootstrap() const noexcept override { return m_bootstrap; }
	[[nodiscard]] config::IConfigurationService& Configuration() noexcept override { return m_configuration; }
	[[nodiscard]] config::IWorkspaceContextService& WorkspaceContext() noexcept override { return m_workspaceContext; }
	[[nodiscard]] config::WorkspaceContextResult SwitchToFolderWorkspace(
		platform::uri::Uri folderUri, std::wstring displayName) override;
	[[nodiscard]] WorkspaceTrustPromptModel WorkspaceTrustPrompt() override;
	[[nodiscard]] WorkspaceTrustGrantResult GrantWorkspaceTrust(EWorkspaceTrustGrantScope scope) override;
	[[nodiscard]] workspace::IWorkspaceEditingService* WorkspaceEditing() noexcept override { return m_workspaceEditing.get(); }
	[[nodiscard]] workspace::WorkspaceEditingResult ReplaceCurrentWorkspaceFolders(
		const workspace::WorkspaceFoldersEditRequest& request) override;
	[[nodiscard]] recent::IRecentlyOpenedWorkspaceService* RecentlyOpenedWorkspaces() noexcept override;
	[[nodiscard]] layout::WorkbenchContributionRegistry& Contributions() noexcept override { return m_contributions; }
	[[nodiscard]] const layout::WorkbenchContributionRegistry& Contributions() const noexcept override { return m_contributions; }
	[[nodiscard]] layout::WorkbenchLayoutStateService& LayoutState() noexcept override { return m_layoutState; }
	[[nodiscard]] const layout::WorkbenchLayoutStateService& LayoutState() const noexcept override { return m_layoutState; }
	[[nodiscard]] statusbar::StatusbarViewModel& StatusbarState() noexcept override { return m_statusbarState; }
	[[nodiscard]] const statusbar::StatusbarViewModel& StatusbarState() const noexcept override { return m_statusbarState; }
	[[nodiscard]] statusbar::StatusbarMementoSaveResult PersistStatusbarVisibility() override;
	[[nodiscard]] workspace::WorkspaceConfigurationRuntimeSnapshot WorkspaceConfiguration() const override;
	[[nodiscard]] const workspace::CWorkspaceArtifactDocumentService& WorkspaceArtifacts() const noexcept override { return m_workspaceArtifacts; }
	[[nodiscard]] problems::MarkerService* Markers() noexcept override;
	[[nodiscard]] const problems::MarkerService* Markers() const noexcept override;
	[[nodiscard]] output::OutputService* Output() noexcept override;
	[[nodiscard]] const output::OutputService* Output() const noexcept override;
	[[nodiscard]] scm::SourceControlService* Scm() noexcept override;
	[[nodiscard]] const scm::SourceControlService* Scm() const noexcept override;
	[[nodiscard]] std::optional<tasks::FolderTaskCatalogSnapshot> TaskCatalogForFolder(
		const platform::uri::Uri& folderUri) const override;
	[[nodiscard]] tasks::TaskExecutionService* TaskExecution() noexcept override;
	[[nodiscard]] const tasks::TaskExecutionService* TaskExecution() const noexcept override;
	[[nodiscard]] WorkbenchRuntimeSnapshot Snapshot() const override;
	[[nodiscard]] config::EConfigurationOutcome SetExtensionRestrictedConfigurations(std::vector<std::string> keys) override;

private:
	struct ListenerGate final {
		std::mutex mutex;
		std::condition_variable drained;
		CWorkbenchRuntime* owner = nullptr;
		std::size_t activeCallbacks = 0;
		bool stopRequested = false;
	};

	[[nodiscard]] WorkbenchRuntimeResult CurrentResult(EWorkbenchRuntimeResultCode code) const;
	[[nodiscard]] WorkbenchRuntimeResult FailStart(
		EWorkbenchRuntimeDiagnosticCode code,
		std::string message);
	[[nodiscard]] bool ApplyBootstrapWorkspace();
	void ReloadProfileSettings();
	void StartFileWatching();
	void RefreshFileWatching();
	void OnConfigurationFileWatchChange(config::EConfigurationFileWatchChange change) noexcept;
	void RestoreInitialLayoutMemento();
	void RestoreStatusbarVisibilityMemento();
	//! Reads the durable Trusted Folders and Workspaces list once, before the first
	//! trust resolution. It must run after the profile settings load and before
	//! ResolveAndApplyWorkspaceTrust, because the resolver reads the list it produces.
	void RestoreTrustedFolders();
	void PersistFinalLayoutMemento() noexcept;
	void ReloadWorkspaceSettings(const config::WorkspaceContextSnapshot& snapshot);
	void ReloadWorkspaceSettingsNow(const config::WorkspaceContextSnapshot& snapshot,
		const std::string* exactWorkspaceDocument = nullptr);
	void StartWorkspaceArtifacts(const config::WorkspaceContextSnapshot& snapshot);
	void UpdateWorkspaceArtifacts(const config::WorkspaceContextSnapshot& snapshot);
	void ReconcileTaskCatalogs(const config::WorkspaceContextSnapshot& snapshot) noexcept;
	void OnWorkspaceArtifactsChanged(
		const workspace::WorkspaceArtifactDocumentServiceSnapshot& snapshot) noexcept;
	void RecordWorkspaceArtifactSourceResult(const workspace::WorkspaceArtifactDocumentSourceResult& result) noexcept;
	void ClearWorkspaceFolderSettings();
	//! Returns true only after the runtime itself has published Stopped. A
	//! callback-originated owned-service Stop deliberately leaves finalization
	//! pending for a later external caller.
	[[nodiscard]] bool CompleteStopAfterListeners() noexcept;
	//! Returns false when an owned service is stopping from one of its own
	//! callbacks and must finish that callback before runtime finalization.
	[[nodiscard]] bool StopOwnedServices() noexcept;
	[[nodiscard]] bool IsStopRequested() const noexcept;
	[[nodiscard]] bool IsReadyForServiceAccessLocked() const noexcept;
	static void DispatchListener(
		const std::shared_ptr<ListenerGate>& gate,
		const std::function<void(CWorkbenchRuntime&)>& callback) noexcept;
	[[nodiscard]] bool IsExecutingListener() const noexcept;
	[[nodiscard]] std::optional<std::string> NextWorkspaceOperationId();
	void RecordWorkspaceDocumentDiagnostic(
		std::string key,
		EWorkbenchRuntimeDiagnosticCode code,
		std::string message);
	[[nodiscard]] bool ApplyWorkspaceSettings(
		const config::WorkspaceContextSnapshot& snapshot,
		const std::optional<platform::serialization::JsoncValue::Object>& settings);
	void ClearWorkspaceSettings();
	void SetWorkspaceConfigurationSnapshot(workspace::WorkspaceConfigurationRuntimeSnapshot snapshot);
	[[nodiscard]] config::WorkspaceTrustSettings ReadWorkspaceTrustSettings() const;
	//! Resolves trust from the workspace shape and the profile trust settings, and
	//! commits it. This is the only production caller of SetTrust.
	void ResolveAndApplyWorkspaceTrust(const config::WorkspaceContextSnapshot& workspace);
	//! Computes whether the Restricted Mode banner Part should be visible and
	//! pushes that single answer through WorkbenchLayoutStateService::SetPartVisibility.
	//! This is the only production writer of the banner Part's visibility: a native
	//! host reads WorkbenchLayoutStateService, it never re-derives the answer from
	//! trust state and configuration itself (see this method's definition for why).
	//! Called after ResolveAndApplyWorkspaceTrust settles a trust decision, after a
	//! profile settings reload, and after a successful WriteSetting -- every place
	//! this runtime already reacts to a configuration or trust change -- so editing
	//! security.workspace.trust.banner or granting trust takes effect without a
	//! restart.
	void UpdateRestrictedModeBannerVisibility();
	//! Recomputes the joint restricted-configuration fact (published key set plus
	//! current trust) and commits it to the configuration service. Called both from
	//! SetExtensionRestrictedConfigurations (key set changed) and from
	//! ResolveAndApplyWorkspaceTrust (trust changed), so either input moving is
	//! enough to keep withholding correct without waiting for the other to change too.
	[[nodiscard]] config::EConfigurationOutcome ApplyRestrictedConfigurationPolicy();
	//! The entries one grant scope would add for one workspace shape. Pure: it reads
	//! only the snapshot, so the prompt and the grant cannot disagree about what a
	//! choice means.
	[[nodiscard]] static std::vector<config::WorkspaceTrustEntry> BuildTrustGrantEntries(
		const config::WorkspaceContextSnapshot& workspace,
		EWorkspaceTrustGrantScope scope);
	void OnWorkspaceContextChanged(const config::WorkspaceContextChange& change) noexcept;
	void OnContributionRegistryChanged(const layout::WorkbenchContributionChange& change) noexcept;
	void RecordFileSourceResult(
		std::string diagnosticKey,
		EWorkbenchRuntimeDiagnosticSource source,
		const config::ConfigurationFileSourceControllerResult& result);
	void SetDiagnostic(std::string key, std::optional<WorkbenchRuntimeDiagnostic> diagnostic);
	void RefreshReadyStateLocked();
	[[nodiscard]] bool HasTerminalState() const;
	[[nodiscard]] std::optional<std::string> NextWorkspaceDocumentKey();

	const WorkbenchBootstrapContext m_bootstrap;
	config::CConfigurationService m_configuration;
	config::CWorkspaceContextService m_workspaceContext;
	//! Contribution built-ins are constructed before the initial layout snapshot.
	//! Registry notifications reconcile this pure model synchronously; adapters never reach HWND state directly.
	layout::WorkbenchContributionRegistry m_contributions;
	layout::WorkbenchLayoutStateService m_layoutState;
	std::unique_ptr<layout::IWorkbenchLayoutMementoStore> m_layoutMementoStore;
	statusbar::StatusbarViewModel m_statusbarState;
	std::unique_ptr<statusbar::IStatusbarVisibilityMementoStore> m_statusbarVisibilityMementoStore;
	bool m_statusbarPersistenceReady = false;
	/*!
		The layout revision the durable memento already records. Atomic because a
		derived, non-durable Part change can advance it from a settings-writeback or
		trust-resolution thread, while orderly shutdown reads it to decide whether a
		write is needed at all.
	*/
	std::atomic<std::uint64_t> m_layoutBaselineRevision { 0 };
	bool m_layoutPersistenceReady = false;
	std::unique_ptr<config::ITrustedFoldersStore> m_trustedFoldersStore;
	//! The durable list, read once at Start and thereafter the runtime's working
	//! copy. Re-reading it on every workspace change would let a concurrent write
	//! from another window silently change this window's trust mid-session; VS
	//! Code's own trust list is likewise session-stable until this window edits it.
	config::TrustedFoldersSnapshot m_trustedFolders;
	/*!
		Whether the durable list is usable for granting. False after a load that
		returned InvalidStoredList or a transport failure. Trust still resolves in
		that state — against the empty in-memory list, so nothing is trusted — but a
		grant must be refused rather than written, because writing would overwrite
		durable bytes this runtime failed to understand.
	 */
	bool m_trustedFoldersPersistenceReady = false;
	std::optional<layout::WorkbenchContributionSubscriptionId> m_contributionSubscription;
	std::unique_ptr<platform::filesystem::IFileService> m_fileService;
	std::unique_ptr<workspace::IWorkspaceEditingService> m_workspaceEditing;
	std::unique_ptr<recent::IRecentlyOpenedWorkspaceService> m_recentlyOpenedWorkspaces;
	std::unique_ptr<config::CConfigurationFileSourceController> m_fileSources;
	std::unique_ptr<config::CSettingsWritebackCoordinator> m_settingsWriteback;
	//! Owns advisory watchers and their cancellation/join boundary. It reports
	//! only resource classes; this runtime resnapshots and applies through the
	//! existing revisioned file-source controller.
	std::unique_ptr<config::CConfigurationFileWatchController> m_fileWatches;
	//! Keep the pure document service before its file-backed controller: the
	//! controller borrows this service and must be joined before it is stopped.
	workspace::CWorkspaceArtifactDocumentService m_workspaceArtifacts;
	std::unique_ptr<workspace::CWorkspaceArtifactDocumentSourceController> m_workspaceArtifactSources;
	//! Catalog selection is semantic-folder scoped and atomically derived from
	//! the artifact service. Execution owns copied definitions, never catalogs.
	tasks::CFolderTaskCatalogRegistry m_taskCatalogs;
	tasks::TaskExecutionService m_taskExecution;
	std::optional<std::uint64_t> m_workspaceArtifactSubscription;
	//! These HWND-free authorities remain internal until Start reaches a ready
	//! terminal state. Their limits are explicit runtime composition policy.
	problems::MarkerService m_markers;
	output::OutputService m_output;
	scm::SourceControlService m_scm;
	std::optional<config::WorkspaceContextSnapshot> m_workspaceArtifactTopology;
	std::shared_ptr<std::atomic_bool> m_stopRequested;
	mutable std::recursive_mutex m_lifecycleMutex;
	mutable std::mutex m_stateMutex;
	std::mutex m_sourceMutex;
	std::shared_ptr<ListenerGate> m_listenerGate;
	config::WorkspaceContextSubscription m_workspaceSubscription;
	//! Each trust resolution needs its own durable operation identifier: the context
	//! service treats a repeated identifier carrying a different value as a conflict,
	//! not as a new request.
	std::atomic<std::uint64_t> m_trustResolutionCount { 0 };
	//! Each banner visibility push needs its own operation identifier for the same
	//! reason m_trustResolutionCount does: WorkbenchLayoutStateService treats a
	//! repeated identifier carrying a different visible value as a conflict.
	std::atomic<std::uint64_t> m_bannerVisibilityUpdateCount { 0 };
	//! The extension-declared restricted-configuration key set most recently
	//! published by CExtensionService::LoadInstalledExtensionRootsWorker, which
	//! runs on that service's own worker thread. ApplyRestrictedConfigurationPolicy
	//! reads it from this runtime's thread (Start()/OnWorkspaceContextChanged/
	//! GrantWorkspaceTrust, all invoked on the runtime's owning thread), so the two
	//! threads can race to write and read the set; this dedicated mutex guards only
	//! the set itself and is held just long enough to copy it, never across the
	//! configuration-service call that follows.
	std::mutex m_extensionRestrictedConfigurationsMutex;
	std::vector<std::string> m_extensionRestrictedConfigurations;
	std::map<std::wstring, std::string, std::less<>> m_activeWorkspaceDocuments;
	std::set<std::string, std::less<>> m_workspaceDiagnosticKeys;
	//! Owner identity is independent from folder order. It determines whether an
	//! incoming context replaces all previously tracked folder sources.
	std::optional<std::wstring> m_workspaceFolderOwnerIdentity;
	config::JsoncConfigurationSourceRevisions m_workspaceSettingsRevisions {};
	bool m_workspaceSettingsActive = false;
	std::optional<platform::uri::Uri> m_workspaceSettingsResource;
	workspace::WorkspaceConfigurationRuntimeSnapshot m_workspaceConfiguration;
	std::optional<std::uint64_t> m_loadedWorkspaceRevision;
	//! Suppresses the ordinary context-listener reload while a CAS result is
	//! being accepted from its exact committed bytes. The accepting operation
	//! performs artifact/watch reconciliation once after the terminal decision.
	bool m_exactWorkspaceAcceptanceActive = false;
	std::optional<config::WorkspaceContextSnapshot> m_pendingWorkspaceSnapshot;
	bool m_workspaceReloadActive = false;
	bool m_startActive = false;
	std::uint64_t m_nextWorkspaceDocument = 1;
	std::uint64_t m_nextWorkspaceOperation = 1;
	std::string m_initializationFailure;

	EWorkbenchRuntimeState m_state = EWorkbenchRuntimeState::Created;
	std::uint64_t m_revision = 0;
	std::map<std::string, WorkbenchRuntimeDiagnostic, std::less<>> m_diagnostics;
};

} // namespace workbench
