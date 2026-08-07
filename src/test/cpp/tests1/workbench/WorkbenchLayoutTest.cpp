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
	for (const auto& rect : { layout.titleBar, layout.topAccessory, layout.activityBar, layout.documentTabs, layout.leftPane,
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

TEST(WorkbenchLayout, ZeroBannerLeavesEveryOtherRectExactlyAsToday)
{
	// The explicit-zero request and the request that never touches the field
	// both default to bannerHeightPixels = 0, so their layouts must be
	// byte-identical: an existing caller that never sets the field must
	// observe no geometry change at all.
	const WorkbenchLayoutRequest withExplicitZero{ .clientWidth = 1600, .clientHeight = 1000, .dpi = 144,
		.topAccessoryHeightPixels = 51, .bannerHeightPixels = 0,
		.rightPane = WorkbenchPaneState::DragResizing, .showMinimap = true };
	const WorkbenchLayoutRequest withFieldUntouched{ .clientWidth = 1600, .clientHeight = 1000, .dpi = 144,
		.topAccessoryHeightPixels = 51,
		.rightPane = WorkbenchPaneState::DragResizing, .showMinimap = true };

	const auto layoutExplicitZero = CalculateWorkbenchLayout(withExplicitZero);
	const auto layoutFieldUntouched = CalculateWorkbenchLayout(withFieldUntouched);
	EXPECT_EQ(layoutExplicitZero, layoutFieldUntouched);

	// A zero-height banner occupies no space and topAccessory starts exactly
	// where it always did: directly below the title bar.
	EXPECT_EQ(0, layoutExplicitZero.banner.Height());
	EXPECT_EQ(layoutExplicitZero.titleBar.bottom, layoutExplicitZero.banner.top);
	EXPECT_EQ(layoutExplicitZero.titleBar.bottom, layoutExplicitZero.topAccessory.top);
}

TEST(WorkbenchLayout, BannerSitsBelowTitleBarAndPushesLowerBandsDown)
{
	const WorkbenchLayoutRequest baseline{ .clientWidth = 1600, .clientHeight = 1000 };
	WorkbenchLayoutRequest withBanner = baseline;
	withBanner.bannerHeightPixels = 40;

	const auto baselineLayout = CalculateWorkbenchLayout(baseline);
	const auto bannerLayout = CalculateWorkbenchLayout(withBanner);

	// The banner sits directly under the title bar and spans the full width.
	EXPECT_EQ(bannerLayout.titleBar.bottom, bannerLayout.banner.top);
	EXPECT_EQ(0, bannerLayout.banner.left);
	EXPECT_EQ(1600, bannerLayout.banner.right);
	EXPECT_EQ(40, bannerLayout.banner.Height());
	EXPECT_EQ(bannerLayout.banner.bottom, bannerLayout.topAccessory.top);

	// Everything that used to start at titleHeight + topAccessoryHeight now
	// starts exactly 40 pixels lower; nothing anchored to the bottom moves.
	EXPECT_EQ(baselineLayout.topAccessory.top + 40, bannerLayout.topAccessory.top);
	EXPECT_EQ(baselineLayout.documentTabs.top + 40, bannerLayout.documentTabs.top);
	EXPECT_EQ(baselineLayout.activityBar.top + 40, bannerLayout.activityBar.top);
	EXPECT_EQ(baselineLayout.leftPane.top + 40, bannerLayout.leftPane.top);
	EXPECT_EQ(baselineLayout.rightPane.top + 40, bannerLayout.rightPane.top);
	EXPECT_EQ(baselineLayout.editor.top + 40, bannerLayout.editor.top);
	EXPECT_EQ(baselineLayout.statusBar.top, bannerLayout.statusBar.top);
	EXPECT_EQ(baselineLayout.statusBar.bottom, bannerLayout.statusBar.bottom);
}

TEST(WorkbenchLayout, BannerTallerThanClientClampsWithoutInvertingAnyEdgeAndStatusStaysOnScreen)
{
	const auto layout = CalculateWorkbenchLayout(
		{ .clientWidth = 400, .clientHeight = 50, .titleBarHeightPixels = 34, .bannerHeightPixels = 5000 });

	for (const auto& rect : { layout.titleBar, layout.banner, layout.topAccessory, layout.activityBar,
		layout.documentTabs, layout.leftPane, layout.leftPaneHeader, layout.leftSplitter, layout.editor,
		layout.minimap, layout.rightSplitter, layout.rightPane, layout.rightPaneHeader, layout.bottomSplitter,
		layout.bottomPane, layout.bottomPaneHeader, layout.bottomAccessory, layout.statusBar }) {
		EXPECT_GE(rect.left, 0);
		EXPECT_GE(rect.top, 0);
		EXPECT_GE(rect.right, rect.left);
		EXPECT_GE(rect.bottom, rect.top);
	}

	EXPECT_LE(layout.banner.bottom, 50);
	EXPECT_EQ(50, layout.statusBar.bottom);
	EXPECT_LE(layout.statusBar.top, layout.statusBar.bottom);
}

TEST(WorkbenchLayout, BannerHeightPixelsIsPhysicalAndIsNotRescaledByDpi)
{
	// bannerHeightPixels behaves exactly like topAccessoryHeightPixels: it is
	// already a physical pixel count supplied by the native control that
	// measured its own text, so DPI must not scale it a second time.
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const auto layout = CalculateWorkbenchLayout(
			{ .clientWidth = 1600, .clientHeight = 1000, .dpi = dpi, .bannerHeightPixels = 40 });
		EXPECT_EQ(40, layout.banner.Height());
	}
}

} // namespace
} // namespace workbench
