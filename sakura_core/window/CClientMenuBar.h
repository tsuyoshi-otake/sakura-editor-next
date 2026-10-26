/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <string>
#include <string_view>
#include <span>
#include <vector>

#include "theme/CThemeService.h"

//! Removes visual mnemonic syntax while preserving the HMENU text used by keyboard handling.
[[nodiscard]] std::wstring FormatClientMenuDisplayText(std::wstring_view text);

//! Returns the top-level item at a client point, or -1 when the point is outside all items.
[[nodiscard]] int HitTestClientMenuItemBounds(std::span<const RECT> itemBounds, POINT clientPoint) noexcept;
//! Returns a sibling menu item to open while a popup is active, or -1 when no switch is needed.
[[nodiscard]] int NextClientMenuPopupItem(int activeItem, int hoveredItem) noexcept;
//! WH_MSGFILTER reports MSGF_MENU through nCode (wParam is reserved).
[[nodiscard]] bool IsClientMenuMouseMoveFilter(int hookCode, UINT message) noexcept;

//! The result of VS Code `MenuBar.updateOverflowAction()` for one measured pass:
//! which leading menus stay visible and whether the More/hamburger button renders.
struct ClientMenuOverflowPlan {
	int shownCount = 0;       //!< Leading top-level menus kept visible (`numMenusShown`).
	bool overflowVisible = false; //!< Whether the overflow button is rendered at all.
	bool hamburgerOnly = false;   //!< `overflow-menu-only`: the button is the only visible item.
};

//! Ports `updateOverflowAction` from `src/vs/base/browser/ui/menu/menubar.ts`:
//! items fit left to right while their cumulative width stays inside
//! `availableWidth`; once `4 * (shown - 1) <= count` every item collapses into a
//! lone hamburger button; otherwise trailing items drop from the right until the
//! overflow button also fits.
[[nodiscard]] ClientMenuOverflowPlan PlanClientMenuOverflow(
	std::span<const int> itemWidths, int availableWidth, int overflowWidth) noexcept;

//! Physical width of the overflow (More/hamburger) button. Upstream reserves
//! 38px at 96 DPI (`.menubar.overflow-menu-only`, `.toolbar-toggle-more` 22px +
//! 8px horizontal padding).
[[nodiscard]] int ClientMenuBarOverflowWidth(UINT dpi) noexcept;

//! Client-owned rendering and keyboard interaction for an existing HMENU model.
//!
//! The HMENU remains owned by CEditWnd.  This class never destroys it and keeps
//! the existing command identifiers and WM_INITMENUPOPUP/WM_COMMAND flow intact.
class CClientMenuBar final {
public:
	CClientMenuBar() noexcept = default;
	~CClientMenuBar() noexcept;
	CClientMenuBar(const CClientMenuBar&) = delete;
	CClientMenuBar& operator=(const CClientMenuBar&) = delete;

	void SetMenu(HMENU menu) noexcept;
	[[nodiscard]] HMENU GetMenu() const noexcept { return m_menu; }

	void SetBounds(const RECT& bounds) noexcept;
	[[nodiscard]] const RECT& GetBounds() const noexcept { return m_bounds; }
	[[nodiscard]] int MeasurePreferredWidth(HWND owner, HFONT font, UINT dpi) const noexcept;
	void UpdateItemLayout(HWND owner, HFONT font, UINT dpi) noexcept;
	void Paint(HWND owner, HDC dc, HFONT font, const theme::ThemePalette& palette, bool active) const noexcept;

	//! `overflow-menu-only`: only the hamburger button renders. The frame then
	//! shrinks the menu rectangle to the overflow width, as upstream's 38px class.
	[[nodiscard]] bool IsOverflowMenuOnly() const noexcept
	{
		return m_overflowVisible && m_numMenusShown == 0;
	}

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
	struct PopupTrackingState;
	class PopupTrackingScope;

	[[nodiscard]] std::wstring ItemText(int index) const;
	[[nodiscard]] std::wstring DisplayItemText(int index) const;
	[[nodiscard]] int FindMnemonic(wchar_t character) const noexcept;
	//! Rendered index of the overflow button, or -1 while it is hidden. The
	//! rendered index space is [0, m_numMenusShown) menu items plus this slot.
	[[nodiscard]] int OverflowItemIndex() const noexcept
	{
		return m_overflowVisible ? m_numMenusShown : -1;
	}
	[[nodiscard]] bool OpenItem(HWND owner, int index, bool fromKeyboard, int overflowTriggerIndex = -1) noexcept;
	[[nodiscard]] UINT TrackOverflowPopup(HWND owner, const RECT& anchor, int triggerIndex) noexcept;
	static LRESULT CALLBACK PopupMenuMessageFilter(int code, WPARAM wParam, LPARAM lParam) noexcept;
	void SetHotItem(HWND owner, int index) noexcept;
	void Invalidate(HWND owner) const noexcept;
	//! Drops hot/pressed/keyboard state for rendered items a new layout removed,
	//! as the frame already does for collapsed title controls.
	void ClearHiddenItemState(HWND owner) noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) const noexcept;
	void ReleaseCodiconFont() const noexcept;

	HMENU m_menu = nullptr;
	RECT m_bounds{};
	std::vector<RECT> m_itemBounds;
	int m_numMenusShown = 0;
	bool m_overflowVisible = false;
	mutable HFONT m_codiconFont = nullptr;
	mutable int m_codiconFontHeight = 0;
	int m_hotItem = -1;
	int m_pressedItem = -1;
	int m_keyboardItem = -1;
	bool m_keyboardMode = false;
	bool m_altDown = false;
	bool m_altChord = false;
	bool m_trackingMouseLeave = false;

	static thread_local PopupTrackingState* s_popupTrackingState;
};
