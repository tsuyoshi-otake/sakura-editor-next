/*! @file
	@brief Pure geometry for ruler labels.
*/
/*
	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_RULERLAYOUT_H_
#define SAKURA_RULERLAYOUT_H_
#pragma once

#include <algorithm>
#include <cstdint>

namespace view::ruler {

struct MajorLabelPosition {
	int x = 0;
	int y = 0;
};

[[nodiscard]] constexpr int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = dpi == 0 ? 96U : dpi;
	const auto scaled = static_cast<std::int64_t>(dip) * effectiveDpi;
	return static_cast<int>((scaled + 48) / 96);
}

// Keep every major label one 4-DIP rhythm unit away from its tick. In
// particular, the first "0" must not share pixels with the text-area edge.
[[nodiscard]] constexpr MajorLabelPosition CalculateMajorLabelPosition(
	int tickX, int textAreaLeft, unsigned int dpi) noexcept
{
	const int inset = ScaleDip(4, dpi);
	return { (std::max)(tickX + inset, textAreaLeft + inset), 0 };
}

} // namespace view::ruler

#endif // SAKURA_RULERLAYOUT_H_
