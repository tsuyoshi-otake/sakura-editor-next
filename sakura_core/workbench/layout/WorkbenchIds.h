/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string_view>

//! Stable workbench identifiers shared by native and extension contributions.
namespace workbench::layout::ids {

namespace part {
inline constexpr std::string_view Titlebar = "workbench.parts.titlebar";
inline constexpr std::string_view Banner = "workbench.parts.banner";
inline constexpr std::string_view Activitybar = "workbench.parts.activitybar";
inline constexpr std::string_view Sidebar = "workbench.parts.sidebar";
inline constexpr std::string_view Panel = "workbench.parts.panel";
inline constexpr std::string_view Auxiliarybar = "workbench.parts.auxiliarybar";
inline constexpr std::string_view Editor = "workbench.parts.editor";
inline constexpr std::string_view Statusbar = "workbench.parts.statusbar";
inline constexpr std::string_view Sessions = "workbench.parts.sessions";
} // namespace part

namespace viewContainer {
inline constexpr std::string_view Explorer = "workbench.view.explorer";
inline constexpr std::string_view Search = "workbench.view.search";
inline constexpr std::string_view RunAndDebug = "workbench.view.debug";
inline constexpr std::string_view SourceControl = "workbench.view.scm";
inline constexpr std::string_view Extensions = "workbench.view.extensions";
inline constexpr std::string_view Problems = "workbench.panel.markers";
inline constexpr std::string_view Output = "workbench.panel.output";
inline constexpr std::string_view Terminal = "terminal";
inline constexpr std::string_view Ports = "~remote.forwardedPortsContainer";
inline constexpr std::string_view DebugConsole = "workbench.panel.repl";
//! Compatibility container for Sakura's legacy extension sidebar. It is never an alias for a part.
inline constexpr std::string_view LegacyExtensionViewsAuxiliary = "sakura.extensionViews";
} // namespace viewContainer

namespace view {
inline constexpr std::string_view Explorer = "workbench.explorer.fileView";
inline constexpr std::string_view Outline = "outline";
inline constexpr std::string_view Search = "workbench.view.search";
inline constexpr std::string_view DebugVariables = "workbench.debug.variablesView";
inline constexpr std::string_view DebugWatch = "workbench.debug.watchExpressionsView";
inline constexpr std::string_view DebugCallStack = "workbench.debug.callStackView";
inline constexpr std::string_view DebugLoadedScripts = "workbench.debug.loadedScriptsView";
inline constexpr std::string_view DebugBreakpoints = "workbench.debug.breakPointsView";
inline constexpr std::string_view SourceControl = "workbench.scm";
inline constexpr std::string_view Extensions = "workbench.views.extensions.installed";
inline constexpr std::string_view Problems = "workbench.panel.markers.view";
inline constexpr std::string_view Output = "workbench.panel.output";
inline constexpr std::string_view Terminal = "terminal";
inline constexpr std::string_view Ports = "~remote.forwardedPorts";
inline constexpr std::string_view DebugConsole = "workbench.panel.repl.view";
//! Root view used by the native adapter to host legacy extension-contributed content.
inline constexpr std::string_view LegacyExtensionViews = "sakura.extensionViews.root";
} // namespace view

inline constexpr std::string_view BuiltinOwner = "sakura.builtin";

} // namespace workbench::layout::ids
