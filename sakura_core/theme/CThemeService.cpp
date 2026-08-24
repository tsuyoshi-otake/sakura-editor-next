/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "theme/CThemeService.h"

#include <algorithm>
#include <cwchar>

namespace {

constexpr UINT kDefaultDpi = 96;

// Theme selection is window/UI-thread state in the native composition root.
// Keeping it process-local also makes existing controls that ask EffectivePalette
// directly observe the same selected VS Code theme as controls receiving a
// palette through CEditWnd::ApplyWorkbenchTheme.
std::optional<theme::ThemePalette> g_activeColorThemePalette;
std::optional<theme::ThemeSyntaxPalette> g_activeColorThemeSyntaxPalette;
std::optional<std::vector<theme::ThemeTokenColorRule>> g_activeColorThemeTokenColors;

[[nodiscard]] theme::ThemeColor FromColorRef(COLORREF color) noexcept
{
	return { GetRValue(color), GetGValue(color), GetBValue(color) };
}

int CALLBACK FindFontFamilyProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter)
{
	*reinterpret_cast<bool*>(parameter) = true;
	return 0;
}

[[nodiscard]] bool IsFontFamilyInstalled(const wchar_t* faceName) noexcept
{
	if (faceName == nullptr || *faceName == L'\0') {
		return false;
	}
	const HDC dc = ::GetDC(nullptr);
	if (dc == nullptr) {
		return false;
	}
	LOGFONTW query{};
	query.lfCharSet = DEFAULT_CHARSET;
	::wcsncpy_s(query.lfFaceName, faceName, _TRUNCATE);
	bool found = false;
	::EnumFontFamiliesExW(dc, &query, FindFontFamilyProc, reinterpret_cast<LPARAM>(&found), 0);
	::ReleaseDC(nullptr, dc);
	return found;
}

[[nodiscard]] int PointSizeToLogicalHeight(int pointSize, UINT dpi) noexcept
{
	const int effectivePoints = std::max(1, pointSize);
	const UINT effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	return -::MulDiv(effectivePoints, static_cast<int>(effectiveDpi), 72);
}

[[nodiscard]] HFONT CreateThemeFont(theme::ThemeFontKind kind, UINT dpi, int pointSize) noexcept
{
	const theme::ThemeFontSpec spec = theme::CThemeService::FontSpec(kind);
	const wchar_t* faceName = theme::CThemeService::ResolveFontFamily(kind);
	LOGFONTW font{};
	font.lfHeight = PointSizeToLogicalHeight(pointSize == 0 ? spec.pointSize : pointSize, dpi);
	font.lfWeight = spec.weight;
	font.lfCharSet = DEFAULT_CHARSET;
	font.lfQuality = CLEARTYPE_QUALITY;
	font.lfPitchAndFamily = spec.fixedPitch ? FIXED_PITCH | FF_MODERN : DEFAULT_PITCH | FF_DONTCARE;
	::wcsncpy_s(font.lfFaceName, faceName, _TRUNCATE);
	return ::CreateFontIndirectW(&font);
}

} // namespace

namespace theme {

const wchar_t* CThemeService::ResolveFontFamily(ThemeFontKind kind) noexcept
{
	const ThemeFontSpec spec = FontSpec(kind);
	return IsFontFamilyInstalled(spec.preferredFamily) ? spec.preferredFamily : spec.fallbackFamily;
}

CThemeFont::~CThemeFont() noexcept
{
	Reset();
}

CThemeFont::CThemeFont(CThemeFont&& other) noexcept
	: m_font(other.m_font)
	, m_dpi(other.m_dpi)
{
	other.m_font = nullptr;
	other.m_dpi = 0;
}

CThemeFont& CThemeFont::operator=(CThemeFont&& other) noexcept
{
	if (this != &other) {
		Reset();
		m_font = other.m_font;
		m_dpi = other.m_dpi;
		other.m_font = nullptr;
		other.m_dpi = 0;
	}
	return *this;
}

bool CThemeFont::Recreate(ThemeFontKind kind, UINT dpi, int pointSize) noexcept
{
	const UINT effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const HFONT replacement = CreateThemeFont(kind, effectiveDpi, pointSize);
	if (replacement == nullptr) {
		return false;
	}
	Reset();
	m_font = replacement;
	m_dpi = effectiveDpi;
	return true;
}

bool CThemeFont::RecreateForWindow(ThemeFontKind kind, HWND window, int pointSize) noexcept
{
	const UINT dpi = window == nullptr ? kDefaultDpi : ::GetDpiForWindow(window);
	return Recreate(kind, dpi, pointSize);
}

void CThemeFont::Reset() noexcept
{
	if (m_font != nullptr) {
		::DeleteObject(m_font);
		m_font = nullptr;
	}
	m_dpi = 0;
}

bool CThemeService::IsHighContrastActive() noexcept
{
	HIGHCONTRASTW highContrast{};
	highContrast.cbSize = sizeof(highContrast);
	return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE
		&& (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

ThemePalette CThemeService::HighContrastPalette() noexcept
{
	const ThemeColor window = FromColorRef(::GetSysColor(COLOR_WINDOW));
	const ThemeColor windowText = FromColorRef(::GetSysColor(COLOR_WINDOWTEXT));
	const ThemeColor face = FromColorRef(::GetSysColor(COLOR_BTNFACE));
	const ThemeColor frame = FromColorRef(::GetSysColor(COLOR_WINDOWFRAME));
	const ThemeColor highlight = FromColorRef(::GetSysColor(COLOR_HIGHLIGHT));
	const ThemeColor highlightText = FromColorRef(::GetSysColor(COLOR_HIGHLIGHTTEXT));
	const ThemeColor grayText = FromColorRef(::GetSysColor(COLOR_GRAYTEXT));
	// High Contrast never lowers contrast to imitate VS Code's translucent description token:
	// the description role takes the full window text color and only the disabled role dims.
	// The three diff roles register `null` for hcDark/hcLight upstream, i.e. High Contrast
	// paints no inserted/removed wash and no diagonal fill at all; giving them the window
	// color is exactly that absence, not a chosen highlight color.
	// The three button roles take the system highlight pair rather than a chosen brand color:
	// a High Contrast theme guarantees that pairing's contrast, and an accent we picked does not.
	// The Git extension registers a separate `highContrast` set, and unlike the
	// three diff roles it is not `null`: a High Contrast theme still colors Git
	// decorations, so taking the system window text here would erase a distinction
	// upstream deliberately keeps. This product resolves one High Contrast palette
	// from system colors and has no hcDark/hcLight kind to select between, so the
	// `highContrast` (dark) set is used for both; see theme/CLAUDE.md.
	ThemePalette palette{ window, face, face, frame, windowText, grayText, windowText, grayText, highlight, highlightText,
		face, face, highlight, highlight, face, face, face, window, windowText, windowText,
		window, window, window, highlight, highlight, highlightText, highlight, highlight, highlightText,
		{ 0xA1, 0xE3, 0xAD }, { 0xE2, 0xC0, 0x8D }, { 0xC7, 0x4E, 0x39 }, { 0x73, 0xC9, 0x91 },
		{ 0xE2, 0xC0, 0x8D }, { 0xC7, 0x4E, 0x39 }, { 0x73, 0xC9, 0x91 }, { 0xA7, 0xA8, 0xA9 },
		{ 0xC7, 0x4E, 0x39 }, { 0x8D, 0xB9, 0xE2 },
		face, face, frame, highlight, highlightText, face, highlightText,
		{ 0x00, 0x00, 0x00, 0x00 }, frame, frame, frame, windowText };
	palette.editorIndentGuideBackground = windowText;
	return palette;
}

ThemePalette CThemeService::EffectivePalette(ThemeMode savedMode) noexcept
{
	if (IsHighContrastActive()) {
		return HighContrastPalette();
	}
	if (g_activeColorThemePalette) {
		return *g_activeColorThemePalette;
	}
	return PaletteFor(savedMode);
}

void CThemeService::SetActiveColorThemePalette(const ThemePalette& palette) noexcept
{
	g_activeColorThemePalette = palette;
}

void CThemeService::ClearActiveColorThemePalette() noexcept
{
	g_activeColorThemePalette.reset();
	g_activeColorThemeSyntaxPalette.reset();
	g_activeColorThemeTokenColors.reset();
}

bool CThemeService::HasActiveColorThemePalette() noexcept
{
	return g_activeColorThemePalette.has_value();
}

const ThemePalette* CThemeService::ActiveColorThemePalette() noexcept
{
	return g_activeColorThemePalette ? &*g_activeColorThemePalette : nullptr;
}

void CThemeService::SetActiveColorThemeSyntaxPalette(const ThemeSyntaxPalette& palette) noexcept
{
	g_activeColorThemeSyntaxPalette = palette;
}

void CThemeService::ClearActiveColorThemeSyntaxPalette() noexcept
{
	g_activeColorThemeSyntaxPalette.reset();
}

bool CThemeService::HasActiveColorThemeSyntaxPalette() noexcept
{
	return g_activeColorThemeSyntaxPalette.has_value();
}

const ThemeSyntaxPalette* CThemeService::ActiveColorThemeSyntaxPalette() noexcept
{
	return g_activeColorThemeSyntaxPalette ? &*g_activeColorThemeSyntaxPalette : nullptr;
}

ThemeSyntaxPalette CThemeService::EffectiveSyntaxPalette(ThemeMode savedMode) noexcept
{
	(void)savedMode;
	if (IsHighContrastActive() || !g_activeColorThemeSyntaxPalette) {
		return {};
	}
	return *g_activeColorThemeSyntaxPalette;
}

void CThemeService::SetActiveColorThemeTokenColors(std::vector<ThemeTokenColorRule> tokenColors)
{
	g_activeColorThemeTokenColors = std::move(tokenColors);
}

void CThemeService::ClearActiveColorThemeTokenColors() noexcept
{
	g_activeColorThemeTokenColors.reset();
}

const std::vector<ThemeTokenColorRule>* CThemeService::ActiveColorThemeTokenColors() noexcept
{
	if (IsHighContrastActive() || !g_activeColorThemeTokenColors) return nullptr;
	return &*g_activeColorThemeTokenColors;
}

} // namespace theme
