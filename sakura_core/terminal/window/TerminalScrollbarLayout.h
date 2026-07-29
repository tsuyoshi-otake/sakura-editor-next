/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstddef>

namespace terminal {

//! Pure geometry for the terminal's thin, overlay scroll bar.
struct TerminalScrollbarLayout {
	RECT track{};
	RECT hitTrack{};
	RECT thumb{};
	std::size_t maximumTop{};
	bool scrollable{};

	[[nodiscard]] bool HitTest(POINT point) const noexcept
	{
		return scrollable && point.x >= hitTrack.left && point.x < hitTrack.right
			&& point.y >= hitTrack.top && point.y < hitTrack.bottom;
	}

	[[nodiscard]] bool ThumbHitTest(POINT point) const noexcept
	{
		return scrollable && point.x >= thumb.left && point.x < thumb.right
			&& point.y >= thumb.top && point.y < thumb.bottom;
	}
};

namespace detail {

constexpr int kTerminalScrollbarDefaultDpi = 96;

[[nodiscard]] constexpr int ScaleTerminalScrollbarDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = static_cast<int>(dpi == 0 ? kTerminalScrollbarDefaultDpi : dpi);
	return std::max(0, (dip * effectiveDpi + kTerminalScrollbarDefaultDpi / 2) / kTerminalScrollbarDefaultDpi);
}

[[nodiscard]] inline int TerminalScrollbarScaledPosition(int extent, std::size_t numerator,
	std::size_t denominator) noexcept
{
	if (extent <= 0 || numerator == 0 || denominator == 0) return 0;
	if (numerator >= denominator) return extent;
	// Keeping the multiplication in long double prevents a large scrollback buffer
	// from overflowing before its pixel position is bounded to this small track.
	const auto scaled = static_cast<long double>(extent) * static_cast<long double>(numerator)
		/ static_cast<long double>(denominator);
	return std::clamp(static_cast<int>(scaled + 0.5L), 0, extent);
}

[[nodiscard]] inline std::size_t TerminalScrollbarScaledRow(std::size_t extent, int numerator,
	int denominator) noexcept
{
	if (extent == 0 || numerator <= 0 || denominator <= 0) return 0;
	if (numerator >= denominator) return extent;
	const auto scaled = static_cast<long double>(extent) * static_cast<long double>(numerator)
		/ static_cast<long double>(denominator);
	if (scaled >= static_cast<long double>(extent)) return extent;
	return static_cast<std::size_t>(scaled + 0.5L);
}

} // namespace detail

//! Calculates a right-edge overlay scrollbar.  It reserves no terminal columns.
[[nodiscard]] inline TerminalScrollbarLayout CalculateTerminalScrollbarLayout(const RECT& client,
	std::size_t totalRows, std::size_t visibleRows, std::size_t topRow, unsigned int dpi) noexcept
{
	TerminalScrollbarLayout result;
	const int left = client.left;
	const int top = client.top;
	const int right = std::max(client.left, client.right);
	const int bottom = std::max(client.top, client.bottom);
	const int height = bottom - top;
	if (right <= left || height <= 0 || visibleRows == 0 || totalRows <= visibleRows) return result;

	const int visualWidth = std::max(1, detail::ScaleTerminalScrollbarDip(6, dpi));
	const int hitWidth = std::max(visualWidth, detail::ScaleTerminalScrollbarDip(10, dpi));
	result.track = RECT{ std::max(left, right - visualWidth), top, right, bottom };
	result.hitTrack = RECT{ std::max(left, right - hitWidth), top, right, bottom };
	result.maximumTop = totalRows - visibleRows;
	const int minimumThumb = std::min(height,
		std::max(1, detail::ScaleTerminalScrollbarDip(20, dpi)));
	const int proportionalThumb = detail::TerminalScrollbarScaledPosition(height, visibleRows, totalRows);
	const int thumbHeight = std::clamp(std::max(minimumThumb, proportionalThumb), 1, height);
	const int travel = height - thumbHeight;
	const int offset = detail::TerminalScrollbarScaledPosition(travel,
		std::min(topRow, result.maximumTop), result.maximumTop);
	result.thumb = RECT{ result.track.left, top + offset, result.track.right, top + offset + thumbHeight };
	result.scrollable = true;
	return result;
}

//! Converts a thumb drag pointer location into a bounded logical top row.
[[nodiscard]] inline std::size_t TerminalScrollbarTopRowFromDrag(const TerminalScrollbarLayout& layout,
	int pointerY, int thumbGrabOffset) noexcept
{
	if (!layout.scrollable || layout.maximumTop == 0) return 0;
	const int travel = (layout.track.bottom - layout.track.top) - (layout.thumb.bottom - layout.thumb.top);
	if (travel <= 0) return 0;
	const auto rawPosition = static_cast<long long>(pointerY) - thumbGrabOffset - layout.track.top;
	const int position = static_cast<int>(std::clamp<long long>(rawPosition, 0, travel));
	return detail::TerminalScrollbarScaledRow(layout.maximumTop, position, travel);
}

} // namespace terminal
