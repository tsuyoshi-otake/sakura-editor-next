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
		// Verified against upstream's workspaceTrust contribution
		// (src/vs/workbench/contrib/workspace/browser/workspace.contribution.ts):
		// default "prompt", enum ["prompt", "open", "newWindow"],
		// ConfigurationScope.APPLICATION. The enum was previously unconstrained
		// here, which let an unknown value through to a consumer that then had
		// to guess; enforcement fails closed on "prompt" instead.
		{ "security.workspace.trust.untrustedFiles", ConfigurationValue(L"prompt"), { Scope::Profile },
			{ Kind::String, 32, { L"prompt", L"open", L"newWindow" } } },
		// Verified against the same upstream contribution: default "never",
		// enum ["always", "once", "never"], ConfigurationScope.APPLICATION.
		// "never" is upstream's default and is the fail-closed value here too --
		// not prompting withholds trust, it never grants it.
		{ "security.workspace.trust.startupPrompt", ConfigurationValue(L"never"), { Scope::Profile },
			{ Kind::String, 32, { L"always", L"once", L"never" } } },
		// Verified against upstream's workspaceTrust contribution
		// (src/vs/workbench/contrib/workspace/browser/workspace.contribution.ts):
		// default "untilDismissed", enum ["always", "untilDismissed", "never"],
		// ConfigurationScope.APPLICATION -- the same scope every other
		// security.workspace.trust.* setting above already registers as
		// Scope::Profile here, for the same recorded reason.
		{ "security.workspace.trust.banner", ConfigurationValue(L"untilDismissed"), { Scope::Profile },
			{ Kind::String, 32, { L"always", L"untilDismissed", L"never" } } },
		// Per-extension user override that lets an untrusted workspace still
		// activate an extension the workspace itself has not been granted an
		// exemption for. This is profile-owned for the same reason the trust
		// policy above is: a repository must never be able to grant its own
		// extensions an exemption by committing a .vscode/settings.json entry
		// here. It is also upstream's `application` scope, which is the
		// Application-to-Profile divergence this file already records for
		// `http.*` and `update.*`.
		{ "extensions.supportUntrustedWorkspaces", ConfigurationValue(ConfigurationValue::Object{}),
			{ Scope::Profile }, { Kind::Object } },

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
