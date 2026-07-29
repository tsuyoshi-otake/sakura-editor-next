/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/window/TerminalFontMetrics.h"

namespace terminal {
namespace {

TEST(TerminalFontMetrics, MatchesWindowsTerminalNinePointAppearanceAtSupportedDpis)
{
	const auto dpi96 = CalculateTerminalFontMetrics(9, 96);
	EXPECT_EQ(12, dpi96.fontPixelHeight);
	EXPECT_EQ(7, dpi96.cellWidth);
	EXPECT_EQ(14, dpi96.cellHeight);

	const auto dpi144 = CalculateTerminalFontMetrics(9, 144);
	EXPECT_EQ(18, dpi144.fontPixelHeight);
	EXPECT_EQ(11, dpi144.cellWidth);
	EXPECT_EQ(22, dpi144.cellHeight);

	const auto dpi192 = CalculateTerminalFontMetrics(9, 192);
	EXPECT_EQ(24, dpi192.fontPixelHeight);
	EXPECT_EQ(14, dpi192.cellWidth);
	EXPECT_EQ(29, dpi192.cellHeight);
}

TEST(TerminalFontMetrics, UsesNinetySixDpiFallbackAndNeverReturnsZero)
{
	EXPECT_EQ(CalculateTerminalFontMetrics(9, 96), CalculateTerminalFontMetrics(9, 0));
	const auto minimum = CalculateTerminalFontMetrics(0, 1);
	EXPECT_GE(minimum.fontPixelHeight, 1);
	EXPECT_GE(minimum.cellWidth, 1);
	EXPECT_GE(minimum.cellHeight, 1);
}

} // namespace
} // namespace terminal
