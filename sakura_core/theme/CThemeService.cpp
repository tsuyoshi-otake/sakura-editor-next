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
	return { window, face, face, frame, windowText, grayText, windowText, grayText, highlight, highlightText,
		face, face, highlight };
}

ThemePalette CThemeService::EffectivePalette(ThemeMode savedMode) noexcept
{
	if (IsHighContrastActive()) {
		return HighContrastPalette();
	}
	return PaletteFor(savedMode);
}

} // namespace theme
