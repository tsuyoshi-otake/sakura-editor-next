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
	};
}

} // namespace config
