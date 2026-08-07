/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/WorkbenchBannerLayout.h"

#include <algorithm>

namespace workbench {
namespace {

constexpr int kDefaultDpi = 96;
//! VS Code's `.monaco-banner` is 26px tall at 100% scaling.  It is a floor, not a
//! fixed size: a larger chrome font must make the strip taller rather than clip.
constexpr int kMinimumHeightDip = 26;
//! Space kept clear at both ends of the strip and above/below the single line.
constexpr int kHorizontalPaddingDip = 8;
constexpr int kVerticalPaddingDip = 4;
//! The message's leading `$(shield)` run is drawn as a square of this side, which
//! is also what sets the strip's minimum content height when the font is small.
constexpr int kIconSideDip = 16;
//! Between two action links, and between the message and the first action.
constexpr int kActionGapDip = 12;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const int effective = dpi == 0 ? kDefaultDpi : static_cast<int>(dpi);
	return std::max(0, (dip * effective + kDefaultDpi / 2) / kDefaultDpi);
}

//! A vertically centred band of `side` pixels inside a strip of `height`.
[[nodiscard]] WorkbenchRect CenteredBand(int left, int right, int side, int height) noexcept
{
	const int top = std::max(0, (height - side) / 2);
	return WorkbenchRect{ left, top, std::max(left, right), std::min(height, top + side) };
}

} // namespace

WorkbenchBannerLayout CalculateWorkbenchBannerLayout(const WorkbenchBannerLayoutInput& input)
{
	WorkbenchBannerLayout layout;
	layout.actions.resize(input.actionWidthPixels.size());

	const unsigned int dpi = input.dpi == 0 ? kDefaultDpi : input.dpi;
	const int contentHeight = std::max(std::max(0, input.textHeightPixels), ScaleDip(kIconSideDip, dpi));
	layout.height = std::max(ScaleDip(kMinimumHeightDip, dpi), contentHeight + 2 * ScaleDip(kVerticalPaddingDip, dpi));

	const int width = std::max(0, input.widthPixels);
	const int padding = ScaleDip(kHorizontalPaddingDip, dpi);
	// A strip too narrow to hold its own padding has nowhere to put anything.
	// Returning the height alone is correct: the band still exists and still
	// paints its background, it simply has no room for content.
	if (width <= 2 * padding) return layout;

	const int contentLeft = padding;
	const int contentRight = width - padding;

	// Actions never shrink: each action always occupies exactly its measured
	// width. When there is not enough room, the rectangle's position -- not its
	// width -- gives way, extending past the strip's left edge (even past x=0)
	// rather than being squeezed into whatever space remains. Painting and hit
	// testing already clip at the strip boundary, so an out-of-bounds-but-
	// full-width rectangle degrades exactly like the message already does when it
	// is clamped to zero: cleanly, not partially.
	const int actionGap = ScaleDip(kActionGapDip, dpi);
	int cursor = contentRight;
	for (std::size_t index = layout.actions.size(); index > 0; --index) {
		const int right = cursor;
		const int left = right - std::max(0, input.actionWidthPixels[index - 1]);
		layout.actions[index - 1] = CenteredBand(left, right, contentHeight, layout.height);
		cursor = left - actionGap;
	}

	const int messageRight = layout.actions.empty()
		? contentRight
		: layout.actions.front().left - actionGap;
	// std::max keeps the rectangle non-inverted; a zero-width message is the
	// documented "no room left, draw nothing" state, not an error.
	layout.message = CenteredBand(contentLeft, std::max(contentLeft, messageRight), contentHeight, layout.height);
	// The measured width is an upper bound, not a demand: a message shorter than
	// the space available must not claim the whole gap, because this rectangle is
	// also what a future hover or focus outline would use.
	layout.message.right = std::min(layout.message.right, layout.message.left + std::max(0, input.messageWidthPixels));
	return layout;
}

int WorkbenchBannerActionAtPoint(const WorkbenchBannerLayout& layout, int x, int y) noexcept
{
	for (std::size_t index = 0; index < layout.actions.size(); ++index) {
		const auto& action = layout.actions[index];
		if (action.Width() <= 0 || action.Height() <= 0) continue;
		if (x >= action.left && x < action.right && y >= action.top && y < action.bottom) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

} // namespace workbench
