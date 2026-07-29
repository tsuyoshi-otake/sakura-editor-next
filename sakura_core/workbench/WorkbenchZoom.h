/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>

namespace workbench {

inline constexpr int kMinimumZoomPercent = 70;
inline constexpr int kMaximumZoomPercent = 200;
inline constexpr int kZoomStepPercent = 10;

[[nodiscard]] constexpr int AdjustZoomPercent(int current, int direction) noexcept
{
	if (direction == 0) return 100;
	return std::clamp(current + (direction < 0 ? -kZoomStepPercent : kZoomStepPercent),
		kMinimumZoomPercent, kMaximumZoomPercent);
}

[[nodiscard]] constexpr unsigned int ScaleDpi(unsigned int dpi, int zoomPercent) noexcept
{
	return static_cast<unsigned int>((static_cast<unsigned long long>(dpi == 0 ? 96 : dpi)
		* std::clamp(zoomPercent, kMinimumZoomPercent, kMaximumZoomPercent) + 50) / 100);
}

} // namespace workbench
