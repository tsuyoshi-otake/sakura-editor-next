/*! @file */
/*
Copyright (C) 2026, Sakura Editor Organization

SPDX-License-Identifier: Zlib
*/
#pragma once

#include <optional>
#include <string_view>

namespace workbench::extensions {

//! String resources that replace one embedded built-in's manifest name and
//! description in the selected UI language.
struct BuiltInLocalizationResources final {
	const unsigned int displayName = 0;
	const unsigned int description = 0;
};

//! Looks up a built-in by stable extension ID. Every ID in
//! senp::EmbeddedBuiltInExtensionIds() must have an entry; the
//! WorkbenchExtensionsLocalization tests enforce that for each shipped
//! language. Developer and publisher packages are never localized and keep
//! their manifest text.
[[nodiscard]] std::optional<BuiltInLocalizationResources> FindBuiltInLocalization(
	std::wstring_view extensionId) noexcept;

} // namespace workbench::extensions
