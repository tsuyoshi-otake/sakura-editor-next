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

//! Composites one VS Code theme color over an opaque native surface.
//! Scrollbar slider tokens deliberately retain their alpha until the owning
//! surface is known, because the same slider is painted over Editor, Side Bar,
//! Panel, Markdown preview, and Quick Input backgrounds.
[[nodiscard]] constexpr ThemeColor CompositeThemeColor(
	ThemeColor foreground, ThemeColor background) noexcept
{
	if (foreground.alpha == 0xFF) {
		return { foreground.red, foreground.green, foreground.blue, 0xFF };
	}
	if (foreground.alpha == 0) {
		return { background.red, background.green, background.blue, 0xFF };
	}
	const unsigned int alpha = foreground.alpha;
	const unsigned int inverse = 0xFFU - alpha;
	return {
		static_cast<std::uint8_t>((foreground.red * alpha + background.red * inverse + 127U) / 255U),
		static_cast<std::uint8_t>((foreground.green * alpha + background.green * inverse + 127U) / 255U),
		static_cast<std::uint8_t>((foreground.blue * alpha + background.blue * inverse + 127U) / 255U),
		0xFF,
	};
}

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
	//! VS Code `editor.findMatchHighlightBackground`, which the Search view's result
	//! rows use to highlight the matched span. Its registered dark default is
	//! `#EA5C0055`; GDI has no alpha channel, so the token is composited over
	//! `sideBar` at design time exactly as the diff line tokens are.
	ThemeColor searchMatchHighlightBackground = { 0x66, 0x37, 0x19 };
	//! VS Code `button.background`. `media/updateTitleBarEntry.css` paints the
	//! actionable ("prominent") title-bar Update button with
	//! `--vscode-button-background` / `--vscode-button-foreground` and hovers it
	//! with `--vscode-button-hoverBackground`, so the Update indicator needs the
	//! button role rather than the badge role. The dark fallback keeps Sakura's
	//! product accent; the bundled Light theme supplies VS Code Light Modern's
	//! `#005FB8` explicitly.
	ThemeColor buttonBackground = { 0x1F, 0x8A, 0xD2 };
	//! VS Code `button.foreground`.
	ThemeColor buttonForeground = { 0xFF, 0xFF, 0xFF };
	//! VS Code `button.hoverBackground`, registered upstream as
	//! `lighten(button.background, 0.2)` for dark and `darken(..., 0.2)` for light.
	ThemeColor buttonHoverBackground = { 0x3F, 0xA1, 0xE3 };
	//! VS Code `activityBarBadge.background`. The badge is its own role: it is not
	//! `accent` (which answers "what does focus look like") and not `button.*`
	//! (which answers "what does an actionable control look like"). `accent` may
	//! already have consumed this key as its fourth fallback candidate, which is a
	//! different question from this one. Upstream's dark default is `#007ACC`.
	ThemeColor activityBarBadgeBackground = { 0x00, 0x7A, 0xCC };
	//! VS Code `activityBarBadge.foreground`, whose registered default is `#FFFFFF`
	//! for every theme kind.
	ThemeColor activityBarBadgeForeground = { 0xFF, 0xFF, 0xFF };
	/*!
		@brief The Git extension's `gitDecoration.*` roles, in its own declared order.

		These are contributed by the built-in Git extension rather than registered by
		the workbench, so their defaults are that extension's `contributes.colors`
		values verbatim rather than a Sakura brand color. A theme overriding one of
		them is overriding a Git key, not a workbench key, which is why they are named
		after the provider and not after the surface that paints them.
	*/
	ThemeColor gitAddedResourceForeground = { 0x81, 0xB8, 0x8B };
	ThemeColor gitModifiedResourceForeground = { 0xE2, 0xC0, 0x8D };
	ThemeColor gitDeletedResourceForeground = { 0xC7, 0x4E, 0x39 };
	ThemeColor gitRenamedResourceForeground = { 0x73, 0xC9, 0x91 };
	ThemeColor gitStageModifiedResourceForeground = { 0xE2, 0xC0, 0x8D };
	ThemeColor gitStageDeletedResourceForeground = { 0xC7, 0x4E, 0x39 };
	ThemeColor gitUntrackedResourceForeground = { 0x73, 0xC9, 0x91 };
	ThemeColor gitIgnoredResourceForeground = { 0x8C, 0x8C, 0x8C };
	ThemeColor gitConflictingResourceForeground = { 0xE4, 0x67, 0x6B };
	ThemeColor gitSubmoduleResourceForeground = { 0x8D, 0xB9, 0xE2 };
	//! VS Code `quickInput.background`, used by Command Palette and Quick Pick.
	ThemeColor quickInputBackground = { 0x25, 0x25, 0x26 };
	//! VS Code `input.background` for single-line text fields.
	ThemeColor inputBackground = { 0x3C, 0x3C, 0x3C };
	//! VS Code `input.border` for single-line text fields.
	ThemeColor inputBorder = { 0x3C, 0x3C, 0x3C };
	//! VS Code `list.activeSelectionBackground`, kept separate from focus/accent.
	ThemeColor listActiveSelectionBackground = { 0x09, 0x47, 0x71 };
	//! VS Code `list.activeSelectionForeground`.
	ThemeColor listActiveSelectionForeground = { 0xFF, 0xFF, 0xFF };
	//! VS Code `list.hoverBackground`.
	ThemeColor listHoverBackground = { 0x2A, 0x2D, 0x2E };
	//! VS Code `list.focusAndSelectionOutline`.
	ThemeColor listFocusAndSelectionOutline = { 0x1F, 0x8A, 0xD2 };
	//! VS Code `scrollbar.background`. `transparent` leaves the owning surface visible.
	ThemeColor scrollbarBackground = { 0x00, 0x00, 0x00, 0x00 };
	//! VS Code `scrollbarSlider.background`. Alpha is resolved over the owning surface.
	ThemeColor scrollbarSliderBackground = { 0x79, 0x79, 0x79, 0x66 };
	//! VS Code `scrollbarSlider.hoverBackground`.
	ThemeColor scrollbarSliderHoverBackground = { 0x64, 0x64, 0x64, 0xB3 };
	//! VS Code `scrollbarSlider.activeBackground` used while the thumb is dragged.
	ThemeColor scrollbarSliderActiveBackground = { 0xBF, 0xBF, 0xBF, 0x66 };
	//! VS Code `editorWhitespace.foreground`. Alpha is resolved over `canvas` by
	//! editor decoration consumers so a theme switch cannot retain a legacy
	//! type-specific background behind tabs, spaces, or EOL marks.
	ThemeColor editorWhitespaceForeground = { 0xE3, 0xE4, 0xE2, 0x29 };
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
	const wchar_t* const preferredFamily{ L"" };
	const wchar_t* const fallbackFamily{ L"" };
	const int pointSize{ 9 };
	const int weight{ FW_NORMAL };
	const bool fixedPitch{ false };
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
	//! Returns the process-local color-theme projection without copying. High
	//! Contrast suppresses this overlay so the legacy accessibility path remains
	//! authoritative. The pointer is invalidated by the next theme application.
	[[nodiscard]] static const ThemePalette* ActiveColorThemePalette() noexcept;
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
	//! The active color theme's ColorThemeKind is Light.  VS Code emits a file icon
	//! theme's `light` section under the `.vs` body class alone, so this predicate --
	//! not "the background is bright" -- is what selects it, and High Contrast Light
	//! keeps the base section.  The composition root evaluates the kind while it
	//! applies the theme and publishes the answer here, because CColorThemeRegistry.h
	//! declares ColorThemeKind and already includes this header.
	static void SetActiveColorThemeLightKind(bool lightKind) noexcept { s_activeColorThemeLightKind = lightKind; }
	[[nodiscard]] static bool IsActiveColorThemeLightKind() noexcept { return s_activeColorThemeLightKind; }

private:
	//! One writer, CEditWnd::ApplyWorkbenchTheme, which assigns it on every path,
	//! and readers on paint paths.  A single predicate stays beside its accessors
	//! instead of joining the two optional palette overlays in the .cpp.
	inline static bool s_activeColorThemeLightKind = false;
};

constexpr ThemePalette CThemeService::PaletteFor(ThemeMode mode) noexcept
{
	if (mode == ThemeMode::Light) {
		return {
			{ 0xFF, 0xFF, 0xFF }, // editor.background
			{ 0xF8, 0xF8, 0xF8 }, // panel.background / workbench chrome
			{ 0xF2, 0xF2, 0xF2 }, // list.hoverBackground / raised surface
			{ 0xE5, 0xE5, 0xE5 }, // shared VS Code Light Modern border
			{ 0x3B, 0x3B, 0x3B }, // foreground
			{ 0x3B, 0x3B, 0x3B }, // sideBar.foreground / descriptionForeground
			{ 0x3B, 0x3B, 0x3B }, // VS Code Light Modern descriptionForeground
			{ 0xB0, 0xB0, 0xB0 }, // #61616180 composited over the white editor canvas
			{ 0x00, 0x5F, 0xB8 }, // VS Code Light Modern focus / active blue
			{ 0xFF, 0xFF, 0xFF }, // highlighted text
			{ 0xF8, 0xF8, 0xF8 }, // titleBar.activeBackground
			{ 0xF8, 0xF8, 0xF8 }, // activityBar.background
			{ 0xF8, 0x51, 0x49 }, // VS Code Light Modern errorForeground
			{ 0xBF, 0x88, 0x00 }, // notificationsWarningIcon.foreground
			{ 0xF8, 0xF8, 0xF8 }, // panel.background
			{ 0xF8, 0xF8, 0xF8 }, // sideBar.background
			{ 0xF8, 0xF8, 0xF8 }, // terminal.background fallback: panel.background
			{ 0xFF, 0xFF, 0xFF }, // editorGutter.background fallback: editor.background
			{ 0x6E, 0x76, 0x81 }, // editorLineNumber.foreground
			{ 0x17, 0x11, 0x84 }, // editorLineNumber.activeForeground
			{ 0xEB, 0xF1, 0xDD }, // rgba(155,185,85,.2) over the white editor canvas
			{ 0xFF, 0xCC, 0xCC }, // rgba(255,0,0,.2) over the white editor canvas
			{ 0xD3, 0xD3, 0xD3 }, // #22222233 over the white editor canvas
			{ 0xF3, 0xC4, 0xA6 }, // #EA5C0055 over the #F8F8F8 side bar
			{ 0x00, 0x5F, 0xB8 }, // button.background
			{ 0xFF, 0xFF, 0xFF }, // button.foreground
			{ 0x02, 0x58, 0xA8 }, // button.hoverBackground: VS Code Light Modern
			{ 0x00, 0x5F, 0xB8 }, // activityBarBadge.background
			{ 0xFF, 0xFF, 0xFF }, // activityBarBadge.foreground
			// The Git extension's registered `light` defaults, verbatim.
			{ 0x58, 0x7C, 0x0C }, // gitDecoration.addedResourceForeground
			{ 0x89, 0x55, 0x03 }, // gitDecoration.modifiedResourceForeground
			{ 0xAD, 0x07, 0x07 }, // gitDecoration.deletedResourceForeground
			{ 0x00, 0x71, 0x00 }, // gitDecoration.renamedResourceForeground
			{ 0x89, 0x55, 0x03 }, // gitDecoration.stageModifiedResourceForeground
			{ 0xAD, 0x07, 0x07 }, // gitDecoration.stageDeletedResourceForeground
			{ 0x00, 0x71, 0x00 }, // gitDecoration.untrackedResourceForeground
			{ 0x8E, 0x8E, 0x90 }, // gitDecoration.ignoredResourceForeground
			{ 0xAD, 0x07, 0x07 }, // gitDecoration.conflictingResourceForeground
			{ 0x12, 0x58, 0xA7 }, // gitDecoration.submoduleResourceForeground
			{ 0xF8, 0xF8, 0xF8 }, // quickInput.background
			{ 0xFF, 0xFF, 0xFF }, // input.background
			{ 0xCE, 0xCE, 0xCE }, // input.border
			{ 0xE8, 0xE8, 0xE8 }, // list.activeSelectionBackground
			{ 0x00, 0x00, 0x00 }, // list.activeSelectionForeground
			{ 0xF2, 0xF2, 0xF2 }, // list.hoverBackground
			{ 0x00, 0x5F, 0xB8 }, // list.focusAndSelectionOutline
			{ 0x00, 0x00, 0x00, 0x00 }, // scrollbar.background: transparent
			{ 0x64, 0x64, 0x64, 0x66 }, // scrollbarSlider.background: #646464 at 40%
			{ 0x64, 0x64, 0x64, 0xB3 }, // scrollbarSlider.hoverBackground: #646464 at 70%
			{ 0x00, 0x00, 0x00, 0x99 }, // scrollbarSlider.activeBackground: black at 60%
			{ 0x33, 0x33, 0x33, 0x33 }, // editorWhitespace.foreground
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
		{ 0x66, 0x37, 0x19 }, // editor.findMatchHighlightBackground: #EA5C0055 over the dark sideBar
		{ 0x1F, 0x8A, 0xD2 }, // button.background: the Sakura dark accent, not VS Code's #0E639C
		{ 0xFF, 0xFF, 0xFF }, // button.foreground
		{ 0x3F, 0xA1, 0xE3 }, // button.hoverBackground: lighten(button.background, 0.2) as upstream registers it for dark
		{ 0x00, 0x7A, 0xCC }, // activityBarBadge.background
		{ 0xFF, 0xFF, 0xFF }, // activityBarBadge.foreground
		// The Git extension's registered `dark` defaults, verbatim.
		{ 0x81, 0xB8, 0x8B }, // gitDecoration.addedResourceForeground
		{ 0xE2, 0xC0, 0x8D }, // gitDecoration.modifiedResourceForeground
		{ 0xC7, 0x4E, 0x39 }, // gitDecoration.deletedResourceForeground
		{ 0x73, 0xC9, 0x91 }, // gitDecoration.renamedResourceForeground
		{ 0xE2, 0xC0, 0x8D }, // gitDecoration.stageModifiedResourceForeground
		{ 0xC7, 0x4E, 0x39 }, // gitDecoration.stageDeletedResourceForeground
		{ 0x73, 0xC9, 0x91 }, // gitDecoration.untrackedResourceForeground
		{ 0x8C, 0x8C, 0x8C }, // gitDecoration.ignoredResourceForeground
		{ 0xE4, 0x67, 0x6B }, // gitDecoration.conflictingResourceForeground
		{ 0x8D, 0xB9, 0xE2 }, // gitDecoration.submoduleResourceForeground
		{ 0x25, 0x25, 0x26 }, // quickInput.background
		{ 0x3C, 0x3C, 0x3C }, // input.background
		{ 0x3C, 0x3C, 0x3C }, // input.border
		{ 0x09, 0x47, 0x71 }, // list.activeSelectionBackground
		{ 0xFF, 0xFF, 0xFF }, // list.activeSelectionForeground
		{ 0x2A, 0x2D, 0x2E }, // list.hoverBackground
		{ 0x1F, 0x8A, 0xD2 }, // list.focusAndSelectionOutline
		{ 0x00, 0x00, 0x00, 0x00 }, // scrollbar.background: transparent
		{ 0x79, 0x79, 0x79, 0x66 }, // scrollbarSlider.background: #797979 at 40%
		{ 0x64, 0x64, 0x64, 0xB3 }, // scrollbarSlider.hoverBackground: #646464 at 70%
		{ 0xBF, 0xBF, 0xBF, 0x66 }, // scrollbarSlider.activeBackground: #BFBFBF at 40%
		{ 0xE3, 0xE4, 0xE2, 0x29 }, // editorWhitespace.foreground
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
