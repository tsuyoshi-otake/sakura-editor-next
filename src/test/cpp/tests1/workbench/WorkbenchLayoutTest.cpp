/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/WorkbenchLayout.h"

namespace workbench {
namespace {

TEST(WorkbenchLayout, ScalesStandardChromeAtExplicitDpi)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const auto layout = CalculateWorkbenchLayout({ .clientWidth = 2000, .clientHeight = 1400, .dpi = dpi });
		EXPECT_EQ(34 * static_cast<int>(dpi) / 96, layout.titleBar.Height());
		EXPECT_EQ(32 * static_cast<int>(dpi) / 96, layout.documentTabs.Height());
		EXPECT_EQ(42 * static_cast<int>(dpi) / 96, layout.activityBar.Width());
		EXPECT_EQ(22 * static_cast<int>(dpi) / 96, layout.statusBar.Height());
		EXPECT_EQ(::MulDiv(1, static_cast<int>(dpi), 96), layout.leftSplitter.Width());
		EXPECT_EQ(::MulDiv(1, static_cast<int>(dpi), 96), layout.rightSplitter.Width());
		EXPECT_EQ(::MulDiv(1, static_cast<int>(dpi), 96), layout.bottomSplitter.Height());
		EXPECT_EQ(30 * static_cast<int>(dpi) / 96, layout.leftPaneHeader.Height());
		EXPECT_EQ(layout.titleBar.bottom, layout.documentTabs.top);
		EXPECT_EQ(layout.titleBar.bottom, layout.activityBar.top);
		EXPECT_EQ(layout.leftSplitter.right, layout.documentTabs.left);
		EXPECT_EQ(layout.rightSplitter.left, layout.documentTabs.right);
		EXPECT_EQ(layout.documentTabs.bottom, layout.editor.top);
	}
}

TEST(WorkbenchLayout, HiddenPanesConsumeNoSpaceAndDragResizingRemainsLaidOut)
{
	auto request = WorkbenchLayoutRequest{ .clientWidth = 1600, .clientHeight = 1000 };
	request.leftPane = WorkbenchPaneState::Hidden;
	request.rightPane = WorkbenchPaneState::DragResizing;
	request.bottomPane = WorkbenchPaneState::Hidden;
	const auto layout = CalculateWorkbenchLayout(request);

	EXPECT_EQ(0, layout.leftPane.Width());
	EXPECT_EQ(0, layout.leftSplitter.Width());
	EXPECT_GT(layout.rightPane.Width(), 0);
	EXPECT_GT(layout.rightSplitter.Width(), 0);
	EXPECT_EQ(0, layout.bottomPane.Height());
	EXPECT_EQ(0, layout.bottomSplitter.Height());
}

TEST(WorkbenchLayout, UsesMeasuredLegacyChromeWithoutDoubleReservingBodySpace)
{
	WorkbenchLayoutRequest request{ .clientWidth = 1200, .clientHeight = 900, .dpi = 192 };
	request.titleBarHeightPixels = 68;
	request.topAccessoryHeightPixels = 51;
	request.documentTabsHeightPixels = 37;
	request.bottomAccessoryHeightPixels = 29;
	request.statusBarHeightPixels = 25;
	const auto layout = CalculateWorkbenchLayout(request);

	EXPECT_EQ(68, layout.titleBar.Height());
	EXPECT_EQ(51, layout.topAccessory.Height());
	EXPECT_EQ(37, layout.documentTabs.Height());
	EXPECT_EQ(29, layout.bottomAccessory.Height());
	EXPECT_EQ(25, layout.statusBar.Height());
	EXPECT_EQ(layout.topAccessory.bottom, layout.activityBar.top);
	EXPECT_EQ(layout.documentTabs.bottom, layout.editor.top);
	EXPECT_EQ(layout.activityBar.bottom, layout.bottomAccessory.top);
	EXPECT_EQ(layout.bottomAccessory.bottom, layout.statusBar.top);
	EXPECT_EQ(900, layout.statusBar.bottom);
}

TEST(WorkbenchLayout, TabsOccupyOnlyTheEditorColumnWhileSidebarsStartAboveThem)
{
	const auto layout = CalculateWorkbenchLayout({ .clientWidth = 1600, .clientHeight = 1000 });

	EXPECT_EQ(layout.leftSplitter.right, layout.documentTabs.left);
	EXPECT_EQ(layout.rightSplitter.left, layout.documentTabs.right);
	EXPECT_EQ(layout.documentTabs.top, layout.leftPane.top);
	EXPECT_EQ(layout.documentTabs.top, layout.activityBar.top);
	EXPECT_EQ(layout.documentTabs.bottom, layout.editor.top);
	EXPECT_LT(layout.leftPane.top, layout.editor.top);
}

TEST(WorkbenchLayout, BottomPaneSpansOnlyCentralEditorColumn)
{
	const auto layout = CalculateWorkbenchLayout({ .clientWidth = 1600, .clientHeight = 1000 });
	EXPECT_EQ(layout.editor.left, layout.bottomPane.left);
	EXPECT_EQ(layout.minimap.right, layout.bottomPane.right);
	EXPECT_GT(layout.leftPane.Width(), 0);
	EXPECT_GT(layout.rightPane.Width(), 0);
}

TEST(WorkbenchLayout, MaximizedBottomPaneReplacesEditorWithoutHidingDocumentTabs)
{
	auto request = WorkbenchLayoutRequest{ .clientWidth = 1600, .clientHeight = 1000 };
	request.bottomPaneMaximized = true;
	const auto layout = CalculateWorkbenchLayout(request);

	EXPECT_GT(layout.documentTabs.Height(), 0);
	EXPECT_EQ(0, layout.editor.Height());
	EXPECT_EQ(0, layout.minimap.Height());
	EXPECT_EQ(0, layout.bottomSplitter.Height());
	EXPECT_EQ(layout.documentTabs.bottom, layout.bottomPane.top);
	EXPECT_EQ(layout.editor.left, layout.bottomPane.left);
	EXPECT_EQ(layout.rightSplitter.left, layout.bottomPane.right);
	EXPECT_GT(layout.bottomPane.Height(), 220);
}

TEST(WorkbenchLayout, MinimapIsInsideEditorAtTheRightEdge)
{
	auto request = WorkbenchLayoutRequest{ .clientWidth = 1800, .clientHeight = 1000 };
	request.showMinimap = true;
	const auto layout = CalculateWorkbenchLayout(request);

	EXPECT_GT(layout.minimap.Width(), 0);
	EXPECT_EQ(layout.editor.right, layout.minimap.left);
	EXPECT_EQ(layout.minimap.right, layout.rightSplitter.left);
	EXPECT_EQ(layout.editor.top, layout.minimap.top);
	EXPECT_EQ(layout.editor.bottom, layout.minimap.bottom);
}

TEST(WorkbenchLayout, UndersizedClientsRemainClampedAndNonNegative)
{
	const auto layout = CalculateWorkbenchLayout({ .clientWidth = 8, .clientHeight = 7, .dpi = 192, .showMinimap = true });
	for (const auto& rect : { layout.titleBar, layout.topAccessory, layout.activityBar, layout.primaryActivityBar,
		layout.secondaryActivityBar, layout.documentTabs, layout.leftPane,
		layout.leftPaneHeader, layout.leftSplitter, layout.editor, layout.minimap, layout.rightSplitter,
		layout.rightPane, layout.rightPaneHeader, layout.bottomSplitter, layout.bottomPane, layout.bottomPaneHeader,
		layout.bottomAccessory, layout.statusBar }) {
		EXPECT_GE(rect.left, 0);
		EXPECT_GE(rect.top, 0);
		EXPECT_GE(rect.right, rect.left);
		EXPECT_GE(rect.bottom, rect.top);
	}
}

TEST(WorkbenchLayout, RepeatedCalculationIsIdempotent)
{
	const WorkbenchLayoutRequest request{ .clientWidth = 1234, .clientHeight = 789, .dpi = 144,
		.leftPane = WorkbenchPaneState::DragResizing, .showMinimap = true };
	EXPECT_EQ(CalculateWorkbenchLayout(request), CalculateWorkbenchLayout(request));
}

TEST(WorkbenchLayout, DefaultActivityBarRemainsVerticalAndExposesPrimaryAlias)
{
	const auto layout = CalculateWorkbenchLayout({ .clientWidth = 1600, .clientHeight = 1000 });

	EXPECT_GT(layout.activityBar.Width(), 0);
	EXPECT_GT(layout.activityBar.Height(), 0);
	EXPECT_EQ(layout.activityBar, layout.primaryActivityBar);
	EXPECT_EQ(WorkbenchRect{}, layout.secondaryActivityBar);
	EXPECT_EQ(layout.activityBar.top, layout.leftPane.top);
	EXPECT_EQ(layout.activityBar.bottom, layout.leftPane.bottom);
}

TEST(WorkbenchLayout, TopActivityBarsReserveBandsInBothVisibleSidebars)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		WorkbenchLayoutRequest request{ .clientWidth = 1600, .clientHeight = 1000, .dpi = dpi };
		request.activityBarLocation = ActivityBarLocation::Top;
		const auto layout = CalculateWorkbenchLayout(request);
		const int expectedBarHeight = ::MulDiv(35, static_cast<int>(dpi), 96);

		EXPECT_EQ(WorkbenchRect{}, layout.activityBar);
		EXPECT_GT(layout.primaryActivityBar.Width(), 0);
		EXPECT_GT(layout.secondaryActivityBar.Width(), 0);
		EXPECT_EQ(expectedBarHeight, layout.primaryActivityBar.Height());
		EXPECT_EQ(expectedBarHeight, layout.secondaryActivityBar.Height());
		EXPECT_EQ(layout.primaryActivityBar.top, layout.leftPane.top - expectedBarHeight);
		EXPECT_EQ(layout.secondaryActivityBar.top, layout.rightPane.top - expectedBarHeight);
		EXPECT_EQ(layout.primaryActivityBar.bottom, layout.leftPane.top);
		EXPECT_EQ(layout.secondaryActivityBar.bottom, layout.rightPane.top);
		EXPECT_GT(layout.leftPane.Height(), 0);
		EXPECT_GT(layout.rightPane.Height(), 0);
		EXPECT_EQ(layout.primaryActivityBar.left, layout.leftPane.left);
		EXPECT_EQ(layout.primaryActivityBar.right, layout.leftPane.right);
		EXPECT_EQ(layout.secondaryActivityBar.left, layout.rightPane.left);
		EXPECT_EQ(layout.secondaryActivityBar.right, layout.rightPane.right);
		EXPECT_EQ(layout.primaryActivityBar.Height() + layout.leftPane.Height(),
			layout.bottomAccessory.top - layout.topAccessory.bottom);
		EXPECT_EQ(layout.secondaryActivityBar.Height() + layout.rightPane.Height(),
			layout.bottomAccessory.top - layout.topAccessory.bottom);
	}
}

TEST(WorkbenchLayout, BottomActivityBarReservesOnlyVisibleSidebars)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		WorkbenchLayoutRequest request{ .clientWidth = 1600, .clientHeight = 1000, .dpi = dpi };
		request.activityBarLocation = ActivityBarLocation::Bottom;
		request.rightPane = WorkbenchPaneState::Hidden;
		const auto layout = CalculateWorkbenchLayout(request);
		const int expectedBarHeight = ::MulDiv(35, static_cast<int>(dpi), 96);

		EXPECT_EQ(WorkbenchRect{}, layout.activityBar);
		EXPECT_GT(layout.primaryActivityBar.Width(), 0);
		EXPECT_EQ(WorkbenchRect{}, layout.secondaryActivityBar);
		EXPECT_EQ(expectedBarHeight, layout.primaryActivityBar.Height());
		EXPECT_EQ(layout.primaryActivityBar.bottom, layout.leftPane.bottom + expectedBarHeight);
		EXPECT_EQ(layout.primaryActivityBar.top, layout.leftPane.bottom);
		EXPECT_EQ(layout.primaryActivityBar.left, layout.leftPane.left);
		EXPECT_EQ(layout.primaryActivityBar.right, layout.leftPane.right);
		EXPECT_EQ(0, layout.rightPane.Width());
		EXPECT_EQ(layout.leftPane.Height() + layout.primaryActivityBar.Height(),
			layout.bottomAccessory.top - layout.topAccessory.bottom);
	}
}

TEST(WorkbenchLayout, UnsupportedActivityBarLocationFallsBackToVerticalDefault)
{
	WorkbenchLayoutRequest request{ .clientWidth = 1000, .clientHeight = 700 };
	request.activityBarLocation = static_cast<ActivityBarLocation>(255);
	const auto fallback = CalculateWorkbenchLayout(request);
	request.activityBarLocation = ActivityBarLocation::Default;
	EXPECT_EQ(fallback, CalculateWorkbenchLayout(request));
}

} // namespace
} // namespace workbench
