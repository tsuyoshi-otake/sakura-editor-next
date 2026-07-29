/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "theme/CThemeService.h"

namespace theme {
namespace {

TEST(CThemeService, DarkPaletteMatchesWorkbenchTokensExactly)
{
	constexpr ThemePalette expected{
		{ 0x18, 0x1A, 0x1F }, { 0x20, 0x23, 0x2A }, { 0x29, 0x2D, 0x35 },
		{ 0x38, 0x3E, 0x49 }, { 0xE8, 0xEB, 0xF0 }, { 0xAA, 0xB1, 0xBC }, { 0xEB, 0x6A, 0x9A }, { 0xFF, 0xFF, 0xFF },
	};
	EXPECT_EQ(expected, CThemeService::PaletteFor(ThemeMode::Dark));
	EXPECT_EQ(ThemeMode::Dark, CThemeService::DefaultMode());
}

TEST(CThemeService, LightPaletteMatchesWorkbenchTokensExactly)
{
	constexpr ThemePalette expected{
		{ 0xF4, 0xF5, 0xF7 }, { 0xFF, 0xFF, 0xFF }, { 0xE9, 0xEC, 0xF1 },
		{ 0xCD, 0xD2, 0xDB }, { 0x1F, 0x23, 0x29 }, { 0x5C, 0x65, 0x73 }, { 0xB8, 0x32, 0x68 }, { 0xFF, 0xFF, 0xFF },
	};
	EXPECT_EQ(expected, CThemeService::PaletteFor(ThemeMode::Light));
}

TEST(CThemeService, HighContrastOverlayDoesNotChangeSavedModeSelection)
{
	constexpr ThemePalette highContrast{
		{ 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, 12 },
		{ 13, 14, 15 }, { 16, 17, 18 }, { 19, 20, 21 }, { 22, 23, 24 },
	};
	EXPECT_EQ(highContrast, CThemeService::SelectPalette(ThemeMode::Dark, true, highContrast));
	EXPECT_EQ(CThemeService::PaletteFor(ThemeMode::Light),
		CThemeService::SelectPalette(ThemeMode::Light, false, highContrast));
}

TEST(CThemeService, FontPolicyPrefersNativeWindowsElevenFamilies)
{
	const auto chrome = CThemeService::FontSpec(ThemeFontKind::Chrome);
	EXPECT_STREQ(L"Segoe UI Variable", chrome.preferredFamily);
	EXPECT_STREQ(L"Segoe UI", chrome.fallbackFamily);
	EXPECT_FALSE(chrome.fixedPitch);

	const auto terminal = CThemeService::FontSpec(ThemeFontKind::Terminal);
	EXPECT_STREQ(L"Cascadia Mono", terminal.preferredFamily);
	EXPECT_STREQ(L"Consolas", terminal.fallbackFamily);
	EXPECT_TRUE(terminal.fixedPitch);
}

} // namespace
} // namespace theme
