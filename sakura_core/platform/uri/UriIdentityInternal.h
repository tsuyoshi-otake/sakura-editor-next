/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

namespace platform::uri::internal {

inline constexpr wchar_t kComparisonKeySeparator = L'\x1f';

constexpr bool IsAsciiAlpha(wchar_t value) noexcept
{
	return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
}

constexpr bool IsAsciiDigit(wchar_t value) noexcept
{
	return value >= L'0' && value <= L'9';
}

constexpr bool IsSchemeCharacter(wchar_t value) noexcept
{
	return IsAsciiAlpha(value) || IsAsciiDigit(value) || value == L'+' || value == L'-' || value == L'.';
}

constexpr bool IsUnreserved(wchar_t value) noexcept
{
	return IsAsciiAlpha(value) || IsAsciiDigit(value) || value == L'-' || value == L'.' || value == L'_' || value == L'~';
}

constexpr wchar_t ToLowerAscii(wchar_t value) noexcept
{
	return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value - L'A' + L'a') : value;
}

} // namespace platform::uri::internal
