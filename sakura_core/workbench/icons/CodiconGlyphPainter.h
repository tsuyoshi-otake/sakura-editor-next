/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"

#include <Windows.h>

#include <algorithm>
#include <string_view>

namespace workbench::icons {

/*!
	@brief A cached HFONT for the embedded codicon.ttf at one pixel height.

	The vector path renderer in CodiconsActivityIcons.h fills GDI paths without
	antialiasing, so a 16 px glyph comes out visibly jagged and, for shapes with
	thin diagonal strokes, distorted.  Drawing the same code point through the
	embedded font instead gets ClearType rasterization, which is what VS Code's
	own codicon rendering looks like.  Keep the path renderer as the fallback
	for the case where the font failed to register.
*/
class CCodiconGlyphFont final {
public:
	CCodiconGlyphFont() = default;
	~CCodiconGlyphFont() { Release(); }

	CCodiconGlyphFont(const CCodiconGlyphFont&) = delete;
	CCodiconGlyphFont& operator=(const CCodiconGlyphFont&) = delete;

	//! Returns a font for the requested pixel height, or nullptr when the
	//! embedded face is unavailable.
	[[nodiscard]] HFONT Acquire(int height) noexcept
	{
		if (height <= 0) return nullptr;
		const auto faceName = CCodiconFont::Instance().FaceName();
		if (faceName.empty() || faceName.size() >= LF_FACESIZE) {
			Release();
			return nullptr;
		}
		if (m_font != nullptr && m_height == height) return m_font;

		Release();
		LOGFONTW logFont{};
		logFont.lfHeight = -height;
		logFont.lfWeight = FW_NORMAL;
		logFont.lfCharSet = DEFAULT_CHARSET;
		logFont.lfOutPrecision = OUT_TT_PRECIS;
		logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
		logFont.lfQuality = CLEARTYPE_QUALITY;
		logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
		std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
		logFont.lfFaceName[faceName.size()] = L'\0';
		m_font = ::CreateFontIndirectW(&logFont);
		if (m_font != nullptr) m_height = height;
		return m_font;
	}

	void Release() noexcept
	{
		if (m_font != nullptr) {
			::DeleteObject(m_font);
			m_font = nullptr;
		}
		m_height = 0;
	}

private:
	HFONT m_font = nullptr;
	int m_height = 0;
};

//! Paints one glyph centered in the box.  Returns false when it drew nothing.
[[nodiscard]] inline bool PaintCodiconGlyph(
	HDC dc,
	const IconRect& box,
	HFONT font,
	wchar_t glyph,
	COLORREF color
) noexcept
{
	if (dc == nullptr || font == nullptr || glyph == L'\0' || box.Width() <= 0 || box.Height() <= 0) {
		return false;
	}
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	const HGDIOBJ oldFont = ::SelectObject(dc, font);
	if (oldFont == nullptr || oldFont == HGDI_ERROR) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, color);
	RECT glyphRect{ box.left, box.top, box.right, box.bottom };
	const wchar_t text[] = { glyph, L'\0' };
	const int drawn = ::DrawTextW(dc, text, 1, &glyphRect,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::RestoreDC(dc, saved);
	return drawn != 0;
}

/*!
	@brief Paints the named codicon through the embedded font.

	@retval false The name is unknown or the font is unavailable; the caller
		must fall back to the vector path renderer.
*/
[[nodiscard]] inline bool PaintCodiconByName(
	HDC dc,
	const IconRect& box,
	std::wstring_view name,
	COLORREF color,
	CCodiconGlyphFont& cache
) noexcept
{
	const auto glyph = FindCodiconGlyph(name);
	if (!glyph) return false;
	// The codicon face draws its 16-unit design grid at the em size, so the box
	// height is the pixel height to ask for.
	return PaintCodiconGlyph(dc, box, cache.Acquire(box.Height()), *glyph, color);
}

} // namespace workbench::icons
