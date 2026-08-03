/*! @file
 * @brief UI-independent request planning shared by native workspace flows.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/workspace/WorkspaceEditingService.h"
#include "workbench/workspace/WorkspaceWindowTransitionService.h"

#include <utility>
#include <vector>

namespace workbench::workspace {

//! Builds document edit requests from an immutable context snapshot.  The
//! planner has no native-dialog, window, process, or persistence dependency so
//! the Empty/Folder/Workspace cases remain directly testable.
class CWorkspaceWindowTransitionPlanner final {
public:
	[[nodiscard]] static WorkspaceFoldersEditRequest BuildWorkspaceDocumentEdit(
		const config::WorkspaceContextSnapshot& current,
		platform::uri::Uri target,
		std::vector<WorkspaceFolderEdit> additionalFolders = {})
	{
		WorkspaceFoldersEditRequest request{
			.source = current.workspaceConfigUri ? *current.workspaceConfigUri : target,
			.target = std::move(target),
		};
		request.folders.reserve(current.folders.size() + additionalFolders.size());
		for (const auto& folder : current.folders) {
			// displayName may have been derived by the parser.  The editing service
			// recovers an explicitly stored `name` from the source document; omitting
			// it here prevents a derived label from becoming persistent metadata.
			request.folders.push_back({ folder.uri, std::nullopt });
		}
		for (auto& folder : additionalFolders) {
			request.folders.push_back(std::move(folder));
		}
		return request;
	}

	[[nodiscard]] static constexpr WorkspaceWindowTransitionRequest ManagedReplacement() noexcept
	{
		return { .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true };
	}

	[[nodiscard]] static constexpr WorkspaceWindowTransitionRequest SaveAsReplacement() noexcept
	{
		// A user-selected target remains available for recovery if successor
		// launch fails; the previous window remains the active owner.
		return { .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = false };
	}

	[[nodiscard]] static constexpr WorkspaceWindowTransitionRequest ManagedDuplicate() noexcept
	{
		return { .replaceCurrentWindow = false, .deleteStagedTargetOnFailure = true };
	}

	[[nodiscard]] static constexpr WorkspaceWindowTransitionRequest CloseToEmpty() noexcept
	{
		return { .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = false };
	}
};

} // namespace workbench::workspace
