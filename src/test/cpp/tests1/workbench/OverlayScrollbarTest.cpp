/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/controls/COverlayScrollbar.h"

namespace {

using workbench::controls::NormalizeOverlayScrollbarModel;
using workbench::controls::OverlayScrollbarModel;

TEST(OverlayScrollbarModelTest, NormalizesNegativeExtentsAndOffset)
{
	const auto model = NormalizeOverlayScrollbarModel(OverlayScrollbarModel{ -1, -2, -3 });
	EXPECT_EQ(0, model.contentExtent);
	EXPECT_EQ(0, model.viewportExtent);
	EXPECT_EQ(0, model.offset);
}

TEST(OverlayScrollbarModelTest, HidesTheRangeWhenTheViewportCoversTheContent)
{
	const auto model = NormalizeOverlayScrollbarModel(OverlayScrollbarModel{ 80, 120, 45 });
	EXPECT_EQ(80, model.contentExtent);
	EXPECT_EQ(120, model.viewportExtent);
	EXPECT_EQ(0, model.offset);
}

TEST(OverlayScrollbarModelTest, ClampsOffsetToTheMaximumPixelOffset)
{
	const auto model = NormalizeOverlayScrollbarModel(OverlayScrollbarModel{ 500, 120, 900 });
	EXPECT_EQ(380, model.offset);
}

TEST(OverlayScrollbarModelTest, PreservesAnOffsetInsideThePixelRange)
{
	const auto model = NormalizeOverlayScrollbarModel(OverlayScrollbarModel{ 500, 120, 75 });
	EXPECT_EQ(75, model.offset);
}

} // namespace
