/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extensions/BuiltInExtensionLocalization.h"

#include "sakura_rc.h"

#include <algorithm>
#include <array>

namespace workbench::extensions {
namespace {

struct BuiltInLocalizationIds final {
	const std::wstring_view extensionId;
	const UINT displayName;
	const UINT description;
};

// One row per entry of senp::EmbeddedBuiltInExtensionIds(), sorted by ID.
// WorkbenchExtensionsLocalization tests fail when a built-in has no row here
// or an empty string in any shipped language.
constexpr std::array kBuiltInLocalizations{
	BuiltInLocalizationIds{ L"sakura-configuration-language-basics",
		STR_SENP_CONFIGURATION_LANGUAGE_BASICS_NAME,
		STR_SENP_CONFIGURATION_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-core-language-basics",
		STR_SENP_CORE_LANGUAGE_BASICS_NAME, STR_SENP_CORE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-database-language-basics",
		STR_SENP_DATABASE_LANGUAGE_BASICS_NAME,
		STR_SENP_DATABASE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-github-actions",
		STR_SENP_GITHUB_ACTIONS_NAME, STR_SENP_GITHUB_ACTIONS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-github-pull-requests",
		STR_SENP_GITHUB_PULL_REQUESTS_NAME, STR_SENP_GITHUB_PULL_REQUESTS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-indent-rainbow",
		STR_SENP_INDENT_RAINBOW_NAME, STR_SENP_INDENT_RAINBOW_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-infrastructure-language-basics",
		STR_SENP_INFRASTRUCTURE_LANGUAGE_BASICS_NAME,
		STR_SENP_INFRASTRUCTURE_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-legacy-language-basics",
		STR_SENP_LEGACY_LANGUAGE_BASICS_NAME, STR_SENP_LEGACY_LANGUAGE_BASICS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-projects",
		STR_SENP_PROJECTS_NAME, STR_SENP_PROJECTS_DESCRIPTION },
	BuiltInLocalizationIds{ L"sakura-shell-language-basics",
		STR_SENP_SHELL_LANGUAGE_BASICS_NAME, STR_SENP_SHELL_LANGUAGE_BASICS_DESCRIPTION },
};

} // namespace

std::optional<BuiltInLocalizationResources> FindBuiltInLocalization(std::wstring_view extensionId) noexcept
{
	const auto found = std::ranges::find(kBuiltInLocalizations, extensionId,
		&BuiltInLocalizationIds::extensionId);
	if (found == kBuiltInLocalizations.end()) return std::nullopt;
	return BuiltInLocalizationResources{ found->displayName, found->description };
}

} // namespace workbench::extensions
