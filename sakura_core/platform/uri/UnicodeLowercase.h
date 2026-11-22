/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <string>
#include <string_view>

namespace platform::uri::internal {

// Applies Unicode 17.0.0 full lowercase mapping and the Final_Sigma context,
// independently of the CRT and process locale. For valid UTF-16 the result
// matches JavaScript toLowerCase and Rust str::to_lowercase.
// An unpaired surrogate is copied unchanged.
std::wstring ToUnicodeLowercase(std::wstring_view utf16);

} // namespace platform::uri::internal
