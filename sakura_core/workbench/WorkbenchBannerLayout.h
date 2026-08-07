/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/WorkbenchLayout.h"

#include <vector>

namespace workbench {

//! Everything the banner's placement depends on, with the text extents already
//! measured by the caller against a real device context.  Keeping the measurement
//! outside makes every placement rule below testable without a window, which is
//! the only way to prove the degradation order stated on WorkbenchBannerLayout.
struct WorkbenchBannerLayoutInput {
	//! Physical width of the strip.  The banner always spans the whole client.
	int widthPixels = 0;
	unsigned int dpi = 96;
	//! Physical height of one line of the chrome font.
	int textHeightPixels = 0;
	//! Physical width the message would need if it were not clipped.  The message
	//! carries its own leading `$(shield)` icon as a label run, exactly as the
	//! `status.workspaceTrust` status-bar item does, so there is no separate icon
	//! rectangle here: one `$(name)` must not render two different ways depending
	//! on which surface drew it.
	int messageWidthPixels = 0;
	//! Physical width of each action link, in the order they are rendered.
	std::vector<int> actionWidthPixels;
};

//! Where each element of the banner strip is drawn and hit-tested.
//!
//! The banner is always exactly one line high, so `height` depends on the font
//! and the DPI but never on the width.  When the content does not fit, the
//! message is the part that gives way: the actions are laid out from the right
//! edge inward at their full measured width and the message takes whatever
//! remains, possibly down to nothing.  That order is deliberate.  The actions
//! are the security-relevant affordances -- they are how the user reaches the
//! trust decision and how they dismiss the strip -- so an action must never be
//! dropped or shrunk to keep prose visible.  When even the actions do not fit,
//! it is their *position* that gives way, not their width: an action's
//! rectangle keeps its full measured width and simply extends past the strip's
//! left edge, possibly to a negative `left`, rather than being squeezed into
//! whatever space remains.
struct WorkbenchBannerLayout {
	int height = 0;
	//! The message text.  May be zero-width, which means "draw no message".
	WorkbenchRect message;
	//! One rectangle per requested action, in the same order.  Always the same
	//! size as `WorkbenchBannerLayoutInput::actionWidthPixels`, so an index is
	//! always valid and no action is ever omitted.  Each rectangle's `Width()`
	//! always equals its corresponding `actionWidthPixels` entry -- it is never
	//! shrunk -- but its position may fall partially or entirely outside the
	//! strip's visible bounds (including a negative `left`) when there is not
	//! enough room for every action at full width.
	std::vector<WorkbenchRect> actions;

	[[nodiscard]] bool operator==(const WorkbenchBannerLayout&) const = default;
};

//! Places the banner's message and action links.  Pure: no HWND, no DC.
[[nodiscard]] WorkbenchBannerLayout CalculateWorkbenchBannerLayout(const WorkbenchBannerLayoutInput& input);

//! Index of the action containing a client point, or -1 when the point is not on
//! one.  Hit testing lives beside the placement it must agree with; a second
//! copy in the window procedure is how a link starts responding somewhere other
//! than where it was drawn.
[[nodiscard]] int WorkbenchBannerActionAtPoint(const WorkbenchBannerLayout& layout, int x, int y) noexcept;

} // namespace workbench
