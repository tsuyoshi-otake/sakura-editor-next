/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/BuiltinConfigurationDescriptors.h"

namespace config {

std::vector<ConfigurationDescriptor> BuiltinConfigurationDescriptors()
{
	using Scope = EConfigurationScope;
	using Kind = EConfigurationValueKind;
	return {
		// VS Code Window-scoped setting. Folder is included because a single-folder
		// workspace persists its window settings in .vscode/settings.json.
		{ "workbench.editor.showTabs", ConfigurationValue(L"multiple"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder } },
		// VS Code's native editor minimap contract. Section-header settings remain
		// unregistered until the editor has a real symbol/folding provider; accepting
		// those keys as inert screenshot decoration would violate the capability boundary.
		{ "editor.minimap.enabled", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "editor.minimap.autohide", ConfigurationValue(L"none"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 16, { L"none", L"mouseover", L"scroll" } } },
		{ "editor.minimap.side", ConfigurationValue(L"right"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 8, { L"left", L"right" } } },
		{ "editor.minimap.size", ConfigurationValue(L"proportional"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 16, { L"proportional", L"fill", L"fit" } } },
		{ "editor.minimap.showSlider", ConfigurationValue(L"mouseover"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 16, { L"always", L"mouseover" } } },
		{ "editor.minimap.renderCharacters", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "editor.minimap.maxColumn", ConfigurationValue(120),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::Integer, std::nullopt, {}, 1, 10000 } },
		{ "editor.minimap.scale", ConfigurationValue(1),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::Integer, std::nullopt, {}, 1, 3 } },
		// VS Code's editor-owned indentation guides. This remains separate from
		// indent-rainbow, whose classic decoration contributes background bands only.
		{ "editor.guides.indentation", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		// VS Code's Activity Bar location is a window-scoped setting. The native
		// workbench exposes only the owner-approved default, top, and bottom layouts.
		// `hidden` is intentionally rejected rather than accepted as a fake capability.
		{ "workbench.activityBar.location", ConfigurationValue(L"default"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 8, { L"default", L"top", L"bottom" } } },
		// VS Code's selected color-theme label/id. An empty value resolves to
		// Sakura's built-in theme matching the legacy profile dark/light preference;
		// the native palette remains the final fail-closed fallback.
		{ "workbench.colorTheme", ConfigurationValue(L""),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 512 } },
		// VS Code's selected file-icon-theme id/label. Empty means the built-in
		// Explorer presentation with no contributed file icons.
		{ "workbench.iconTheme", ConfigurationValue(L""),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 512 } },
		// VS Code's bounded terminal history setting. Sakura applies the upstream
		// default dynamically but keeps an explicit implementation ceiling so one
		// terminal cannot retain unbounded dense native row storage.
		{ "terminal.integrated.scrollback", ConfigurationValue(1000),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::Integer, std::nullopt, {}, 0, 100000 } },
		// VS Code's terminal tab presentation contract. These are window-scoped
		// settings, so profile/workspace/folder layers participate in one effective
		// snapshot. The resolver applies sanitization and bounded output after the
		// snapshot is projected to the terminal tool.
		{ "terminal.integrated.tabs.title", ConfigurationValue(L"${process}"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 256 } },
		{ "terminal.integrated.tabs.description", ConfigurationValue(L"${task}${separator}${local}${separator}${cwdFolder}"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 256 } },
		{ "terminal.integrated.tabs.separator", ConfigurationValue(L" - "),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 256 } },
		{ "terminal.integrated.tabs.allowAgentCliTitle", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "terminal.integrated.tabs.enabled", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "terminal.integrated.tabs.hideCondition", ConfigurationValue(L"singleTerminal"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 32, { L"never", L"singleTerminal", L"singleGroup" } } },
		{ "terminal.integrated.tabs.showActiveTerminal", ConfigurationValue(L"singleTerminalOrNarrow"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 32, { L"always", L"singleTerminal", L"singleTerminalOrNarrow", L"never" } } },
		{ "terminal.integrated.tabs.showActions", ConfigurationValue(L"singleTerminalOrNarrow"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 32, { L"always", L"singleTerminal", L"singleTerminalOrNarrow", L"never" } } },
		{ "terminal.integrated.tabs.location", ConfigurationValue(L"right"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 8, { L"left", L"right" } } },
		// VS Code's `scm.countBadge`, which gates the Source Control ViewContainer's
		// Activity Bar badge. Upstream's values and default are `all` / `focused` /
		// `off` with `all` registered. Documented divergence: `focused` is answered
		// as `all` here, because this fork publishes a single repository and the two
		// then count the same resources by construction; it is not silently ignored
		// for a multi-repository workspace, which does not exist yet.
		{ "scm.countBadge", ConfigurationValue(L"all"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 16, { L"all", L"focused", L"off" } } },
		// VS Code's `scm.inputMinLineCount` / `scm.inputMaxLineCount`, which bound
		// the Source Control commit box: it opens at the minimum and auto-grows to
		// the maximum as the message wraps. Upstream registers 1 and 10 with the
		// same 1..50 bounds, so both are honoured here rather than hard-coded.
		{ "scm.inputMinLineCount", ConfigurationValue(1),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::Integer, std::nullopt, {}, 1, 50 } },
		{ "scm.inputMaxLineCount", ConfigurationValue(10),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::Integer, std::nullopt, {}, 1, 50 } },
		// VS Code 1.18's Git status in the File Explorer. `git.decorations.enabled`
		// belongs to the Git extension and decides whether decorations are provided
		// at all; the two `explorer.decorations.*` keys belong to the workbench and
		// decide which half of a provided decoration the Explorer renders. All three
		// are registered `true` upstream and are folder-scoped, because which files
		// a repository decorates is a property of the folder being viewed.
		{ "git.decorations.enabled", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "explorer.decorations.colors", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		{ "explorer.decorations.badges", ConfigurationValue(true),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::Boolean } },
		// The terminal panel's multiplexer keybinding preset. VS Code has no
		// equivalent setting, so this cannot reuse an upstream identifier; it is
		// deliberately kept out of the `terminal.integrated.*` namespace, whose keys
		// must keep upstream semantics, and recorded as a fork extension in
		// terminal/CLAUDE.md. Window-scoped like the rest of the workbench chrome.
		// The owner-chosen default is `screen`; `none` restores an unclaimed Ctrl+A.
		{ "sakura.terminal.shortcutPreset", ConfigurationValue(L"screen"),
			{ Scope::Profile, Scope::Workspace, Scope::Folder },
			{ Kind::String, 16, { L"none", L"tmux", L"screen" } } },
		// Network policy is deliberately application/profile-only.  A repository
		// must not be able to redirect an editor to a proxy or registry, weaken
		// certificate handling, or make requests wait longer by committing a
		// .vscode/settings.json file.
		{ "http.proxy", ConfigurationValue(L""), { Scope::Application, Scope::Profile },
			{ Kind::String, 2048 } },
		{ "http.proxySupport", ConfigurationValue(L"fallback"), { Scope::Application, Scope::Profile },
			{ Kind::String, 16, { L"off", L"on", L"fallback", L"override" } } },
		{ "http.noProxy", ConfigurationValue(ConfigurationValue::Array{}),
			{ Scope::Application, Scope::Profile },
			{ Kind::Array, std::nullopt, {}, std::nullopt, std::nullopt, 128, true, 256 } },
		{ "http.proxyStrictSSL", ConfigurationValue(true), { Scope::Application, Scope::Profile },
			{ Kind::Boolean } },
		{ "http.systemCertificates", ConfigurationValue(true), { Scope::Application, Scope::Profile },
			{ Kind::Boolean } },
		{ "http.timeout", ConfigurationValue(30000), { Scope::Application, Scope::Profile },
			{ Kind::Integer, std::nullopt, {}, 1000, 120000 } },
		// Update policy is application/profile-only for the same reason network
		// policy is: opening a repository must never be able to change whether,
		// how, or from where this editor updates itself. VS Code registers all
		// four with ConfigurationScope.APPLICATION; the divergence to
		// Application/Profile is the one already recorded for `http.*` in
		// config/CLAUDE.md, not a new one.
		{ "update.mode", ConfigurationValue(L"default"), { Scope::Application, Scope::Profile },
			{ Kind::String, 16, { L"none", L"manual", L"start", L"default" } } },
		{ "update.enableWindowsBackgroundUpdates", ConfigurationValue(true),
			{ Scope::Application, Scope::Profile }, { Kind::Boolean } },
		{ "update.showReleaseNotes", ConfigurationValue(true),
			{ Scope::Application, Scope::Profile }, { Kind::Boolean } },
		{ "update.titleBar", ConfigurationValue(true),
			{ Scope::Application, Scope::Profile }, { Kind::Boolean } },
	};
}

} // namespace config
