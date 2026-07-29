/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <string_view>

namespace terminal::unicode {

//! Result for the first extended grapheme cluster in a UTF-16 view.
struct GraphemeMeasurement {
	std::size_t codeUnits = 0;
	int width = 0;
};

//! Uses the pinned Windows Terminal Unicode 16.0 tables behind a Sakura type.
//! No vendored type is exposed through this header.
[[nodiscard]] GraphemeMeasurement MeasureFirstGrapheme(std::wstring_view text) noexcept;

} // namespace terminal::unicode
