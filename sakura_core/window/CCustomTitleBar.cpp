/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CCustomTitleBar.h"

#include <algorithm>
#include <array>
#include <string_view>

#include "window/CCustomFrameController.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/CodiconGlyphTable.h"

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
	case CustomFrameControl::Manage: return layout.manageButton;
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

[[nodiscard]] wchar_t CodiconGlyph(std::wstring_view name) noexcept
{
	return workbench::icons::FindCodiconGlyph(name).value_or(L'\0');
}

[[nodiscard]] bool PaintFontGlyph(
	HDC dc,
	const workbench::icons::IconRect& box,
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

void PaintGlyph(
	HDC dc,
	const RECT& rect,
	LRESULT hit,
	COLORREF color,
	[[maybe_unused]] int thickness,
	bool maximized,
	HFONT codiconFont
) noexcept
{
	using workbench::icons::codicons::Icon;
	Icon icon = Icon::ChromeClose;
	std::wstring_view glyphName;
	switch (hit) {
	case HTMINBUTTON:
		icon = Icon::ChromeMinimize;
		glyphName = L"chrome-minimize";
		break;
	case HTMAXBUTTON:
		icon = maximized ? Icon::ChromeRestore : Icon::ChromeMaximize;
		glyphName = maximized ? L"chrome-restore" : L"chrome-maximize";
		break;
	case HTCLOSE:
		icon = Icon::ChromeClose;
		glyphName = L"chrome-close";
		break;
	default: return;
	}
	const UINT dpi = static_cast<UINT>(std::max(96, ::GetDeviceCaps(dc, LOGPIXELSX)));
	const auto box = workbench::icons::CenteredIconBounds(
		{ rect.left, rect.top, rect.right, rect.bottom }, workbench::icons::kStatusIconDip, dpi);
	if (PaintFontGlyph(dc, box, codiconFont, CodiconGlyph(glyphName), color)) return;
	workbench::icons::codicons::Draw(dc, box, icon, color);
}

void PaintTitleControlGlyph(
	HDC dc,
	const RECT& rect,
	CustomFrameControl control,
	COLORREF color,
	HFONT codiconFont
) noexcept
{
	using workbench::icons::codicons::Icon;
	Icon icon = Icon::Layout;
	std::wstring_view glyphName;
	switch (control) {
	case CustomFrameControl::Layout:
		icon = Icon::Layout;
		glyphName = L"layout";
		break;
	case CustomFrameControl::PrimarySidebar:
		icon = Icon::LayoutSidebarLeft;
		glyphName = L"layout-sidebar-left";
		break;
	case CustomFrameControl::BottomPanel:
		icon = Icon::LayoutPanel;
		glyphName = L"layout-panel";
		break;
	case CustomFrameControl::SecondarySidebar:
		icon = Icon::LayoutSidebarRight;
		glyphName = L"layout-sidebar-right";
		break;
	case CustomFrameControl::Account:
		icon = Icon::Account;
		glyphName = L"account";
		break;
	case CustomFrameControl::Manage:
		icon = Icon::Gear;
		glyphName = L"gear";
		break;
	case CustomFrameControl::None: return;
	}
	const UINT dpi = static_cast<UINT>(std::max(96, ::GetDeviceCaps(dc, LOGPIXELSX)));
	const auto box = workbench::icons::CenteredIconBounds(
		{ rect.left, rect.top, rect.right, rect.bottom }, workbench::icons::kStatusIconDip, dpi);
	if (PaintFontGlyph(dc, box, codiconFont, CodiconGlyph(glyphName), color)) return;
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

CCustomTitleBar::~CCustomTitleBar() noexcept
{
	ReleaseCodiconFont();
}

HFONT CCustomTitleBar::AcquireCodiconFont(int height) const noexcept
{
	if (height <= 0) return nullptr;
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) {
		ReleaseCodiconFont();
		return nullptr;
	}
	if (m_codiconFont != nullptr && m_codiconFontHeight == height) return m_codiconFont;

	ReleaseCodiconFont();
	LOGFONTW logFont{};
	logFont.lfHeight = -height;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), std::begin(logFont.lfFaceName));
	logFont.lfFaceName[faceName.size()] = L'\0';

	m_codiconFont = ::CreateFontIndirectW(&logFont);
	if (m_codiconFont != nullptr) m_codiconFontHeight = height;
	return m_codiconFont;
}

void CCustomTitleBar::ReleaseCodiconFont() const noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

const wchar_t* CustomFrameControlName(CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return L"Layout";
	case CustomFrameControl::PrimarySidebar: return L"Toggle Primary Side Bar";
	case CustomFrameControl::BottomPanel: return L"Toggle Bottom Panel";
	case CustomFrameControl::SecondarySidebar: return L"Toggle Secondary Side Bar";
	case CustomFrameControl::Account: return L"Account";
	case CustomFrameControl::Manage: return L"Manage";
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
	case CustomFrameControl::Manage: return L"Sakura.TitleBar.Manage";
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
	const UINT dpi = static_cast<UINT>(std::max(96, ::GetDeviceCaps(dc, LOGPIXELSX)));
	const HFONT codiconFont = AcquireCodiconFont(
		workbench::icons::ScaleDip(workbench::icons::kStatusIconDip, dpi));
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
		CustomFrameControl::Manage,
	}) {
		const RECT rect = TitleControlRect(layout, control);
		if (::IsRectEmpty(&rect)) continue;
		PaintTitleControlBackground(dc, rect, control, hotControl, pressedControl, palette);
		const COLORREF controlColor = control == pressedControl
			? palette.highlightText.ToColorRef()
			: (active ? palette.primaryText : palette.secondaryText).ToColorRef();
		PaintTitleControlGlyph(dc, rect, control, controlColor, codiconFont);
		if (control == focusedControl) PaintTitleControlFocus(dc, rect, palette);
	}

	const COLORREF glyphColor = (active ? palette.primaryText : palette.secondaryText).ToColorRef();
	PaintGlyph(dc, layout.minimizeButton, HTMINBUTTON, glyphColor, 1, false, codiconFont);
	PaintGlyph(dc, layout.maximizeButton, HTMAXBUTTON, glyphColor, 1, ::IsZoomed(owner) != FALSE, codiconFont);
	PaintGlyph(dc, layout.closeButton, HTCLOSE,
		CustomTitleBarGlyphColor(palette, active, HTCLOSE, hotHit, pressedHit).ToColorRef(), 1, false,
		codiconFont);

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
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
