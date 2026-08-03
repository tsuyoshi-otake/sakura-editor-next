/*! @file
 * @brief UI-independent ownership state machine for workspace window changes.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>

namespace workbench::workspace {

//! Every transition operation has one terminal result.  This type is shared by
//! the native adapter and tests; it deliberately contains no HWND or process
//! implementation detail.
enum class EWorkspaceWindowTransitionOutcome : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
};

struct WorkspaceWindowTransitionRequest final {
	//! Replacement invokes dirty preflight and closes the current window only
	//! after the launched successor has acknowledged ready.
	bool replaceCurrentWindow = false;
	//! A managed/untitled target is deleted if launch or ready observation fails.
	//! User-selected Save As targets deliberately leave this false.
	bool deleteStagedTargetOnFailure = false;
};

class IWorkspaceWindowTransitionHost {
public:
	virtual ~IWorkspaceWindowTransitionHost() = default;
	//! Must use the native working-copy save/discard/cancel path and leave the
	//! window usable on Cancelled/Failed.
	virtual EWorkspaceWindowTransitionOutcome PrepareReplacement() = 0;
	//! Starts the new editor and waits through the bounded ready IPC boundary.
	virtual EWorkspaceWindowTransitionOutcome LaunchAndWaitForReady() = 0;
	//! Called exactly once and only after LaunchAndWaitForReady succeeds.
	virtual EWorkspaceWindowTransitionOutcome CloseCurrentWindowOnce() = 0;
	//! Best-effort cleanup for a managed target.  Its failure remains observable
	//! as Failed, but it must never attempt to close the old window.
	virtual EWorkspaceWindowTransitionOutcome DeleteStagedTarget() = 0;
};

class CWorkspaceWindowTransitionService final {
public:
	[[nodiscard]] static EWorkspaceWindowTransitionOutcome Execute(
		const WorkspaceWindowTransitionRequest& request, IWorkspaceWindowTransitionHost& host)
	{
		if (request.replaceCurrentWindow) {
			const auto preflight = host.PrepareReplacement();
			if (preflight != EWorkspaceWindowTransitionOutcome::Succeeded) {
				return FinalizePreReadyFailure(request, preflight, host);
			}
		}
		const auto launch = host.LaunchAndWaitForReady();
		if (launch != EWorkspaceWindowTransitionOutcome::Succeeded) {
			return FinalizePreReadyFailure(request, launch, host);
		}
		if (!request.replaceCurrentWindow) return EWorkspaceWindowTransitionOutcome::Succeeded;
		return host.CloseCurrentWindowOnce();
	}

private:
	[[nodiscard]] static EWorkspaceWindowTransitionOutcome FinalizePreReadyFailure(
		const WorkspaceWindowTransitionRequest& request,
		EWorkspaceWindowTransitionOutcome original,
		IWorkspaceWindowTransitionHost& host)
	{
		if (!request.deleteStagedTargetOnFailure) return original;
		// Cleanup is part of the managed-target transaction, including a dirty
		// preflight rejection before any successor is launched.  Preserve a user
		// cancellation when cleanup succeeds; an undeleted managed target is a
		// terminal failure because it would otherwise leak durable state.
		return host.DeleteStagedTarget() == EWorkspaceWindowTransitionOutcome::Succeeded
			? original : EWorkspaceWindowTransitionOutcome::Failed;
	}
};

} // namespace workbench::workspace
