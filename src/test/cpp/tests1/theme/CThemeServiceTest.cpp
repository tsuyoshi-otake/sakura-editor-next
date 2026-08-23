/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "theme/CThemeService.h"
#include "types/CTypeSupport.h"
#include "view/figures/CFigureStrategy.h"

namespace theme {
namespace {

class VisibleWhitespaceBackgroundProbe final : public CFigureSpace {
public:
	[[nodiscard]] static COLORREF Resolve(
		EColorIndexType current, EColorIndexType underlying,
		COLORREF paintedBackground, COLORREF fallback) noexcept
	{
		return ResolveVisibleWhitespaceBackground(
			current, underlying, paintedBackground, fallback);
	}

	bool Match(const wchar_t*, int) const override { return false; }
	void DispSpace(CGraphics&, DispPos*, CEditView*, bool) const override { }
	EColorIndexType GetColorIdx() const override { return COLORIDX_SPACE; }
};

TEST(CThemeService, DarkPaletteMatchesWorkbenchTokensExactly)
{
	constexpr ThemePalette expected{
		{ 0x1E, 0x1E, 0x1E }, { 0x25, 0x25, 0x26 }, { 0x2A, 0x2D, 0x2E },
		{ 0x45, 0x45, 0x45 }, { 0xCC, 0xCC, 0xCC }, { 0x96, 0x96, 0x96 },
		{ 0x98, 0x98, 0x98 }, { 0x75, 0x75, 0x75 }, { 0x1F, 0x8A, 0xD2 }, { 0xFF, 0xFF, 0xFF },
		{ 0x3C, 0x3C, 0x3C }, { 0x33, 0x33, 0x33 }, { 0xC4, 0x2B, 0x1C }, { 0xCC, 0xA7, 0x00 },
		{ 0x25, 0x25, 0x26 }, { 0x29, 0x31, 0x34 }, { 0x25, 0x25, 0x26 },
		{ 0x1E, 0x1E, 0x1E }, { 0x85, 0x85, 0x85 }, { 0xCC, 0xCC, 0xCC },
		{ 0x37, 0x3D, 0x29 }, { 0x4B, 0x18, 0x18 }, { 0x41, 0x41, 0x41 },
		// `editor.findMatchHighlightBackground` (#EA5C0055) composited over each mode's own
		// sideBar, which is the surface the Search view draws its match highlight on.
		{ 0x66, 0x37, 0x19 },
		// The dark button role: the Sakura dark accent with lighten(accent, 0.2) as its hover.
		// These equal the ThemePalette member initializers, so omitting them would still pass
		// while asserting nothing about what the registry actually produces.
		{ 0x1F, 0x8A, 0xD2 }, { 0xFF, 0xFF, 0xFF }, { 0x3F, 0xA1, 0xE3 },
		{ 0x00, 0x7A, 0xCC }, { 0xFF, 0xFF, 0xFF },
		// The Git extension's registered `dark` gitDecoration defaults, verbatim and in
		// EFileDecorationColor order: added, modified, deleted, renamed, stageModified,
		// stageDeleted, untracked, ignored, conflicting, submodule.
		{ 0x81, 0xB8, 0x8B }, { 0xE2, 0xC0, 0x8D }, { 0xC7, 0x4E, 0x39 }, { 0x73, 0xC9, 0x91 },
		{ 0xE2, 0xC0, 0x8D }, { 0xC7, 0x4E, 0x39 }, { 0x73, 0xC9, 0x91 }, { 0x8C, 0x8C, 0x8C },
		{ 0xE4, 0x67, 0x6B }, { 0x8D, 0xB9, 0xE2 },
		{ 0x25, 0x25, 0x26 }, { 0x3C, 0x3C, 0x3C }, { 0x3C, 0x3C, 0x3C },
		{ 0x09, 0x47, 0x71 }, { 0xFF, 0xFF, 0xFF }, { 0x2A, 0x2D, 0x2E },
		{ 0x1F, 0x8A, 0xD2 },
		{ 0x00, 0x00, 0x00, 0x00 }, { 0x79, 0x79, 0x79, 0x66 },
		{ 0x64, 0x64, 0x64, 0xB3 }, { 0xBF, 0xBF, 0xBF, 0x66 },
		{ 0xE3, 0xE4, 0xE2, 0x29 },
	};
	EXPECT_EQ(expected, CThemeService::PaletteFor(ThemeMode::Dark));
	EXPECT_EQ(ThemeMode::Dark, CThemeService::DefaultMode());

	// VS Code colors the watermark shortcut list with descriptionForeground, whose dark value is
	// transparent(foreground #CCCCCC, 0.7). Composited over the #1E1E1E editor canvas that is
	// 0.7 * 0xCC + 0.3 * 0x1E = 151.8, which rounds to the 0x98 channel value asserted above.
	constexpr ThemePalette dark = CThemeService::PaletteFor(ThemeMode::Dark);
	static_assert(dark.descriptionText.red == 0x98 && dark.descriptionText.green == 0x98
		&& dark.descriptionText.blue == 0x98);
	EXPECT_EQ(RGB(0x98, 0x98, 0x98), dark.descriptionText.ToColorRef());
}

TEST(CThemeService, LightPaletteMatchesWorkbenchTokensExactly)
{
	constexpr ThemePalette expected{
		{ 0xFF, 0xFF, 0xFF }, { 0xF8, 0xF8, 0xF8 }, { 0xF2, 0xF2, 0xF2 },
		{ 0xE5, 0xE5, 0xE5 }, { 0x3B, 0x3B, 0x3B }, { 0x3B, 0x3B, 0x3B },
		{ 0x3B, 0x3B, 0x3B }, { 0xB0, 0xB0, 0xB0 }, { 0x00, 0x5F, 0xB8 }, { 0xFF, 0xFF, 0xFF },
		{ 0xF8, 0xF8, 0xF8 }, { 0xF8, 0xF8, 0xF8 }, { 0xF8, 0x51, 0x49 }, { 0xBF, 0x88, 0x00 },
		{ 0xF8, 0xF8, 0xF8 }, { 0xF8, 0xF8, 0xF8 }, { 0xF8, 0xF8, 0xF8 },
		{ 0xFF, 0xFF, 0xFF }, { 0x6E, 0x76, 0x81 }, { 0x17, 0x11, 0x84 },
		{ 0xEB, 0xF1, 0xDD }, { 0xFF, 0xCC, 0xCC }, { 0xD3, 0xD3, 0xD3 },
		// `editor.findMatchHighlightBackground` (#EA5C0055) composited over each mode's own
		// sideBar, which is the surface the Search view draws its match highlight on.
		{ 0xF3, 0xC4, 0xA6 },
		// The Light Modern workbench uses the same blue family for focus and buttons.
		{ 0x00, 0x5F, 0xB8 }, { 0xFF, 0xFF, 0xFF }, { 0x02, 0x58, 0xA8 },
		{ 0x00, 0x5F, 0xB8 }, { 0xFF, 0xFF, 0xFF },
		// The Git extension's registered `light` gitDecoration defaults, verbatim and in
		// EFileDecorationColor order. Spelled out because the ThemePalette member
		// initializers hold the dark values, which would make this assertion vacuous.
		{ 0x58, 0x7C, 0x0C }, { 0x89, 0x55, 0x03 }, { 0xAD, 0x07, 0x07 }, { 0x00, 0x71, 0x00 },
		{ 0x89, 0x55, 0x03 }, { 0xAD, 0x07, 0x07 }, { 0x00, 0x71, 0x00 }, { 0x8E, 0x8E, 0x90 },
		{ 0xAD, 0x07, 0x07 }, { 0x12, 0x58, 0xA7 },
		{ 0xF8, 0xF8, 0xF8 }, { 0xFF, 0xFF, 0xFF }, { 0xCE, 0xCE, 0xCE },
		{ 0xE8, 0xE8, 0xE8 }, { 0x00, 0x00, 0x00 }, { 0xF2, 0xF2, 0xF2 },
		{ 0x00, 0x5F, 0xB8 },
		{ 0x00, 0x00, 0x00, 0x00 }, { 0x64, 0x64, 0x64, 0x66 },
		{ 0x64, 0x64, 0x64, 0xB3 }, { 0x00, 0x00, 0x00, 0x99 },
		{ 0x33, 0x33, 0x33, 0x33 },
	};
	EXPECT_EQ(expected, CThemeService::PaletteFor(ThemeMode::Light));
}

TEST(CThemeService, EditorWhitespaceRolesUseTheVsCodeTokenOverTheEditorCanvas)
{
	constexpr ThemePalette light = CThemeService::PaletteFor(ThemeMode::Light);
	constexpr ThemeColor resolved = CompositeThemeColor(light.editorWhitespaceForeground, light.canvas);
	static_assert(resolved == ThemeColor{ 0xD6, 0xD6, 0xD6 });

	EXPECT_TRUE(IsThemeEditorWhitespaceColor(COLORIDX_TAB));
	EXPECT_TRUE(IsThemeEditorWhitespaceColor(COLORIDX_SPACE));
	EXPECT_TRUE(IsThemeEditorWhitespaceColor(COLORIDX_ZENSPACE));
	EXPECT_TRUE(IsThemeEditorWhitespaceColor(COLORIDX_CTRLCODE));
	EXPECT_TRUE(IsThemeEditorWhitespaceColor(COLORIDX_EOL));
	EXPECT_FALSE(IsThemeEditorWhitespaceColor(COLORIDX_TEXT));
	EXPECT_FALSE(IsThemeEditorWhitespaceColor(COLORIDX_EOF));
}

TEST(CThemeService, VisibleWhitespaceKeepsThePaintedSyntaxBackgroundAcrossScrollRepaints)
{
	CThemeService::ClearActiveColorThemePalette();
	const COLORREF fallback = RGB(0x1E, 0x1E, 0x1E);
	const COLORREF paintedSyntaxBackground = RGB(0xFA, 0xFB, 0xFC);

	const auto light = CThemeService::PaletteFor(ThemeMode::Light);
	CThemeService::SetActiveColorThemePalette(light);
	EXPECT_EQ(paintedSyntaxBackground, VisibleWhitespaceBackgroundProbe::Resolve(
		COLORIDX_KEYWORD1, COLORIDX_KEYWORD1, paintedSyntaxBackground, fallback));

	EXPECT_EQ(fallback, VisibleWhitespaceBackgroundProbe::Resolve(
		COLORIDX_SELECT, COLORIDX_KEYWORD1, paintedSyntaxBackground, fallback));
	EXPECT_EQ(fallback, VisibleWhitespaceBackgroundProbe::Resolve(
		COLORIDX_SEARCH, COLORIDX_SEARCH, paintedSyntaxBackground, fallback));

	CThemeService::ClearActiveColorThemePalette();
	EXPECT_EQ(fallback, VisibleWhitespaceBackgroundProbe::Resolve(
		COLORIDX_KEYWORD1, COLORIDX_KEYWORD1, paintedSyntaxBackground, fallback));
}

TEST(CThemeService, HighContrastOverlayDoesNotChangeSavedModeSelection)
{
	constexpr ThemePalette highContrast{
		{ 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 }, { 10, 11, 12 },
		{ 13, 14, 15 }, { 16, 17, 18 }, { 19, 20, 21 }, { 22, 23, 24 },
		{ 25, 26, 27 }, { 28, 29, 30 }, { 31, 32, 33 }, { 34, 35, 36 }, { 37, 38, 39 }, { 40, 41, 42 },
		{ 43, 44, 45 }, { 46, 47, 48 }, { 49, 50, 51 },
		{ 52, 53, 54 }, { 55, 56, 57 }, { 58, 59, 60 },
		{ 61, 62, 63 }, { 64, 65, 66 }, { 67, 68, 69 },
		{ 70, 71, 72 },
	};
	EXPECT_EQ(highContrast, CThemeService::SelectPalette(ThemeMode::Dark, true, highContrast));
	EXPECT_EQ(CThemeService::PaletteFor(ThemeMode::Light),
		CThemeService::SelectPalette(ThemeMode::Light, false, highContrast));
}

TEST(CThemeService, HighContrastTerminalBackgroundUsesThePanelFace)
{
	const ThemePalette highContrast = CThemeService::HighContrastPalette();
	EXPECT_EQ(highContrast.bottomPanel, highContrast.terminalBackground);
	EXPECT_EQ(highContrast.canvas, highContrast.editorGutterBackground);
	EXPECT_EQ(highContrast.primaryText, highContrast.editorLineNumberForeground);
	EXPECT_EQ(highContrast.primaryText, highContrast.editorLineNumberActiveForeground);
	EXPECT_EQ(highContrast.primaryText, highContrast.editorWhitespaceForeground);
}

TEST(CThemeService, ActiveColorThemePalettePublishesEditorProjectionWithoutCopying)
{
	CThemeService::ClearActiveColorThemePalette();
	EXPECT_EQ(nullptr, CThemeService::ActiveColorThemePalette());
	const auto light = CThemeService::PaletteFor(ThemeMode::Light);
	CThemeService::SetActiveColorThemePalette(light);
	const auto* active = CThemeService::ActiveColorThemePalette();
	ASSERT_NE(nullptr, active);
	EXPECT_EQ(light.canvas, active->canvas);
	EXPECT_EQ(light.primaryText, active->primaryText);
	CThemeService::ClearActiveColorThemePalette();
}

TEST(CThemeService, FontPolicyPrefersNativeWindowsElevenFamilies)
{
	const auto chrome = CThemeService::FontSpec(ThemeFontKind::Chrome);
	EXPECT_STREQ(L"Segoe UI Variable", chrome.preferredFamily);
	EXPECT_STREQ(L"Segoe UI", chrome.fallbackFamily);
	EXPECT_EQ(8, chrome.pointSize);
	EXPECT_FALSE(chrome.fixedPitch);

	const auto editor = CThemeService::FontSpec(ThemeFontKind::Editor);
	EXPECT_STREQ(L"Consolas", editor.preferredFamily);
	EXPECT_STREQ(L"Cascadia Mono", editor.fallbackFamily);
	EXPECT_EQ(10, editor.pointSize);
	EXPECT_TRUE(editor.fixedPitch);

	const auto terminal = CThemeService::FontSpec(ThemeFontKind::Terminal);
	EXPECT_STREQ(L"Cascadia Mono", terminal.preferredFamily);
	EXPECT_STREQ(L"Consolas", terminal.fallbackFamily);
	EXPECT_EQ(9, terminal.pointSize);
	EXPECT_EQ(FW_NORMAL, terminal.weight);
	EXPECT_TRUE(terminal.fixedPitch);
}

} // namespace
} // namespace theme
