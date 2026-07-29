/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CCustomTitleBar.h"

#include <array>

#include "window/CCustomFrameController.h"

namespace {

void Fill(HDC dc, const RECT& rect, COLORREF color) noexcept
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	::FillRect(dc, &rect, brush);
	::DeleteObject(brush);
}

void PaintButtonBackground(
	HDC dc,
	const RECT& rect,
	LRESULT hit,
	LRESULT hotHit,
	LRESULT pressedHit,
	const theme::ThemePalette& palette
) noexcept
{
	if (hit != hotHit && hit != pressedHit) {
		return;
	}
	const COLORREF color = hit == HTCLOSE
		? palette.accent.ToColorRef()
		: palette.raised.ToColorRef();
	Fill(dc, rect, color);
}

void PaintGlyph(HDC dc, const RECT& rect, LRESULT hit, COLORREF color, int thickness, bool maximized) noexcept
{
	const int centerX = (rect.left + rect.right) / 2;
	const int centerY = (rect.top + rect.bottom) / 2;
	const int half = std::max(4, static_cast<int>((rect.bottom - rect.top) / 7));
	const HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
	const HGDIOBJ oldPen = ::SelectObject(dc, pen);
	const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
	switch (hit) {
	case HTMINBUTTON:
		::MoveToEx(dc, centerX - half, centerY + half / 2, nullptr);
		::LineTo(dc, centerX + half + 1, centerY + half / 2);
		break;
	case HTMAXBUTTON:
		if (maximized) {
			::Rectangle(dc, centerX - half + 2, centerY - half - 1, centerX + half + 2, centerY + half - 1);
			::Rectangle(dc, centerX - half - 2, centerY - half + 2, centerX + half - 2, centerY + half + 2);
		} else {
			::Rectangle(dc, centerX - half, centerY - half, centerX + half + 1, centerY + half + 1);
		}
		break;
	case HTCLOSE:
		::MoveToEx(dc, centerX - half, centerY - half, nullptr);
		::LineTo(dc, centerX + half + 1, centerY + half + 1);
		::MoveToEx(dc, centerX + half, centerY - half, nullptr);
		::LineTo(dc, centerX - half - 1, centerY + half + 1);
		break;
	default:
		break;
	}
	::SelectObject(dc, oldBrush);
	::SelectObject(dc, oldPen);
	::DeleteObject(pen);
}

} // namespace

int CalculateCustomTitleBarIconSize(int titleHeight, UINT dpi) noexcept
{
	const UINT effectiveDpi = dpi == 0 ? 96 : dpi;
	const int minimum = ::MulDiv(16, static_cast<int>(effectiveDpi), 96);
	const int maximum = ::MulDiv(20, static_cast<int>(effectiveDpi), 96);
	const int padding = ::MulDiv(12, static_cast<int>(effectiveDpi), 96);
	return std::clamp(titleHeight - padding, minimum, maximum);
}

theme::ThemeColor CustomTitleBarGlyphColor(
	const theme::ThemePalette& palette,
	bool active,
	LRESULT hit,
	LRESULT hotHit,
	LRESULT pressedHit
) noexcept
{
	return hit == HTCLOSE && (hotHit == HTCLOSE || pressedHit == HTCLOSE)
		? palette.highlightText
		: (active ? palette.primaryText : palette.secondaryText);
}

void CCustomTitleBar::Paint(
	HWND owner,
	HDC dc,
	const CustomFrameLayout& layout,
	const theme::ThemePalette& palette,
	HFONT font,
	bool active,
	LRESULT hotHit,
	LRESULT pressedHit
) const noexcept
{
	if (owner == nullptr || dc == nullptr) {
		return;
	}
	Fill(dc, layout.title, palette.canvas.ToColorRef());
	PaintButtonBackground(dc, layout.minimizeButton, HTMINBUTTON, hotHit, pressedHit, palette);
	PaintButtonBackground(dc, layout.maximizeButton, HTMAXBUTTON, hotHit, pressedHit, palette);
	PaintButtonBackground(dc, layout.closeButton, HTCLOSE, hotHit, pressedHit, palette);

	const COLORREF glyphColor = (active ? palette.primaryText : palette.secondaryText).ToColorRef();
	PaintGlyph(dc, layout.minimizeButton, HTMINBUTTON, glyphColor, 1, false);
	PaintGlyph(dc, layout.maximizeButton, HTMAXBUTTON, glyphColor, 1, ::IsZoomed(owner) != FALSE);
	PaintGlyph(dc, layout.closeButton, HTCLOSE,
		CustomTitleBarGlyphColor(palette, active, HTCLOSE, hotHit, pressedHit).ToColorRef(), 1, false);

	HICON icon = reinterpret_cast<HICON>(::SendMessageW(owner, WM_GETICON, ICON_SMALL2, 0));
	if (icon == nullptr) {
		icon = reinterpret_cast<HICON>(::GetClassLongPtrW(owner, GCLP_HICONSM));
	}
	if (icon != nullptr) {
		const int iconSize = CalculateCustomTitleBarIconSize(
			static_cast<int>(layout.systemMenu.bottom - layout.systemMenu.top), ::GetDpiForWindow(owner));
		const int x = layout.systemMenu.left + (layout.systemMenu.right - layout.systemMenu.left - iconSize) / 2;
		const int y = layout.systemMenu.top + (layout.systemMenu.bottom - layout.systemMenu.top - iconSize) / 2;
		::DrawIconEx(dc, x, y, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
	}

	std::array<wchar_t, 1024> title{};
	const int titleLength = ::GetWindowTextW(owner, title.data(), static_cast<int>(title.size()));
	if (titleLength > 0 && !::IsRectEmpty(&layout.captionText)) {
		const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, (active ? palette.primaryText : palette.secondaryText).ToColorRef());
		RECT textRect = layout.captionText;
		::DrawTextW(dc, title.data(), titleLength, &textRect,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		if (oldFont != nullptr) {
			::SelectObject(dc, oldFont);
		}
	}

	const HPEN borderPen = ::CreatePen(PS_SOLID, 1, palette.border.ToColorRef());
	const HGDIOBJ oldPen = ::SelectObject(dc, borderPen);
	::MoveToEx(dc, layout.title.left, layout.title.bottom - 1, nullptr);
	::LineTo(dc, layout.title.right, layout.title.bottom - 1);
	::SelectObject(dc, oldPen);
	::DeleteObject(borderPen);
}
