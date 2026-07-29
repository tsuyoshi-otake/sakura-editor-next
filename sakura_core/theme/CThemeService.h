/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <cstdint>

namespace theme {

//! Persisted user preference.  High Contrast is intentionally not represented here.
enum class ThemeMode : std::uint8_t {
	Light,
	Dark,
};

//! An sRGB color.  This remains independent of COLORREF so palette selection is pure.
struct ThemeColor {
	std::uint8_t red = 0;
	std::uint8_t green = 0;
	std::uint8_t blue = 0;

	[[nodiscard]] constexpr bool operator==(const ThemeColor&) const noexcept = default;
	[[nodiscard]] constexpr COLORREF ToColorRef() const noexcept
	{
		return RGB(red, green, blue);
	}
};

//! The complete set of colors used by Sakura-owned workbench chrome.
struct ThemePalette {
	ThemeColor canvas;
	ThemeColor panel;
	ThemeColor raised;
	ThemeColor border;
	ThemeColor primaryText;
	ThemeColor secondaryText;
	ThemeColor accent;
	//! Text drawn over the system highlight/accent color (COLOR_HIGHLIGHTTEXT in High Contrast).
	ThemeColor highlightText;
	ThemeColor titleBar;
	ThemeColor activityBar;
	ThemeColor danger;

	[[nodiscard]] constexpr bool operator==(const ThemePalette&) const noexcept = default;
};

//! Font role used by the native workbench chrome and GDI terminal renderer.
enum class ThemeFontKind : std::uint8_t {
	Chrome,
	Editor,
	Terminal,
};

//! The first family is tried before the fallback.  Point size is in typographic points.
struct ThemeFontSpec {
	const wchar_t* preferredFamily = L"";
	const wchar_t* fallbackFamily = L"";
	int pointSize = 9;
	int weight = FW_NORMAL;
	bool fixedPitch = false;
};

//! An owning HFONT.  Recreate is explicit so every DPI transition has a single owner.
class CThemeFont final {
public:
	CThemeFont() noexcept = default;
	~CThemeFont() noexcept;
	CThemeFont(const CThemeFont&) = delete;
	CThemeFont& operator=(const CThemeFont&) = delete;
	CThemeFont(CThemeFont&& other) noexcept;
	CThemeFont& operator=(CThemeFont&& other) noexcept;

	//! Replaces this font using the supplied physical DPI.  Returns false without replacing on failure.
	[[nodiscard]] bool Recreate(ThemeFontKind kind, UINT dpi, int pointSize = 0) noexcept;
	//! Same as Recreate, obtaining the current per-window DPI with GetDpiForWindow.
	[[nodiscard]] bool RecreateForWindow(ThemeFontKind kind, HWND window, int pointSize = 0) noexcept;
	void Reset() noexcept;
	[[nodiscard]] HFONT Get() const noexcept { return m_font; }
	[[nodiscard]] UINT Dpi() const noexcept { return m_dpi; }

private:
	HFONT m_font = nullptr;
	UINT m_dpi = 0;
};

class CThemeService final {
public:
	CThemeService() = delete;

	//! New profiles are dark; callers preserve an explicitly saved bDarkMode value.
	[[nodiscard]] static constexpr ThemeMode DefaultMode() noexcept { return ThemeMode::Dark; }
	//! Pure, deterministic Sakura palette selection for a persisted mode.
	[[nodiscard]] static constexpr ThemePalette PaletteFor(ThemeMode mode) noexcept;
	//! Pure selection hook for tests and callers that already acquired system high-contrast colors.
	[[nodiscard]] static constexpr ThemePalette SelectPalette(
		ThemeMode savedMode,
		bool highContrastActive,
		const ThemePalette& highContrastPalette
	) noexcept;
	//! Queries SPI_GETHIGHCONTRAST. Failure is treated as not active.
	[[nodiscard]] static bool IsHighContrastActive() noexcept;
	//! Builds a palette from system colors. This never writes or changes the saved ThemeMode.
	[[nodiscard]] static ThemePalette HighContrastPalette() noexcept;
	//! Uses the system palette only while High Contrast is active.
	[[nodiscard]] static ThemePalette EffectivePalette(ThemeMode savedMode) noexcept;
	//! Preferred/fallback font policy. Creation verifies installed faces at runtime.
	[[nodiscard]] static constexpr ThemeFontSpec FontSpec(ThemeFontKind kind) noexcept;
	//! Resolves the preferred family when installed, otherwise returns the fallback family.
	[[nodiscard]] static const wchar_t* ResolveFontFamily(ThemeFontKind kind) noexcept;
};

constexpr ThemePalette CThemeService::PaletteFor(ThemeMode mode) noexcept
{
	if (mode == ThemeMode::Light) {
		return {
			{ 0xF4, 0xF5, 0xF7 }, // canvas
			{ 0xFF, 0xFF, 0xFF }, // panel
			{ 0xE9, 0xEC, 0xF1 }, // raised
			{ 0xCD, 0xD2, 0xDB }, // border
			{ 0x1F, 0x23, 0x29 }, // primary text
			{ 0x5C, 0x65, 0x73 }, // secondary text
			{ 0xB8, 0x32, 0x68 }, // Sakura accent / focus
			{ 0xFF, 0xFF, 0xFF }, // highlighted text
			{ 0xF3, 0xF3, 0xF3 }, // title bar
			{ 0xF3, 0xF3, 0xF3 }, // activity bar
			{ 0xC4, 0x2B, 0x1C }, // destructive hover
		};
	}
	return {
		{ 0x1E, 0x1E, 0x1E }, // canvas: classic editor charcoal
		{ 0x25, 0x25, 0x26 }, // panel: distinguish chrome from the editor surface
		{ 0x2A, 0x2D, 0x2E }, // raised / hover
		{ 0x45, 0x45, 0x45 }, // border
		{ 0xCC, 0xCC, 0xCC }, // primary text
		{ 0x96, 0x96, 0x96 }, // secondary text
		{ 0x1F, 0x8A, 0xD2 }, // focus / active status
		{ 0xFF, 0xFF, 0xFF }, // highlighted text
		{ 0x3C, 0x3C, 0x3C }, // active title bar
		{ 0x33, 0x33, 0x33 }, // activity bar
		{ 0xC4, 0x2B, 0x1C }, // destructive hover
	};
}

constexpr ThemePalette CThemeService::SelectPalette(
	ThemeMode savedMode,
	bool highContrastActive,
	const ThemePalette& highContrastPalette
) noexcept
{
	return highContrastActive ? highContrastPalette : PaletteFor(savedMode);
}

constexpr ThemeFontSpec CThemeService::FontSpec(ThemeFontKind kind) noexcept
{
	switch (kind) {
	case ThemeFontKind::Chrome:
		return { L"Segoe UI Variable", L"Segoe UI", 8, FW_NORMAL, false };
	case ThemeFontKind::Editor:
		// VS Code's Windows editor density is approximately 14 device pixels at 100% DPI.
		return { L"Consolas", L"Cascadia Mono", 10, FW_NORMAL, true };
	case ThemeFontKind::Terminal:
	default:
		return { L"Cascadia Mono", L"Consolas", 9, FW_LIGHT, true };
	}
}

} // namespace theme
