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
using workbench::controls::ResolveOverlayScrollbarColors;

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

TEST(OverlayScrollbarColorTest, ResolvesVsCodeLightSliderStatesOverTheOwningSurface)
{
	const auto palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Light);
	const auto colors = ResolveOverlayScrollbarColors(palette, palette.sideBar);

	EXPECT_EQ(RGB(0xF8, 0xF8, 0xF8), colors.background);
	EXPECT_EQ(colors.background, colors.trackHover);
	EXPECT_EQ(RGB(0xBD, 0xBD, 0xBD), colors.thumb);
	EXPECT_EQ(RGB(0x90, 0x90, 0x90), colors.thumbHover);
	EXPECT_EQ(RGB(0x63, 0x63, 0x63), colors.thumbActive);
}

TEST(OverlayScrollbarWindowTest, ScrollbarControlCoversTheReservedModelRectangle)
{
	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
		0, 0, 320, 240, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	const HWND model = ::CreateWindowExW(0, WC_SCROLLBAR, L"",
		WS_CHILD | WS_VISIBLE | SBS_VERT, 40, 20, 17, 150,
		parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, model);

	SCROLLINFO info{ sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
	info.nMin = 0;
	info.nMax = 9;
	info.nPage = 10;
	info.nPos = 0;
	(void)::SetScrollInfo(model, SB_CTL, &info, FALSE);

	workbench::controls::COverlayScrollbar overlay;
	ASSERT_TRUE(overlay.Create(parent, model, [](int) {},
		workbench::controls::OverlayScrollbarSource::ScrollbarControl));
	overlay.SetBounds(RECT{ 40, 20, 57, 170 });
	overlay.Update();

	EXPECT_EQ(0, ::GetWindowLongPtrW(model, GWL_STYLE) & WS_VISIBLE);
	ASSERT_NE(nullptr, overlay.Get());
	EXPECT_NE(0, ::GetWindowLongPtrW(overlay.Get(), GWL_STYLE) & WS_VISIBLE);
	RECT modelRect{};
	RECT overlayRect{};
	ASSERT_TRUE(::GetWindowRect(model, &modelRect));
	ASSERT_TRUE(::GetWindowRect(overlay.Get(), &overlayRect));
	EXPECT_EQ(modelRect.left, overlayRect.left);
	EXPECT_EQ(modelRect.top, overlayRect.top);
	EXPECT_EQ(modelRect.right, overlayRect.right);
	EXPECT_EQ(modelRect.bottom, overlayRect.bottom);

	overlay.Destroy();
	::DestroyWindow(model);
	::DestroyWindow(parent);
}

} // namespace
