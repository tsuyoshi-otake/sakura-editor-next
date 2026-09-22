/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"
#include "env/ShareDataTestSuite.hpp"
#include "mem/CMemory.h"
#include "uiparts/CImageListMgr.h"
#include "uiparts/CMenuDrawer.h"
#include "util/window.h"
#include <cstdint>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

DWORD GdiObjectCount()
{
	::GdiFlush();
	return ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
}

// Keep resource lifetime independent of profile-specific icon files.
class LifetimeTestIcons final : public CImageListMgr {
public:
	void SetCount(int count) { m_nIconCount = count; }
};

class MenuResourceLifetime : public ::testing::Test, public env::ShareDataTestSuite {
public:
	static void SetUpTestSuite() { SetUpShareData(); }
	static void TearDownTestSuite() { TearDownShareData(); }
};

TEST_F(MenuResourceLifetime, RecreateReleasesPreviousBitmaps)
{
	LifetimeTestIcons icons;
	icons.SetCount(4);
	const DWORD before = GdiObjectCount();
	{
		CMenuDrawer drawer;
		for (int iteration = 0; iteration < 20; ++iteration) {
			drawer.Create(::GetModuleHandleW(nullptr), nullptr, &icons);
		}
		EXPECT_EQ(before + 4, GdiObjectCount());
	}
	EXPECT_EQ(before, GdiObjectCount());
}

TEST_F(MenuResourceLifetime, ShrinkingAndClearingReleaseRemovedBitmaps)
{
	LifetimeTestIcons icons;
	const DWORD before = GdiObjectCount();
	{
		CMenuDrawer drawer;
		icons.SetCount(6);
		drawer.Create(::GetModuleHandleW(nullptr), nullptr, &icons);
		ASSERT_EQ(before + 6, GdiObjectCount());
		icons.SetCount(2);
		drawer.Create(::GetModuleHandleW(nullptr), nullptr, &icons);
		EXPECT_EQ(before + 2, GdiObjectCount());
		icons.SetCount(0);
		drawer.Create(::GetModuleHandleW(nullptr), nullptr, &icons);
		EXPECT_EQ(before, GdiObjectCount());
	}
	EXPECT_EQ(before, GdiObjectCount());
}

TEST_F(MenuResourceLifetime, AcceleratorMatchingPreservesSelectionAndExecution)
{
	LifetimeTestIcons icons;
	icons.SetCount(0);
	CMenuDrawer drawer;
	drawer.Create(::GetModuleHandleW(nullptr), nullptr, &icons);
	cxx::ResourceHolder<&::DestroyMenu> menu = ::CreatePopupMenu();
	ASSERT_NE(nullptr, menu.get());
	// A command with no toolbar mapping keeps this test independent of icons.
	constexpr int noIconCommand = -2;
	drawer.MyAppendMenu(menu, MF_STRING, F_FILENEW, L"&Alpha", L"", FALSE, noIconCommand);
	drawer.MyAppendMenu(menu, MF_STRING, F_FILEOPEN, L"&Beta", L"", FALSE, noIconCommand);
	drawer.MyAppendMenu(menu, MF_STRING, F_FILESAVE, L"&Another", L"", FALSE, noIconCommand);
	ASSERT_EQ(3, ::GetMenuItemCount(menu));
	const auto invoke = [&](wchar_t key) {
		return drawer.OnMenuChar(nullptr, WM_MENUCHAR, key,
			reinterpret_cast<std::intptr_t>(menu.get()));
	};
	EXPECT_EQ(MAKELONG(0, MNC_IGNORE), invoke(L'Z'));
	EXPECT_EQ(MAKELONG(1, MNC_EXECUTE), invoke(L'B'));
	EXPECT_EQ(MAKELONG(0, MNC_SELECT), invoke(L'A'));
	MENUITEMINFOW state{ sizeof(state) };
	state.fMask = MIIM_STATE;
	state.fState = MFS_HILITE;
	ASSERT_TRUE(::SetMenuItemInfoW(menu, 0, TRUE, &state));
	EXPECT_EQ(MAKELONG(2, MNC_SELECT), invoke(L'a'));
	state.fState = 0;
	ASSERT_TRUE(::SetMenuItemInfoW(menu, 0, TRUE, &state));
	state.fState = MFS_HILITE;
	ASSERT_TRUE(::SetMenuItemInfoW(menu, 2, TRUE, &state));
	EXPECT_EQ(MAKELONG(0, MNC_SELECT), invoke(L'A'));
}

TEST(NativeResourceLifetime, WindowFontReleasesFontAfterScope)
{
	const auto instance = ::GetModuleHandleW(nullptr);
	cxx::ResourceHolder<&::DestroyWindow> window = ::CreateWindowExW(0,
		L"STATIC", L"", WS_POPUP, 0, 0, 16, 16,
		nullptr, nullptr, instance, nullptr);
	ASSERT_NE(nullptr, window.get());
	LOGFONTW font{};
	font.lfHeight = -16;
	HFONT selectedFont = nullptr;
	LOGFONTW selectedFontData{};
	{
		CDCFont owner(font, window);
		const HDC dc = owner.GetHDC();
		ASSERT_NE(nullptr, dc);
		selectedFont = static_cast<HFONT>(::GetCurrentObject(dc, OBJ_FONT));
		ASSERT_NE(nullptr, selectedFont);
		ASSERT_NE(::GetStockObject(SYSTEM_FONT), selectedFont);
		ASSERT_EQ(sizeof(selectedFontData),
			::GetObjectW(selectedFont, sizeof(selectedFontData), &selectedFontData));
	}
	::GdiFlush();
	// GetObjectType can retain the type even after a font handle is deleted.
	EXPECT_EQ(0, ::GetObjectW(selectedFont, sizeof(selectedFontData), &selectedFontData));
}

TEST(NativeResourceLifetime, RepeatedWindowFontScopesDoNotAccumulateGdiObjects)
{
	LOGFONTW font{};
	font.lfHeight = -16;
	{ CDCFont warmup(font); }
	const DWORD before = GdiObjectCount();
	for (int iteration = 0; iteration < 128; ++iteration) {
		CDCFont owner(font);
		ASSERT_NE(nullptr, owner.GetHDC());
	}
	EXPECT_EQ(before, GdiObjectCount());
}

#if defined(_MSC_VER) && defined(_DEBUG)
TEST(NativeResourceLifetime, MemoryBufferReleasesCopyMoveGrowthAndResetAllocations)
{
	const std::array<char, 1024> data{};
	_CrtMemState before{}, after{};
	_CrtMemCheckpoint(&before);
	for (int iteration = 0; iteration < 128; ++iteration) {
		CMemory source(data.data(), data.size());
		CMemory copy(source);
		copy.AppendRawData(data.data(), data.size());
		CMemory moved(std::move(copy));
		source = std::move(moved);
		source.AllocBuffer(8192);
		source.ShrinkBuffer();
		copy = source;
		source.Reset();
	}
	_CrtMemCheckpoint(&after);
	EXPECT_EQ(before.lCounts[_NORMAL_BLOCK], after.lCounts[_NORMAL_BLOCK]);
	EXPECT_EQ(before.lSizes[_NORMAL_BLOCK], after.lSizes[_NORMAL_BLOCK]);
}
#endif

} // namespace
