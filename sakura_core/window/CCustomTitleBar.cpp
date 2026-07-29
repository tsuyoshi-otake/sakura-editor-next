/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CCustomTitleBar.h"

#include <array>

#include "window/CCustomFrameController.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"

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
		? palette.danger.ToColorRef()
		: palette.raised.ToColorRef();
	Fill(dc, rect, color);
}

RECT TitleControlRect(const CustomFrameLayout& layout, CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return layout.layoutButton;
	case CustomFrameControl::PrimarySidebar: return layout.primarySidebarButton;
	case CustomFrameControl::BottomPanel: return layout.bottomPanelButton;
	case CustomFrameControl::SecondarySidebar: return layout.secondarySidebarButton;
	case CustomFrameControl::Account: return layout.accountButton;
	case CustomFrameControl::Settings: return layout.settingsButton;
	case CustomFrameControl::None: break;
	}
	return {};
}

void PaintTitleControlBackground(
	HDC dc,
	const RECT& rect,
	CustomFrameControl control,
	CustomFrameControl hotControl,
	CustomFrameControl pressedControl,
	const theme::ThemePalette& palette
) noexcept
{
	if (control == pressedControl) {
		Fill(dc, rect, palette.accent.ToColorRef());
	} else if (control == hotControl) {
		Fill(dc, rect, palette.raised.ToColorRef());
	}
}

void PaintGlyph(HDC dc, const RECT& rect, LRESULT hit, COLORREF color, [[maybe_unused]] int thickness, bool maximized) noexcept
{
	using workbench::icons::codicons::Icon;
	Icon icon = Icon::ChromeClose;
	switch (hit) {
	case HTMINBUTTON: icon = Icon::ChromeMinimize; break;
	case HTMAXBUTTON: icon = maximized ? Icon::ChromeRestore : Icon::ChromeMaximize; break;
	case HTCLOSE: icon = Icon::ChromeClose; break;
	default: return;
	}
	const UINT dpi = static_cast<UINT>(std::max(96, ::GetDeviceCaps(dc, LOGPIXELSX)));
	const auto box = workbench::icons::CenteredIconBounds(
		{ rect.left, rect.top, rect.right, rect.bottom }, workbench::icons::kStatusIconDip, dpi);
	workbench::icons::codicons::Draw(dc, box, icon, color);
}

void PaintTitleControlGlyph(HDC dc, const RECT& rect, CustomFrameControl control, COLORREF color) noexcept
{
	using workbench::icons::codicons::Icon;
	Icon icon = Icon::Layout;
	switch (control) {
	case CustomFrameControl::Layout: icon = Icon::Layout; break;
	case CustomFrameControl::PrimarySidebar: icon = Icon::LayoutSidebarLeft; break;
	case CustomFrameControl::BottomPanel: icon = Icon::LayoutPanel; break;
	case CustomFrameControl::SecondarySidebar: icon = Icon::LayoutSidebarRight; break;
	case CustomFrameControl::Account: icon = Icon::Account; break;
	case CustomFrameControl::Settings: icon = Icon::Gear; break;
	case CustomFrameControl::None: return;
	}
	const UINT dpi = static_cast<UINT>(std::max(96, ::GetDeviceCaps(dc, LOGPIXELSX)));
	const auto box = workbench::icons::CenteredIconBounds(
		{ rect.left, rect.top, rect.right, rect.bottom }, workbench::icons::kStatusIconDip, dpi);
	workbench::icons::codicons::Draw(dc, box, icon, color);
}

void PaintTitleControlFocus(HDC dc, const RECT& rect, const theme::ThemePalette& palette) noexcept
{
	RECT focus = rect;
	::InflateRect(&focus, -3, -3);
	const HPEN pen = ::CreatePen(PS_SOLID, 1, palette.accent.ToColorRef());
	const HGDIOBJ oldPen = ::SelectObject(dc, pen);
	const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
	::Rectangle(dc, focus.left, focus.top, focus.right, focus.bottom);
	::SelectObject(dc, oldBrush);
	::SelectObject(dc, oldPen);
	::DeleteObject(pen);
}

} // namespace

const wchar_t* CustomFrameControlName(CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return L"Layout";
	case CustomFrameControl::PrimarySidebar: return L"Toggle Primary Side Bar";
	case CustomFrameControl::BottomPanel: return L"Toggle Bottom Panel";
	case CustomFrameControl::SecondarySidebar: return L"Toggle Secondary Side Bar";
	case CustomFrameControl::Account: return L"Account";
	case CustomFrameControl::Settings: return L"Settings";
	case CustomFrameControl::None: return L"";
	}
	return L"";
}

const wchar_t* CustomFrameControlAutomationId(CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return L"Sakura.TitleBar.Layout";
	case CustomFrameControl::PrimarySidebar: return L"Sakura.TitleBar.PrimarySidebar";
	case CustomFrameControl::BottomPanel: return L"Sakura.TitleBar.BottomPanel";
	case CustomFrameControl::SecondarySidebar: return L"Sakura.TitleBar.SecondarySidebar";
	case CustomFrameControl::Account: return L"Sakura.TitleBar.Account";
	case CustomFrameControl::Settings: return L"Sakura.TitleBar.Settings";
	case CustomFrameControl::None: return L"";
	}
	return L"";
}

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
	LRESULT pressedHit,
	CustomFrameControl hotControl,
	CustomFrameControl pressedControl,
	CustomFrameControl focusedControl
) const noexcept
{
	if (owner == nullptr || dc == nullptr) {
		return;
	}
	Fill(dc, layout.title, palette.titleBar.ToColorRef());
	PaintButtonBackground(dc, layout.minimizeButton, HTMINBUTTON, hotHit, pressedHit, palette);
	PaintButtonBackground(dc, layout.maximizeButton, HTMAXBUTTON, hotHit, pressedHit, palette);
	PaintButtonBackground(dc, layout.closeButton, HTCLOSE, hotHit, pressedHit, palette);
	for (const CustomFrameControl control : {
		CustomFrameControl::Layout,
		CustomFrameControl::PrimarySidebar,
		CustomFrameControl::BottomPanel,
		CustomFrameControl::SecondarySidebar,
		CustomFrameControl::Account,
		CustomFrameControl::Settings,
	}) {
		const RECT rect = TitleControlRect(layout, control);
		if (::IsRectEmpty(&rect)) continue;
		PaintTitleControlBackground(dc, rect, control, hotControl, pressedControl, palette);
		const COLORREF controlColor = control == pressedControl
			? palette.highlightText.ToColorRef()
			: (active ? palette.primaryText : palette.secondaryText).ToColorRef();
		PaintTitleControlGlyph(dc, rect, control, controlColor);
		if (control == focusedControl) PaintTitleControlFocus(dc, rect, palette);
	}

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
