/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/projects/ProjectsListPane.h"

#include "workbench/controls/ListBoxOverlayScrolling.h"

#include <CommCtrl.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace workbench::projects {
namespace {

constexpr UINT_PTR kListSubclassId = 0x7072;
constexpr int kEmptyMessageInsetDip = 20;
constexpr int kEmptyMessageTopDip = 4;

int ScaleDip(const int value, const unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

} // namespace

CProjectsListPane::~CProjectsListPane()
{
	Destroy();
}

bool CProjectsListPane::Create(const HWND parent, const int controlId, const HINSTANCE instance,
	const HWND tooltip, ProjectsListPaneCallbacks callbacks) noexcept
{
	if (m_list != nullptr || parent == nullptr) return false;
	try {
		m_callbacks = std::move(callbacks);
	} catch (...) {
		return false;
	}
	m_parent = parent;
	m_list = ::CreateWindowExW(0, L"LISTBOX", L"",
		WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED
			| LBS_HASSTRINGS | LBS_NOTIFY,
		0, 0, 0, 0, parent,
		reinterpret_cast<HMENU>(static_cast<UINT_PTR>(controlId)), instance, nullptr);
	if (m_list == nullptr) return false;
	// Hover text is optional: without the tool the rows still work. The tool's
	// TTF_SUBCLASS hook is installed before the list subclass below; the later
	// subclass runs first, so a move retargets the text before the tooltip
	// relays it.
	if (tooltip != nullptr) {
		m_tooltip = tooltip;
		TOOLINFOW tool = Tool();
		tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
		tool.lpszText = const_cast<wchar_t*>(L"");
		m_toolAdded = ::SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE;
	}
	if (::SetWindowSubclass(m_list, &CProjectsListPane::SubclassProc,
		kListSubclassId, reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
		Destroy();
		return false;
	}
	// The VS Code overlay every other workbench list draws (#407). The list
	// keeps WS_VSCROLL for its own scroll position and the overlay hides the
	// platform bar. Without the overlay the list still scrolls natively.
	try {
		if (m_scrollbar.Create(parent, m_list, [this](const int topRow) {
			if (m_list == nullptr) return;
			(void)::SendMessageW(m_list, LB_SETTOPINDEX, static_cast<WPARAM>(topRow), 0);
			// The overlay repaints from this model right after the callback.
			m_scrollbar.SetScrollModel(controls::ListBoxOverlayScrollbarModel(m_list));
			HoverRowUnderCursor();
		}, controls::OverlayScrollbarSource::ExplicitModel)) {
			m_scrollbar.SetHideNativeBar(true);
		}
	} catch (...) {
		m_scrollbar.Destroy();
	}
	return true;
}

void CProjectsListPane::Destroy() noexcept
{
	// The tooltip is a top-level popup owned by the page, so it can outlive
	// this list; take the tool back while both still exist.
	if (m_toolAdded && m_tooltip != nullptr && ::IsWindow(m_tooltip) && m_list != nullptr) {
		TOOLINFOW tool = Tool();
		(void)::SendMessageW(m_tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
	}
	m_toolAdded = false;
	m_tooltip = nullptr;
	m_scrollbar.Destroy();
	if (m_list != nullptr && ::IsWindow(m_list)) ::DestroyWindow(m_list);
	m_list = nullptr;
	m_parent = nullptr;
	m_hoveredRow.reset();
	m_pressedRow.reset();
}

void CProjectsListPane::SetRows(const std::span<const std::wstring> labels,
	const std::optional<std::size_t> selected) noexcept
{
	m_pressedRow.reset();
	if (m_list == nullptr) return;
	::SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
	(void)::SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
	for (const auto& label : labels) {
		(void)::SendMessageW(m_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
	}
	if (selected && *selected < labels.size()) {
		(void)::SendMessageW(m_list, LB_SETCURSEL, *selected, 0);
	}
	::SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
	// LB_ADDSTRING can re-show the platform bar; hide it before the paint.
	UpdateScrollbar();
	::RedrawWindow(m_list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
	// The hovered index may now name another row.
	HoverRowUnderCursor();
}

std::optional<std::size_t> CProjectsListPane::Selection() const noexcept
{
	if (m_list == nullptr) return std::nullopt;
	const LRESULT selected = ::SendMessageW(m_list, LB_GETCURSEL, 0, 0);
	if (selected == LB_ERR) return std::nullopt;
	return static_cast<std::size_t>(selected);
}

void CProjectsListPane::Select(const std::size_t row) noexcept
{
	if (m_list != nullptr) (void)::SendMessageW(m_list, LB_SETCURSEL, row, 0);
}

std::optional<std::size_t> CProjectsListPane::RowAt(const POINT point) const noexcept
{
	if (m_list == nullptr || point.x < 0 || point.y < 0) return std::nullopt;
	const LRESULT count = ::SendMessageW(m_list, LB_GETCOUNT, 0, 0);
	const LRESULT hit = ::SendMessageW(m_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
	if (count == LB_ERR || hit == LB_ERR || HIWORD(hit) != 0 || LOWORD(hit) >= count) return std::nullopt;
	RECT item{};
	if (::SendMessageW(m_list, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&item)) == LB_ERR
		|| ::PtInRect(&item, point) == FALSE) return std::nullopt;
	return static_cast<std::size_t>(LOWORD(hit));
}

void CProjectsListPane::SetBounds(const RECT& bounds) noexcept
{
	if (m_list == nullptr) return;
	(void)::SetWindowPos(m_list, nullptr, bounds.left, bounds.top,
		std::max(0L, bounds.right - bounds.left), std::max(0L, bounds.bottom - bounds.top),
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
	UpdateScrollbar();
}

void CProjectsListPane::SetVisible(const bool visible) noexcept
{
	if (m_list == nullptr) return;
	::ShowWindow(m_list, visible ? SW_SHOW : SW_HIDE);
	// The overlay follows the list's own WS_VISIBLE.
	UpdateScrollbar();
}

void CProjectsListPane::ApplyDpi(const unsigned int dpi, const HFONT font, const int rowHeight) noexcept
{
	m_dpi = dpi == 0 ? 96 : dpi;
	m_font = font;
	if (m_list != nullptr) {
		::SendMessageW(m_list, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
		::SendMessageW(m_list, LB_SETITEMHEIGHT, 0, rowHeight);
	}
	UpdateScrollbar();
}

void CProjectsListPane::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	UpdateScrollbar();
}

void CProjectsListPane::SetEmptyMessage(std::wstring message) noexcept
{
	if (message == m_emptyMessage) return;
	m_emptyMessage = std::move(message);
	Invalidate();
}

void CProjectsListPane::UpdateScrollbar() noexcept
{
	if (m_scrollbar.Get() == nullptr || m_list == nullptr) return;
	try {
		// The Side Bar tokens the Explorer and Source Control lists map theirs from.
		m_scrollbar.SetDpi(m_dpi);
		m_scrollbar.SetColors(controls::ResolveOverlayScrollbarColors(m_palette, m_palette.sideBar));
		m_scrollbar.SetScrollModel(controls::ListBoxOverlayScrollbarModel(m_list));
		m_scrollbar.Update();
	} catch (...) {
		// The rows stay usable; only the overlay keeps its previous state.
	}
}

void CProjectsListPane::HoverRowUnderCursor() noexcept
{
	m_hoveredRow.reset();
	SetTooltipText({});
	POINT cursor{};
	if (m_list == nullptr || ::GetCursorPos(&cursor) == FALSE
		|| ::WindowFromPoint(cursor) != m_list
		|| ::ScreenToClient(m_list, &cursor) == FALSE) return;
	HoverRowAt(cursor);
}

void CProjectsListPane::Invalidate() noexcept
{
	if (m_list != nullptr) ::InvalidateRect(m_list, nullptr, FALSE);
}

TOOLINFOW CProjectsListPane::Tool() const noexcept
{
	TOOLINFOW tool{};
	tool.cbSize = sizeof(tool);
	tool.hwnd = m_parent;
	tool.uId = reinterpret_cast<UINT_PTR>(m_list);
	return tool;
}

std::optional<std::size_t> CProjectsListPane::SingleClickRowAt(const POINT point) const noexcept
{
	const auto row = RowAt(point);
	if (!row || !m_callbacks.opensOnSingleClick) return std::nullopt;
	try {
		if (!m_callbacks.opensOnSingleClick(*row)) return std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
	return row;
}

void CProjectsListPane::HoverRowAt(const POINT point) noexcept
{
	const auto hovered = RowAt(point);
	if (hovered == m_hoveredRow) return;
	m_hoveredRow = hovered;
	std::wstring text;
	if (hovered && m_callbacks.tooltip) {
		try { text = m_callbacks.tooltip(*hovered); }
		catch (...) { text.clear(); }
	}
	SetTooltipText(text);
}

void CProjectsListPane::SetTooltipText(const std::wstring& text) noexcept
{
	if (!m_toolAdded || m_tooltip == nullptr || m_list == nullptr) return;
	// Hide the previous row's tip; the next pointer move over the new row
	// starts the tooltip's own initial delay again.
	(void)::SendMessageW(m_tooltip, TTM_POP, 0, 0);
	TOOLINFOW tool = Tool();
	tool.lpszText = const_cast<wchar_t*>(text.c_str());
	(void)::SendMessageW(m_tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
}

bool CProjectsListPane::PaintEmptyMessage() noexcept
{
	if (m_list == nullptr || m_emptyMessage.empty()
		|| ::SendMessageW(m_list, LB_GETCOUNT, 0, 0) != 0) return false;
	PAINTSTRUCT paint{};
	const HDC dc = ::BeginPaint(m_list, &paint);
	if (dc == nullptr) return true;
	RECT client{};
	::GetClientRect(m_list, &client);
	::SetDCBrushColor(dc, m_palette.sideBar.ToColorRef());
	::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	const HGDIOBJ previousFont = m_font != nullptr ? ::SelectObject(dc, m_font) : nullptr;
	const int previousMode = ::SetBkMode(dc, TRANSPARENT);
	const COLORREF previousColor = ::SetTextColor(dc, m_palette.descriptionText.ToColorRef());
	RECT body = client;
	body.left += ScaleDip(kEmptyMessageInsetDip, m_dpi);
	body.right -= ScaleDip(kEmptyMessageInsetDip, m_dpi);
	body.top += ScaleDip(kEmptyMessageTopDip, m_dpi);
	::DrawTextW(dc, m_emptyMessage.c_str(), static_cast<int>(m_emptyMessage.size()), &body,
		DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
	::SetTextColor(dc, previousColor);
	::SetBkMode(dc, previousMode);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	::EndPaint(m_list, &paint);
	return true;
}

LRESULT CALLBACK CProjectsListPane::SubclassProc(const HWND window, const UINT message,
	const WPARAM wParam, const LPARAM lParam, const UINT_PTR id, const DWORD_PTR data)
{
	auto* self = reinterpret_cast<CProjectsListPane*>(data);
	if (message == WM_NCDESTROY) {
		(void)::RemoveWindowSubclass(window, &CProjectsListPane::SubclassProc, id);
		if (self != nullptr && self->m_list == window) self->m_list = nullptr;
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (self == nullptr) return ::DefSubclassProc(window, message, wParam, lParam);
	switch (message) {
	case WM_PAINT:
		if (self->PaintEmptyMessage()) return 0;
		break;
	case WM_CONTEXTMENU:
		if (self->m_callbacks.contextMenu) {
			try { self->m_callbacks.contextMenu(lParam); } catch (...) {}
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (self->m_callbacks.keyDown) {
			bool handled = false;
			try { handled = self->m_callbacks.keyDown(wParam); } catch (...) { handled = false; }
			if (handled) return 0;
		}
		break;
	case WM_LBUTTONDOWN:
		self->m_pressedRow = self->SingleClickRowAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		break;
	case WM_LBUTTONDBLCLK:
		// The second press of a double click belongs to LBN_DBLCLK.
		self->m_pressedRow.reset();
		break;
	case WM_LBUTTONUP: {
		// VS Code's singleClick open and expand modes (#412): releasing the
		// press on the same row opens it. The list selects the row and
		// releases capture inside DefSubclassProc.
		const auto pressed = std::exchange(self->m_pressedRow, std::nullopt);
		const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
		if (pressed && pressed == self->SingleClickRowAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) })
			&& self->Selection() == pressed && self->m_callbacks.clicked) {
			try { self->m_callbacks.clicked(*pressed); } catch (...) {}
		}
		return result;
	}
	case WM_MOUSEMOVE:
		self->HoverRowAt({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		break;
	case WM_MOUSEWHEEL:
		// The overlay hides the platform bar, and a list box without a visible
		// bar drops the wheel, so the rows move here (#407).
		controls::ScrollListBoxByWheel(window, wParam);
		self->UpdateScrollbar();
		// Scrolling moves another row under a pointer that did not move.
		self->HoverRowUnderCursor();
		return 0;
	default:
		break;
	}
	if (message == WM_VSCROLL || message == WM_KEYDOWN || message == WM_SIZE) {
		// The control moves its own top index inside DefSubclassProc; the
		// overlay follows it afterwards.
		const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
		self->UpdateScrollbar();
		if (message == WM_VSCROLL) self->HoverRowUnderCursor();
		return result;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

} // namespace workbench::projects
