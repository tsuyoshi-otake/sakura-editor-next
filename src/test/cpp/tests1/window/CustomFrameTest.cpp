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

namespace {

//! Every title-control rectangle except the indicator's own, so a hidden indicator can be
//! proven to change nothing at all rather than only nothing obvious.
void ExpectSameFrameExceptUpdateIndicator(const CustomFrameLayout& expected, const CustomFrameLayout& actual)
{
	const auto same = [](const RECT& a, const RECT& b) {
		EXPECT_EQ(a.left, b.left);
		EXPECT_EQ(a.top, b.top);
		EXPECT_EQ(a.right, b.right);
		EXPECT_EQ(a.bottom, b.bottom);
	};
	same(expected.title, actual.title);
	same(expected.systemMenu, actual.systemMenu);
	same(expected.menu, actual.menu);
	same(expected.captionText, actual.captionText);
	same(expected.layoutButton, actual.layoutButton);
	same(expected.primarySidebarButton, actual.primarySidebarButton);
	same(expected.bottomPanelButton, actual.bottomPanelButton);
	same(expected.secondarySidebarButton, actual.secondarySidebarButton);
	same(expected.accountButton, actual.accountButton);
	same(expected.manageButton, actual.manageButton);
	same(expected.minimizeButton, actual.minimizeButton);
	same(expected.maximizeButton, actual.maximizeButton);
	same(expected.closeButton, actual.closeButton);
}

} // namespace

TEST(CustomFrameUpdateControl, LeavesEveryOtherRectangleUntouchedWhileTheStateIsNotActionable)
{
	// The indicator exists only for `available for download` / `downloaded` / `ready`.
	// In all nine other states the composition root passes width zero, and zero must be
	// indistinguishable from a build that never had an indicator at all.
	for (const UINT dpi : { 96u, 120u, 144u, 192u }) {
		const auto withoutIndicator = CalculateCustomFrameLayout(1200, dpi, 430);
		const auto hidden = CalculateCustomFrameLayout(1200, dpi, 430, 0);
		ExpectSameFrameExceptUpdateIndicator(withoutIndicator, hidden);
		EXPECT_TRUE(::IsRectEmpty(&hidden.updateButton));
		// A degenerate rectangle at the current offset would still hit-test as absent, but
		// it would also paint and expose an accessible node; the layout skips it outright.
		EXPECT_EQ(0, hidden.updateButton.left);
		EXPECT_EQ(0, hidden.updateButton.right);

		// A negative measurement is clamped to hidden rather than shifting controls right.
		const auto negative = CalculateCustomFrameLayout(1200, dpi, 430, -40);
		ExpectSameFrameExceptUpdateIndicator(withoutIndicator, negative);
		EXPECT_TRUE(::IsRectEmpty(&negative.updateButton));
	}
}

TEST(CustomFrameUpdateControl, InsertsTheLabelledIndicatorBetweenSecondarySideBarAndAccount)
{
	constexpr int kIndicatorWidth = 56;
	const auto hidden = CalculateCustomFrameLayout(1200, 96, 430, 0);
	const auto shown = CalculateCustomFrameLayout(1200, 96, 430, kIndicatorWidth);

	EXPECT_FALSE(::IsRectEmpty(&shown.updateButton));
	EXPECT_EQ(kIndicatorWidth, shown.updateButton.right - shown.updateButton.left);
	EXPECT_EQ(shown.title.bottom, shown.updateButton.bottom);
	EXPECT_EQ(shown.secondarySidebarButton.right, shown.updateButton.left);
	EXPECT_EQ(shown.updateButton.right, shown.accountButton.left);
	EXPECT_EQ(shown.accountButton.right, shown.manageButton.left);
	EXPECT_EQ(shown.manageButton.right, shown.minimizeButton.left);

	// The run is right-aligned against the native caption buttons and the indicator is
	// inserted before Account, so Account and Manage do not move at all and the four
	// glyph controls to its left move left by exactly the measured width.
	EXPECT_EQ(hidden.accountButton.left, shown.accountButton.left);
	EXPECT_EQ(hidden.manageButton.left, shown.manageButton.left);
	EXPECT_EQ(hidden.minimizeButton.left, shown.minimizeButton.left);
	EXPECT_EQ(hidden.layoutButton.left - kIndicatorWidth, shown.layoutButton.left);
	EXPECT_EQ(hidden.primarySidebarButton.left - kIndicatorWidth, shown.primarySidebarButton.left);
	EXPECT_EQ(hidden.bottomPanelButton.left - kIndicatorWidth, shown.bottomPanelButton.left);
	EXPECT_EQ(hidden.secondarySidebarButton.left - kIndicatorWidth, shown.secondarySidebarButton.left);
	// The indicator takes its width from its label; the glyph controls keep the fixed one.
	EXPECT_EQ(ScaleCustomFrameDip(30, 96), shown.layoutButton.right - shown.layoutButton.left);
	EXPECT_EQ(ScaleCustomFrameDip(30, 96), shown.accountButton.right - shown.accountButton.left);
	EXPECT_LE(shown.captionText.right, shown.layoutButton.left);
}

TEST(CustomFrameUpdateControl, CollapsesTheWholeRunWhenTheIndicatorNoLongerFits)
{
	// 380px shows all six glyph controls, but not once the indicator claims 56 more.
	// Partially drawing the run, or letting it overlap the system menu, is not an option.
	const auto hidden = CalculateCustomFrameLayout(380, 96, 300, 0);
	EXPECT_FALSE(::IsRectEmpty(&hidden.layoutButton));
	EXPECT_FALSE(::IsRectEmpty(&hidden.manageButton));

	const auto shown = CalculateCustomFrameLayout(380, 96, 300, 56);
	EXPECT_TRUE(::IsRectEmpty(&shown.updateButton));
	EXPECT_TRUE(::IsRectEmpty(&shown.layoutButton));
	EXPECT_TRUE(::IsRectEmpty(&shown.secondarySidebarButton));
	EXPECT_TRUE(::IsRectEmpty(&shown.accountButton));
	EXPECT_TRUE(::IsRectEmpty(&shown.manageButton));
	EXPECT_EQ(CustomFrameControl::None, HitTestCustomFrameControl(shown, { 300, 16 }));

	// 418px is the first width that fits the indicator as well.
	const auto fits = CalculateCustomFrameLayout(418, 96, 300, 56);
	EXPECT_FALSE(::IsRectEmpty(&fits.updateButton));
	EXPECT_EQ(ScaleCustomFrameDip(42, 96), fits.layoutButton.left);
}

TEST(CustomFrameUpdateControl, HitTestsTheIndicatorWithTheSameHalfOpenBoundsAsEveryOtherControl)
{
	const auto layout = CalculateCustomFrameLayout(1200, 96, 300, 56);
	const auto center = [](const RECT& rect) {
		return POINT{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
	};
	EXPECT_EQ(CustomFrameControl::Update, HitTestCustomFrameControl(layout, center(layout.updateButton)));
	EXPECT_EQ(CustomFrameControl::Update, HitTestCustomFrameControl(layout, { layout.updateButton.left, 16 }));
	EXPECT_EQ(CustomFrameControl::SecondarySidebar,
		HitTestCustomFrameControl(layout, { layout.updateButton.left - 1, 16 }));
	EXPECT_EQ(CustomFrameControl::Account, HitTestCustomFrameControl(layout, { layout.updateButton.right, 16 }));
	EXPECT_EQ(HTCLIENT, HitTestCustomFrame(layout, center(layout.updateButton), 1200, 700, 8, false));

	// While the indicator is hidden its empty rectangle can never contain a point, so no
	// client point anywhere in the caption resolves to Update.  The point that used to be
	// the indicator is not caption drag either: the layout is right-aligned, so the fixed
	// controls reclaim exactly the width the indicator gave up and one of them now owns
	// that x.  Asserting None there would pin a false fact about right alignment.
	const auto hidden = CalculateCustomFrameLayout(1200, 96, 300, 0);
	EXPECT_TRUE(::IsRectEmpty(&hidden.updateButton));
	EXPECT_EQ(CustomFrameControl::None, HitTestCustomFrameControl(hidden, { 0, 0 }));
	for (int x = 0; x < 1200; ++x) {
		ASSERT_NE(CustomFrameControl::Update, HitTestCustomFrameControl(hidden, { x, 16 }))
			<< "x = " << x;
	}
}

TEST(CustomFrameUpdateControl, HasNoLegacyFunctionCodeBecauseItsCommandDependsOnTheUpdateState)
{
	// `update.downloadNow` / `update.install` / `update.restart` are resolved from the
	// current context snapshot by the registry, so pinning one `EFunctionCode` here would
	// be wrong in two of the three actionable states.
	EXPECT_EQ(0u, CustomFrameControlCommand(CustomFrameControl::Update));
}

TEST(CustomFrameUpdateControl, ExposesUpstreamsUpdateTitleAsItsAccessibleNameAndAutomationId)
{
	const auto layout = CalculateCustomFrameLayout(1200, 96, 300, 56);
	const auto node = CustomFrameControlAccessibilityNode(CustomFrameControl::Update, layout, true);
	EXPECT_EQ(L"Update", node.name);
	EXPECT_EQ(L"Sakura.TitleBar.Update", node.automationId);
	EXPECT_EQ(UIA_ButtonControlTypeId, node.controlType);
	EXPECT_EQ(layout.updateButton.left, node.bounds.left);
	EXPECT_EQ(layout.updateButton.right, node.bounds.right);
	EXPECT_TRUE(node.enabled);
	EXPECT_TRUE(node.focused);
	EXPECT_TRUE(node.invoke);

	const auto hidden = CalculateCustomFrameLayout(1200, 96, 300, 0);
	const auto hiddenNode = CustomFrameControlAccessibilityNode(CustomFrameControl::Update, hidden, false);
	EXPECT_EQ(hiddenNode.bounds.left, hiddenNode.bounds.right);
	EXPECT_EQ(hiddenNode.bounds.top, hiddenNode.bounds.bottom);
}

TEST(CustomFrameUpdateControl, FallsBackToAMinimumButtonWidthWhenTheCaptionFontCannotBeMeasured)
{
	// A zero width here would mean "hidden", which is a different fact from "unmeasurable".
	for (const UINT dpi : { 96u, 120u, 144u, 192u }) {
		EXPECT_EQ(ScaleCustomFrameDip(56, dpi), MeasureCustomFrameUpdateButtonWidth(nullptr, dpi));
		EXPECT_GT(MeasureCustomFrameUpdateButtonWidth(nullptr, dpi), ScaleCustomFrameDip(30, dpi));
	}
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

TEST(CustomFrame, CoversEveryClientEdgeWithNonOverlappingResizeOverlays)
{
	const auto bounds = CalculateCustomFrameResizeOverlayBounds(1000, 700, 8, false);
	const auto at = [&](CustomFrameResizeEdge edge) -> const RECT& {
		return bounds[static_cast<size_t>(edge)];
	};
	const auto expectRect = [](const RECT& rect, LONG left, LONG top, LONG right, LONG bottom) {
		EXPECT_EQ(left, rect.left);
		EXPECT_EQ(top, rect.top);
		EXPECT_EQ(right, rect.right);
		EXPECT_EQ(bottom, rect.bottom);
	};
	expectRect(at(CustomFrameResizeEdge::Top), 0, 0, 1000, 8);
	expectRect(at(CustomFrameResizeEdge::Bottom), 0, 692, 1000, 700);
	expectRect(at(CustomFrameResizeEdge::Left), 0, 8, 8, 692);
	expectRect(at(CustomFrameResizeEdge::Right), 992, 8, 1000, 692);

	const auto maximized = CalculateCustomFrameResizeOverlayBounds(1000, 700, 8, true);
	for (const RECT& rect : maximized) EXPECT_TRUE(::IsRectEmpty(&rect));
}

TEST(CustomFrame, ClampsResizeOverlaysForTinyWindows)
{
	const auto bounds = CalculateCustomFrameResizeOverlayBounds(6, 4, 8, false);
	const auto at = [&](CustomFrameResizeEdge edge) -> const RECT& {
		return bounds[static_cast<size_t>(edge)];
	};
	EXPECT_EQ(0, at(CustomFrameResizeEdge::Top).left);
	EXPECT_EQ(0, at(CustomFrameResizeEdge::Top).top);
	EXPECT_EQ(6, at(CustomFrameResizeEdge::Top).right);
	EXPECT_EQ(4, at(CustomFrameResizeEdge::Top).bottom);
	EXPECT_EQ(0, at(CustomFrameResizeEdge::Bottom).left);
	EXPECT_EQ(0, at(CustomFrameResizeEdge::Bottom).top);
	EXPECT_EQ(6, at(CustomFrameResizeEdge::Bottom).right);
	EXPECT_EQ(4, at(CustomFrameResizeEdge::Bottom).bottom);
	EXPECT_TRUE(::IsRectEmpty(&at(CustomFrameResizeEdge::Left)));
	EXPECT_TRUE(::IsRectEmpty(&at(CustomFrameResizeEdge::Right)));
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
