/*! @file
 * @brief Adapts extension-host diagnostics and output notifications to runtime workbench services.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "extension/CExtensionWorkbenchUi.h"
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*!
 * @brief Extension-layer translation to borrowed MarkerService and OutputService instances.
 *
 * The legacy models are deliberately only best-effort decoration caches.  This
 * bridge never stops, clears, or otherwise owns the runtime services.
 */
class CExtensionWorkbenchServiceBridge final {
public:
	CExtensionWorkbenchServiceBridge(
		workbench::problems::MarkerService* markerService = nullptr,
		workbench::output::OutputService* outputService = nullptr,
		std::size_t maximumTrackedOwnerGenerations = 128);

	[[nodiscard]] bool HasMarkerService() const noexcept { return m_markerService != nullptr; }
	[[nodiscard]] bool HasOutputService() const noexcept { return m_outputService != nullptr; }

	[[nodiscard]] bool SetDiagnostics(
		std::wstring_view extensionId, std::uint64_t generation, std::wstring_view collection,
		std::wstring_view uri, const std::vector<SExtensionDiagnostic>& diagnostics,
		CExtensionDiagnostics& legacyCache);
	[[nodiscard]] bool DeleteDiagnostics(
		std::wstring_view extensionId, std::uint64_t generation, std::wstring_view collection,
		std::wstring_view uri, CExtensionDiagnostics& legacyCache);
	[[nodiscard]] bool ClearDiagnosticsCollection(
		std::wstring_view extensionId, std::uint64_t generation, std::wstring_view collection,
		CExtensionDiagnostics& legacyCache);

	[[nodiscard]] bool CreateOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::wstring_view name, std::wstring_view languageId, std::wstring_view source,
		workbench::output::EOutputChannelKind kind, std::string_view operationId,
		CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool AppendOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::wstring_view value, std::string_view operationId, CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool ReplaceOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::wstring_view value, std::string_view operationId, CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool ClearOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::string_view operationId, CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool ShowOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		bool preserveFocus, std::string_view operationId, CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool HideOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::string_view operationId, CExtensionOutputChannel& legacyCache);
	[[nodiscard]] bool DisposeOutput(
		std::wstring_view handle, std::wstring_view extensionId, std::uint64_t generation,
		std::string_view operationId, CExtensionOutputChannel& legacyCache);

	//! Dispose exactly one accepted owner generation before its legacy projections are removed.
	[[nodiscard]] bool DisposeOwner(
		std::wstring_view extensionId, std::uint64_t generation,
		CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache);
	//! Host loss disposes all accepted owner generations before the caller clears legacy workbench state.
	[[nodiscard]] bool DisposeAll(
		CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache);

private:
	struct TrackedOwner final {
		std::string id;
		std::uint64_t generation{};
	};

	[[nodiscard]] bool PrepareOwner(std::string_view id, std::uint64_t generation, TrackedOwner& prepared) const;
	void RememberPreparedOwner(TrackedOwner&& prepared) noexcept;
	[[nodiscard]] bool IsTracked(std::string_view id, std::uint64_t generation) const noexcept;
	void ForgetTracked(std::string_view id, std::uint64_t generation) noexcept;
	[[nodiscard]] bool DisposeTrackedOwner(
		const TrackedOwner& owner, CExtensionDiagnostics& diagnosticsCache, CExtensionOutputChannel& outputCache);
	[[nodiscard]] bool NextDisposeOperationId(std::string& operationId) noexcept;

	workbench::problems::MarkerService* m_markerService = nullptr;
	workbench::output::OutputService* m_outputService = nullptr;
	std::size_t m_maximumTrackedOwnerGenerations = 0;
	std::vector<TrackedOwner> m_trackedOwners;
	std::uint64_t m_nextDisposeOperationId = 1;
};
