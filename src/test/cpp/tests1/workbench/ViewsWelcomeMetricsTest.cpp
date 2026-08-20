/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/ViewsWelcomeMetrics.h"

namespace {

using namespace workbench::views;

TEST(ViewsWelcomeMetrics, ButtonBoxIsUpstreamsFixedTwentySixPixels)
{
	// `.monaco-text-button` in VS Code 1.133.0: line-height 16px, padding 4px
	// top and bottom, a 1px border, and `box-sizing: border-box`. Both
	// font-size and line-height are set by that rule, so the box never grows
	// with the label's font.
	EXPECT_EQ(26, WelcomeButtonHeight(96));
	EXPECT_EQ(4, WelcomeButtonCornerRadius(96));
	EXPECT_EQ(20, WelcomeHorizontalInset(96));
}

TEST(ViewsWelcomeMetrics, ScalesTheButtonBoxWithTheMonitorDpi)
{
	EXPECT_EQ(33, WelcomeButtonHeight(120));  // 125%
	EXPECT_EQ(39, WelcomeButtonHeight(144));  // 150%
	EXPECT_EQ(52, WelcomeButtonHeight(192));  // 200%
	EXPECT_EQ(26, WelcomeButtonHeight(0));    // an unknown DPI means 96
}

TEST(ViewsWelcomeMetrics, CapsOnlyTheActionColumnAtThreeHundred)
{
	// `.welcome-view-content > .button-container` is `width: 100%` with a
	// 300px maximum; paragraphs are not capped and stay full width.
	EXPECT_EQ(200, WelcomeButtonColumnWidth(200, 96));
	EXPECT_EQ(300, WelcomeButtonColumnWidth(300, 96));
	EXPECT_EQ(300, WelcomeButtonColumnWidth(900, 96));
	EXPECT_EQ(450, WelcomeButtonColumnWidth(900, 144));
	EXPECT_EQ(0, WelcomeButtonColumnWidth(-10, 96));
}

} // namespace
