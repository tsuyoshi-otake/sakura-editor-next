/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/window/TerminalScrollbarLayout.h"

namespace terminal {
namespace {

TEST(TerminalScrollbarLayout, PositionsThumbAtTopMiddleAndBottom)
{
	const RECT client{ 0, 0, 800, 400 };
	const auto top = CalculateTerminalScrollbarLayout(client, 1000, 100, 0, 96);
	const auto middle = CalculateTerminalScrollbarLayout(client, 1000, 100, 450, 96);
	const auto bottom = CalculateTerminalScrollbarLayout(client, 1000, 100, 900, 96);
	ASSERT_TRUE(top.scrollable);
	EXPECT_EQ(top.track.top, top.thumb.top);
	EXPECT_EQ(bottom.track.bottom, bottom.thumb.bottom);
	EXPECT_GT(middle.thumb.top, top.thumb.top);
	EXPECT_LT(middle.thumb.bottom, bottom.thumb.bottom);
	EXPECT_EQ(6, top.track.right - top.track.left);
	EXPECT_EQ(10, top.hitTrack.right - top.hitTrack.left);
	EXPECT_GE(top.thumb.bottom - top.thumb.top, 20);
}

TEST(TerminalScrollbarLayout, MapsThumbDragToBoundedLogicalRows)
{
	const auto layout = CalculateTerminalScrollbarLayout(RECT{ 0, 0, 600, 400 }, 1000, 100, 0, 96);
	const int grabOffset = (layout.thumb.bottom - layout.thumb.top) / 2;
	EXPECT_EQ(0U, TerminalScrollbarTopRowFromDrag(layout, layout.track.top + grabOffset, grabOffset));
	EXPECT_EQ(900U, TerminalScrollbarTopRowFromDrag(layout, layout.track.bottom, grabOffset));
	EXPECT_NEAR(450U, TerminalScrollbarTopRowFromDrag(layout,
		(layout.track.top + layout.track.bottom) / 2, grabOffset), 2U);
	EXPECT_EQ(0U, TerminalScrollbarTopRowFromDrag(layout, -1000, grabOffset));
	EXPECT_EQ(900U, TerminalScrollbarTopRowFromDrag(layout, 10000, grabOffset));
}

TEST(TerminalScrollbarLayout, ScalesDimensionsForDpiAndSuppressesUnnecessaryTrack)
{
	const auto scaled = CalculateTerminalScrollbarLayout(RECT{ 0, 0, 1000, 800 }, 1000, 100, 0, 192);
	EXPECT_EQ(12, scaled.track.right - scaled.track.left);
	EXPECT_EQ(20, scaled.hitTrack.right - scaled.hitTrack.left);
	EXPECT_GE(scaled.thumb.bottom - scaled.thumb.top, 40);

	for (const auto layout : {
		CalculateTerminalScrollbarLayout(RECT{ 0, 0, 300, 200 }, 100, 100, 0, 96),
		CalculateTerminalScrollbarLayout(RECT{ 0, 0, 300, 200 }, 99, 100, 0, 96),
		CalculateTerminalScrollbarLayout(RECT{ 0, 0, 300, 200 }, 100, 0, 0, 96) }) {
		EXPECT_FALSE(layout.scrollable);
		EXPECT_FALSE(layout.HitTest(POINT{ 299, 100 }));
	}
}

} // namespace
} // namespace terminal
