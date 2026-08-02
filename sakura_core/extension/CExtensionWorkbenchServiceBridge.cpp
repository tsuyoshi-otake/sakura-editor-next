/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "extension/CExtensionWorkbenchServiceBridge.h"

#include "config/editing/CJsoncConfigurationEditor.h"
#include "workbench/IWorkbenchRuntime.h"
#include "workbench/WorkbenchBootstrapContext.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "util/string_ex.h"

namespace {

using workbench::output::EOutputOperationStatus;
using workbench::problems::EMarkerOperationStatus;

bool IsAccepted(const workbench::problems::MarkerOperationResult& result) noexcept
{
	return result.status == EMarkerOperationStatus::Replaced || result.status == EMarkerOperationStatus::Deleted ||
		result.status == EMarkerOperationStatus::CollectionCleared || result.status == EMarkerOperationStatus::OwnerDisposed;
}

bool IsAccepted(const workbench::output::OutputOperationResult& result) noexcept
{
	return result.status == EOutputOperationStatus::Succeeded || result.status == EOutputOperationStatus::Replayed;
}

bool IsFresh(const workbench::output::OutputOperationResult& result) noexcept
{
	return result.status == EOutputOperationStatus::Succeeded;
}

std::optional<std::string> OptionalUtf8(const std::wstring_view value)
{
	if (value.empty()) return std::nullopt;
	return wcstou8s(std::wstring(value));
}

workbench::problems::MarkerCollectionIdentity Collection(
	const std::string& extensionId, const std::uint64_t generation, const std::string& collection)
{
	return { .owner = { .id = extensionId, .generation = generation }, .id = collection };
}

workbench::output::OutputOwner Owner(const std::string& extensionId, const std::uint64_t generation)
{
	return { .ownerId = extensionId, .generation = generation };
}

//! Fixed owner identity for the host-owned Extension Host log channel. This never equals any real
//! extension ID, so it can never be matched by DisposeOwner(extensionId, generation) and is never
//! visited by DisposeAll's m_trackedOwners walk (the channel is deliberately never remembered there).
workbench::output::OutputOwner ExtensionHostLogOwner()
{
	return { .ownerId = "sakura.workbench.extensionHost", .generation = 1 };
}

//! Extension-supplied text reaching this channel is untrusted and unbounded in principle (for example a
//! long stack trace). Defensively bound the total stored message regardless of what the caller composed.
constexpr std::size_t kMaximumExtensionHostLogMessageCodeUnits = 4000;

std::wstring BoundedExtensionHostLogMessage(const std::wstring_view message)
{
	if (message.size() <= kMaximumExtensionHostLogMessageCodeUnits) return std::wstring(message);
	std::wstring bounded(message.substr(0, kMaximumExtensionHostLogMessageCodeUnits));
	bounded += L"…[truncated]";
	return bounded;
}

} // namespace

CExtensionWorkbenchServiceBridge::CExtensionWorkbenchServiceBridge(
	workbench::problems::MarkerService* markerService,
	workbench::output::OutputService* outputService,
	workbench::IWorkbenchRuntime* workbenchRuntime,
	const std::size_t maximumTrackedOwnerGenerations,
	workbench::scm::SourceControlService* scmService)
	: m_markerService(markerService)
	, m_outputService(outputService)
	, m_workbenchRuntime(workbenchRuntime)
	, m_scmService(scmService)
	, m_maximumTrackedOwnerGenerations(maximumTrackedOwnerGenerations)
{
	m_trackedOwners.reserve(m_maximumTrackedOwnerGenerations);
}

workbench::scm::SourceControlService* CExtensionWorkbenchServiceBridge::Scm() const noexcept
{
	if (m_scmService) return m_scmService;
	return m_workbenchRuntime ? m_workbenchRuntime->Scm() : nullptr;
}

config::SettingsWritebackResult CExtensionWorkbenchServiceBridge::WriteGlobalConfiguration(
	const std::string_view key,
	const std::optional<config::ConfigurationValue>& value,
	const std::wstring_view overrideLanguageId)
{
	if (!m_workbenchRuntime) {
		return { .status = config::ESettingsWritebackStatus::Stopped,
			.diagnostic = "no workbench runtime is bound to this bridge" };
	}

	// Mirrors CWorkbenchRuntime::ReloadProfileSettings's own source identity exactly
	// (scope, sourceId, priority, and a target that carries only profileId): the file
	// source controller rejects a Reload whose source identity differs from the one
	// already tracked for this document key, so this write must reuse it verbatim
	// rather than constructing a plausible-looking but distinct identity.
	const auto& profile = m_workbenchRuntime->Bootstrap().UserDataProfile();
	config::ConfigurationTarget sourceTarget;
	sourceTarget.profileId = profile.SelectedProfileId();
	const config::ConfigurationSource source {
		config::EConfigurationScope::Profile,
		sourceTarget,
		"profile.settings",
		0,
	};

	config::editing::ConfigurationDocumentEditTarget editTarget;
	editTarget.resource = profile.Resources().Settings();
	editTarget.target = sourceTarget;
	if (overrideLanguageId.empty()) {
		editTarget.scope = config::editing::EConfigurationDocumentScope::Profile;
	} else {
		// The base source identity's languageId must stay unset (IsSameBaseTarget requires
		// it); only the edit target's languageId selects the "[languageId]" override block.
		editTarget.scope = config::editing::EConfigurationDocumentScope::LanguageOverride;
		editTarget.target.languageId = std::wstring(overrideLanguageId);
	}

	const config::SettingsWritebackRequest request {
		.edit = { .target = std::move(editTarget), .key = std::string(key), .value = value },
		.documentKey = "profile.settings",
		.source = source,
	};
	return m_workbenchRuntime->WriteSetting(request);
}

bool CExtensionWorkbenchServiceBridge::PrepareOwner(
	const std::string_view id, const std::uint64_t generation, TrackedOwner& prepared) const
{
	if (IsTracked(id, generation)) return true;
	if (m_trackedOwners.size() >= m_maximumTrackedOwnerGenerations) return false;
	try {
		prepared = { .id = std::string(id), .generation = generation };
		return true;
	} catch (...) {
		return false;
	}
}

void CExtensionWorkbenchServiceBridge::RememberPreparedOwner(TrackedOwner&& prepared) noexcept
{
	if (prepared.id.empty() || IsTracked(prepared.id, prepared.generation)) return;
	m_trackedOwners.emplace_back(std::move(prepared));
}

bool CExtensionWorkbenchServiceBridge::IsTracked(const std::string_view id, const std::uint64_t generation) const noexcept
{
	return std::ranges::any_of(m_trackedOwners, [id, generation](const TrackedOwner& entry) {
		return entry.id == id && entry.generation == generation;
	});
}

void CExtensionWorkbenchServiceBridge::ForgetTracked(const std::string_view id, const std::uint64_t generation) noexcept
{
	std::erase_if(m_trackedOwners, [id, generation](const TrackedOwner& entry) {
		return entry.id == id && entry.generation == generation;
	});
}

bool CExtensionWorkbenchServiceBridge::SetDiagnostics(
	const std::wstring_view extensionId, const std::uint64_t generation, const std::wstring_view collection,
	const std::wstring_view uri, const std::vector<SExtensionDiagnostic>& diagnostics, CExtensionDiagnostics& legacyCache)
{
	if (!m_markerService) {
		return legacyCache.Set(std::wstring(extensionId), generation, std::wstring(collection), std::wstring(uri), diagnostics);
	}
	const auto resource = platform::uri::Uri::Parse(uri);
	if (!resource) return false;
	const auto ownerId = wcstou8s(std::wstring(extensionId));
	const auto collectionId = wcstou8s(std::wstring(collection));
	TrackedOwner prepared;
	if (!PrepareOwner(ownerId, generation, prepared)) return false;
	std::vector<workbench::problems::ProblemMarker> markers;
	try {
		markers.reserve(diagnostics.size());
		for (const auto& value : diagnostics) {
			markers.push_back({
				.range = { value.range.start.line, value.range.start.character, value.range.end.line, value.range.end.character },
				.severity = static_cast<workbench::problems::EMarkerSeverity>(value.severity),
				.message = wcstou8s(value.message), .code = OptionalUtf8(value.code), .source = OptionalUtf8(value.source),
			});
		}
	} catch (...) {
		return false;
	}
	const auto result = m_markerService->Replace({ .collection = Collection(ownerId, generation, collectionId),
		.resource = *resource.value, .markers = std::move(markers) });
	const bool desiredAbsence = diagnostics.empty();
	if (!IsAccepted(result) && !(desiredAbsence && result.status == EMarkerOperationStatus::NotApplicable)) return false;
	if (IsAccepted(result)) RememberPreparedOwner(std::move(prepared));
	try {
		if (desiredAbsence) (void)legacyCache.Delete(extensionId, generation, collection, resource.value->ToString());
		else (void)legacyCache.Set(std::wstring(extensionId), generation, std::wstring(collection), resource.value->ToString(), diagnostics);
	}
	catch (...) {}
	return true;
}

bool CExtensionWorkbenchServiceBridge::DeleteDiagnostics(
	const std::wstring_view extensionId, const std::uint64_t generation, const std::wstring_view collection,
	const std::wstring_view uri, CExtensionDiagnostics& legacyCache)
{
	if (!m_markerService) return legacyCache.Delete(extensionId, generation, collection, uri);
	const auto resource = platform::uri::Uri::Parse(uri);
	if (!resource) return false;
	const auto ownerId = wcstou8s(std::wstring(extensionId));
	const auto collectionId = wcstou8s(std::wstring(collection));
	const auto result = m_markerService->Delete({ .collection = Collection(ownerId, generation, collectionId), .resource = *resource.value });
	if (!IsAccepted(result) && result.status != EMarkerOperationStatus::NotApplicable) return false;
	try { (void)legacyCache.Delete(extensionId, generation, collection, resource.value->ToString()); }
	catch (...) {}
	return true;
}

bool CExtensionWorkbenchServiceBridge::ClearDiagnosticsCollection(
	const std::wstring_view extensionId, const std::uint64_t generation, const std::wstring_view collection,
	CExtensionDiagnostics& legacyCache)
{
	if (!m_markerService) {
		legacyCache.ClearCollection(extensionId, generation, collection);
		return true;
	}
	const auto ownerId = wcstou8s(std::wstring(extensionId));
	const auto collectionId = wcstou8s(std::wstring(collection));
	const auto result = m_markerService->ClearCollection({ .collection = Collection(ownerId, generation, collectionId) });
	if (!IsAccepted(result) && result.status != EMarkerOperationStatus::NotApplicable) return false;
	try { legacyCache.ClearCollection(extensionId, generation, collection); }
	catch (...) {}
	return true;
}

bool CExtensionWorkbenchServiceBridge::CreateOutput(
	const std::wstring_view handle, const std::wstring_view extensionId, const std::uint64_t generation,
	const std::wstring_view name, const std::wstring_view languageId, const std::wstring_view source,
	const workbench::output::EOutputChannelKind kind, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.Create({ .handle = std::wstring(handle), .extensionId = std::wstring(extensionId),
		.generation = generation, .name = std::wstring(name), .languageId = std::wstring(languageId) });
	const auto ownerId = wcstou8s(std::wstring(extensionId));
	TrackedOwner prepared;
	if (!PrepareOwner(ownerId, generation, prepared)) return false;
	const auto result = m_outputService->CreateChannel({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(ownerId, generation), .channelId = wcstou8s(std::wstring(handle)), .label = wcstou8s(std::wstring(name)),
		.kind = kind, .metadata = { .languageId = OptionalUtf8(languageId), .source = OptionalUtf8(source) } });
	if (!IsAccepted(result)) return false;
	RememberPreparedOwner(std::move(prepared));
	if (IsFresh(result)) {
		try { (void)legacyCache.Create({ .handle = std::wstring(handle), .extensionId = std::wstring(extensionId),
			.generation = generation, .name = std::wstring(name), .languageId = std::wstring(languageId) }); } catch (...) {}
	}
	return true;
}

bool CExtensionWorkbenchServiceBridge::AppendOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const std::wstring_view value, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.Append(handle, extensionId, generation, value);
	const auto result = m_outputService->AppendOutput({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)), .text = wcstou8s(std::wstring(value)) });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.Append(handle, extensionId, generation, value); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::ReplaceOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const std::wstring_view value, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.Replace(handle, extensionId, generation, std::wstring(value));
	const auto result = m_outputService->ReplaceOutput({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)), .text = wcstou8s(std::wstring(value)) });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.Replace(handle, extensionId, generation, std::wstring(value)); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::ClearOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.Clear(handle, extensionId, generation);
	const auto result = m_outputService->Clear({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)) });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.Clear(handle, extensionId, generation); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::ShowOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const bool preserveFocus, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.SetVisible(handle, extensionId, generation, true);
	const auto result = m_outputService->Show({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)), .preserveFocus = preserveFocus });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.SetVisible(handle, extensionId, generation, true); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::HideOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.SetVisible(handle, extensionId, generation, false);
	const auto result = m_outputService->Hide({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)) });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.SetVisible(handle, extensionId, generation, false); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::DisposeOutput(const std::wstring_view handle, const std::wstring_view extensionId,
	const std::uint64_t generation, const std::string_view operationId, CExtensionOutputChannel& legacyCache)
{
	if (!m_outputService) return legacyCache.Dispose(handle, extensionId, generation);
	const auto result = m_outputService->Dispose({ .operation = { .operationId = std::string(operationId) },
		.owner = Owner(wcstou8s(std::wstring(extensionId)), generation), .channelId = wcstou8s(std::wstring(handle)) });
	if (!IsAccepted(result)) return false;
	if (IsFresh(result)) { try { (void)legacyCache.Dispose(handle, extensionId, generation); } catch (...) {} }
	return true;
}

bool CExtensionWorkbenchServiceBridge::NextDisposeOperationId(std::string& operationId) noexcept
{
	if (m_nextDisposeOperationId == 0 || m_nextDisposeOperationId == (std::numeric_limits<std::uint64_t>::max)()) return false;
	try {
		operationId = "extension-bridge-dispose-" + std::to_string(m_nextDisposeOperationId++);
		return workbench::output::OutputService::IsValidOperationId(operationId);
	} catch (...) {
		return false;
	}
}

bool CExtensionWorkbenchServiceBridge::NextExtensionHostLogOperationId(std::string& operationId) noexcept
{
	if (m_nextExtensionHostLogOperationId == 0 ||
		m_nextExtensionHostLogOperationId == (std::numeric_limits<std::uint64_t>::max)()) return false;
	try {
		operationId = "extension-bridge-host-log-" + std::to_string(m_nextExtensionHostLogOperationId++);
		return workbench::output::OutputService::IsValidOperationId(operationId);
	} catch (...) {
		return false;
	}
}

bool CExtensionWorkbenchServiceBridge::AppendExtensionHostLog(
	const workbench::output::EOutputLogLevel level, const std::wstring_view message)
{
	// No runtime-owned Output authority to record into (for example isolated legacy tests that construct
	// the bridge without one). The caller's RPC still succeeds; only this diagnostic is unavailable.
	if (!m_outputService) return false;

	if (!m_extensionHostLogChannelCreated) {
		std::string createOperationId;
		if (!NextExtensionHostLogOperationId(createOperationId)) return false;
		const auto created = m_outputService->CreateChannel({
			.operation = { .operationId = std::move(createOperationId) },
			.owner = ExtensionHostLogOwner(),
			.channelId = std::string(kExtensionHostLogChannelId),
			.label = std::string(kExtensionHostLogChannelLabel),
			.kind = workbench::output::EOutputChannelKind::Log,
		});
		// Succeeded/Replayed both mean the channel now exists. Any other status (for example the service's
		// owner/channel limits, its non-wrapping operation ID space exhausted, or Stopped) leaves the
		// created flag unset so a later call retries channel creation instead of permanently caching a
		// stale failure.
		if (!IsAccepted(created)) return false;
		m_extensionHostLogChannelCreated = true;
	}

	std::string appendOperationId;
	if (!NextExtensionHostLogOperationId(appendOperationId)) return false;
	const auto appended = m_outputService->AppendLog({
		.operation = { .operationId = std::move(appendOperationId) },
		.owner = ExtensionHostLogOwner(),
		.channelId = std::string(kExtensionHostLogChannelId),
		.entries = { { .level = level, .message = wcstou8s(BoundedExtensionHostLogMessage(message)) } },
	});
	return IsAccepted(appended);
}

bool CExtensionWorkbenchServiceBridge::DisposeTrackedOwner(
	const TrackedOwner& owner, CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache)
{
	if (m_markerService) {
		const auto result = m_markerService->DisposeOwner({ .owner = { .id = owner.id, .generation = owner.generation } });
		if (result.status != EMarkerOperationStatus::OwnerDisposed && result.status != EMarkerOperationStatus::NotApplicable &&
			result.status != EMarkerOperationStatus::StaleGeneration && result.status != EMarkerOperationStatus::Stopped) return false;
	}
	if (m_outputService) {
		std::string operationId;
		if (!NextDisposeOperationId(operationId)) return false;
		const auto result = m_outputService->DisposeOwner({ .operation = { .operationId = std::move(operationId) },
			.owner = Owner(owner.id, owner.generation) });
		if (result.status != EOutputOperationStatus::Succeeded && result.status != EOutputOperationStatus::Replayed &&
			result.status != EOutputOperationStatus::NotApplicable && result.status != EOutputOperationStatus::Conflict &&
			result.status != EOutputOperationStatus::Stopped) return false;
	}
	if (auto* scm = Scm()) {
		const auto result = scm->DisposeOwner({ .extensionId = owner.id, .generation = owner.generation });
		if (result.status != workbench::scm::EScmOperationStatus::Succeeded &&
			result.status != workbench::scm::EScmOperationStatus::Replayed &&
			result.status != workbench::scm::EScmOperationStatus::NotApplicable &&
			result.status != workbench::scm::EScmOperationStatus::Stopped) return false;
	}
	const auto wideId = u8stowcs(owner.id);
	try { diagnosticsCache.RemoveOwnedBy(wideId, owner.generation); } catch (...) {}
	try { outputCache.RemoveOwnedBy(wideId, owner.generation); } catch (...) {}
	return true;
}

bool CExtensionWorkbenchServiceBridge::DisposeOwner(
	const std::wstring_view extensionId, const std::uint64_t generation,
	CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache)
{
	const auto ownerId = wcstou8s(std::wstring(extensionId));
	const TrackedOwner owner { ownerId, generation };
	if (IsTracked(ownerId, generation)) {
		if (!DisposeTrackedOwner(owner, diagnosticsCache, outputCache)) return false;
		ForgetTracked(ownerId, generation);
	} else if (auto* scm = Scm()) {
		const auto result = scm->DisposeOwner({ .extensionId = ownerId, .generation = generation });
		if (result.status != workbench::scm::EScmOperationStatus::Succeeded &&
			result.status != workbench::scm::EScmOperationStatus::Replayed &&
			result.status != workbench::scm::EScmOperationStatus::NotApplicable &&
			result.status != workbench::scm::EScmOperationStatus::Stopped) return false;
	}
	return true;
}

bool CExtensionWorkbenchServiceBridge::DisposeAll(CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache)
{
	while (!m_trackedOwners.empty()) {
		const auto owner = m_trackedOwners.back();
		if (!DisposeTrackedOwner(owner, diagnosticsCache, outputCache)) return false;
		m_trackedOwners.pop_back();
	}
	if (auto* scm = Scm()) {
		const auto result = scm->DisposeAll();
		if (result.status != workbench::scm::EScmOperationStatus::Succeeded &&
			result.status != workbench::scm::EScmOperationStatus::Replayed &&
			result.status != workbench::scm::EScmOperationStatus::NotApplicable &&
			result.status != workbench::scm::EScmOperationStatus::Stopped) return false;
	}
	return true;
}
