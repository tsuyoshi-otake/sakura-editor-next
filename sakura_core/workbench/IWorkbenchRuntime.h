/*! @file
 * @brief Process-local workbench service boundary.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/IConfigurationService.h"
#include "config/IWorkspaceContextService.h"
#include "config/SettingsWritebackCoordinator.h"
#include "workbench/WorkbenchBootstrapContext.h"
#include "workbench/tasks/FolderTaskCatalogRegistry.h"
#include "workbench/tasks/TaskExecutionService.h"
#include "workbench/workspace/WorkspaceConfigurationTypes.h"
#include "workbench/workspace/WorkspaceEditingService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::layout {
class WorkbenchContributionRegistry;
class WorkbenchLayoutStateService;
}

namespace workbench::workspace {
	class CWorkspaceArtifactDocumentService;
}

namespace workbench::recent {
	class IRecentlyOpenedWorkspaceService;
}

namespace workbench::problems {
class MarkerService;
}

namespace workbench::output {
class OutputService;
}

namespace workbench::scm {
class SourceControlService;
}

namespace senp {
class ISenpLanguageService;
class ISenpManagementService;
class ISenpRuntimeService;
}

namespace workbench::statusbar {
class StatusbarViewModel;
struct StatusbarMementoSaveResult;
}

namespace workbench {

//! Only terminal states are observable outside the runtime. Starting and
//! stopping remain implementation details owned by the composition root.
enum class EWorkbenchRuntimeState : std::uint8_t {
	Created,
	Ready,
	ReadyWithDiagnostics,
	Failed,
	Stopped,
};

enum class EWorkbenchRuntimeResultCode : std::uint8_t {
	Ready,
	ReadyWithDiagnostics,
	AlreadyReady,
	//! A lifecycle operation is already in progress on this runtime. In
	//! particular, a reentrant Start never begins a second bootstrap sequence.
	Busy,
	Stopped,
	Failed,
};

enum class EWorkbenchRuntimeDiagnosticSource : std::uint8_t {
	Bootstrap,
	ProfileSettings,
	WorkspaceSettings,
	WorkspaceContext,
	WorkspaceArtifacts,
	Layout,
	Extensions,
};

enum class EWorkbenchRuntimeDiagnosticCode : std::uint8_t {
	ReadFailed,
	ParseFailed,
	ApplyFailed,
	UnsupportedWorkspaceConfiguration,
	WorkspaceTransitionFailed,
	LayoutReconcileFailed,
	LayoutRestoreFailed,
	LayoutPersistenceUnavailable,
	LayoutPersistenceConflict,
	LayoutPersistFailed,
	WorkspaceFolderDuplicate,
	InternalFailure,
};

//! Diagnostics intentionally omit profile IDs and resource paths. Consumers
//! can route them to Problems/Output without exposing local secrets or paths.
struct WorkbenchRuntimeDiagnostic final {
	EWorkbenchRuntimeDiagnosticSource source = EWorkbenchRuntimeDiagnosticSource::Bootstrap;
	EWorkbenchRuntimeDiagnosticCode code = EWorkbenchRuntimeDiagnosticCode::InternalFailure;
	std::string message;
};

struct WorkbenchRuntimeSnapshot final {
	EWorkbenchRuntimeState state = EWorkbenchRuntimeState::Created;
	std::uint64_t revision = 0;
	std::vector<WorkbenchRuntimeDiagnostic> diagnostics;
};

struct WorkbenchRuntimeResult final {
	EWorkbenchRuntimeResultCode code = EWorkbenchRuntimeResultCode::Failed;
	WorkbenchRuntimeSnapshot snapshot;

	[[nodiscard]] bool IsUsable() const noexcept
	{
		return code == EWorkbenchRuntimeResultCode::Ready
			|| code == EWorkbenchRuntimeResultCode::ReadyWithDiagnostics
			|| code == EWorkbenchRuntimeResultCode::AlreadyReady;
	}
};

//! Narrow service surface borrowed by the native window. Lifetime and final
//! Stop ownership remain in CEditApp, so a window cannot outlive service truth.
class IWorkbenchRuntime {
public:
	virtual ~IWorkbenchRuntime() = default;

	[[nodiscard]] virtual const WorkbenchBootstrapContext& Bootstrap() const noexcept = 0;
	[[nodiscard]] virtual config::IConfigurationService& Configuration() noexcept = 0;
	[[nodiscard]] virtual config::IWorkspaceContextService& WorkspaceContext() noexcept = 0;
	//! Enter a single-folder workspace in this process. The runtime owns the
	//! bounded operation id, CAS revision, settings/artifact reload, and watcher
	//! refresh; native windows only project the accepted terminal snapshot.
	[[nodiscard]] virtual config::WorkspaceContextResult SwitchToFolderWorkspace(
		platform::uri::Uri, std::wstring)
	{
		return {
			.outcome = config::EWorkspaceContextOutcome::Failed,
			.reason = "runtime does not support an in-process folder transition",
			.snapshot = WorkspaceContext().Snapshot(),
		};
	}
	//! Runtime-owned editor for .code-workspace documents. It is the only window-facing
	//! write path and keeps the process-local file provider private.
	[[nodiscard]] virtual workspace::IWorkspaceEditingService* WorkspaceEditing() noexcept = 0;
	//! Production overrides this operation to make a successful CAS edit and the
	//! corresponding semantic workspace/context update one synchronous terminal
	//! result. Narrow test runtimes retain a fail-closed default.
	[[nodiscard]] virtual workspace::WorkspaceEditingResult ReplaceCurrentWorkspaceFolders(
		const workspace::WorkspaceFoldersEditRequest&)
	{
		return { .diagnostic = "runtime does not support synchronous workspace-folder acceptance" };
	}
	//! Control-owned Profile/User recent-workspace state.  A default null borrow
	//! keeps existing narrow test runtimes source compatible; production exposes
	//! it only while the runtime is ready.
	[[nodiscard]] virtual recent::IRecentlyOpenedWorkspaceService* RecentlyOpenedWorkspaces() noexcept { return nullptr; }
	//! Stable-ID contributions and HWND-free layout state remain separate from native workbench adapters.
	[[nodiscard]] virtual layout::WorkbenchContributionRegistry& Contributions() noexcept = 0;
	[[nodiscard]] virtual const layout::WorkbenchContributionRegistry& Contributions() const noexcept = 0;
	[[nodiscard]] virtual layout::WorkbenchLayoutStateService& LayoutState() noexcept = 0;
	[[nodiscard]] virtual const layout::WorkbenchLayoutStateService& LayoutState() const noexcept = 0;
	[[nodiscard]] virtual statusbar::StatusbarViewModel& StatusbarState() noexcept = 0;
	[[nodiscard]] virtual const statusbar::StatusbarViewModel& StatusbarState() const noexcept = 0;
	[[nodiscard]] virtual statusbar::StatusbarMementoSaveResult PersistStatusbarVisibility() = 0;
	//! Typed workspace-document state. This is metadata only; consumers must use
	//! Configuration() for effective settings and cannot accidentally merge tasks,
	//! launch, or extension declarations into it.
	[[nodiscard]] virtual workspace::WorkspaceConfigurationRuntimeSnapshot WorkspaceConfiguration() const = 0;
	//! Typed task/debug/recommendation documents stay outside Configuration().
	//! The runtime remains their lifetime owner; consumers receive read-only
	//! routing and last-good document state only.
	[[nodiscard]] virtual const workspace::CWorkspaceArtifactDocumentService& WorkspaceArtifacts() const noexcept = 0;
	//! Nullable running-only borrows. These accessors never construct or
	//! resurrect a service for an inactive runtime.
	[[nodiscard]] virtual problems::MarkerService* Markers() noexcept = 0;
	[[nodiscard]] virtual const problems::MarkerService* Markers() const noexcept = 0;
	[[nodiscard]] virtual output::OutputService* Output() noexcept = 0;
	[[nodiscard]] virtual const output::OutputService* Output() const noexcept = 0;
	[[nodiscard]] virtual scm::SourceControlService* Scm() noexcept = 0;
	[[nodiscard]] virtual const scm::SourceControlService* Scm() const noexcept = 0;
	//! Profile-scoped package management and isolated runtime are distinct
	//! services. Both are running-only borrows and never expose Wasm internals.
	[[nodiscard]] virtual senp::ISenpManagementService* Extensions() noexcept { return nullptr; }
	[[nodiscard]] virtual senp::ISenpRuntimeService* ExtensionRuntime() noexcept { return nullptr; }
	[[nodiscard]] virtual senp::ISenpLanguageService* ExtensionLanguages() noexcept { return nullptr; }
	//! Explicit folder lookup only. Empty workbenches and unknown folders return
	//! no catalog instead of silently borrowing a first/default workspace folder.
	[[nodiscard]] virtual std::optional<tasks::FolderTaskCatalogSnapshot> TaskCatalogForFolder(
		const platform::uri::Uri& folderUri) const = 0;
	//! Nullable running-only execution authority. Definitions passed to Start are
	//! immutable copies, so catalog refresh cannot mutate an in-flight run.
	[[nodiscard]] virtual tasks::TaskExecutionService* TaskExecution() noexcept = 0;
	[[nodiscard]] virtual const tasks::TaskExecutionService* TaskExecution() const noexcept = 0;
	[[nodiscard]] virtual WorkbenchRuntimeSnapshot Snapshot() const = 0;
	//! The sole production-facing Settings writeback entry point. It is on this
	//! interface rather than only on the concrete runtime because native window
	//! borrowers must depend on the stable workbench boundary and never on the
	//! runtime implementation.
	//! A runtime that is not running returns a `Stopped` status instead of
	//! performing any filesystem work, so callers have one status to branch on.
	[[nodiscard]] virtual config::SettingsWritebackResult WriteSetting(
		const config::SettingsWritebackRequest& request) = 0;
};

} // namespace workbench
