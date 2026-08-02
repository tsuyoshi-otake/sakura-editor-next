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
inline constexpr std::string_view Save = "workbench.action.files.save";
inline constexpr std::string_view SaveAs = "workbench.action.files.saveAs";
inline constexpr std::string_view Revert = "workbench.action.files.revert";
inline constexpr std::string_view CloseActiveEditor = "workbench.action.closeActiveEditor";
inline constexpr std::string_view ShowCommands = "workbench.action.showCommands";
inline constexpr std::string_view OpenSettings = "workbench.action.openSettings";
inline constexpr std::string_view ShowExtensions = "workbench.view.extensions";
inline constexpr std::string_view OpenGlobalKeybindings = "workbench.action.openGlobalKeybindings";
inline constexpr std::string_view SelectTheme = "workbench.action.selectTheme";
inline constexpr std::string_view SelectIconTheme = "workbench.action.selectIconTheme";

} // namespace workbench::editor::command_ids
