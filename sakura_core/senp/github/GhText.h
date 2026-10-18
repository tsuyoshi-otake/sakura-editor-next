/*! @file
 * @brief Shared ordinal wide-text comparison for the GitHub tool boundary.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <Windows.h>

#include <string_view>

namespace senp::github {

//! Ordinal case-insensitive comparison for the hostnames and account names this
//! boundary validates; never locale-dependent.
inline bool EqualsInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept
{
	return left.size() == right.size() && ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

} // namespace senp::github
