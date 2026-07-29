/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CClientMenuBar.h"

#include <algorithm>
#include <cwctype>

namespace {

int ScaleDip(int value, UINT dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

bool Contains(const RECT& rect, POINT point) noexcept
{
	return point.x >= rect.left && point.x < rect.right
		&& point.y >= rect.top && point.y < rect.bottom;
}

} // namespace

void CClientMenuBar::SetMenu(HMENU menu) noexcept
{
	m_menu = menu;
	m_itemBounds.clear();
	m_hotItem = -1;
	m_pressedItem = -1;
	m_keyboardItem = -1;
	m_keyboardMode = false;
}

void CClientMenuBar::SetBounds(const RECT& bounds) noexcept
{
	if (!::EqualRect(&m_bounds, &bounds)) {
		m_bounds = bounds;
		m_itemBounds.clear();
	}
}

std::wstring CClientMenuBar::ItemText(int index) const
{
	if (m_menu == nullptr || index < 0) {
		return {};
	}
	const int length = ::GetMenuStringW(m_menu, static_cast<UINT>(index), nullptr, 0, MF_BYPOSITION);
	if (length <= 0) {
		return {};
	}
	std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
	const int copied = ::GetMenuStringW(
		m_menu,
		static_cast<UINT>(index),
		result.data(),
		static_cast<int>(result.size()),
		MF_BYPOSITION
	);
	result.resize(static_cast<std::size_t>(std::max(0, copied)));
	return result;
}

int CClientMenuBar::MeasurePreferredWidth(HWND owner, HFONT font, UINT dpi) const noexcept
{
	if (m_menu == nullptr || owner == nullptr) {
		return 0;
	}
	const HDC dc = ::GetDC(owner);
	if (dc == nullptr) {
		return 0;
	}
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	const int count = ::GetMenuItemCount(m_menu);
	const int horizontalPadding = ScaleDip(12, dpi);
	int width = 0;
	for (int index = 0; index < count; ++index) {
		const std::wstring text = ItemText(index);
		SIZE extent{};
		if (!text.empty()) {
			::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
		}
		width += extent.cx + horizontalPadding * 2;
	}
	if (oldFont != nullptr) {
		::SelectObject(dc, oldFont);
	}
	::ReleaseDC(owner, dc);
	return width;
}

void CClientMenuBar::UpdateItemLayout(HWND owner, HFONT font) noexcept
{
	m_itemBounds.clear();
	if (m_menu == nullptr || owner == nullptr || ::IsRectEmpty(&m_bounds)) {
		return;
	}
	const HDC dc = ::GetDC(owner);
	if (dc == nullptr) {
		return;
	}
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	const UINT dpi = ::GetDpiForWindow(owner);
	const int horizontalPadding = ScaleDip(12, dpi);
	const int count = ::GetMenuItemCount(m_menu);
	int left = m_bounds.left;
	for (int index = 0; index < count && left < m_bounds.right; ++index) {
		const std::wstring text = ItemText(index);
		SIZE extent{};
		if (!text.empty()) {
			::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
		}
		const int right = std::min(m_bounds.right, left + extent.cx + horizontalPadding * 2);
		m_itemBounds.push_back({ left, m_bounds.top, right, m_bounds.bottom });
		left = right;
	}
	if (oldFont != nullptr) {
		::SelectObject(dc, oldFont);
	}
	::ReleaseDC(owner, dc);
}

void CClientMenuBar::Paint(
	[[maybe_unused]] HWND owner,
	HDC dc,
	HFONT font,
	const theme::ThemePalette& palette,
	bool active
) const noexcept
{
	if (m_menu == nullptr || dc == nullptr) {
		return;
	}
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, (active ? palette.primaryText : palette.secondaryText).ToColorRef());
	for (std::size_t index = 0; index < m_itemBounds.size(); ++index) {
		const bool highlighted = static_cast<int>(index) == m_hotItem
			|| (m_keyboardMode && static_cast<int>(index) == m_keyboardItem);
		if (highlighted) {
			const HBRUSH brush = ::CreateSolidBrush(palette.raised.ToColorRef());
			::FillRect(dc, &m_itemBounds[index], brush);
			::DeleteObject(brush);
		}
		RECT textRect = m_itemBounds[index];
		const UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE
			| (m_keyboardMode ? 0 : DT_HIDEPREFIX);
		const std::wstring text = ItemText(static_cast<int>(index));
		::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, format);
		if (m_keyboardMode && static_cast<int>(index) == m_keyboardItem) {
			const HPEN pen = ::CreatePen(PS_SOLID, 1, palette.accent.ToColorRef());
			const HGDIOBJ oldPen = ::SelectObject(dc, pen);
			const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
			::Rectangle(dc, textRect.left, textRect.top, textRect.right, textRect.bottom);
			::SelectObject(dc, oldBrush);
			::SelectObject(dc, oldPen);
			::DeleteObject(pen);
		}
	}
	if (oldFont != nullptr) {
		::SelectObject(dc, oldFont);
	}
}

bool CClientMenuBar::ContainsPoint(POINT clientPoint) const noexcept
{
	return Contains(m_bounds, clientPoint);
}

int CClientMenuBar::HitTestItem(POINT clientPoint) const noexcept
{
	for (std::size_t index = 0; index < m_itemBounds.size(); ++index) {
		if (Contains(m_itemBounds[index], clientPoint)) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

int CClientMenuBar::AccessibilityItemCount() const noexcept
{
	return m_menu == nullptr ? 0 : ::GetMenuItemCount(m_menu);
}

std::wstring CClientMenuBar::AccessibilityItemText(int index) const
{
	return ItemText(index);
}

RECT CClientMenuBar::AccessibilityItemBounds(int index) const noexcept
{
	return index >= 0 && index < static_cast<int>(m_itemBounds.size()) ? m_itemBounds[index] : RECT{};
}

bool CClientMenuBar::AccessibilityItemEnabled(int index) const noexcept
{
	if (m_menu == nullptr || index < 0 || index >= ::GetMenuItemCount(m_menu)) return false;
	const UINT state = ::GetMenuState(m_menu, static_cast<UINT>(index), MF_BYPOSITION);
	return state != static_cast<UINT>(-1) && (state & (MF_DISABLED | MF_GRAYED)) == 0;
}

bool CClientMenuBar::InvokeAccessibilityItem(HWND owner, int index) noexcept
{
	return AccessibilityItemEnabled(index) && OpenItem(owner, index, true);
}

void CClientMenuBar::SetAccessibilityFocusedItem(HWND owner, int index) noexcept
{
	if (!AccessibilityItemEnabled(index)) return;
	m_keyboardMode = true;
	m_keyboardItem = index;
	Invalidate(owner);
}

void CClientMenuBar::Invalidate(HWND owner) const noexcept
{
	if (owner != nullptr && !::IsRectEmpty(&m_bounds)) {
		::InvalidateRect(owner, &m_bounds, FALSE);
	}
}

void CClientMenuBar::SetHotItem(HWND owner, int index) noexcept
{
	if (m_hotItem != index) {
		m_hotItem = index;
		Invalidate(owner);
	}
}

bool CClientMenuBar::OpenItem(HWND owner, int index, bool fromKeyboard) noexcept
{
	if (owner == nullptr || m_menu == nullptr || index < 0
		|| index >= ::GetMenuItemCount(m_menu)) {
		return false;
	}
	const HMENU submenu = ::GetSubMenu(m_menu, index);
	if (submenu == nullptr) {
		const UINT command = ::GetMenuItemID(m_menu, index);
		if (command != static_cast<UINT>(-1)) {
			::SendMessageW(owner, WM_COMMAND, MAKEWPARAM(command, 0), 0);
			return true;
		}
		return false;
	}

	m_pressedItem = index;
	m_keyboardItem = index;
	Invalidate(owner);
	RECT anchor = index < static_cast<int>(m_itemBounds.size()) ? m_itemBounds[index] : m_bounds;
	POINT origin{ anchor.left, anchor.bottom };
	::ClientToScreen(owner, &origin);
	::SetForegroundWindow(owner);
	const UINT command = ::TrackPopupMenuEx(
		submenu,
		TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_VERTICAL,
		origin.x,
		origin.y,
		owner,
		nullptr
	);
	m_pressedItem = -1;
	if (command != 0) {
		::SendMessageW(owner, WM_COMMAND, MAKEWPARAM(command, 0), 0);
	}
	if (!fromKeyboard) {
		m_keyboardMode = false;
		m_keyboardItem = -1;
	}
	Invalidate(owner);
	return true;
}

bool CClientMenuBar::HandleMouseMessage(
	HWND owner,
	UINT message,
	[[maybe_unused]] WPARAM wParam,
	LPARAM lParam,
	LRESULT& result
) noexcept
{
	POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	switch (message) {
	case WM_MOUSEMOVE: {
		if (!ContainsPoint(point)) {
			return false;
		}
		SetHotItem(owner, HitTestItem(point));
		if (!m_trackingMouseLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, owner, 0 };
			m_trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		result = 0;
		return true;
	}
	case WM_MOUSELEAVE:
		m_trackingMouseLeave = false;
		SetHotItem(owner, -1);
		result = 0;
		return true;
	case WM_LBUTTONDOWN:
		if (!ContainsPoint(point)) {
			return false;
		}
		m_keyboardMode = false;
		m_keyboardItem = -1;
		result = OpenItem(owner, HitTestItem(point), false) ? 0 : 0;
		return true;
	default:
		return false;
	}
}

int CClientMenuBar::FindMnemonic(wchar_t character) const noexcept
{
	const wchar_t sought = static_cast<wchar_t>(std::towlower(character));
	const int count = m_menu == nullptr ? 0 : ::GetMenuItemCount(m_menu);
	for (int index = 0; index < count; ++index) {
		const std::wstring text = ItemText(index);
		for (std::size_t position = 0; position + 1 < text.size(); ++position) {
			if (text[position] != L'&') {
				continue;
			}
			if (text[position + 1] == L'&') {
				++position;
				continue;
			}
			if (std::towlower(text[position + 1]) == sought) {
				return index;
			}
		}
	}
	return -1;
}

void CClientMenuBar::CancelKeyboardMode(HWND owner) noexcept
{
	m_keyboardMode = false;
	m_keyboardItem = -1;
	m_altDown = false;
	m_altChord = false;
	Invalidate(owner);
}

bool CClientMenuBar::PreTranslateMessage(HWND owner, MSG& message) noexcept
{
	if (m_menu == nullptr) {
		return false;
	}
	const int count = ::GetMenuItemCount(m_menu);
	if (message.message == WM_SYSKEYDOWN && message.wParam == VK_MENU) {
		m_altDown = true;
		m_altChord = false;
		return false;
	}
	if (message.message == WM_SYSKEYDOWN && m_altDown && message.wParam != VK_MENU) {
		m_altChord = true;
	}
	if (message.message == WM_SYSKEYUP && message.wParam == VK_MENU) {
		const bool activate = m_altDown && !m_altChord;
		m_altDown = false;
		m_altChord = false;
		if (activate && count > 0) {
			m_keyboardMode = !m_keyboardMode;
			m_keyboardItem = m_keyboardMode ? 0 : -1;
			Invalidate(owner);
			return true;
		}
		return false;
	}
	if (message.message == WM_KEYDOWN && message.wParam == VK_F10) {
		m_keyboardMode = !m_keyboardMode;
		m_keyboardItem = m_keyboardMode && count > 0 ? 0 : -1;
		Invalidate(owner);
		return true;
	}
	if (message.message == WM_SYSCHAR) {
		const int mnemonic = FindMnemonic(static_cast<wchar_t>(message.wParam));
		if (mnemonic >= 0) {
			m_keyboardMode = true;
			m_keyboardItem = mnemonic;
			return OpenItem(owner, mnemonic, true);
		}
	}
	if (!m_keyboardMode || message.message != WM_KEYDOWN || count <= 0) {
		return false;
	}
	switch (message.wParam) {
	case VK_LEFT:
		m_keyboardItem = (m_keyboardItem + count - 1) % count;
		Invalidate(owner);
		return true;
	case VK_RIGHT:
		m_keyboardItem = (m_keyboardItem + 1) % count;
		Invalidate(owner);
		return true;
	case VK_DOWN:
	case VK_RETURN:
		return OpenItem(owner, std::max(0, m_keyboardItem), true);
	case VK_ESCAPE:
		CancelKeyboardMode(owner);
		return true;
	default:
		return false;
	}
}
