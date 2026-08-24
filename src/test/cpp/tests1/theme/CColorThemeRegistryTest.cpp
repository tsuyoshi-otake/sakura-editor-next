/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "theme/CColorThemeRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace {
void ExpectParsedColor(std::wstring_view value, theme::ThemeColor expected)
{
	const auto parsed = theme::CColorThemeRegistry::ParseColor(value);
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(expected, *parsed);
}

TEST(CColorThemeRegistry, RegistersBuiltinThemesThroughTheVsCodeProjectionPath)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::CThemeService;
	using theme::ThemeMode;
	using theme::ThemeColor;

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterBuiltinThemes());
	const auto themes = registry.Themes();
	ASSERT_EQ(2U, themes.size());

	const auto darkInfo = std::find_if(themes.begin(), themes.end(), [](const auto& info) {
		return info.id == L"sakura.default-dark";
	});
	const auto lightInfo = std::find_if(themes.begin(), themes.end(), [](const auto& info) {
		return info.id == L"sakura.default-light";
	});
	ASSERT_NE(themes.end(), darkInfo);
	ASSERT_NE(themes.end(), lightInfo);
	EXPECT_EQ(L"Sakura Default Dark", darkInfo->label);
	EXPECT_EQ(L"Sakura Default Light", lightInfo->label);
	EXPECT_EQ(ColorThemeKind::Dark, darkInfo->kind);
	EXPECT_EQ(ColorThemeKind::Light, lightInfo->kind);

	const auto dark = registry.Load(L"sakura.default-dark");
	ASSERT_TRUE(dark.Succeeded()) << dark.diagnostic.c_str();
	ASSERT_TRUE(dark.theme.has_value());
	EXPECT_EQ((ThemeColor{ 0x1E, 0x1E, 0x1E, 0xFF }), dark.theme->colors.at(L"editor.background"));
	EXPECT_EQ((ThemeColor{ 0x40, 0x40, 0x40, 0xFF }),
		dark.theme->colors.at(L"editorIndentGuide.background1"));
	EXPECT_FALSE(dark.theme->tokenColors.empty());
	ASSERT_TRUE(dark.theme->syntaxPalette.comment.foreground.has_value());
	EXPECT_EQ((ThemeColor{ 0x6A, 0x99, 0x55, 0xFF }), *dark.theme->syntaxPalette.comment.foreground);
	ASSERT_TRUE(dark.theme->syntaxPalette.string.foreground.has_value());
	EXPECT_EQ((ThemeColor{ 0xCE, 0x91, 0x78, 0xFF }), *dark.theme->syntaxPalette.string.foreground);
	EXPECT_TRUE(dark.theme->semanticTokenColors.empty());
	EXPECT_FALSE(dark.theme->semanticHighlighting);
	EXPECT_EQ(CThemeService::PaletteFor(ThemeMode::Dark), dark.theme->palette);

	const auto light = registry.Load(L"Sakura Default Light");
	ASSERT_TRUE(light.Succeeded()) << light.diagnostic.c_str();
	ASSERT_TRUE(light.theme.has_value());
	EXPECT_EQ((ThemeColor{ 0xFF, 0xFF, 0xFF, 0xFF }), light.theme->colors.at(L"editor.background"));
	EXPECT_EQ((ThemeColor{ 0xF8, 0xF8, 0xF8, 0xFF }), light.theme->colors.at(L"sideBar.background"));
	EXPECT_EQ((ThemeColor{ 0x00, 0x5F, 0xB8, 0xFF }), light.theme->colors.at(L"focusBorder"));
	EXPECT_EQ((ThemeColor{ 0xE8, 0xE8, 0xE8, 0xFF }),
		light.theme->colors.at(L"list.activeSelectionBackground"));
	EXPECT_FALSE(light.theme->tokenColors.empty());
	EXPECT_TRUE(light.theme->semanticTokenColors.empty());
	EXPECT_FALSE(light.theme->semanticHighlighting);
	EXPECT_EQ(CThemeService::PaletteFor(ThemeMode::Light), light.theme->palette);
	EXPECT_EQ((ThemeColor{ 0xF8, 0xF8, 0xF8 }), light.theme->palette.quickInputBackground);
	EXPECT_EQ((ThemeColor{ 0xFF, 0xFF, 0xFF }), light.theme->palette.inputBackground);
	EXPECT_EQ((ThemeColor{ 0xCE, 0xCE, 0xCE }), light.theme->palette.inputBorder);
	EXPECT_EQ((ThemeColor{ 0xE8, 0xE8, 0xE8 }), light.theme->palette.listActiveSelectionBackground);
	EXPECT_EQ((ThemeColor{ 0x00, 0x00, 0x00 }), light.theme->palette.listActiveSelectionForeground);
	EXPECT_EQ((ThemeColor{ 0xF2, 0xF2, 0xF2 }), light.theme->palette.listHoverBackground);
	EXPECT_EQ((ThemeColor{ 0x00, 0x5F, 0xB8 }), light.theme->palette.listFocusAndSelectionOutline);
	EXPECT_EQ((ThemeColor{ 0x00, 0x00, 0x00, 0x00 }), light.theme->palette.scrollbarBackground);
	EXPECT_EQ((ThemeColor{ 0x64, 0x64, 0x64, 0x66 }), light.theme->palette.scrollbarSliderBackground);
	EXPECT_EQ((ThemeColor{ 0x64, 0x64, 0x64, 0xB3 }),
		light.theme->palette.scrollbarSliderHoverBackground);
	EXPECT_EQ((ThemeColor{ 0x00, 0x00, 0x00, 0x99 }),
		light.theme->palette.scrollbarSliderActiveBackground);
	EXPECT_EQ((ThemeColor{ 0x33, 0x33, 0x33, 0x33 }),
		light.theme->palette.editorWhitespaceForeground);
	EXPECT_EQ((ThemeColor{ 0xD3, 0xD3, 0xD3 }),
		light.theme->palette.editorIndentGuideBackground);

	// A refresh can rebuild the built-ins without leaving stale virtual files
	// behind. Clear is still a complete registry reset for callers that need it.
	ASSERT_TRUE(registry.RegisterBuiltinThemes());
	EXPECT_EQ(2U, registry.Themes().size());
	registry.Clear();
	EXPECT_TRUE(registry.Themes().empty());
}

TEST(CColorThemeRegistry, ParsesVsCodeColorForms)
{
	using theme::CColorThemeRegistry;
	using theme::ThemeColor;

	ExpectParsedColor(L"#1234", ThemeColor{ 0x11, 0x22, 0x33, 0x44 });
	ExpectParsedColor(L"#11223380", ThemeColor{ 0x11, 0x22, 0x33, 0x80 });
	ExpectParsedColor(L"rgba(10, 20, 30, 0.5)", ThemeColor{ 10, 20, 30, 128 });
	ExpectParsedColor(L"rgb(10,20,30)", ThemeColor{ 10, 20, 30, 0xFF });
	ExpectParsedColor(L"transparent", ThemeColor{ 0, 0, 0, 0 });
	EXPECT_FALSE(CColorThemeRegistry::ParseColor(L"#12").has_value());
}

TEST(CColorThemeRegistry, PreservesScrollbarAlphaUntilTheOwningSurfaceIsKnown)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::ThemeColor;

	const std::map<std::wstring, ThemeColor, std::less<>> colors{
		{ L"scrollbar.background", ThemeColor{ 0x12, 0x34, 0x56, 0x20 } },
		{ L"scrollbarSlider.background", ThemeColor{ 0x21, 0x43, 0x65, 0x40 } },
		{ L"scrollbarSlider.hoverBackground", ThemeColor{ 0x32, 0x54, 0x76, 0x80 } },
		{ L"scrollbarSlider.activeBackground", ThemeColor{ 0x43, 0x65, 0x87, 0xC0 } },
	};
	const auto palette = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Light, colors);

	EXPECT_EQ((ThemeColor{ 0x12, 0x34, 0x56, 0x20 }), palette.scrollbarBackground);
	EXPECT_EQ((ThemeColor{ 0x21, 0x43, 0x65, 0x40 }), palette.scrollbarSliderBackground);
	EXPECT_EQ((ThemeColor{ 0x32, 0x54, 0x76, 0x80 }), palette.scrollbarSliderHoverBackground);
	EXPECT_EQ((ThemeColor{ 0x43, 0x65, 0x87, 0xC0 }), palette.scrollbarSliderActiveBackground);
}

TEST(CColorThemeRegistry, ProjectsExplicitMinimapTokensAndDerivesSliderFallbacks)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::ThemeColor;

	const std::map<std::wstring, ThemeColor, std::less<>> fallbackColors{
		{ L"editor.background", ThemeColor{ 0x10, 0x20, 0x30, 0xFF } },
		{ L"scrollbarSlider.background", ThemeColor{ 0x21, 0x43, 0x65, 0x40 } },
		{ L"scrollbarSlider.hoverBackground", ThemeColor{ 0x32, 0x54, 0x76, 0x80 } },
		{ L"scrollbarSlider.activeBackground", ThemeColor{ 0x43, 0x65, 0x87, 0xC0 } },
	};
	auto palette = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Light, fallbackColors);
	EXPECT_EQ((ThemeColor{ 0x10, 0x20, 0x30, 0xFF }), palette.minimapBackground);
	EXPECT_EQ((ThemeColor{ 0x21, 0x43, 0x65, 0x20 }), palette.minimapSliderBackground);
	EXPECT_EQ((ThemeColor{ 0x32, 0x54, 0x76, 0x40 }), palette.minimapSliderHoverBackground);
	EXPECT_EQ((ThemeColor{ 0x43, 0x65, 0x87, 0x60 }), palette.minimapSliderActiveBackground);

	const std::map<std::wstring, ThemeColor, std::less<>> explicitColors{
		{ L"minimap.background", ThemeColor{ 0xAA, 0xBB, 0xCC, 0xFF } },
		{ L"minimap.foregroundOpacity", ThemeColor{ 0, 0, 0, 0x80 } },
		{ L"minimapSlider.background", ThemeColor{ 1, 2, 3, 0x70 } },
	};
	palette = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Dark, explicitColors);
	EXPECT_EQ((ThemeColor{ 0xAA, 0xBB, 0xCC, 0xFF }), palette.minimapBackground);
	EXPECT_EQ(0x80, palette.minimapForegroundOpacity.alpha);
	EXPECT_EQ((ThemeColor{ 1, 2, 3, 0x70 }), palette.minimapSliderBackground);
}

TEST(CColorThemeRegistry, PreservesEditorWhitespaceAlphaUntilTheEditorCanvasIsKnown)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::ThemeColor;

	const std::map<std::wstring, ThemeColor, std::less<>> colors{
		{ L"editorWhitespace.foreground", ThemeColor{ 0x12, 0x34, 0x56, 0x40 } },
	};
	const auto palette = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Light, colors);

	EXPECT_EQ((ThemeColor{ 0x12, 0x34, 0x56, 0x40 }), palette.editorWhitespaceForeground);
}

TEST(CColorThemeRegistry, ProjectsTheStableIndentGuideTokenAndDeprecatedFallback)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::ThemeColor;

	const auto deprecated = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Dark, {
		{ L"editorIndentGuide.background", ThemeColor{ 1, 2, 3, 0x80 } },
	});
	EXPECT_EQ((ThemeColor{ 1, 2, 3, 0x80 }), deprecated.editorIndentGuideBackground);

	const auto stable = CColorThemeRegistry::ProjectPalette(ColorThemeKind::Dark, {
		{ L"editorIndentGuide.background", ThemeColor{ 1, 2, 3, 0x80 } },
		{ L"editorIndentGuide.background1", ThemeColor{ 4, 5, 6, 0x90 } },
	});
	EXPECT_EQ((ThemeColor{ 4, 5, 6, 0x90 }), stable.editorIndentGuideBackground);
}


} // namespace
