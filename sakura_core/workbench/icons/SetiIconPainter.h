/*! @file
	@brief Shared painter for the bundled `vs-seti` file icon theme

	Both the Explorer and the Source Control view draw a file row's icon with the
	same theme VS Code selects by default, so the glyph metrics and the colour rule
	live here instead of being restated per view. The association table itself stays
	in icons/SetiFileIcon.h; this header only puts one resolved glyph on a DC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>

#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/SetiFileIcon.h"

namespace workbench::icons::seti {

//! The generated Seti table stores upstream's `#rrggbb` fontColor as 0x00RRGGBB.
[[nodiscard]] constexpr COLORREF ColorRefFromThemeRgb(std::uint32_t rgb) noexcept
{
	return RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
}

/*!
	@brief Draws one glyph of the bundled Seti file icon theme

	Unlike a Codicon, a Seti glyph does not fill its em box -- it inks about 0.56 em.
	Upstream compensates in the theme document itself (`fonts[0].size` is `150%`), so
	the em box here is larger than the icon slot; icons/SetiFileIcon.h derives the
	ratio. Drawing it at the slot size instead would render every file icon visibly
	smaller than VS Code does.
*/
inline void DrawSetiIcon(HDC dc, const RECT& bounds, wchar_t glyph, COLORREF color) noexcept
{
	if (dc == nullptr || glyph == L'\0') return;
	const int side = std::min(static_cast<int>(bounds.right - bounds.left),
		static_cast<int>(bounds.bottom - bounds.top));
	if (side <= 0) return;
	const int em = std::max(1, ::MulDiv(side, kEmToIconSlotNumerator, kEmToIconSlotDenominator));
	const HFONT glyphFont = icons::CreateLabelRunGlyphFont(kFontFamily, em);
	if (glyphFont == nullptr) return;
	const HGDIOBJ previousFont = ::SelectObject(dc, glyphFont);
	const int previousBackgroundMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF previousTextColor = ::SetTextColor(dc, color);
	const wchar_t text[] = { glyph, L'\0' };
	RECT box = bounds;
	::DrawTextW(dc, text, 1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::SetTextColor(dc, previousTextColor);
	::SetBkMode(dc, previousBackgroundMode);
	::SelectObject(dc, previousFont);
	::DeleteObject(glyphFont);
}

} // namespace workbench::icons::seti
