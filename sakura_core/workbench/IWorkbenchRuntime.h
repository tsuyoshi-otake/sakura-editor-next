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
	//! The durable Trusted Folders and Workspaces list. Separate from
	//! WorkspaceContext because a failure here is a persistence failure, not a
	//! failure of the semantic workspace state that trust is applied to.
	WorkspaceTrust,
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
	//! The stored trusted-folders list could not be read or decoded. Trust still
	//! resolves — against an empty list, so nothing is trusted — but the durable
	//! bytes are left untouched, so a grant must be refused rather than written.
	TrustRestoreFailed,
	TrustPersistenceUnavailable,
	TrustPersistenceConflict,
	TrustPersistFailed,
	WorkspaceFolderDuplicate,
	InternalFailure,
};

//! The choices VS Code's trust prompt actually offers that change the durable
//! Trusted Folders and Workspaces list. Declining is not one of them: it commits
//! nothing, so it has no scope here and no code path that writes.
enum class EWorkspaceTrustGrantScope : std::uint8_t {
	//! Trust exactly what this window has open -- the `.code-workspace` file for a
	//! Workspace, or every folder root for a Folder.
	CurrentWorkspace,
	//! Upstream's "Trust the authors of all files in the parent folder". Offered
	//! only for a single folder root that actually has a parent.
	ParentFolder,
};

enum class EWorkspaceTrustGrantStatus : std::uint8_t {
	Granted,
	//! The list already covers the requested resource with at least the same
	//! reach, so nothing was appended and nothing was written.
	AlreadyTrusted,
	//! Nothing about this window can be trusted through the requested scope: an
	//! empty window, or a root with no parent folder.
	NotApplicable,
	//! The durable list could not be read or is not writable. The grant is
	//! refused rather than applied in memory, because a session-only grant would
	//! report trust this window cannot keep.
	PersistenceUnavailable,
	Conflict,
	Stopped,
	Failed,
};

struct WorkspaceTrustGrantResult final {
	EWorkspaceTrustGrantStatus status = EWorkspaceTrustGrantStatus::Failed;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWorkspaceTrustGrantStatus::Granted
			|| status == EWorkspaceTrustGrantStatus::AlreadyTrusted;
	}
};

//! One offerable choice, already resolved to the resource it would trust. The
//! display text is the canonical URI the runtime would actually write; a prompt
//! shows it and never re-parses it to decide anything.
struct WorkspaceTrustGrantOption final {
	EWorkspaceTrustGrantScope scope = EWorkspaceTrustGrantScope::CurrentWorkspace;
	std::wstring displayUri;
	std::size_t resourceCount = 0;
};

//! Everything a native trust prompt needs, with no policy left for it to decide.
//! An empty option list means there is nothing to grant, which is a real state
//! and must be shown as such rather than filled with a disabled placeholder.
struct WorkspaceTrustPromptModel final {
	config::EWorkspaceTrustState state = config::EWorkspaceTrustState::Unknown;
	//! False when the durable list is missing or was preserved as invalid. A
	//! prompt must not offer a grant it already knows will be refused.
	bool persistenceReady = false;
	std::vector<WorkspaceTrustGrantOption> options;
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
	//! What a native Workspace Trust prompt may offer for the current workspace.
	//! A narrow test runtime has no durable list, so the fail-closed default
	//! offers nothing rather than pretending a grant would stick.
	[[nodiscard]] virtual WorkspaceTrustPromptModel WorkspaceTrustPrompt() { return {}; }
	//! Grant workspace trust and persist it. The durable write happens first: a
	//! runtime that cannot commit the bytes refuses instead of trusting for this
	//! session only, so trust never outlives, or falls short of, the record.
	[[nodiscard]] virtual WorkspaceTrustGrantResult GrantWorkspaceTrust(EWorkspaceTrustGrantScope)
	{
		return {
			.status = EWorkspaceTrustGrantStatus::PersistenceUnavailable,
			.diagnostic = "runtime does not own a durable trusted folders list",
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
	//! interface rather than only on the concrete runtime because the borrowers
	//! that need it -- the native window and, through it, the extension service
	//! bridge behind `workspace/configuration/update` -- must depend on the
	//! stable workbench boundary and never on the runtime implementation.
	//! A runtime that is not running returns a `Stopped` status instead of
	//! performing any filesystem work, so callers have one status to branch on.
	[[nodiscard]] virtual config::SettingsWritebackResult WriteSetting(
		const config::SettingsWritebackRequest& request) = 0;
};

} // namespace workbench
