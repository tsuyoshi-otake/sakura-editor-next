/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "theme/CThemeService.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace theme {

//! The four values used by VS Code's ColorThemeKind API and theme manifests.
enum class ColorThemeKind : std::uint8_t {
	Light,
	Dark,
	HighContrast,
	HighContrastLight,
};

struct ColorThemeInfo final {
	std::wstring id;
	std::wstring label;
	std::wstring extensionId;
	std::filesystem::path extensionRoot;
	std::filesystem::path themePath;
	ColorThemeKind kind = ColorThemeKind::Dark;
	bool isBuiltin = false;

	[[nodiscard]] bool operator==(const ColorThemeInfo&) const noexcept = default;
};

//! Parsed token-color metadata is retained before being projected into Sakura's
//! coarse native syntax categories. The original scopes remain available for
//! future grammar-aware support and for diagnostics.
struct ThemeTokenColorRule final {
	std::vector<std::wstring> scopes;
	std::optional<ThemeColor> foreground;
	std::optional<ThemeColor> background;
	std::wstring fontStyle;
};

struct ColorThemeSnapshot final {
	ColorThemeInfo info;
	std::map<std::wstring, ThemeColor, std::less<>> colors;
	std::vector<ThemeTokenColorRule> tokenColors;
	std::map<std::wstring, ThemeColor, std::less<>> semanticTokenColors;
	bool semanticHighlighting = false;
	ThemePalette palette{};
	ThemeSyntaxPalette syntaxPalette{};
};

struct ColorThemeLoadResult final {
	std::optional<ColorThemeSnapshot> theme;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return theme.has_value(); }
};

//! Window-local registry of VS Code `contributes.themes` entries discovered in
//! installed extension roots. The registry does not perform network or install
//! work; OpenVSX installation remains owned by CExtensionPane/CExtensionManager.
class CColorThemeRegistry final {
public:
	CColorThemeRegistry() = default;
	CColorThemeRegistry(const CColorThemeRegistry&) = delete;
	CColorThemeRegistry& operator=(const CColorThemeRegistry&) = delete;

	//! Replaces any prior entries owned by extensionId. Returns false when the
	//! manifest cannot be parsed or contains no valid theme contribution.
	[[nodiscard]] bool RegisterExtension(
		std::wstring_view extensionId, const std::filesystem::path& extensionRoot);
	//! Registers Sakura's own defaults as VS Code JSONC theme documents. The
	//! definitions remain selectable and load through the same projection path as
	//! extension-contributed themes.
	[[nodiscard]] bool RegisterBuiltinThemes();
	void Clear() noexcept;

	[[nodiscard]] std::vector<ColorThemeInfo> Themes() const;
	//! Resolves by stable manifest id first, then by the user-facing label. Label
	//! matching is case-insensitive to match VS Code's setting behavior in practice.
	[[nodiscard]] ColorThemeLoadResult Load(std::wstring_view idOrLabel) const;
	//! Stable IDs used when no explicit workbench.colorTheme setting exists.
	[[nodiscard]] static constexpr std::wstring_view BuiltinThemeId(ThemeMode mode) noexcept
	{
		return mode == ThemeMode::Light ? L"sakura.default-light" : L"sakura.default-dark";
	}

	//! Supports VS Code's documented #RGB/#RGBA/#RRGGBB/#RRGGBBAA values and the
	//! common transparent/rgb()/rgba() forms found in published themes.
	[[nodiscard]] static std::optional<ThemeColor> ParseColor(std::wstring_view value) noexcept;
	[[nodiscard]] static ThemeMode ModeForKind(ColorThemeKind kind) noexcept;
	//! Maps the standard VS Code workbench color IDs into Sakura's semantic palette.
	[[nodiscard]] static ThemePalette ProjectPalette(
		ColorThemeKind kind, const std::map<std::wstring, ThemeColor, std::less<>>& colors) noexcept;
	//! Maps standard TextMate scopes and semantic token names into the native
	//! renderer's typed categories. The mapping is intentionally fail-closed for
	//! scopes that do not have a Sakura equivalent.
	[[nodiscard]] static ThemeSyntaxPalette ProjectSyntaxPalette(
		const std::vector<ThemeTokenColorRule>& tokenColors,
		const std::map<std::wstring, ThemeColor, std::less<>>& semanticTokenColors,
		bool semanticHighlighting) noexcept;

private:
	std::vector<ColorThemeInfo> m_themes;
	std::map<std::wstring, std::string, std::less<>> m_builtinThemeDocuments;
};

} // namespace theme
