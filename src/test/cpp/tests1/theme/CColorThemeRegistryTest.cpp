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
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

class TemporaryThemeExtension final {
public:
	TemporaryThemeExtension()
	{
		const auto suffix = std::to_wstring(::GetCurrentProcessId()) + L"." +
			std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
		m_root = std::filesystem::temp_directory_path() / (L"SakuraEditor.ColorThemeRegistry." + suffix);
		std::error_code error;
		std::filesystem::create_directories(m_root, error);
		if (error) m_root.clear();
	}

	~TemporaryThemeExtension()
	{
		if (!m_root.empty()) {
			std::error_code error;
			std::filesystem::remove_all(m_root, error);
		}
	}

	TemporaryThemeExtension(const TemporaryThemeExtension&) = delete;
	TemporaryThemeExtension& operator=(const TemporaryThemeExtension&) = delete;

	[[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_root; }

	[[nodiscard]] bool Write(std::wstring_view relative, std::string_view content) const
	{
		if (m_root.empty()) return false;
		const auto path = m_root / std::filesystem::path(std::wstring(relative));
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error) return false;
		std::ofstream output(path, std::ios::binary);
		if (!output) return false;
		output.write(content.data(), static_cast<std::streamsize>(content.size()));
		return output.good();
	}

private:
	std::filesystem::path m_root;
};

void WriteManifest(const TemporaryThemeExtension& extension)
{
	ASSERT_TRUE(extension.Write(L"package.json", R"json({
		// VS Code extension manifests are JSONC.
		"contributes": {
			"themes": [{
				"id": "publisher.test-theme",
				"label": "Test Theme",
				"uiTheme": "vs-dark",
				"path": "theme.json"
			}]
		}
	})json"));
}

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
	EXPECT_EQ(L"sakura.builtin", darkInfo->extensionId);
	EXPECT_EQ(L"sakura.builtin", lightInfo->extensionId);
	EXPECT_EQ(ColorThemeKind::Dark, darkInfo->kind);
	EXPECT_EQ(ColorThemeKind::Light, lightInfo->kind);
	EXPECT_TRUE(darkInfo->isBuiltin);
	EXPECT_TRUE(lightInfo->isBuiltin);

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

TEST(CColorThemeRegistry, KeepsBuiltinThemesAlongsideExtensionThemes)
{
	using theme::CColorThemeRegistry;

	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"type": "light",
		"colors": {
			"editor.background": "#ffffff",
			"sideBar.background": "#f0f0f0"
		}
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterBuiltinThemes());
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));

	const auto themes = registry.Themes();
	ASSERT_EQ(3U, themes.size());
	EXPECT_NE(themes.end(), std::find_if(themes.begin(), themes.end(), [](const auto& info) {
		return info.id == L"sakura.default-dark" && info.isBuiltin;
	}));
	EXPECT_NE(themes.end(), std::find_if(themes.begin(), themes.end(), [](const auto& info) {
		return info.id == L"sakura.default-light" && info.isBuiltin;
	}));
	EXPECT_NE(themes.end(), std::find_if(themes.begin(), themes.end(), [](const auto& info) {
		return info.id == L"publisher.test-theme" && !info.isBuiltin;
	}));

	EXPECT_TRUE(registry.Load(L"sakura.default-dark").Succeeded());
	EXPECT_TRUE(registry.Load(L"publisher.test-theme").Succeeded());
}

TEST(CColorThemeRegistry, DiscoversLoadsAndProjectsJsoncThemeWithInclude)
{
	using theme::CColorThemeRegistry;
	using theme::ColorThemeKind;
	using theme::ThemeColor;

	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"base.json", R"json({
		"colors": {
			"editor.background": "#1e1e1e",
			"sideBar.background": "#111111"
		},
		"tokenColors": [{
			"scope": ["comment", "punctuation"],
			"settings": { "foreground": "#808080", "fontStyle": "italic" }
		}]
	})json"));
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		// Root values override included values, like VS Code themes.
		"include": "base.json",
		"type": "dark",
		"colors": {
			"editor.background": "#202020",
			"editorGutter.background": "#101112",
			"editorLineNumber.foreground": "#858585",
			"editorLineNumber.activeForeground": "#cccccc",
			"sideBar.background": "#293134",
			"panel.background": "#252526",
			"terminal.background": "#12345680",
			"foreground": "#d4d4d4",
			"descriptionForeground": "#ffffffb3",
			"disabledForeground": "#ffffff80",
			"activityBar.background": "#333333",
			"focusBorder": "#0078d4"
		},
		"tokenColors": [{
			"scope": "string",
			"settings": { "foreground": "#ce9178" }
		}],
		"semanticTokenColors": {
			"variable.readonly": { "foreground": "#ff0000" }
		},
		"semanticHighlighting": true
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));
	const auto themes = registry.Themes();
	ASSERT_EQ(1U, themes.size());
	EXPECT_EQ(L"publisher.test-theme", themes[0].extensionId);
	EXPECT_EQ(L"publisher.test-theme", themes[0].id);
	EXPECT_EQ(L"Test Theme", themes[0].label);
	EXPECT_EQ(ColorThemeKind::Dark, themes[0].kind);

	const auto loaded = registry.Load(L"test theme");
	ASSERT_TRUE(loaded.Succeeded()) << loaded.diagnostic.c_str();
	ASSERT_TRUE(loaded.theme.has_value());
	const auto& snapshot = *loaded.theme;
	EXPECT_EQ(L"Test Theme", snapshot.info.label);
	EXPECT_EQ((ThemeColor{ 0x20, 0x20, 0x20, 0xFF }), snapshot.colors.at(L"editor.background"));
	EXPECT_EQ((ThemeColor{ 0x29, 0x31, 0x34, 0xFF }), snapshot.colors.at(L"sideBar.background"));
	EXPECT_EQ(2U, snapshot.tokenColors.size());
	ASSERT_EQ(1U, snapshot.semanticTokenColors.size());
	EXPECT_EQ((ThemeColor{ 0xFF, 0, 0, 0xFF }), snapshot.semanticTokenColors.at(L"variable.readonly"));
	EXPECT_TRUE(snapshot.semanticHighlighting);

	EXPECT_EQ((ThemeColor{ 0x20, 0x20, 0x20, 0xFF }), snapshot.palette.canvas);
	EXPECT_EQ((ThemeColor{ 0x10, 0x11, 0x12, 0xFF }), snapshot.palette.editorGutterBackground);
	EXPECT_EQ((ThemeColor{ 0x85, 0x85, 0x85, 0xFF }), snapshot.palette.editorLineNumberForeground);
	EXPECT_EQ((ThemeColor{ 0xCC, 0xCC, 0xCC, 0xFF }), snapshot.palette.editorLineNumberActiveForeground);
	EXPECT_EQ((ThemeColor{ 0x29, 0x31, 0x34, 0xFF }), snapshot.palette.sideBar);
	EXPECT_EQ((ThemeColor{ 0x25, 0x25, 0x26, 0xFF }), snapshot.palette.panel);
	EXPECT_EQ((ThemeColor{ 0x25, 0x25, 0x26, 0xFF }), snapshot.palette.bottomPanel);
	EXPECT_EQ((ThemeColor{ 0x1B, 0x2D, 0x3E, 0xFF }), snapshot.palette.terminalBackground);
	EXPECT_EQ((ThemeColor{ 0xD4, 0xD4, 0xD4, 0xFF }), snapshot.palette.primaryText);
	EXPECT_EQ((ThemeColor{ 189, 189, 189, 0xFF }), snapshot.palette.descriptionText);
	EXPECT_EQ((ThemeColor{ 144, 144, 144, 0xFF }), snapshot.palette.disabledText);
	EXPECT_EQ((ThemeColor{ 0x33, 0x33, 0x33, 0xFF }), snapshot.palette.activityBar);
	EXPECT_EQ((ThemeColor{ 0x00, 0x78, 0xD4, 0xFF }), snapshot.palette.accent);
	// statusBarItem.prominentBackground: not set by this theme, so it falls back to
	// black.transparent(0.5) composited over the *resolved* accent above (#0078D4),
	// not over the compiled default (#1F8AD2) — proving the JSON-key mapping actually
	// composites over what the theme itself resolved.
	EXPECT_EQ((ThemeColor{ 0x00, 0x3C, 0x6A, 0xFF }), snapshot.palette.statusBarProminentBackground);
	// Neither `banner.*` nor either of their alias candidates
	// (`list.activeSelectionBackground`/`list.activeSelectionForeground`/
	// `editorInfo.foreground`) is set by this theme, so all three roles fall back to
	// the compiled dark defaults untouched.
	EXPECT_EQ((ThemeColor{ 0x04, 0x39, 0x5E, 0xFF }), snapshot.palette.bannerBackground);
	EXPECT_EQ((ThemeColor{ 0xFF, 0xFF, 0xFF, 0xFF }), snapshot.palette.bannerForeground);
	EXPECT_EQ((ThemeColor{ 0x59, 0xA4, 0xF9, 0xFF }), snapshot.palette.bannerIconForeground);
	ASSERT_TRUE(snapshot.syntaxPalette.string.foreground.has_value());
	EXPECT_EQ((ThemeColor{ 0xCE, 0x91, 0x78, 0xFF }), *snapshot.syntaxPalette.string.foreground);
	ASSERT_TRUE(snapshot.syntaxPalette.variable.foreground.has_value());
	EXPECT_EQ((ThemeColor{ 0xFF, 0, 0, 0xFF }), *snapshot.syntaxPalette.variable.foreground);
}

TEST(CColorThemeRegistry, FallsBackToProjectedPanelBackgroundWhenTerminalTokenIsAbsent)
{
	using theme::CColorThemeRegistry;
	using theme::ThemeColor;

	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"type": "dark",
		"colors": {
			"editor.background": "#202122",
			"panel.background": "#123456"
		}
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));
	const auto loaded = registry.Load(L"publisher.test-theme");
	ASSERT_TRUE(loaded.Succeeded()) << loaded.diagnostic.c_str();
	ASSERT_TRUE(loaded.theme.has_value());
	const auto& snapshot = *loaded.theme;
	EXPECT_EQ(snapshot.colors.end(), snapshot.colors.find(L"terminal.background"));
	EXPECT_EQ((ThemeColor{ 0x12, 0x34, 0x56, 0xFF }), snapshot.palette.bottomPanel);
	EXPECT_EQ(snapshot.palette.bottomPanel, snapshot.palette.terminalBackground);
	EXPECT_EQ((ThemeColor{ 0x20, 0x21, 0x22, 0xFF }), snapshot.palette.editorGutterBackground);
	// No `focusBorder`/`textLink.foreground`/`button.background`/`activityBarBadge.background`
	// override here, so accent stays the compiled dark default (#1F8AD2) and
	// statusBarItem.prominentBackground's fallback composites over exactly that,
	// matching the compiled ThemePalette default field-for-field.
	EXPECT_EQ((ThemeColor{ 0x0F, 0x45, 0x69, 0xFF }), snapshot.palette.statusBarProminentBackground);
}

TEST(CColorThemeRegistry, ProjectsBannerRolesFromAliasedTokensWhenBannerKeysAreAbsent)
{
	using theme::CColorThemeRegistry;
	using theme::ThemeColor;

	// `banner.background`/`banner.foreground`/`banner.iconForeground` are each registered
	// upstream as a bare alias of another color, not an independent per-kind object. A
	// theme that sets only the aliased key must still resolve the banner role through it.
	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"type": "dark",
		"colors": {
			"editor.background": "#202020",
			"list.activeSelectionBackground": "#334455",
			"list.activeSelectionForeground": "#eeeeee",
			"editorInfo.foreground": "#66ccff"
		}
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));
	const auto loaded = registry.Load(L"publisher.test-theme");
	ASSERT_TRUE(loaded.Succeeded()) << loaded.diagnostic.c_str();
	ASSERT_TRUE(loaded.theme.has_value());
	const auto& snapshot = *loaded.theme;
	EXPECT_EQ((ThemeColor{ 0x33, 0x44, 0x55, 0xFF }), snapshot.palette.bannerBackground);
	EXPECT_EQ((ThemeColor{ 0xEE, 0xEE, 0xEE, 0xFF }), snapshot.palette.bannerForeground);
	EXPECT_EQ((ThemeColor{ 0x66, 0xCC, 0xFF, 0xFF }), snapshot.palette.bannerIconForeground);
}

TEST(CColorThemeRegistry, PrefersDirectBannerTokensOverTheirAliasedCandidates)
{
	using theme::CColorThemeRegistry;
	using theme::ThemeColor;

	// A theme that sets `banner.*` directly must win over its alias candidate, the same
	// first-listed-candidate priority every other multi-source role in ProjectPalette uses.
	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"type": "dark",
		"colors": {
			"editor.background": "#202020",
			"banner.background": "#112233",
			"banner.foreground": "#ddddff",
			"banner.iconForeground": "#ffcc00",
			"list.activeSelectionBackground": "#334455",
			"list.activeSelectionForeground": "#eeeeee",
			"editorInfo.foreground": "#66ccff"
		}
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));
	const auto loaded = registry.Load(L"publisher.test-theme");
	ASSERT_TRUE(loaded.Succeeded()) << loaded.diagnostic.c_str();
	ASSERT_TRUE(loaded.theme.has_value());
	const auto& snapshot = *loaded.theme;
	EXPECT_EQ((ThemeColor{ 0x11, 0x22, 0x33, 0xFF }), snapshot.palette.bannerBackground);
	EXPECT_EQ((ThemeColor{ 0xDD, 0xDD, 0xFF, 0xFF }), snapshot.palette.bannerForeground);
	EXPECT_EQ((ThemeColor{ 0xFF, 0xCC, 0x00, 0xFF }), snapshot.palette.bannerIconForeground);
}

TEST(CColorThemeRegistry, RejectsThemeIncludesOutsideExtensionRoot)
{
	using theme::CColorThemeRegistry;

	TemporaryThemeExtension extension;
	ASSERT_FALSE(extension.Root().empty());
	ASSERT_TRUE(extension.Write(L"theme.json", R"json({
		"include": "../outside.json",
		"colors": { "editor.background": "#ffffff" }
	})json"));
	WriteManifest(extension);

	CColorThemeRegistry registry;
	ASSERT_TRUE(registry.RegisterBuiltinThemes());
	const auto builtinBefore = registry.Load(L"sakura.default-dark");
	ASSERT_TRUE(builtinBefore.Succeeded());
	ASSERT_TRUE(builtinBefore.theme.has_value());
	ASSERT_TRUE(registry.RegisterExtension(L"publisher.test-theme", extension.Root()));
	const auto loaded = registry.Load(L"publisher.test-theme");
	EXPECT_FALSE(loaded.Succeeded());
	EXPECT_NE(std::wstring::npos, loaded.diagnostic.find(L"outside"));
	const auto builtinAfter = registry.Load(L"sakura.default-dark");
	ASSERT_TRUE(builtinAfter.Succeeded());
	ASSERT_TRUE(builtinAfter.theme.has_value());
	EXPECT_EQ(builtinBefore.theme->palette, builtinAfter.theme->palette);
}

} // namespace
