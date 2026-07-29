/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace terminal {

//! Windows Terminal-compatible terminal font and cell geometry.
struct TerminalFontMetrics {
	int fontPixelHeight{ 1 };
	int cellWidth{ 1 };
	int cellHeight{ 1 };

	friend constexpr bool operator==(const TerminalFontMetrics&, const TerminalFontMetrics&) noexcept = default;
};

namespace detail {

[[nodiscard]] constexpr int RoundedRatio(int value, int numerator, int denominator) noexcept
{
	if (value <= 0 || numerator <= 0 || denominator <= 0) return 1;
	const auto product = static_cast<std::int64_t>(value) * numerator;
	const auto rounded = (product + denominator / 2) / denominator;
	return static_cast<int>(std::clamp<std::int64_t>(rounded, 1, std::numeric_limits<int>::max()));
}

} // namespace detail

//! Mirrors the selected Windows Terminal appearance: 9 pt, cell width 0.6,
//! line height 1.2.  Keeping this calculation independent of GDI's
//! tmAveCharWidth prevents integer average-width rounding from inserting a
//! visible gap between every terminal cell.
[[nodiscard]] constexpr TerminalFontMetrics CalculateTerminalFontMetrics(
	int pointSize, unsigned int dpi) noexcept
{
	const int effectivePoints = std::max(1, pointSize);
	const int effectiveDpi = static_cast<int>(dpi == 0 ? 96U : dpi);
	const int fontPixels = detail::RoundedRatio(effectivePoints, effectiveDpi, 72);
	return {
		fontPixels,
		detail::RoundedRatio(fontPixels, 3, 5),
		detail::RoundedRatio(fontPixels, 6, 5),
	};
}

} // namespace terminal
