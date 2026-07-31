/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "extension/CExtensionWorkbenchServiceBridge.h"

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

} // namespace

CExtensionWorkbenchServiceBridge::CExtensionWorkbenchServiceBridge(
	workbench::problems::MarkerService* markerService,
	workbench::output::OutputService* outputService,
	const std::size_t maximumTrackedOwnerGenerations)
	: m_markerService(markerService)
	, m_outputService(outputService)
	, m_maximumTrackedOwnerGenerations(maximumTrackedOwnerGenerations)
{
	m_trackedOwners.reserve(m_maximumTrackedOwnerGenerations);
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
	if (!IsTracked(ownerId, generation)) return true;
	const TrackedOwner owner { ownerId, generation };
	if (!DisposeTrackedOwner(owner, diagnosticsCache, outputCache)) return false;
	ForgetTracked(ownerId, generation);
	return true;
}

bool CExtensionWorkbenchServiceBridge::DisposeAll(CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache)
{
	while (!m_trackedOwners.empty()) {
		const auto owner = m_trackedOwners.back();
		if (!DisposeTrackedOwner(owner, diagnosticsCache, outputCache)) return false;
		m_trackedOwners.pop_back();
	}
	return true;
}
