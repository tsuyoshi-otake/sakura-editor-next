/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/WorkbenchBannerLayout.h"

namespace workbench {
namespace {

// The header documents the strip as VS Code's `.monaco-banner`, "26px tall at
// 100% scaling". Deriving the floor from that DIP value here -- the same way
// WorkbenchLayoutTest.cpp derives its DPI-scaled expectations -- pins the
// contract instead of copying the implementation's internal constant. 96 and
// 144 are both multiples of 48, so the scaling stays exact integer math.
constexpr int kDocumentedMinimumHeightDip = 26;

TEST(WorkbenchBannerLayout, HeightMeetsTheDocumented26DipFloorAtStandardDpiAndGrowsWhenTextIsTaller)
{
	const auto floorLayout = CalculateWorkbenchBannerLayout({ .widthPixels = 400, .dpi = 96, .textHeightPixels = 0 });
	EXPECT_EQ(kDocumentedMinimumHeightDip, floorLayout.height);

	// A chrome font tall enough that its own content plus padding exceeds the
	// floor must make the strip taller instead of clipping the text: the floor
	// is a minimum, not a fixed size.
	const auto tallLayout = CalculateWorkbenchBannerLayout({ .widthPixels = 400, .dpi = 96, .textHeightPixels = 60 });
	EXPECT_GT(tallLayout.height, kDocumentedMinimumHeightDip);
}

TEST(WorkbenchBannerLayout, HeightFloorScalesProportionallyWithDpi)
{
	for (const unsigned int dpi : { 96U, 144U }) {
		const auto layout = CalculateWorkbenchBannerLayout({ .widthPixels = 400, .dpi = dpi, .textHeightPixels = 0 });
		EXPECT_EQ(kDocumentedMinimumHeightDip * static_cast<int>(dpi) / 96, layout.height);
	}
}

TEST(WorkbenchBannerLayout, MessageWithNoActionsGetsPaddedContentWidthClampedByMessageWidthPixels)
{
	// A short message must not claim the whole strip: it is clamped to its own
	// measured width even though the padded content area is much wider.
	const auto shortMessage = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 400, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 50 });
	EXPECT_EQ(50, shortMessage.message.Width());
	EXPECT_TRUE(shortMessage.actions.empty());

	// A message that would need more than the available content width is
	// clamped down to what the strip can offer, not clipped off-screen.
	const auto longMessage = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 400, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 100000 });
	EXPECT_LT(longMessage.message.Width(), 100000);
	EXPECT_LE(longMessage.message.right, 400);
}

TEST(WorkbenchBannerLayout, ActionsAreLaidOutRightToLeftInInputOrderWithTheLastActionRightmost)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200,
			.actionWidthPixels = { 30, 50, 70 } });

	ASSERT_EQ(3u, layout.actions.size());
	EXPECT_EQ(30, layout.actions[0].Width());
	EXPECT_EQ(50, layout.actions[1].Width());
	EXPECT_EQ(70, layout.actions[2].Width());

	// The last action in the input vector is the rightmost on screen, and each
	// earlier action sits strictly to its left.
	EXPECT_LT(layout.actions[0].right, layout.actions[1].left);
	EXPECT_LT(layout.actions[1].right, layout.actions[2].left);
	EXPECT_LT(layout.actions[2].right, 800);
}

TEST(WorkbenchBannerLayout, MessageDegradesToZeroWidthButActionsKeepFullWidthWhenNothingFits)
{
	// Chosen so the three actions plus their gaps exactly consume the padded
	// content area (8 + 60 + 12 + 60 + 12 + 60 == 220 - 8): there is zero room
	// left for the message, but every action still fits at its full measured
	// width. The message is what gives way; the actions never do.
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 220, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 500,
			.actionWidthPixels = { 60, 60, 60 } });

	ASSERT_EQ(3u, layout.actions.size());
	EXPECT_EQ(60, layout.actions[0].Width());
	EXPECT_EQ(60, layout.actions[1].Width());
	EXPECT_EQ(60, layout.actions[2].Width());
	EXPECT_EQ(0, layout.message.Width());
	EXPECT_LE(layout.message.left, layout.message.right);
}

TEST(WorkbenchBannerLayout, ActionsNeverShrinkBelowTheirMeasuredWidthEvenWhenTheyOverflowTheStrip)
{
	// At dpi=96 padding is 8 and the action gap is 12, so the padded content
	// area is only 100 - 2*8 == 84 pixels wide -- nowhere near enough for three
	// 60px actions plus their gaps (60*3 + 12*2 == 204).  This is well past the
	// "exactly fits" case covered above: the actions themselves must overflow,
	// not merely crowd out the message.
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 100, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 500,
			.actionWidthPixels = { 60, 60, 60 } });

	ASSERT_EQ(3u, layout.actions.size());
	EXPECT_EQ(60, layout.actions[0].Width());
	EXPECT_EQ(60, layout.actions[1].Width());
	EXPECT_EQ(60, layout.actions[2].Width());
	EXPECT_EQ(0, layout.message.Width());
}

TEST(WorkbenchBannerLayout, OverflowingActionsExtendPastTheStripsLeftEdgeInsteadOfShrinking)
{
	// Same overflowing input as
	// ActionsNeverShrinkBelowTheirMeasuredWidthEvenWhenTheyOverflowTheStrip.  A
	// prior implementation floored both edges of each action rectangle at zero,
	// which silently shrank the first action below its measured width instead
	// of letting it run off the left edge; this is the regression guard for
	// that difference.
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 100, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 500,
			.actionWidthPixels = { 60, 60, 60 } });

	ASSERT_FALSE(layout.actions.empty());
	EXPECT_LT(layout.actions.front().left, 0);
	EXPECT_EQ(60, layout.actions.front().Width());
}

TEST(WorkbenchBannerLayout, NarrowerThanDoublePaddingReturnsHeightOnlyWithEmptyMessageAndNonInvertedActions)
{
	// A strip too narrow to even hold its own padding has nowhere to put
	// anything; only the height (the background band) survives.
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 4, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 100,
			.actionWidthPixels = { 40, 40 } });

	EXPECT_GT(layout.height, 0);
	EXPECT_EQ(0, layout.message.Width());
	ASSERT_EQ(2u, layout.actions.size());
	for (const auto& action : layout.actions) {
		EXPECT_LE(action.left, action.right);
		EXPECT_EQ(0, action.Width());
	}
}

TEST(WorkbenchBannerLayout, NoRectangleIsEverInvertedAcrossNarrowWideAndZeroActionLayouts)
{
	const std::vector<WorkbenchBannerLayoutInput> inputs = {
		{ .widthPixels = 0, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200, .actionWidthPixels = { 40 } },
		{ .widthPixels = 4, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200, .actionWidthPixels = { 40, 40 } },
		{ .widthPixels = 100, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 500,
			.actionWidthPixels = { 60, 60, 60 } },
		{ .widthPixels = 1600, .dpi = 144, .textHeightPixels = 16, .messageWidthPixels = 300,
			.actionWidthPixels = { 80, 80 } },
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200, .actionWidthPixels = {} },
	};

	for (const auto& input : inputs) {
		const auto layout = CalculateWorkbenchBannerLayout(input);
		EXPECT_LE(layout.message.left, layout.message.right);
		EXPECT_LE(layout.message.top, layout.message.bottom);
		for (const auto& action : layout.actions) {
			// Non-inverted, not non-negative: an overflowing action keeps its
			// full measured width by extending past the strip's left edge, so
			// `left` may legitimately be negative here while `left <= right`
			// still holds.
			EXPECT_LE(action.left, action.right);
			EXPECT_LE(action.top, action.bottom);
		}
	}
}

TEST(WorkbenchBannerLayout, ActionsAreVerticallyCenteredInsideTheStrip)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200,
			.actionWidthPixels = { 40, 40 } });

	for (const auto& action : layout.actions) {
		const int topMargin = action.top;
		const int bottomMargin = layout.height - action.bottom;
		// Centred within a pixel of rounding, not merely somewhere inside the
		// strip: the top and bottom clearances must be (almost) equal.
		EXPECT_LE(topMargin - bottomMargin, 1);
		EXPECT_LE(bottomMargin - topMargin, 1);
	}
}

TEST(WorkbenchBannerLayout, ActionAtPointReturnsTheActionIndexForAPointInsideIt)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200,
			.actionWidthPixels = { 40, 60 } });

	ASSERT_EQ(2u, layout.actions.size());
	const auto& secondAction = layout.actions[1];
	const int midX = (secondAction.left + secondAction.right) / 2;
	const int midY = (secondAction.top + secondAction.bottom) / 2;
	EXPECT_EQ(1, WorkbenchBannerActionAtPoint(layout, midX, midY));
}

TEST(WorkbenchBannerLayout, ActionAtPointReturnsMinusOneForAPointInTheMessageArea)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200,
			.actionWidthPixels = { 40, 60 } });

	ASSERT_GT(layout.message.Width(), 0);
	const int midX = (layout.message.left + layout.message.right) / 2;
	const int midY = (layout.message.top + layout.message.bottom) / 2;
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, midX, midY));
}

TEST(WorkbenchBannerLayout, ActionAtPointReturnsMinusOneForAPointOutsideTheStrip)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200,
			.actionWidthPixels = { 40, 60 } });

	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, -5, 5));
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, 10, layout.height + 10));
}

TEST(WorkbenchBannerLayout, ActionAtPointReturnsMinusOneForACollapsedZeroAreaAction)
{
	// A strip too narrow to place a real action still produces a rectangle for
	// it (see NarrowerThanDoublePaddingReturnsHeightOnlyWithEmptyMessageAndNonInvertedActions),
	// but that collapsed, zero-area rectangle must never be clickable.
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 4, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 100, .actionWidthPixels = { 40 } });

	ASSERT_EQ(1u, layout.actions.size());
	const auto& action = layout.actions[0];
	EXPECT_EQ(0, action.Width());
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, action.left, action.top));
}

TEST(WorkbenchBannerLayout, ZeroActionsProduceAnEmptyActionsVectorAndActionAtPointAlwaysReturnsMinusOne)
{
	const auto layout = CalculateWorkbenchBannerLayout(
		{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16, .messageWidthPixels = 200 });

	EXPECT_TRUE(layout.actions.empty());
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, 0, 0));
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, 400, layout.height / 2));
	EXPECT_EQ(-1, WorkbenchBannerActionAtPoint(layout, 799, layout.height / 2));
}

TEST(WorkbenchBannerLayout, DpiZeroIsTreatedAsTheDocumented96Fallback)
{
	const WorkbenchBannerLayoutInput inputWithZeroDpi{ .widthPixels = 800, .dpi = 0, .textHeightPixels = 16,
		.messageWidthPixels = 200, .actionWidthPixels = { 40, 60 } };
	const WorkbenchBannerLayoutInput inputWith96Dpi{ .widthPixels = 800, .dpi = 96, .textHeightPixels = 16,
		.messageWidthPixels = 200, .actionWidthPixels = { 40, 60 } };

	EXPECT_EQ(CalculateWorkbenchBannerLayout(inputWith96Dpi), CalculateWorkbenchBannerLayout(inputWithZeroDpi));
}

} // namespace
} // namespace workbench
