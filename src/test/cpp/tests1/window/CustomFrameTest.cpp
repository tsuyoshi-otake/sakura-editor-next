/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include "window/CCustomFrameController.h"
#include "window/CClientMenuBar.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"

TEST(CustomFrame, MenuLabelsHideMnemonicSuffixWithoutChangingMenuModel)
{
	EXPECT_EQ(L"ファイル", FormatClientMenuDisplayText(L"ファイル(&F)"));
	EXPECT_EQ(L"File", FormatClientMenuDisplayText(L"&File"));
	EXPECT_EQ(L"Research & Development", FormatClientMenuDisplayText(L"Research && Development"));
}

TEST(CustomFrame, ClientMenuHitTestingUsesHalfOpenItemBounds)
{
	const std::vector<RECT> items{
		{ 10, 0, 30, 34 },
		{ 30, 0, 60, 34 },
	};
	EXPECT_EQ(0, HitTestClientMenuItemBounds(items, { 10, 0 }));
	EXPECT_EQ(0, HitTestClientMenuItemBounds(items, { 29, 33 }));
	EXPECT_EQ(1, HitTestClientMenuItemBounds(items, { 30, 0 }));
	EXPECT_EQ(-1, HitTestClientMenuItemBounds(items, { 60, 0 }));
	EXPECT_EQ(-1, HitTestClientMenuItemBounds(items, { 20, 34 }));
}

TEST(CustomFrame, PopupHotTrackingOnlyRequestsSiblingMenuSwitches)
{
	EXPECT_EQ(-1, NextClientMenuPopupItem(1, -1));
	EXPECT_EQ(-1, NextClientMenuPopupItem(1, 1));
	EXPECT_EQ(0, NextClientMenuPopupItem(1, 0));
	EXPECT_EQ(2, NextClientMenuPopupItem(1, 2));
}

TEST(CustomFrame, PopupHotTrackingUsesTheWhMsgFilterCodeContract)
{
	EXPECT_TRUE(IsClientMenuMouseMoveFilter(MSGF_MENU, WM_MOUSEMOVE));
	EXPECT_FALSE(IsClientMenuMouseMoveFilter(MSGF_DIALOGBOX, WM_MOUSEMOVE));
	EXPECT_FALSE(IsClientMenuMouseMoveFilter(MSGF_MENU, WM_LBUTTONDOWN));
}

TEST(CustomFrame, ScalesFixedTitleMetricsPerDpi)
{
	EXPECT_EQ(34, ScaleCustomFrameDip(34, 96));
	EXPECT_EQ(43, ScaleCustomFrameDip(34, 120));
	EXPECT_EQ(51, ScaleCustomFrameDip(34, 144));
	EXPECT_EQ(68, ScaleCustomFrameDip(34, 192));
	const auto layout = CalculateCustomFrameLayout(1200, 144, 430);
	EXPECT_EQ(51, layout.title.bottom);
	EXPECT_EQ(45, layout.layoutButton.right - layout.layoutButton.left);
	EXPECT_EQ(layout.minimizeButton.left, layout.manageButton.right);
}

TEST(CustomFrame, CentersCaptionInAWindowSymmetricSafeRectangleAcrossDpi)
{
	for (const UINT dpi : { 96u, 120u, 144u, 192u }) {
		constexpr int clientWidth = 1601;
		const auto layout = CalculateCustomFrameLayout(clientWidth, dpi, 430);
		EXPECT_EQ(clientWidth, layout.captionText.left + layout.captionText.right);
		EXPECT_LE(layout.menu.right, layout.captionText.left);
		EXPECT_LE(layout.captionText.right, layout.layoutButton.left);
		EXPECT_EQ(ScaleCustomFrameDip(30, dpi), layout.layoutButton.right - layout.layoutButton.left);
		EXPECT_EQ(ScaleCustomFrameDip(34, dpi), layout.layoutButton.bottom - layout.layoutButton.top);
	}
}

TEST(CustomFrame, RetainsAnEllipsizedCaptionSafeFallbackWhenTheCenteredRegionDoesNotFit)
{
	const auto layout = CalculateCustomFrameLayout(500, 96, 430);
	EXPECT_LT(layout.captionText.left, layout.captionText.right);
	EXPECT_GE(layout.captionText.left, layout.menu.right);
	EXPECT_LE(layout.captionText.right, layout.layoutButton.left);
	EXPECT_NE(500, layout.captionText.left + layout.captionText.right);

	const auto oddNarrowLayout = CalculateCustomFrameLayout(351, 96, 300);
	EXPECT_EQ(351, oddNarrowLayout.captionText.left + oddNarrowLayout.captionText.right);
	EXPECT_LE(oddNarrowLayout.captionText.right, oddNarrowLayout.minimizeButton.left);
}

TEST(CustomFrame, UsesSixteenDipGlyphsInsideThirtyByThirtyFourDipTitleTargets)
{
	for (const UINT dpi : { 96u, 120u, 144u, 192u }) {
		const auto layout = CalculateCustomFrameLayout(1601, dpi, 430);
		const auto glyph = workbench::icons::CenteredIconBounds(
			{ layout.layoutButton.left, layout.layoutButton.top, layout.layoutButton.right, layout.layoutButton.bottom },
			workbench::icons::kStatusIconDip, dpi);
		EXPECT_EQ(ScaleCustomFrameDip(16, dpi), glyph.Width());
		EXPECT_EQ(ScaleCustomFrameDip(16, dpi), glyph.Height());
	}
}

TEST(CustomFrame, LetterboxesSvgCodiconsInsteadOfStretchingNonSquareBoxes)
{
	using workbench::icons::codicons::detail::SvgIconLetterboxBounds;
	const auto wide = SvgIconLetterboxBounds({ 10, 20, 50, 40 });
	EXPECT_EQ(20, wide.left);
	EXPECT_EQ(20, wide.top);
	EXPECT_EQ(40, wide.right);
	EXPECT_EQ(40, wide.bottom);

	const auto tall = SvgIconLetterboxBounds({ 10, 20, 30, 60 });
	EXPECT_EQ(10, tall.left);
	EXPECT_EQ(30, tall.top);
	EXPECT_EQ(30, tall.right);
	EXPECT_EQ(50, tall.bottom);

	const auto empty = SvgIconLetterboxBounds({ 10, 20, 10, 40 });
	EXPECT_EQ(10, empty.left);
	EXPECT_EQ(30, empty.top);
	EXPECT_EQ(10, empty.right);
	EXPECT_EQ(30, empty.bottom);
}

TEST(CustomFrame, CompactsStatusItemsToAnEightDipAdjacentGapWithoutNativeChrome)
{
	using namespace workbench::icons;
	for (const UINT dpi : { 96u, 120u, 144u, 192u }) {
		EXPECT_EQ(ScaleDip(kStatusItemAdjacentGapDip, dpi), StatusItemHorizontalPaddingPixels(dpi));
		EXPECT_EQ(StatusItemHorizontalPaddingPixels(dpi), StatusItemPartWidthPaddingPixels(dpi));
		EXPECT_EQ(StatusItemPartWidthPaddingPixels(dpi), StatusItemPartWidthPixels(0, dpi));
		EXPECT_EQ(25 + StatusItemPartWidthPaddingPixels(dpi), StatusItemPartWidthPixels(25, dpi));
		EXPECT_EQ(0, ScaleDip(kNativeStatusPartChromeDip, dpi));
	}
}

TEST(CustomFrame, UsesDpiScaledIconSizesAndHighlightTextForCloseButton)
{
	EXPECT_EQ(20, CalculateCustomTitleBarIconSize(34, 96));
	EXPECT_EQ(40, CalculateCustomTitleBarIconSize(68, 192));
	auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	palette.highlightText = { 1, 2, 3 };
	EXPECT_EQ(palette.highlightText, CustomTitleBarGlyphColor(palette, true, HTCLOSE, HTCLOSE, HTNOWHERE));
	EXPECT_EQ(palette.highlightText, CustomTitleBarGlyphColor(palette, true, HTCLOSE, HTNOWHERE, HTCLOSE));
	EXPECT_EQ(palette.primaryText, CustomTitleBarGlyphColor(palette, true, HTCLOSE, HTNOWHERE, HTNOWHERE));
}

TEST(CustomFrame, CaptionButtonsRemainAtRightEdgeWithoutOverlap)
{
	const auto layout = CalculateCustomFrameLayout(1000, 96, 500);
	EXPECT_EQ(1000, layout.closeButton.right);
	EXPECT_EQ(layout.closeButton.left, layout.maximizeButton.right);
	EXPECT_EQ(layout.maximizeButton.left, layout.minimizeButton.right);
	EXPECT_LE(layout.menu.right, layout.captionText.left);
	EXPECT_LE(layout.captionText.right, layout.minimizeButton.left);
}

TEST(CustomFrame, PlacesAllCompactTitleControlsImmediatelyBeforeNativeCaptionButtons)
{
	const auto layout = CalculateCustomFrameLayout(1200, 96, 430);
	EXPECT_FALSE(::IsRectEmpty(&layout.layoutButton));
	EXPECT_EQ(layout.minimizeButton.left, layout.manageButton.right);
	EXPECT_EQ(layout.manageButton.left, layout.accountButton.right);
	EXPECT_EQ(layout.accountButton.left, layout.secondarySidebarButton.right);
	EXPECT_EQ(layout.secondarySidebarButton.left, layout.bottomPanelButton.right);
	EXPECT_EQ(layout.bottomPanelButton.left, layout.primarySidebarButton.right);
	EXPECT_EQ(layout.primarySidebarButton.left, layout.layoutButton.right);
	EXPECT_LE(layout.menu.right, layout.captionText.left);
	EXPECT_LE(layout.captionText.right, layout.layoutButton.left);
}

TEST(CustomFrame, CollapsesTitleControlsTogetherOnNarrowWidthsWithoutCreatingCaptionOverlap)
{
	const auto layout = CalculateCustomFrameLayout(350, 96, 300);
	EXPECT_TRUE(::IsRectEmpty(&layout.layoutButton));
	EXPECT_TRUE(::IsRectEmpty(&layout.manageButton));
	const POINT captionPoint{
		(layout.captionText.left + layout.captionText.right) / 2,
		(layout.captionText.top + layout.captionText.bottom) / 2,
	};
	EXPECT_EQ(HTCAPTION, HitTestCustomFrame(layout, captionPoint, 350, 700, 8, false));
}

TEST(CustomFrame, CompactTitleControlHitTestingUsesHalfOpenBoundsAndClientHits)
{
	const auto layout = CalculateCustomFrameLayout(1200, 96, 300);
	const auto center = [](const RECT& rect) {
		return POINT{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
	};
	EXPECT_EQ(CustomFrameControl::Layout, HitTestCustomFrameControl(layout, center(layout.layoutButton)));
	EXPECT_EQ(CustomFrameControl::PrimarySidebar, HitTestCustomFrameControl(layout, center(layout.primarySidebarButton)));
	EXPECT_EQ(CustomFrameControl::BottomPanel, HitTestCustomFrameControl(layout, center(layout.bottomPanelButton)));
	EXPECT_EQ(CustomFrameControl::SecondarySidebar, HitTestCustomFrameControl(layout, center(layout.secondarySidebarButton)));
	EXPECT_EQ(CustomFrameControl::Account, HitTestCustomFrameControl(layout, center(layout.accountButton)));
	EXPECT_EQ(CustomFrameControl::Manage, HitTestCustomFrameControl(layout, center(layout.manageButton)));
	EXPECT_EQ(CustomFrameControl::None, HitTestCustomFrameControl(layout, { layout.manageButton.right, 16 }));
	EXPECT_EQ(HTCLIENT, HitTestCustomFrame(layout, center(layout.manageButton), 1200, 700, 8, false));
}

TEST(CustomFrame, CompactTitleControlInvokeMappingsUseExistingEditorCommands)
{
	EXPECT_EQ(static_cast<UINT>(F_TOGGLE_LEFT_EXPLORER), CustomFrameControlCommand(CustomFrameControl::PrimarySidebar));
	EXPECT_EQ(static_cast<UINT>(F_TOGGLE_BOTTOM_PANEL), CustomFrameControlCommand(CustomFrameControl::BottomPanel));
	EXPECT_EQ(static_cast<UINT>(F_TOGGLE_SECONDARY_SIDEBAR), CustomFrameControlCommand(CustomFrameControl::SecondarySidebar));
	EXPECT_EQ(0u, CustomFrameControlCommand(CustomFrameControl::Manage));
	EXPECT_EQ(0u, CustomFrameControlCommand(CustomFrameControl::Layout));
	EXPECT_EQ(0u, CustomFrameControlCommand(CustomFrameControl::Account));
}

TEST(CustomFrame, CompactTitleControlsExposeAccessibleButtonsWithInvokeMetadata)
{
	const auto layout = CalculateCustomFrameLayout(1200, 96, 300);
	const auto manage = CustomFrameControlAccessibilityNode(CustomFrameControl::Manage, layout, true);
	EXPECT_EQ(L"Manage", manage.name);
	EXPECT_EQ(L"Sakura.TitleBar.Manage", manage.automationId);
	EXPECT_EQ(UIA_ButtonControlTypeId, manage.controlType);
	EXPECT_EQ(layout.manageButton.left, manage.bounds.left);
	EXPECT_EQ(layout.manageButton.right, manage.bounds.right);
	EXPECT_TRUE(manage.enabled);
	EXPECT_TRUE(manage.focused);
	EXPECT_TRUE(manage.invoke);

	const auto account = CustomFrameControlAccessibilityNode(CustomFrameControl::Account, layout, false);
	EXPECT_EQ(L"Account", account.name);
	EXPECT_EQ(L"Sakura.TitleBar.Account", account.automationId);
	EXPECT_TRUE(account.enabled);
	EXPECT_TRUE(account.invoke);
}

TEST(CustomFrame, ReturnsSnapCompatibleCaptionButtonHits)
{
	const auto layout = CalculateCustomFrameLayout(1000, 96, 400);
	const auto center = [](const RECT& rect) {
		return POINT{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
	};
	EXPECT_EQ(HTMINBUTTON, HitTestCustomFrame(layout, center(layout.minimizeButton), 1000, 700, 8, false));
	EXPECT_EQ(HTMAXBUTTON, HitTestCustomFrame(layout, center(layout.maximizeButton), 1000, 700, 8, false));
	EXPECT_EQ(HTCLOSE, HitTestCustomFrame(layout, center(layout.closeButton), 1000, 700, 8, false));
	EXPECT_EQ(HTSYSMENU, HitTestCustomFrame(layout, center(layout.systemMenu), 1000, 700, 8, false));
}

TEST(CustomFrame, PreservesCornerAndEdgeResizeHits)
{
	const auto layout = CalculateCustomFrameLayout(1000, 96, 300);
	EXPECT_EQ(HTTOPLEFT, HitTestCustomFrame(layout, { 1, 1 }, 1000, 700, 8, false));
	EXPECT_EQ(HTTOPRIGHT, HitTestCustomFrame(layout, { 998, 1 }, 1000, 700, 8, false));
	EXPECT_EQ(HTBOTTOMLEFT, HitTestCustomFrame(layout, { 1, 698 }, 1000, 700, 8, false));
	EXPECT_EQ(HTBOTTOMRIGHT, HitTestCustomFrame(layout, { 998, 698 }, 1000, 700, 8, false));
	EXPECT_EQ(HTRIGHT, HitTestCustomFrame(layout, { 998, 300 }, 1000, 700, 8, false));
	EXPECT_EQ(HTLEFT, HitTestCustomFrame(layout, { -1, 300 }, 1000, 700, 8, false));
	EXPECT_EQ(HTRIGHT, HitTestCustomFrame(layout, { 1001, 300 }, 1000, 700, 8, false));
	EXPECT_EQ(HTTOP, HitTestCustomFrame(layout, { 500, -1 }, 1000, 700, 8, false));
	EXPECT_EQ(HTBOTTOM, HitTestCustomFrame(layout, { 500, 701 }, 1000, 700, 8, false));
	EXPECT_EQ(HTTOPLEFT, HitTestCustomFrame(layout, { -1, -1 }, 1000, 700, 8, false));
	EXPECT_EQ(HTBOTTOMRIGHT, HitTestCustomFrame(layout, { 1001, 701 }, 1000, 700, 8, false));
}

TEST(CustomFrame, MaximizedWindowDisablesResizeBorderButKeepsSnapButton)
{
	const auto layout = CalculateCustomFrameLayout(1000, 96, 300);
	const POINT captionPoint{ layout.captionText.left + 4, layout.captionText.top + 16 };
	EXPECT_EQ(HTCAPTION, HitTestCustomFrame(layout, captionPoint, 1000, 700, 8, true));
	const POINT maximizeCenter{
		(layout.maximizeButton.left + layout.maximizeButton.right) / 2,
		(layout.maximizeButton.top + layout.maximizeButton.bottom) / 2,
	};
	EXPECT_EQ(HTMAXBUTTON, HitTestCustomFrame(layout, maximizeCenter, 1000, 700, 8, true));
}

TEST(CustomFrame, MenuIsClientOwnedAndRemainingTitleDrags)
{
	const auto layout = CalculateCustomFrameLayout(1000, 96, 360);
	const POINT menuPoint{ layout.menu.left + 4, layout.menu.top + 16 };
	const POINT captionPoint{ layout.captionText.left + 4, layout.captionText.top + 16 };
	EXPECT_EQ(HTCLIENT, HitTestCustomFrame(layout, menuPoint, 1000, 700, 8, false));
	EXPECT_EQ(HTCAPTION, HitTestCustomFrame(layout, captionPoint, 1000, 700, 8, false));
}

TEST(CustomFrame, PrefersProcessedDwmTargetsButKeepsExtendedClientGeometry)
{
	EXPECT_TRUE(ShouldPreferDwmNonClientResult(WM_NCHITTEST, HTMAXBUTTON));
	EXPECT_FALSE(ShouldPreferDwmNonClientResult(WM_NCHITTEST, HTCLIENT));
	EXPECT_FALSE(ShouldPreferDwmNonClientResult(WM_NCHITTEST, HTNOWHERE));
	EXPECT_TRUE(ShouldPreferDwmNonClientResult(WM_NCLBUTTONDBLCLK, 0));
	EXPECT_TRUE(ShouldPreferDwmNonClientResult(WM_NCRBUTTONUP, 0));
	EXPECT_FALSE(ShouldPreferDwmNonClientResult(WM_SYSCOMMAND, 0));
}

TEST(CustomFrame, MapsCaptionButtonsToStandardWindowCommands)
{
	EXPECT_EQ(SC_MINIMIZE, CaptionButtonSystemCommand(HTMINBUTTON, false));
	EXPECT_EQ(SC_MAXIMIZE, CaptionButtonSystemCommand(HTMAXBUTTON, false));
	EXPECT_EQ(SC_RESTORE, CaptionButtonSystemCommand(HTMAXBUTTON, true));
	EXPECT_EQ(SC_CLOSE, CaptionButtonSystemCommand(HTCLOSE, false));
	EXPECT_EQ(0u, CaptionButtonSystemCommand(HTCAPTION, false));
}
