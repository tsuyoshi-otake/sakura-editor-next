/*! @file
	@brief Pure user-data profile identity contract shared by bootstrap, workbench, and profile-scoped services.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <string_view>

namespace platform::profiles {

//! A user-data profile id is opaque and bounded, and is never derived from a display name.
inline constexpr std::size_t kMaximumUserDataProfileIdCharacters = 128;

//! Deliberately wider than the control authority's canonical 32-hex form: VS Code's own
//! Default profile carries a fixed literal id, so a profile-scoped service must validate
//! this identity space rather than the control authority's.
[[nodiscard]] constexpr bool IsOpaqueUserDataProfileId(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumUserDataProfileIdCharacters) return false;
	for (const wchar_t character : value) {
		if (!((character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')
			|| (character >= L'0' && character <= L'9') || character == L'-' || character == L'_')) return false;
	}
	return true;
}

} // namespace platform::profiles
