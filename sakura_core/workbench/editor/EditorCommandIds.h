/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string_view>

namespace workbench::editor::command_ids {

// These identifiers are the stable VS Code workbench command boundary used by
// native menus, keybindings, context keys, watermark actions, and extension RPC.
inline constexpr std::string_view NewUntitledFile = "workbench.action.files.newUntitledFile";
inline constexpr std::string_view OpenFile = "workbench.action.files.openFile";
inline constexpr std::string_view OpenFolder = "workbench.action.files.openFolder";
inline constexpr std::string_view OpenWorkspace = "workbench.action.openWorkspace";
inline constexpr std::string_view OpenRecent = "workbench.action.openRecent";
inline constexpr std::string_view ClearRecentFiles = "workbench.action.clearRecentFiles";
inline constexpr std::string_view AddRootFolder = "workbench.action.addRootFolder";
inline constexpr std::string_view SaveWorkspaceAs = "workbench.action.saveWorkspaceAs";
inline constexpr std::string_view DuplicateWorkspaceInNewWindow = "workbench.action.duplicateWorkspaceInNewWindow";
inline constexpr std::string_view Save = "workbench.action.files.save";
inline constexpr std::string_view SaveAs = "workbench.action.files.saveAs";
inline constexpr std::string_view SaveAll = "workbench.action.files.saveAll";
inline constexpr std::string_view Revert = "workbench.action.files.revert";
inline constexpr std::string_view CloseActiveEditor = "workbench.action.closeActiveEditor";
inline constexpr std::string_view CloseFolder = "workbench.action.closeFolder";
inline constexpr std::string_view CloseWindow = "workbench.action.closeWindow";
inline constexpr std::string_view Quit = "workbench.action.quit";
inline constexpr std::string_view NewWindow = "workbench.action.newWindow";
inline constexpr std::string_view ShowCommands = "workbench.action.showCommands";
inline constexpr std::string_view OpenSettings = "workbench.action.openSettings";
inline constexpr std::string_view OpenGlobalKeybindings = "workbench.action.openGlobalKeybindings";
inline constexpr std::string_view SelectTheme = "workbench.action.selectTheme";
inline constexpr std::string_view MarkdownShowPreview = "markdown.showPreview";
inline constexpr std::string_view MarkdownShowPreviewToSide = "markdown.showPreviewToSide";
inline constexpr std::string_view MarkdownShowLockedPreviewToSide = "markdown.showLockedPreviewToSide";
inline constexpr std::string_view MarkdownShowSource = "markdown.showSource";
inline constexpr std::string_view MarkdownShowPreviewSecuritySelector = "markdown.showPreviewSecuritySelector";
inline constexpr std::string_view MarkdownPreviewRefresh = "markdown.preview.refresh";
inline constexpr std::string_view MarkdownPreviewToggleLock = "markdown.preview.toggleLock";
inline constexpr std::string_view MarkdownReopenAsPreview = "markdown.reopenAsPreview";
inline constexpr std::string_view MarkdownReopenAsSource = "markdown.reopenAsSource";
inline constexpr std::string_view MarkdownTogglePreview = "markdown.togglePreview";

} // namespace workbench::editor::command_ids
