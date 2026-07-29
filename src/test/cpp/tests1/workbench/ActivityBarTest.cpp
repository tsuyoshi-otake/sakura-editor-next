/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/activity/ActivityBarModel.h"
#include "workbench/IconMetrics.h"
#include "workbench/scm/GitScmModel.h"

namespace workbench::activity {
namespace {

TEST(ActivityBarModel, Uses42DipWidthAndSquareVerticalButtonsAtDpi)
{
	ActivityBarModel model;
	model.SetViewport(500, 400, 144);

	EXPECT_EQ(63, model.GetPreferredWidthPixels());
	ASSERT_EQ(2U, model.GetButtonCount());
	const auto explorer = model.GetButton(0);
	const auto sourceControl = model.GetButton(1);
	EXPECT_EQ((ActivityBarRect{ 0, 0, 63, 63 }), explorer.bounds);
	EXPECT_EQ((ActivityBarRect{ 0, 63, 63, 126 }), sourceControl.bounds);
	EXPECT_EQ(ActivityBarItem::Explorer, *model.HitTest(62, 62));
	EXPECT_EQ(ActivityBarItem::SourceControl, *model.HitTest(10, 100));
	EXPECT_FALSE(model.HitTest(10, 150).has_value());
	EXPECT_FALSE(model.HitTest(63, 10).has_value());
}

TEST(ActivityBarModel, ExposesIndependentVisualStateForProviders)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetSelectedItem(ActivityBarItem::SourceControl);
	model.SetHoveredItem(ActivityBarItem::Explorer);
	model.SetPressedItem(ActivityBarItem::Explorer);
	model.SetFocusedItem(ActivityBarItem::Explorer);

	const auto explorer = model.GetButton(0);
	const auto sourceControl = model.GetButton(1);
	EXPECT_TRUE(explorer.hovered);
	EXPECT_TRUE(explorer.pressed);
	EXPECT_TRUE(explorer.focused);
	EXPECT_TRUE(sourceControl.selected);
	EXPECT_EQ(ActivityBarItem::Explorer, *model.GetFocusedItem());
}

TEST(ActivityBarModel, FocusNavigationSkipsDisabledItemsAndWraps)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetItemEnabled(ActivityBarItem::SourceControl, false);
	model.SetFocusedItem(ActivityBarItem::Explorer);

	EXPECT_EQ(ActivityBarItem::Explorer, *model.MoveFocus(1));
	EXPECT_EQ(ActivityBarItem::Explorer, *model.MoveFocus(-1));
	EXPECT_FALSE(model.GetButton(1).enabled);
	EXPECT_FALSE(model.HitTest(10, 100).has_value());
}

TEST(ActivityBarModel, InvokeOnlyReturnsEnabledRequestedItemAndDoesNotChangeSelection)
{
	ActivityBarModel model;
	model.SetViewport(42, 200);
	model.SetSelectedItem(ActivityBarItem::Explorer);
	model.SetFocusedItem(ActivityBarItem::SourceControl);

	EXPECT_EQ(ActivityBarItem::SourceControl, *model.InvokeFocused());
	EXPECT_EQ(ActivityBarItem::Explorer, *model.GetSelectedItem());
	model.SetItemEnabled(ActivityBarItem::SourceControl, false);
	EXPECT_FALSE(model.Invoke(ActivityBarItem::SourceControl).has_value());
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

TEST(IconMetrics, UsesSharedOpticalSizesAndDpiStableBounds)
{
	using namespace workbench::icons;
	EXPECT_EQ(20, ScaleDip(kActivityIconDip, 96));
	EXPECT_EQ(30, ScaleDip(kActivityIconDip, 144));
	EXPECT_EQ(16, ScaleDip(kStatusIconDip, 96));
	EXPECT_EQ(24, ScaleDip(kStatusIconDip, 144));
	EXPECT_EQ(1, LineStrokePixels(96));
	EXPECT_EQ(2, LineStrokePixels(192));
	EXPECT_EQ(24, StatusTextInsetPixels(96));
	EXPECT_EQ((IconRect{ 11, 11, 31, 31 }), CenteredIconBounds({ 0, 0, 42, 42 }, kActivityIconDip, 96));
	EXPECT_EQ((IconRect{ 4, 4, 20, 20 }), LeadingStatusIconBounds({ 0, 0, 100, 24 }, 96));
}

TEST(IconMetrics, ScmStatusLeavesTheBranchIconToTheSharedNativeRenderer)
{
	scm::GitScmState state;
	state.repository = true;
	state.branch = L"main";
	const auto status = scm::FormatStatusLine(state);
	EXPECT_EQ(L"main", status);
	EXPECT_EQ(std::wstring::npos, status.find(L'\x2387'));
}

} // namespace
} // namespace workbench::activity
