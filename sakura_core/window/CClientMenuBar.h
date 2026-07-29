/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <string>
#include <vector>

#include "theme/CThemeService.h"

//! Client-owned rendering and keyboard interaction for an existing HMENU model.
//!
//! The HMENU remains owned by CEditWnd.  This class never destroys it and keeps
//! the existing command identifiers and WM_INITMENUPOPUP/WM_COMMAND flow intact.
class CClientMenuBar final {
public:
	void SetMenu(HMENU menu) noexcept;
	[[nodiscard]] HMENU GetMenu() const noexcept { return m_menu; }

	void SetBounds(const RECT& bounds) noexcept;
	[[nodiscard]] const RECT& GetBounds() const noexcept { return m_bounds; }
	[[nodiscard]] int MeasurePreferredWidth(HWND owner, HFONT font, UINT dpi) const noexcept;
	void UpdateItemLayout(HWND owner, HFONT font) noexcept;
	void Paint(HWND owner, HDC dc, HFONT font, const theme::ThemePalette& palette, bool active) const noexcept;

	//! Handles client mouse messages whose point lies in the title/menu strip.
	[[nodiscard]] bool HandleMouseMessage(HWND owner, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) noexcept;
	//! Runs before the editor accelerator table so Alt/F10 and menu navigation keep standard semantics.
	[[nodiscard]] bool PreTranslateMessage(HWND owner, MSG& message) noexcept;
	void CancelKeyboardMode(HWND owner) noexcept;

	[[nodiscard]] int HitTestItem(POINT clientPoint) const noexcept;
	[[nodiscard]] bool ContainsPoint(POINT clientPoint) const noexcept;
	[[nodiscard]] bool IsKeyboardMode() const noexcept { return m_keyboardMode; }
	[[nodiscard]] int AccessibilityItemCount() const noexcept;
	[[nodiscard]] std::wstring AccessibilityItemText(int index) const;
	[[nodiscard]] RECT AccessibilityItemBounds(int index) const noexcept;
	[[nodiscard]] bool AccessibilityItemEnabled(int index) const noexcept;
	[[nodiscard]] int AccessibilityFocusedItem() const noexcept { return m_keyboardMode ? m_keyboardItem : -1; }
	[[nodiscard]] bool InvokeAccessibilityItem(HWND owner, int index) noexcept;
	void SetAccessibilityFocusedItem(HWND owner, int index) noexcept;

private:
	[[nodiscard]] std::wstring ItemText(int index) const;
	[[nodiscard]] int FindMnemonic(wchar_t character) const noexcept;
	[[nodiscard]] bool OpenItem(HWND owner, int index, bool fromKeyboard) noexcept;
	void SetHotItem(HWND owner, int index) noexcept;
	void Invalidate(HWND owner) const noexcept;

	HMENU m_menu = nullptr;
	RECT m_bounds{};
	std::vector<RECT> m_itemBounds;
	int m_hotItem = -1;
	int m_pressedItem = -1;
	int m_keyboardItem = -1;
	bool m_keyboardMode = false;
	bool m_altDown = false;
	bool m_altChord = false;
	bool m_trackingMouseLeave = false;
};
