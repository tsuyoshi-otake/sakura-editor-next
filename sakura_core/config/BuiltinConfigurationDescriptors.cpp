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
		// VS Code's selected color-theme label/id. An empty value resolves to
		// Sakura's built-in theme matching the legacy profile dark/light preference;
		// the native palette remains the final fail-closed fallback.
		{ "workbench.colorTheme", ConfigurationValue(L""),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 512 } },
		// VS Code's selected file-icon-theme id/label. Empty means the built-in
		// Explorer presentation with no contributed file icons.
		{ "workbench.iconTheme", ConfigurationValue(L""),
			{ Scope::Profile, Scope::Workspace, Scope::Folder }, { Kind::String, 512 } },
		// Application-scoped trust policy is profile-owned in Sakura. It is never
		// accepted from a workspace/folder document.
		{ "security.workspace.trust.enabled", ConfigurationValue(true), { Scope::Profile } },
		{ "security.workspace.trust.emptyWindow", ConfigurationValue(true), { Scope::Profile } },
		{ "security.workspace.trust.untrustedFiles", ConfigurationValue(L"prompt"), { Scope::Profile } },

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
		{ "extensions.openVsx.registry", ConfigurationValue(L"https://open-vsx.org"),
			{ Scope::Application, Scope::Profile }, { Kind::String, 2048 } },

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
