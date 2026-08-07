/*! @file
 * @brief Adapts extension-host diagnostics and output notifications to runtime workbench services.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/ConfigurationTypes.h"
#include "config/SettingsWritebackCoordinator.h"
#include "config/WorkspaceContextTypes.h"
#include "extension/CExtensionContributionRegistry.h"
#include "extension/CExtensionWorkbenchUi.h"
#include "extension/ExtensionUntrustedWorkspaceOverride.h"
#include "workbench/output/OutputService.h"
#include "workbench/problems/MarkerService.h"
#include "workbench/scm/SourceControlService.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench {
class IWorkbenchRuntime;
}

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
		workbench::IWorkbenchRuntime* workbenchRuntime = nullptr,
		std::size_t maximumTrackedOwnerGenerations = 128,
		workbench::scm::SourceControlService* scmService = nullptr);

	[[nodiscard]] bool HasMarkerService() const noexcept { return m_markerService != nullptr; }
	[[nodiscard]] bool HasOutputService() const noexcept { return m_outputService != nullptr; }
	[[nodiscard]] bool HasWorkbenchRuntime() const noexcept { return m_workbenchRuntime != nullptr; }
	[[nodiscard]] workbench::scm::SourceControlService* Scm() const noexcept;

	//! Writes (or, when `value` is empty, removes) one key in the Global/User settings
	//! document through the runtime's sole settings-writeback owner. `overrideLanguageId`
	//! selects the "[languageId]" override block within that same physical document when
	//! non-empty; the base document is targeted otherwise. Returns Stopped when no runtime
	//! is bound (for example isolated legacy tests), mirroring CWorkbenchRuntime::WriteSetting's
	//! own not-ready status so callers have exactly one status to branch on either way.
	//!
	//! This method never targets Workspace or WorkspaceFolder scope: the runtime does not
	//! expose a safe accessor to its dynamically-assigned workspace/folder document keys from
	//! this bridge, so callers must reject those targets themselves before reaching here. See
	//! extension/CLAUDE.md for the full record of this boundary.
	[[nodiscard]] config::SettingsWritebackResult WriteGlobalConfiguration(
		std::string_view key,
		const std::optional<config::ConfigurationValue>& value,
		std::wstring_view overrideLanguageId);

	//! Builds the effective Profile-scoped value for every built-in configuration
	//! descriptor key, for delivery to the extension host as the `configuration`
	//! snapshot in `host/registerExtensions` (backs
	//! `vscode.workspace.getConfiguration(...).get(...)` before any `update()` is
	//! ever called). This deliberately only covers keys with a registered
	//! `ConfigurationDescriptor`: `IConfigurationService::ReadSnapshot` rejects the
	//! whole batch if any requested key is unregistered, and there is no bulk
	//! "list every effective key" accessor to fall back on. A key an extension has
	//! durably written through `workspace/configuration/update` but that has no
	//! built-in descriptor (for example an extension's own private setting) is
	//! therefore not included here either; see extension/CLAUDE.md's descriptor-
	//! gating note. Returns an empty vector when no runtime is bound or the read
	//! is rejected, so callers degrade to "no native snapshot" rather than fail
	//! extension registration.
	[[nodiscard]] std::vector<config::ConfigurationEntry> BuildConfigurationSnapshot() const;

	//! Read-only projection of the runtime's current workspace identity, for
	//! describing `vscode.workspace.workspaceFolders` to the extension host.
	//! Returns a default-constructed (EWorkspaceKind::Empty, no folders) snapshot
	//! when no runtime is bound, matching "no workspace is open" rather than
	//! signaling a distinct failure.
	[[nodiscard]] config::WorkspaceContextSnapshot WorkspaceContextSnapshotForExtensions() const;

	//! Forwards the restricted-configuration key set to the runtime. Returns
	//! Unsupported when no runtime is bound, matching every other bridge accessor's
	//! "no runtime" degradation rather than inventing a distinct failure.
	[[nodiscard]] config::EConfigurationOutcome PublishExtensionRestrictedConfigurations(std::vector<std::string> keys) const;

	//! Reads and parses the `extensions.supportUntrustedWorkspaces` user override at
	//! the selected-profile target. Returns an empty map when no runtime is bound or
	//! the read is rejected, so a failed read falls back to the manifest declaration
	//! rather than silently widening or narrowing an exemption.
	[[nodiscard]] std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>>
	ExtensionUntrustedWorkspaceOverrides() const;

	//! Stable identity for the host-owned Extension Host log channel (see AppendExtensionHostLog).
	//! This is a native-only identifier; it has no legacy CExtensionOutputChannel projection because
	//! the channel is not owned by any extension.
	static constexpr std::string_view kExtensionHostLogChannelId = "sakura.workbench.extensionHostLog";
	static constexpr std::string_view kExtensionHostLogChannelLabel = "Extension Host";

	//! Records a host-owned diagnostic into the Extension Host log channel.
	//!
	//! The channel is lazily created on first use with a fixed, host-owned `OutputOwner` that never
	//! matches any extension ID, so it is deliberately excluded from `DisposeOwner`/`DisposeAll`
	//! owner-generation tracking: an extension reload, disable, or full host loss must never remove it.
	//! `message` is caller-composed and is bounded/truncated defensively before being stored, since the
	//! caller typically embeds extension-supplied text. Returns false (without throwing and without
	//! failing the caller's RPC) when there is no OutputService to record into, or when the OutputService
	//! rejects the mutation (for example because it has reached its owner/channel limits, its operation ID
	//! space is exhausted, or it has already stopped); in every such case the channel-created flag is left
	//! unset so a later call can retry channel creation.
	[[nodiscard]] bool AppendExtensionHostLog(workbench::output::EOutputLogLevel level, std::wstring_view message);

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

	/*!
	 * @brief Registers manifest-declared view containers and views into the workbench layout registry.
	 *
	 * The layout registry is the single source of truth for container/view identity, ordering,
	 * duplicate rejection and owner generations, so extension-contributed containers must live
	 * there rather than in a parallel extension-side store. Only the attributes that registry
	 * deliberately does not model (icon, `when`, tree/webview kind) stay in
	 * CExtensionContributionRegistry.
	 *
	 * Re-registration is a full replace: any previously registered generation of the same
	 * extension is disposed first, because the registry rejects duplicate container IDs. Returns
	 * false when there is no runtime to register into or the registry rejected the batch; the
	 * caller's RPC still succeeds, so a rejected batch degrades to "no contributed containers"
	 * rather than to a failed extension activation.
	 */
	[[nodiscard]] bool RegisterViewContributions(
		std::wstring_view extensionId, std::uint64_t generation,
		const std::vector<SExtensionViewContainerDeclaration>& containers,
		const std::vector<SExtensionViewDeclaration>& views);

	//! Removes every layout contribution owned by one extension generation. Safe when nothing was registered.
	bool DisposeViewContributions(std::wstring_view extensionId, std::uint64_t generation) noexcept;

	/*!
	 * @brief The ViewContainer a manifest-declared view belongs to.
	 *
	 * Read back from the layout registry, which is the sole owner of that identity, so a runtime
	 * `createTreeView` lands in the container its manifest declared instead of in the host's
	 * default bucket. Returns empty when the view was never declared or there is no runtime; the
	 * caller then keeps the default bucket rather than inventing a container.
	 */
	[[nodiscard]] std::wstring ViewContainerOf(std::wstring_view viewId) const;

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
	//! Separate non-wrapping operation ID sequence dedicated to the host-owned Extension Host log
	//! channel, following the same fail-closed-on-exhaustion contract as NextDisposeOperationId.
	[[nodiscard]] bool NextExtensionHostLogOperationId(std::string& operationId) noexcept;
	//! Separate non-wrapping sequence for layout contribution batches, same fail-closed contract.
	[[nodiscard]] bool NextContributionOperationId(std::string& operationId) noexcept;

	workbench::problems::MarkerService* m_markerService = nullptr;
	workbench::output::OutputService* m_outputService = nullptr;
	workbench::IWorkbenchRuntime* m_workbenchRuntime = nullptr;
	workbench::scm::SourceControlService* m_scmService = nullptr;
	std::size_t m_maximumTrackedOwnerGenerations = 0;
	std::vector<TrackedOwner> m_trackedOwners;
	std::uint64_t m_nextDisposeOperationId = 1;
	//! Deliberately untracked in m_trackedOwners: see AppendExtensionHostLog.
	bool m_extensionHostLogChannelCreated = false;
	std::uint64_t m_nextExtensionHostLogOperationId = 1;
	std::uint64_t m_nextContributionOperationId = 1;
};
