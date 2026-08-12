/*! @file
	@brief Pure profile-authority identity contract shared by persistence and IPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <string_view>

namespace platform::profiles {

//! ProfileAuthorityStore issues 16 random bytes in this canonical textual form.
inline constexpr std::size_t kCanonicalProfileAuthorityIdCharacters = 32;

//! Stable identity validation used at every durable and cross-process boundary.
[[nodiscard]] constexpr bool IsCanonicalProfileAuthorityId(std::string_view value) noexcept
{
	if (value.size() != kCanonicalProfileAuthorityIdCharacters) return false;
	for (const char character : value) {
		if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return false;
	}
	return true;
}

//! Configuration targets use UTF-16 on Windows but carry the same ASCII identity.
[[nodiscard]] constexpr bool IsCanonicalProfileAuthorityId(std::wstring_view value) noexcept
{
	if (value.size() != kCanonicalProfileAuthorityIdCharacters) return false;
	for (const wchar_t character : value) {
		if (!((character >= L'0' && character <= L'9') || (character >= L'a' && character <= L'f'))) return false;
	}
	return true;
}

} // namespace platform::profiles
