/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <cstdint>
#include <optional>

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
	//! VS Code theme JSON may carry an alpha channel. Native GDI consumers use
	//! ToColorRef(), while the color-theme loader composites translucent values
	//! before projecting them into ThemePalette.
	std::uint8_t alpha = 0xFF;

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
	//! VS Code `descriptionForeground`. GDI text has no alpha channel, so the CSS
	//! `transparent(foreground, 0.7)` token is composited over `canvas` at design time.
	ThemeColor descriptionText;
	//! VS Code `disabledForeground`, composited over `canvas` for the same reason.
	ThemeColor disabledText;
	ThemeColor accent;
	//! Text drawn over the system highlight/accent color (COLOR_HIGHLIGHTTEXT in High Contrast).
	ThemeColor highlightText;
	ThemeColor titleBar;
	ThemeColor activityBar;
	ThemeColor danger;
	ThemeColor warning;
	//! VS Code `panel.background`, distinct from the Primary/Secondary Side Bar.
	//! Existing consumers that only know the legacy panel role continue to use `panel`.
	ThemeColor bottomPanel = { 0x25, 0x25, 0x26 };
	//! VS Code `sideBar.background`, used by both Primary and Secondary Side Bar
	//! hosts; Explorer receives it while located in the Primary Side Bar.
	//! This is separate from `bottomPanel` because Side Bar and Panel are distinct Parts.
	ThemeColor sideBar = { 0x25, 0x25, 0x26 };
	//! VS Code `terminal.background`. The Terminal ViewContainer belongs to the
	//! Panel; color-theme projection falls back to the resolved `bottomPanel` role
	//! when this optional token is absent.
	ThemeColor terminalBackground = { 0x25, 0x25, 0x26 };
	//! VS Code `editorGutter.background`; falls back to `editor.background`.
	ThemeColor editorGutterBackground = { 0x1E, 0x1E, 0x1E };
	//! VS Code `editorLineNumber.foreground`.
	ThemeColor editorLineNumberForeground = { 0x85, 0x85, 0x85 };
	//! VS Code `editorLineNumber.activeForeground`.
	ThemeColor editorLineNumberActiveForeground = { 0xCC, 0xCC, 0xCC };
	//! VS Code `diffEditor.insertedLineBackground`, whose registered default is the
	//! shared `defaultInsertColor` = `rgba(155, 185, 85, .2)` for both dark and
	//! light. GDI has no alpha, so it is composited over `canvas` at design time.
	ThemeColor diffInsertedLineBackground = { 0x37, 0x3D, 0x29 };
	//! VS Code `diffEditor.removedLineBackground`, registered default
	//! `defaultRemoveColor` = `rgba(255, 0, 0, .2)`, composited the same way.
	ThemeColor diffRemovedLineBackground = { 0x4B, 0x18, 0x18 };
	//! VS Code `diffEditor.diagonalFill` (`#cccccc33` dark, `#22222233` light):
	//! the hatch drawn where one side of a side-by-side diff has no line at all.
	ThemeColor diffDiagonalFill = { 0x41, 0x41, 0x41 };
	//! VS Code `button.background`. `media/updateTitleBarEntry.css` paints the
	//! actionable ("prominent") title-bar Update button with
	//! `--vscode-button-background` / `--vscode-button-foreground` and hovers it
	//! with `--vscode-button-hoverBackground`, so the Update indicator needs the
	//! button role rather than the badge role. Sakura's built-in defaults keep the
	//! product's own accent instead of importing VS Code's `#0E639C`/`#007ACC`.
	ThemeColor buttonBackground = { 0x1F, 0x8A, 0xD2 };
	//! VS Code `button.foreground`.
	ThemeColor buttonForeground = { 0xFF, 0xFF, 0xFF };
	//! VS Code `button.hoverBackground`, registered upstream as
	//! `lighten(button.background, 0.2)` for dark and `darken(..., 0.2)` for light.
	ThemeColor buttonHoverBackground = { 0x3F, 0xA1, 0xE3 };

	[[nodiscard]] constexpr bool operator==(const ThemePalette&) const noexcept = default;
};

//! Coarse native editor categories projected from VS Code TextMate/semantic scopes.
//! The legacy renderer has no TextMate scope tree, so this is deliberately a
//! typed category boundary rather than a claim of full grammar compatibility.
enum class ThemeSyntaxTokenKind : std::uint8_t {
	Comment,
	String,
	Number,
	Keyword,
	Type,
	Function,
	Variable,
	Constant,
	Regexp,
	Tag,
	Attribute,
	Invalid,
};

struct ThemeSyntaxStyle final {
	std::optional<ThemeColor> foreground;
	std::optional<ThemeColor> background;
	bool bold = false;
	bool underline = false;

	[[nodiscard]] constexpr bool operator==(const ThemeSyntaxStyle&) const noexcept = default;
};

//! Projected syntax colors consumed by Sakura's existing syntax-category renderer.
struct ThemeSyntaxPalette final {
	ThemeSyntaxStyle comment;
	ThemeSyntaxStyle string;
	ThemeSyntaxStyle number;
	ThemeSyntaxStyle keyword;
	ThemeSyntaxStyle type;
	ThemeSyntaxStyle function;
	ThemeSyntaxStyle variable;
	ThemeSyntaxStyle constant;
	ThemeSyntaxStyle regexp;
	ThemeSyntaxStyle tag;
	ThemeSyntaxStyle attribute;
	ThemeSyntaxStyle invalid;

	[[nodiscard]] ThemeSyntaxStyle& For(ThemeSyntaxTokenKind kind) noexcept
	{
		switch (kind) {
		case ThemeSyntaxTokenKind::Comment: return comment;
		case ThemeSyntaxTokenKind::String: return string;
		case ThemeSyntaxTokenKind::Number: return number;
		case ThemeSyntaxTokenKind::Keyword: return keyword;
		case ThemeSyntaxTokenKind::Type: return type;
		case ThemeSyntaxTokenKind::Function: return function;
		case ThemeSyntaxTokenKind::Variable: return variable;
		case ThemeSyntaxTokenKind::Constant: return constant;
		case ThemeSyntaxTokenKind::Regexp: return regexp;
		case ThemeSyntaxTokenKind::Tag: return tag;
		case ThemeSyntaxTokenKind::Attribute: return attribute;
		case ThemeSyntaxTokenKind::Invalid: return invalid;
		}
		return invalid;
	}

	[[nodiscard]] const ThemeSyntaxStyle& For(ThemeSyntaxTokenKind kind) const noexcept
	{
		return const_cast<ThemeSyntaxPalette*>(this)->For(kind);
	}

	[[nodiscard]] constexpr bool operator==(const ThemeSyntaxPalette&) const noexcept = default;
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
	//! Installs the currently selected VS Code color-theme projection for this
	//! editor process. High Contrast still takes precedence at read time.
	static void SetActiveColorThemePalette(const ThemePalette& palette) noexcept;
	static void ClearActiveColorThemePalette() noexcept;
	[[nodiscard]] static bool HasActiveColorThemePalette() noexcept;
	//! Installs the projected token/semantic colors for the selected VS Code theme.
	//! High Contrast suppresses this overlay at read time, like the palette overlay.
	static void SetActiveColorThemeSyntaxPalette(const ThemeSyntaxPalette& palette) noexcept;
	static void ClearActiveColorThemeSyntaxPalette() noexcept;
	[[nodiscard]] static bool HasActiveColorThemeSyntaxPalette() noexcept;
	//! Returns the process-local projected syntax palette without copying. The
	//! pointer is valid until the next theme application on the UI thread.
	[[nodiscard]] static const ThemeSyntaxPalette* ActiveColorThemeSyntaxPalette() noexcept;
	[[nodiscard]] static ThemeSyntaxPalette EffectiveSyntaxPalette(ThemeMode savedMode) noexcept;
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
			{ 0x71, 0x71, 0x71 }, // description text: VS Code light descriptionForeground literal
			{ 0xAA, 0xAB, 0xAC }, // disabled text: #61616180 composited over the light canvas
			{ 0xB8, 0x32, 0x68 }, // Sakura accent / focus
			{ 0xFF, 0xFF, 0xFF }, // highlighted text
			{ 0xF3, 0xF3, 0xF3 }, // title bar
			{ 0xF3, 0xF3, 0xF3 }, // activity bar
			{ 0xC4, 0x2B, 0x1C }, // destructive hover
			{ 0xBF, 0x88, 0x00 }, // notificationsWarningIcon.foreground
			{ 0xFF, 0xFF, 0xFF }, // panel.background
			{ 0xFF, 0xFF, 0xFF }, // sideBar.background
			{ 0xFF, 0xFF, 0xFF }, // terminal.background fallback: panel.background
			{ 0xF4, 0xF5, 0xF7 }, // editorGutter.background fallback: editor.background
			{ 0x23, 0x78, 0x93 }, // editorLineNumber.foreground
			{ 0x17, 0x11, 0x84 }, // editorLineNumber.activeForeground
			{ 0xE2, 0xE9, 0xD7 }, // diffEditor.insertedLineBackground: rgba(155,185,85,.2) over the light canvas
			{ 0xF6, 0xC4, 0xC6 }, // diffEditor.removedLineBackground: rgba(255,0,0,.2) over the light canvas
			{ 0xCA, 0xCB, 0xCC }, // diffEditor.diagonalFill: #22222233 over the light canvas
			{ 0xB8, 0x32, 0x68 }, // button.background: the Sakura light accent, not VS Code's #007ACC
			{ 0xFF, 0xFF, 0xFF }, // button.foreground
			{ 0x93, 0x28, 0x53 }, // button.hoverBackground: darken(button.background, 0.2) as upstream registers it for light
		};
	}
	return {
		{ 0x1E, 0x1E, 0x1E }, // canvas: classic editor charcoal
		{ 0x25, 0x25, 0x26 }, // panel: distinguish chrome from the editor surface
		{ 0x2A, 0x2D, 0x2E }, // raised / hover
		{ 0x45, 0x45, 0x45 }, // border
		{ 0xCC, 0xCC, 0xCC }, // primary text
		{ 0x96, 0x96, 0x96 }, // secondary text
		{ 0x98, 0x98, 0x98 }, // description text: transparent(#CCCCCC, 0.7) over the #1E1E1E canvas
		{ 0x75, 0x75, 0x75 }, // disabled text: #CCCCCC80 composited over the same canvas
		{ 0x1F, 0x8A, 0xD2 }, // focus / active status
		{ 0xFF, 0xFF, 0xFF }, // highlighted text
		{ 0x3C, 0x3C, 0x3C }, // active title bar
		{ 0x33, 0x33, 0x33 }, // activity bar
		{ 0xC4, 0x2B, 0x1C }, // destructive hover
		{ 0xCC, 0xA7, 0x00 }, // notificationsWarningIcon.foreground
		{ 0x25, 0x25, 0x26 }, // panel.background
		{ 0x29, 0x31, 0x34 }, // sideBar.background
		{ 0x25, 0x25, 0x26 }, // terminal.background fallback: panel.background
		{ 0x1E, 0x1E, 0x1E }, // editorGutter.background fallback: editor.background
		{ 0x85, 0x85, 0x85 }, // editorLineNumber.foreground
		{ 0xCC, 0xCC, 0xCC }, // editorLineNumber.activeForeground
		{ 0x37, 0x3D, 0x29 }, // diffEditor.insertedLineBackground: rgba(155,185,85,.2) over the dark canvas
		{ 0x4B, 0x18, 0x18 }, // diffEditor.removedLineBackground: rgba(255,0,0,.2) over the dark canvas
		{ 0x41, 0x41, 0x41 }, // diffEditor.diagonalFill: #cccccc33 over the dark canvas
		{ 0x1F, 0x8A, 0xD2 }, // button.background: the Sakura dark accent, not VS Code's #0E639C
		{ 0xFF, 0xFF, 0xFF }, // button.foreground
		{ 0x3F, 0xA1, 0xE3 }, // button.hoverBackground: lighten(button.background, 0.2) as upstream registers it for dark
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
		// VS Code's terminal defaults to a normal weight.  FW_LIGHT makes small
		// rasterized strokes disappear before ClearType can make them readable.
		return { L"Cascadia Mono", L"Consolas", 9, FW_NORMAL, true };
	}
}

} // namespace theme
