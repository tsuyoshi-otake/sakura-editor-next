/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <algorithm>
#include <cstdint>

namespace view::line_number {

//! VS Code's `editor.lineNumbersMinChars` default.
inline constexpr int kMinimumDigitCount = 5;
//! VS Code reserves this width for line decorations between numbers and text.
inline constexpr int kDecorationWidthDip = 10;
inline constexpr unsigned int kDefaultDpi = 96;

struct WorkbenchLineNumberLayout final {
	int digitCount = kMinimumDigitCount;
	int maxDigitWidth = 1;
	int lineNumbersWidth = kMinimumDigitCount;
	int decorationsWidth = kDecorationWidthDip;
	int totalWidth = kMinimumDigitCount + kDecorationWidthDip;

	[[nodiscard]] constexpr bool operator==(const WorkbenchLineNumberLayout&) const noexcept = default;
};

[[nodiscard]] constexpr int DecimalDigitCount(int lineCount) noexcept
{
	int value = std::max(1, lineCount);
	int digits = 1;
	while (value >= 10) {
		value /= 10;
		++digits;
	}
	return digits;
}

[[nodiscard]] constexpr int ScaleDip(int value, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	return static_cast<int>((static_cast<std::int64_t>(value) * effectiveDpi + kDefaultDpi / 2)
		/ kDefaultDpi);
}

[[nodiscard]] constexpr WorkbenchLineNumberLayout CalculateWorkbenchLineNumberLayout(
	int lineCount, int maxDigitWidth, unsigned int dpi) noexcept
{
	WorkbenchLineNumberLayout layout;
	layout.digitCount = std::max(kMinimumDigitCount, DecimalDigitCount(lineCount));
	layout.maxDigitWidth = std::max(1, maxDigitWidth);
	layout.lineNumbersWidth = layout.digitCount * layout.maxDigitWidth;
	layout.decorationsWidth = ScaleDip(kDecorationWidthDip, dpi);
	layout.totalWidth = layout.lineNumbersWidth + layout.decorationsWidth;
	return layout;
}

//! Returns the zero-based x coordinate for a tabular-number run in the number column.
[[nodiscard]] constexpr int RightAlignedDigitX(
	const WorkbenchLineNumberLayout& layout, int drawnDigitCount) noexcept
{
	const int width = std::max(0, drawnDigitCount) * layout.maxDigitWidth;
	return std::max(0, layout.lineNumbersWidth - width);
}

} // namespace view::line_number
