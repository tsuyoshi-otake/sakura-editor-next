/*! @file
 * @brief Upstream VS Code's stable IDs for the Explorer file-operation commands.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <string_view>

namespace workbench::commands {

//! These are VS Code's own command IDs (fileActions.contribution.ts and the
//! electron-browser revealFileInOS contribution), not a parallel naming scheme.
//! `RegisterExplorerCommands` registers exactly this set; a surface that builds
//! menu rows or keybinding routes for the Explorer must name commands through
//! these constants so the ID exists in one place.
inline constexpr std::string_view kExplorerNewFileCommandId = "explorer.newFile";
inline constexpr std::string_view kExplorerNewFolderCommandId = "explorer.newFolder";
inline constexpr std::string_view kRenameFileCommandId = "renameFile";
inline constexpr std::string_view kMoveFileToTrashCommandId = "moveFileToTrash";
inline constexpr std::string_view kDeleteFileCommandId = "deleteFile";
inline constexpr std::string_view kCopyFilePathCommandId = "copyFilePath";
inline constexpr std::string_view kCopyRelativeFilePathCommandId = "copyRelativeFilePath";
inline constexpr std::string_view kRevealFileInOSCommandId = "revealFileInOS";

//! VS Code's Explorer ViewTitle actions. These are distinct from the
//! resource-scoped context-menu commands above: the title actions resolve the
//! current Explorer selection/root inside the view and carry no payload.
inline constexpr std::string_view kCreateFileFromExplorerCommandId =
	"workbench.files.action.createFileFromExplorer";
inline constexpr std::string_view kCreateFolderFromExplorerCommandId =
	"workbench.files.action.createFolderFromExplorer";
inline constexpr std::string_view kRefreshFilesExplorerCommandId =
	"workbench.files.action.refreshFilesExplorer";
inline constexpr std::string_view kCollapseExplorerFoldersCommandId =
	"workbench.files.action.collapseExplorerFolders";

} // namespace workbench::commands
