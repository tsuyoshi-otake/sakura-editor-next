/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>

namespace workbench::icons {

//! Shared optical metrics for Sakura Code's compact, single-colour line icons.
constexpr int kActivityIconDip = 20;
constexpr int kStatusIconDip = 16;
constexpr int kLineStrokeDip = 1;
constexpr int kStatusIconLeadingInsetDip = 4;
constexpr int kStatusIconTextGapDip = 4;
constexpr int kStatusItemInsetDip = 8;
constexpr int kNativeStatusPartChromeDip = 4;

struct IconRect {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;

	[[nodiscard]] constexpr int Width() const noexcept { return right - left; }
	[[nodiscard]] constexpr int Height() const noexcept { return bottom - top; }
	[[nodiscard]] constexpr bool operator==(const IconRect&) const noexcept = default;
};

[[nodiscard]] constexpr int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const unsigned int effectiveDpi = dpi == 0 ? 96U : dpi;
	return std::max(0, (dip * static_cast<int>(effectiveDpi) + 48) / 96);
}

[[nodiscard]] constexpr int LineStrokePixels(unsigned int dpi) noexcept
{
	return std::max(1, ScaleDip(kLineStrokeDip, dpi));
}

[[nodiscard]] constexpr int StatusTextInsetPixels(unsigned int dpi) noexcept
{
	return ScaleDip(kStatusIconLeadingInsetDip + kStatusIconDip + kStatusIconTextGapDip, dpi);
}

[[nodiscard]] constexpr int StatusItemHorizontalPaddingPixels(unsigned int dpi) noexcept
{
	return 2 * ScaleDip(kStatusItemInsetDip, dpi);
}

//! Includes the space reserved internally by the native status-bar part rectangle.
[[nodiscard]] constexpr int StatusItemPartWidthPaddingPixels(unsigned int dpi) noexcept
{
	return StatusItemHorizontalPaddingPixels(dpi) + ScaleDip(kNativeStatusPartChromeDip, dpi);
}

//! Centers an icon's optical square inside a caller-owned physical-pixel rectangle.
[[nodiscard]] constexpr IconRect CenteredIconBounds(IconRect bounds, int iconDip, unsigned int dpi) noexcept
{
	const int side = std::min({ ScaleDip(iconDip, dpi), std::max(0, bounds.Width()), std::max(0, bounds.Height()) });
	const int left = bounds.left + (bounds.Width() - side) / 2;
	const int top = bounds.top + (bounds.Height() - side) / 2;
	return { left, top, left + side, top + side };
}

//! Places a status icon before its label while preserving the label's established hit rectangle.
[[nodiscard]] constexpr IconRect LeadingStatusIconBounds(IconRect bounds, unsigned int dpi) noexcept
{
	const int side = std::min(ScaleDip(kStatusIconDip, dpi), std::max(0, bounds.Height()));
	const int left = std::min(bounds.right, bounds.left + ScaleDip(kStatusIconLeadingInsetDip, dpi));
	const int top = bounds.top + (bounds.Height() - side) / 2;
	return { left, top, std::min(bounds.right, left + side), top + side };
}

} // namespace workbench::icons
