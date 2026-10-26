/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CClientMenuBar.h"

#include "CSelectLang.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"

#include <algorithm>
#include <cwctype>
#include <optional>

namespace {

int ScaleDip(int value, UINT dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

// The overflow button renders a single codicon inside the 22x22 DIP box
// `toolbar-toggle-more` reserves; the 16 DIP glyph matches the title controls.
constexpr int kOverflowIconDip = 16;

constexpr wchar_t kOverflowPartialGlyphName[] = L"more"; // Codicon.menuBarMore
constexpr wchar_t kOverflowHamburgerGlyphName[] = L"menu"; // --vscode-icon-menu-content

bool Contains(const RECT& rect, POINT point) noexcept
{
	return point.x >= rect.left && point.x < rect.right
		&& point.y >= rect.top && point.y < rect.bottom;
}

} // namespace

int HitTestClientMenuItemBounds(std::span<const RECT> itemBounds, POINT clientPoint) noexcept
{
	for (std::size_t index = 0; index < itemBounds.size(); ++index) {
		if (Contains(itemBounds[index], clientPoint)) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

int NextClientMenuPopupItem(int activeItem, int hoveredItem) noexcept
{
	return hoveredItem >= 0 && hoveredItem != activeItem ? hoveredItem : -1;
}

bool IsClientMenuMouseMoveFilter(int hookCode, UINT message) noexcept
{
	return hookCode == MSGF_MENU && message == WM_MOUSEMOVE;
}

ClientMenuOverflowPlan PlanClientMenuOverflow(
	std::span<const int> itemWidths,
	int availableWidth,
	int overflowWidth
) noexcept
{
	ClientMenuOverflowPlan plan{};
	const int count = static_cast<int>(itemWidths.size());
	if (count == 0) {
		return plan;
	}
	const int available = std::max(0, availableWidth);
	int currentSize = 0;
	int shown = 0;
	bool full = false;
	for (const int width : itemWidths) {
		if (!full) {
			if (currentSize + std::max(0, width) > available) {
				full = true;
			} else {
				currentSize += std::max(0, width);
				++shown;
			}
		}
	}
	// Upstream collapses to the lone hamburger when
	// `numMenusShown - 1 <= showableMenus.length / 4`; multiplying through keeps
	// the boundary exact in integers, including the single-menu corner where a
	// fitting menu still collapses.
	if (4 * (shown - 1) <= count) {
		shown = 0;
		currentSize = 0;
		full = true;
	}
	if (full) {
		const int overflow = std::max(0, overflowWidth);
		while (currentSize + overflow > available && shown > 0) {
			--shown;
			currentSize -= std::max(0, itemWidths[shown]);
		}
		plan.overflowVisible = true;
	}
	plan.shownCount = shown;
	plan.hamburgerOnly = plan.overflowVisible && shown == 0;
	return plan;
}

int ClientMenuBarOverflowWidth(UINT dpi) noexcept
{
	return ScaleDip(38, dpi);
}

struct CClientMenuBar::PopupTrackingState final {
	CClientMenuBar* menuBar = nullptr;
	HWND owner = nullptr;
	int activeItem = -1;
	int pendingItem = -1;
	HHOOK hook = nullptr;
	PopupTrackingState* previous = nullptr;
};

thread_local CClientMenuBar::PopupTrackingState* CClientMenuBar::s_popupTrackingState = nullptr;

class CClientMenuBar::PopupTrackingScope final {
public:
	PopupTrackingScope(CClientMenuBar& menuBar, HWND owner, int activeItem) noexcept
	{
		m_state.menuBar = &menuBar;
		m_state.owner = owner;
		m_state.activeItem = activeItem;
		m_state.previous = s_popupTrackingState;
		s_popupTrackingState = &m_state;
		m_state.hook = ::SetWindowsHookExW(
			WH_MSGFILTER,
			&CClientMenuBar::PopupMenuMessageFilter,
			nullptr,
			::GetCurrentThreadId()
		);
	}

	~PopupTrackingScope()
	{
		if (m_state.hook != nullptr) {
			::UnhookWindowsHookEx(m_state.hook);
		}
		s_popupTrackingState = m_state.previous;
	}

	[[nodiscard]] int PendingItem() const noexcept { return m_state.pendingItem; }

private:
	PopupTrackingState m_state;
};

CClientMenuBar::~CClientMenuBar() noexcept
{
	ReleaseCodiconFont();
}

HFONT CClientMenuBar::AcquireCodiconFont(int height) const noexcept
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

void CClientMenuBar::ReleaseCodiconFont() const noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

std::wstring FormatClientMenuDisplayText(std::wstring_view text)
{
	// Japanese resources conventionally append "(&F)" to a localized label.
	// Keep that mnemonic in the HMENU model, but do not expose the suffix in the
	// always-visible client-owned title bar.
	if (text.size() >= 4 && text[text.size() - 4] == L'('
		&& text[text.size() - 3] == L'&' && text.back() == L')') {
		text.remove_suffix(4);
	}

	std::wstring result;
	result.reserve(text.size());
	for (std::size_t index = 0; index < text.size(); ++index) {
		if (text[index] != L'&') {
			result.push_back(text[index]);
			continue;
		}
		if (index + 1 < text.size() && text[index + 1] == L'&') {
			result.push_back(L'&');
			++index;
		}
	}
	return result;
}

void CClientMenuBar::SetMenu(HMENU menu) noexcept
{
	m_menu = menu;
	m_itemBounds.clear();
	m_numMenusShown = 0;
	m_overflowVisible = false;
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

std::wstring CClientMenuBar::DisplayItemText(int index) const
{
	return FormatClientMenuDisplayText(ItemText(index));
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
	const int horizontalPadding = ScaleDip(8, dpi);
	int width = 0;
	for (int index = 0; index < count; ++index) {
		const std::wstring text = DisplayItemText(index);
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

void CClientMenuBar::UpdateItemLayout(HWND owner, HFONT font, UINT dpi) noexcept
{
	m_itemBounds.clear();
	m_numMenusShown = 0;
	m_overflowVisible = false;
	const auto finish = [this, owner]() noexcept { ClearHiddenItemState(owner); };
	if (m_menu == nullptr || owner == nullptr || ::IsRectEmpty(&m_bounds)) {
		finish();
		return;
	}
	const HDC dc = ::GetDC(owner);
	if (dc == nullptr) {
		finish();
		return;
	}
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	const int horizontalPadding = ScaleDip(8, dpi);
	const int count = ::GetMenuItemCount(m_menu);
	std::vector<int> itemWidths(static_cast<std::size_t>(std::max(0, count)), 0);
	for (int index = 0; index < count; ++index) {
		const std::wstring text = DisplayItemText(index);
		SIZE extent{};
		if (!text.empty()) {
			::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
		}
		itemWidths[static_cast<std::size_t>(index)] = extent.cx + horizontalPadding * 2;
	}
	if (oldFont != nullptr) {
		::SelectObject(dc, oldFont);
	}
	::ReleaseDC(owner, dc);

	const int available = m_bounds.right - m_bounds.left;
	const int overflowWidth = ClientMenuBarOverflowWidth(dpi);
	const ClientMenuOverflowPlan plan = PlanClientMenuOverflow(itemWidths, available, overflowWidth);
	m_numMenusShown = plan.shownCount;
	m_overflowVisible = plan.overflowVisible;
	LONG left = m_bounds.left;
	for (int index = 0; index < plan.shownCount; ++index) {
		const LONG right = std::min<LONG>(m_bounds.right,
			left + itemWidths[static_cast<std::size_t>(index)]);
		m_itemBounds.push_back({ left, m_bounds.top, right, m_bounds.bottom });
		left = right;
	}
	if (plan.overflowVisible) {
		// The overflow button is inserted directly after the last shown menu,
		// exactly where upstream moves it in the DOM.
		const LONG right = std::min<LONG>(m_bounds.right, left + overflowWidth);
		m_itemBounds.push_back({ left, m_bounds.top, right, m_bounds.bottom });
	}
	ClearHiddenItemState(owner);
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
	const int overflowIndex = OverflowItemIndex();
	const UINT dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
	const HFONT codiconFont = overflowIndex >= 0
		? AcquireCodiconFont(ScaleDip(kOverflowIconDip, dpi)) : nullptr;
	const HGDIOBJ oldFont = font == nullptr ? nullptr : ::SelectObject(dc, font);
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, (active ? palette.primaryText : palette.secondaryText).ToColorRef());
	for (std::size_t index = 0; index < m_itemBounds.size(); ++index) {
		const int itemIndex = static_cast<int>(index);
		const bool highlighted = itemIndex == m_hotItem
			|| itemIndex == m_pressedItem
			|| (m_keyboardMode && itemIndex == m_keyboardItem);
		if (highlighted) {
			const HBRUSH brush = ::CreateSolidBrush(palette.raised.ToColorRef());
			::FillRect(dc, &m_itemBounds[index], brush);
			::DeleteObject(brush);
		}
		RECT textRect = m_itemBounds[index];
		if (itemIndex == overflowIndex) {
			// Upstream CSS swaps the `Codicon.menuBarMore` glyph for the
			// hamburger `--vscode-icon-menu-content` once it is the bar's only
			// child. Both glyphs come from the bundled codicon font.
			const std::wstring_view glyphName = m_numMenusShown == 0
				? std::wstring_view(kOverflowHamburgerGlyphName)
				: std::wstring_view(kOverflowPartialGlyphName);
			const wchar_t codePoint =
				workbench::icons::FindCodiconGlyph(glyphName).value_or(L'\0');
			const auto iconBox = workbench::icons::CenteredIconBounds(
				{ textRect.left, textRect.top, textRect.right, textRect.bottom },
				kOverflowIconDip, dpi);
			RECT glyphRect{ iconBox.left, iconBox.top, iconBox.right, iconBox.bottom };
			bool painted = false;
			if (codiconFont != nullptr && codePoint != L'\0') {
				const wchar_t text[] = { codePoint, L'\0' };
				::SelectObject(dc, codiconFont);
				painted = ::DrawTextW(dc, text, 1, &glyphRect,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX) != 0;
				::SelectObject(dc, font);
			}
			if (!painted) {
				// Degraded fallback: still a visible affordance that opens the popup.
				static const wchar_t kFallback[] = L"...";
				::DrawTextW(dc, kFallback, 3, &textRect,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			}
		} else {
			const UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE
				| (m_keyboardMode ? 0 : DT_HIDEPREFIX);
			const std::wstring text = DisplayItemText(itemIndex);
			::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect, format);
		}
		if (m_keyboardMode && itemIndex == m_keyboardItem) {
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
	return HitTestClientMenuItemBounds(m_itemBounds, clientPoint);
}

int CClientMenuBar::AccessibilityItemCount() const noexcept
{
	// Only the rendered items are exposed: the shown menus plus the overflow
	// button. Hidden menus stay reachable through that button's popup and must
	// not appear as stray UIA peers at stale coordinates.
	return static_cast<int>(m_itemBounds.size());
}

std::wstring CClientMenuBar::AccessibilityItemText(int index) const
{
	if (index == OverflowItemIndex()) {
		return LS(STR_WORKBENCH_ACCESSIBILITY_MENUBAR_MORE);
	}
	return DisplayItemText(index);
}

RECT CClientMenuBar::AccessibilityItemBounds(int index) const noexcept
{
	return index >= 0 && index < static_cast<int>(m_itemBounds.size()) ? m_itemBounds[index] : RECT{};
}

bool CClientMenuBar::AccessibilityItemEnabled(int index) const noexcept
{
	if (index == OverflowItemIndex()) {
		return true;
	}
	if (m_menu == nullptr || index < 0 || index >= m_numMenusShown) return false;
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

bool CClientMenuBar::OpenItem(HWND owner, int index, bool fromKeyboard, int overflowTriggerIndex) noexcept
{
	if (owner == nullptr || m_menu == nullptr || index < 0
		|| index >= static_cast<int>(m_itemBounds.size())) {
		return false;
	}
	int currentItem = index;
	int pendingTrigger = overflowTriggerIndex;
	UINT command = 0;
	for (;;) {
		const RECT anchor = currentItem < static_cast<int>(m_itemBounds.size()) ? m_itemBounds[currentItem] : m_bounds;
		int nextItem = -1;
		if (currentItem == OverflowItemIndex()) {
			m_pressedItem = currentItem;
			m_keyboardItem = currentItem;
			Invalidate(owner);
			PopupTrackingScope tracking(*this, owner, currentItem);
			command = TrackOverflowPopup(owner, anchor, pendingTrigger);
			pendingTrigger = -1;
			nextItem = tracking.PendingItem();
		} else {
			const HMENU submenu = ::GetSubMenu(m_menu, currentItem);
			if (submenu == nullptr) {
				const UINT itemCommand = ::GetMenuItemID(m_menu, currentItem);
				if (itemCommand != static_cast<UINT>(-1)) {
					::SendMessageW(owner, WM_COMMAND, MAKEWPARAM(itemCommand, 0), 0);
					return true;
				}
				return false;
			}
			m_pressedItem = currentItem;
			m_keyboardItem = currentItem;
			Invalidate(owner);
			POINT origin{ anchor.left, anchor.bottom };
			::ClientToScreen(owner, &origin);
			::SetForegroundWindow(owner);
			// TrackPopupMenuEx treats the supplied submenu as a standalone popup and
			// reports position zero in WM_INITMENUPOPUP. Sakura builds each top-level
			// submenu lazily from that position, so explicitly initialize it with its
			// real root-menu index before entering the modal popup loop. Without this,
			// only the first (File) menu receives contents and every later menu is empty.
			::SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), MAKELPARAM(currentItem, FALSE));
			PopupTrackingScope tracking(*this, owner, currentItem);
			command = ::TrackPopupMenuEx(
				submenu,
				TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_VERTICAL,
				origin.x,
				origin.y,
				owner,
				nullptr
			);
			nextItem = tracking.PendingItem();
		}
		if (command != 0 || nextItem < 0) {
			break;
		}
		currentItem = nextItem;
	}
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

UINT CClientMenuBar::TrackOverflowPopup(HWND owner, const RECT& anchor, int triggerIndex) noexcept
{
	const int count = ::GetMenuItemCount(m_menu);
	if (m_numMenusShown >= count) {
		return 0;
	}
	const HMENU popup = ::CreatePopupMenu();
	if (popup == nullptr) {
		return 0;
	}
	// One SubmenuAction per hidden top-level menu, in order. The submenus stay
	// owned by CEditWnd's HMENU; the popup only borrows them for the duration of
	// the track and detaches them before it is destroyed.
	int triggerIsCascade = 0;
	for (int index = m_numMenusShown; index < count; ++index) {
		const std::wstring text = ItemText(index);
		const HMENU submenu = ::GetSubMenu(m_menu, index);
		if (submenu != nullptr) {
			// Same lazy-build contract as OpenItem: initialize each hidden
			// submenu with its real root-menu index so recent-file lists and
			// other dynamic contents exist. The modal loop's own INITMENUPOPUP
			// later reports the popup-relative position and only refreshes
			// enable/check states.
			::SendMessageW(owner, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), MAKELPARAM(index, FALSE));
			if (::AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), text.c_str()) == FALSE) {
				continue;
			}
			if (index - m_numMenusShown == triggerIndex) {
				triggerIsCascade = 1;
			}
		} else {
			const UINT itemCommand = ::GetMenuItemID(m_menu, index);
			if (itemCommand == static_cast<UINT>(-1)
				|| ::AppendMenuW(popup, MF_STRING, itemCommand, text.c_str()) == FALSE) {
				continue;
			}
		}
	}
	if (::GetMenuItemCount(popup) <= 0) {
		::DestroyMenu(popup);
		return 0;
	}
	POINT origin{ anchor.left, anchor.bottom };
	::ClientToScreen(owner, &origin);
	::SetForegroundWindow(owner);
	// menuWidget.trigger(index): a mnemonic on a hidden menu opens the overflow
	// with that menu's row selected and its cascade run. Plain arrow keys need
	// no modifier state, so the modal loop consumes the queued keys directly.
	if (triggerIndex >= 0) {
		for (int step = 0; step <= triggerIndex; ++step) {
			::PostMessageW(owner, WM_KEYDOWN, VK_DOWN, 0);
		}
		::PostMessageW(owner, WM_KEYDOWN, triggerIsCascade ? VK_RIGHT : VK_RETURN, 0);
	}
	const UINT command = ::TrackPopupMenuEx(
		popup,
		TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_VERTICAL,
		origin.x,
		origin.y,
		owner,
		nullptr
	);
	// Detach every borrowed submenu before the container dies; DestroyMenu would
	// otherwise recurse into and destroy CEditWnd's live submenus.
	while (::RemoveMenu(popup, 0, MF_BYPOSITION) != FALSE) {
	}
	::DestroyMenu(popup);
	return command;
}

void CClientMenuBar::ClearHiddenItemState(HWND owner) noexcept
{
	const int rendered = static_cast<int>(m_itemBounds.size());
	bool invalidated = false;
	if (m_hotItem >= rendered) {
		m_hotItem = -1;
		invalidated = true;
	}
	if (m_pressedItem >= rendered) {
		m_pressedItem = -1;
		invalidated = true;
	}
	if (m_keyboardItem >= rendered) {
		m_keyboardItem = -1;
		m_keyboardMode = false;
		invalidated = true;
	}
	if (invalidated) {
		Invalidate(owner);
	}
}

LRESULT CALLBACK CClientMenuBar::PopupMenuMessageFilter(int code, WPARAM wParam, LPARAM lParam) noexcept
{
	(void)wParam; // Reserved by WH_MSGFILTER; MSGF_MENU is delivered in nCode.
	PopupTrackingState* const state = s_popupTrackingState;
	if (code >= 0 && state != nullptr && lParam != 0) {
		const MSG* const message = reinterpret_cast<const MSG*>(lParam);
		if (IsClientMenuMouseMoveFilter(code, message->message) && state->pendingItem < 0) {
			POINT point{};
			if (::GetCursorPos(&point) != FALSE) {
				::ScreenToClient(state->owner, &point);
				const int hoveredItem = state->menuBar->HitTestItem(point);
				const int nextItem = NextClientMenuPopupItem(state->activeItem, hoveredItem);
				if (nextItem >= 0) {
					state->pendingItem = nextItem;
					::EndMenu();
					return 1;
				}
			}
		}
	}
	// The hook handle is intentionally null: the system does not require it for
	// a thread hook, and a nested menu loop can temporarily replace the active
	// thread-local state while an earlier hook is still unwinding.
	return ::CallNextHookEx(nullptr, code, wParam, lParam);
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
		m_pressedItem = HitTestItem(point);
		SetHotItem(owner, m_pressedItem);
		if (m_pressedItem >= 0) ::SetCapture(owner);
		Invalidate(owner);
		result = 0;
		return true;
	case WM_LBUTTONUP: {
		if (m_pressedItem < 0) return false;
		const int pressedItem = m_pressedItem;
		m_pressedItem = -1;
		if (::GetCapture() == owner) ::ReleaseCapture();
		const bool invoke = ContainsPoint(point) && HitTestItem(point) == pressedItem;
		Invalidate(owner);
		if (invoke) (void)OpenItem(owner, pressedItem, false);
		result = 0;
		return true;
	}
	case WM_CAPTURECHANGED:
		if (m_pressedItem < 0) return false;
		m_pressedItem = -1;
		Invalidate(owner);
		result = 0;
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
	const int rendered = static_cast<int>(m_itemBounds.size());
	// Alt/F10 lands on the overflow button once every menu is hidden, matching
	// upstream `focusedMenu.index = numMenusShown > 0 ? 0 : OVERFLOW_INDEX`.
	const auto firstFocusable = [this]() noexcept {
		return m_numMenusShown > 0 ? 0 : OverflowItemIndex();
	};
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
		if (activate && rendered > 0) {
			m_keyboardMode = !m_keyboardMode;
			m_keyboardItem = m_keyboardMode ? firstFocusable() : -1;
			Invalidate(owner);
			return true;
		}
		return false;
	}
	if (message.message == WM_KEYDOWN && message.wParam == VK_F10) {
		m_keyboardMode = !m_keyboardMode;
		m_keyboardItem = m_keyboardMode && rendered > 0 ? firstFocusable() : -1;
		Invalidate(owner);
		return true;
	}
	if (message.message == WM_SYSCHAR) {
		const int mnemonic = FindMnemonic(static_cast<wchar_t>(message.wParam));
		if (mnemonic >= 0) {
			m_keyboardMode = true;
			if (mnemonic < m_numMenusShown) {
				m_keyboardItem = mnemonic;
				return OpenItem(owner, mnemonic, true);
			}
			// A hidden menu's mnemonic opens the overflow and triggers the
			// matching row, as upstream `menuWidget.trigger(menuIndex)`.
			m_keyboardItem = OverflowItemIndex();
			return OpenItem(owner, m_keyboardItem, true, mnemonic - m_numMenusShown);
		}
	}
	if (!m_keyboardMode || message.message != WM_KEYDOWN || rendered <= 0) {
		return false;
	}
	switch (message.wParam) {
	case VK_LEFT:
		m_keyboardItem = (m_keyboardItem + rendered - 1) % rendered;
		Invalidate(owner);
		return true;
	case VK_RIGHT:
		m_keyboardItem = (m_keyboardItem + 1) % rendered;
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
