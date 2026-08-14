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
	EXPECT_EQ((ThemeColor{ 0xF4, 0xF5, 0xF7, 0xFF }), light.theme->colors.at(L"editor.background"));
	EXPECT_FALSE(light.theme->tokenColors.empty());
	EXPECT_TRUE(light.theme->semanticTokenColors.empty());
	EXPECT_FALSE(light.theme->semanticHighlighting);
	EXPECT_EQ(CThemeService::PaletteFor(ThemeMode::Light), light.theme->palette);

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


} // namespace
