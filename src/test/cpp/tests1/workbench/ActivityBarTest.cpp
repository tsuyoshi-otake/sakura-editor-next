/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/activity/ActivityBarModel.h"

namespace workbench::activity {
namespace {

TEST(ActivityBarModel, Uses42DipWidthAndSquareVerticalButtonsAtDpi)
{
	ActivityBarModel model;
	model.SetViewport(500, 400, 144);

	EXPECT_EQ(63, model.GetPreferredWidthPixels());
	ASSERT_EQ(3U, model.GetButtonCount());
	const auto explorer = model.GetButton(0);
	const auto outline = model.GetButton(1);
	const auto terminal = model.GetButton(2);
	EXPECT_EQ((ActivityBarRect{ 0, 0, 63, 63 }), explorer.bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 63, 63, 126 }), outline.bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 126, 63, 189 }), terminal.bounds);
	EXPECT_EQ(ActivityBarItem::Explorer, *model.HitTest(62, 62));
	EXPECT_EQ(ActivityBarItem::Outline, *model.HitTest(10, 100));
	EXPECT_FALSE(model.HitTest(63, 10).has_value());
}

TEST(ActivityBarModel, ExposesIndependentVisualStateForProviders)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetSelectedItem(ActivityBarItem::Terminal);
	model.SetHoveredItem(ActivityBarItem::Outline);
	model.SetPressedItem(ActivityBarItem::Outline);
	model.SetFocusedItem(ActivityBarItem::Outline);

	const auto explorer = model.GetButton(0);
	const auto outline = model.GetButton(1);
	const auto terminal = model.GetButton(2);
	EXPECT_FALSE(explorer.selected);
	EXPECT_TRUE(outline.hovered);
	EXPECT_TRUE(outline.pressed);
	EXPECT_TRUE(outline.focused);
	EXPECT_TRUE(terminal.selected);
	EXPECT_EQ(ActivityBarItem::Outline, *model.GetFocusedItem());
}

TEST(ActivityBarModel, FocusNavigationSkipsDisabledItemsAndWraps)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetItemEnabled(ActivityBarItem::Outline, false);
	model.SetFocusedItem(ActivityBarItem::Explorer);

	EXPECT_EQ(ActivityBarItem::Terminal, *model.MoveFocus(1));
	EXPECT_EQ(ActivityBarItem::Explorer, *model.MoveFocus(1));
	EXPECT_EQ(ActivityBarItem::Terminal, *model.MoveFocus(-1));
	EXPECT_FALSE(model.GetButton(1).enabled);
	EXPECT_FALSE(model.HitTest(10, 50).has_value());
}

TEST(ActivityBarModel, InvokeOnlyReturnsEnabledRequestedItemAndDoesNotChangeSelection)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetSelectedItem(ActivityBarItem::Explorer);
	model.SetFocusedItem(ActivityBarItem::Terminal);

	EXPECT_EQ(ActivityBarItem::Terminal, *model.InvokeFocused());
	EXPECT_EQ(ActivityBarItem::Explorer, *model.GetSelectedItem());
	model.SetItemEnabled(ActivityBarItem::Terminal, false);
	EXPECT_FALSE(model.Invoke(ActivityBarItem::Terminal).has_value());
	EXPECT_FALSE(model.InvokeFocused().has_value());
}

TEST(ActivityBarModel, ClampsShortClientsWithoutInvertedButtonBounds)
{
	ActivityBarModel model;
	model.SetViewport(-1, 20, 192);

	EXPECT_EQ(84, model.GetPreferredWidthPixels());
	for (std::size_t index = 0; index < model.GetButtonCount(); ++index) {
		const auto bounds = model.GetButton(index).bounds;
		EXPECT_GE(bounds.left, 0);
		EXPECT_GE(bounds.top, 0);
		EXPECT_GE(bounds.right, bounds.left);
		EXPECT_GE(bounds.bottom, bounds.top);
		EXPECT_LE(bounds.bottom, 20);
	}
}

} // namespace
} // namespace workbench::activity
