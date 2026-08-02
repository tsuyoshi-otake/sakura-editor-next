/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace terminal {

//! Interactive areas in the native, single-row terminal panel header.
enum class TerminalHeaderTarget : std::uint8_t {
	None,
	Profile,
	New,
	Dropdown,
	Split,
	Kill,
	More,
	Maximize,
	Close,
	Count,
};

//! Pure pixel layout used by terminal header painting and pointer hit testing.
struct TerminalHeaderLayout {
	RECT header{};
	RECT title{};
	RECT underline{};
	std::array<RECT, static_cast<std::size_t>(TerminalHeaderTarget::Count)> targets{};

	[[nodiscard]] const RECT& RectFor(TerminalHeaderTarget target) const noexcept
	{
		const auto index = static_cast<std::size_t>(target);
		return targets[index < targets.size() ? index : 0];
	}

	[[nodiscard]] TerminalHeaderTarget HitTest(POINT point) const noexcept
	{
		constexpr std::array kHitOrder{
			TerminalHeaderTarget::Close,
			TerminalHeaderTarget::Maximize,
			TerminalHeaderTarget::More,
			TerminalHeaderTarget::Kill,
			TerminalHeaderTarget::Split,
			TerminalHeaderTarget::Dropdown,
			TerminalHeaderTarget::New,
			TerminalHeaderTarget::Profile,
		};
		for (const auto target : kHitOrder) {
			const RECT& rect = RectFor(target);
			if (rect.right > rect.left && rect.bottom > rect.top
				&& point.x >= rect.left && point.x < rect.right
				&& point.y >= rect.top && point.y < rect.bottom) {
				return target;
			}
		}
		return TerminalHeaderTarget::None;
	}
};

namespace detail {

constexpr int kTerminalHeaderDefaultDpi = 96;

[[nodiscard]] constexpr int ScaleTerminalHeaderDip(int dip, unsigned int dpi) noexcept
{
	const auto effectiveDpi = static_cast<int>(dpi == 0 ? kTerminalHeaderDefaultDpi : dpi);
	return std::max(0, (dip * effectiveDpi + kTerminalHeaderDefaultDpi / 2) / kTerminalHeaderDefaultDpi);
}

[[nodiscard]] constexpr RECT EmptyTerminalHeaderRect(int x, int top, int bottom) noexcept
{
	return RECT{ x, top, x, std::max(top, bottom) };
}

} // namespace detail

//! Calculates fixed-size action bounds from the trailing edge toward the title.
//!
//! An action is omitted (represented by an empty rectangle) when the client is too
//! narrow to contain its complete hit target.  This keeps all returned rectangles
//! inside the header and prevents partially clickable icons at small window widths.
[[nodiscard]] inline TerminalHeaderLayout CalculateTerminalHeaderLayout(
	const RECT& bounds, unsigned int dpi, bool includePanelActions = true,
	int headerHeightDip = 30) noexcept
{
	using detail::ScaleTerminalHeaderDip;
	TerminalHeaderLayout result;
	result.header.left = bounds.left;
	result.header.top = bounds.top;
	result.header.right = std::max(bounds.left, bounds.right);
	const LONG availableBottom = std::max(bounds.top, bounds.bottom);
	result.header.bottom = std::min<LONG>(availableBottom,
		bounds.top + ScaleTerminalHeaderDip(std::max(0, headerHeightDip), dpi));

	const LONG top = result.header.top;
	const LONG bottom = result.header.bottom;
	const LONG left = result.header.left;
	const int rightInset = ScaleTerminalHeaderDip(4, dpi);
	const int actionGap = ScaleTerminalHeaderDip(2, dpi);
	LONG cursor = std::max<LONG>(left, result.header.right - rightInset);
	bool hasActionSpace = true;

	auto reserve = [&](TerminalHeaderTarget target, int widthDip, int leadingGapDip = 0) {
		const int width = ScaleTerminalHeaderDip(widthDip, dpi);
		const int leadingGap = ScaleTerminalHeaderDip(leadingGapDip, dpi);
		const int candidateLeft = cursor - width;
		if (hasActionSpace && width > 0 && candidateLeft >= left) {
			result.targets[static_cast<std::size_t>(target)] = RECT{ candidateLeft, top, cursor, bottom };
			cursor = std::max<LONG>(left, candidateLeft - actionGap - leadingGap);
		} else {
			result.targets[static_cast<std::size_t>(target)] = detail::EmptyTerminalHeaderRect(cursor, top, bottom);
			hasActionSpace = false;
		}
	};

	// Visual order, left-to-right: profile, new, dropdown, split, kill, more,
	// separator, maximize, close.  Allocation proceeds from the stable right edge.
	// Maximize and close belong to the physical Panel Part. They are omitted when
	// the containing Part supplies that common chrome above this view.
	if (includePanelActions) {
		reserve(TerminalHeaderTarget::Close, 28);
		reserve(TerminalHeaderTarget::Maximize, 28, 4);
	}
	reserve(TerminalHeaderTarget::More, 28);
	reserve(TerminalHeaderTarget::Kill, 28);
	reserve(TerminalHeaderTarget::Split, 28);
	reserve(TerminalHeaderTarget::Dropdown, 18);
	reserve(TerminalHeaderTarget::New, 24);
	reserve(TerminalHeaderTarget::Profile, 82);

	const LONG titleLeft = std::min<LONG>(result.header.right,
		result.header.left + ScaleTerminalHeaderDip(12, dpi));
	const LONG titleRight = std::max<LONG>(titleLeft, cursor - ScaleTerminalHeaderDip(4, dpi));
	result.title = RECT{ titleLeft, top, std::min<LONG>(titleRight, result.header.right), bottom };

	const LONG underlineHeight = std::min<LONG>(std::max<LONG>(0, bottom - top), ScaleTerminalHeaderDip(2, dpi));
	const LONG underlineWidth = std::min<LONG>(std::max<LONG>(0, result.title.right - result.title.left),
		ScaleTerminalHeaderDip(54, dpi));
	result.underline = RECT{
		result.title.left,
		bottom - underlineHeight,
		result.title.left + underlineWidth,
		bottom,
	};
	return result;
}

} // namespace terminal
