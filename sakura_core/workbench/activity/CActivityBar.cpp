/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/CActivityBar.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <CommCtrl.h>
#include <windowsx.h>

#include <algorithm>

namespace workbench {
namespace {

constexpr wchar_t kActivityBarClass[] = L"SakuraWorkbenchActivityBar";
constexpr int kDefaultDpi = 96;
constexpr int kIndicatorWidthDip = 2;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return ::MulDiv(std::max(0, dip), static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi);
}

[[nodiscard]] bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	windowClass.lpfnWndProc = CActivityBar::WindowProc;
	windowClass.lpszClassName = kActivityBarClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

ActivityBarPalette ActivityBarPalette::Dark() noexcept
{
	return {};
}

ActivityBarPalette ActivityBarPalette::Light() noexcept
{
	return {
		.background = RGB(245, 245, 245),
		.hoverBackground = RGB(230, 230, 230),
		.pressedBackground = RGB(210, 210, 210),
		.selectedBackground = RGB(225, 225, 225),
		.activeIndicator = RGB(0, 95, 184),
		.icon = RGB(92, 101, 115),
		.activeIcon = RGB(31, 35, 41),
		.disabledIcon = RGB(150, 150, 150),
		.focusBorder = RGB(0, 0, 0),
	};
}

ActivityBarPalette ActivityBarPalette::HighContrast(COLORREF window, COLORREF windowText,
	COLORREF highlight, COLORREF highlightText) noexcept
{
	return {
		.background = window,
		.hoverBackground = highlight,
		.pressedBackground = highlight,
		.selectedBackground = highlight,
		.activeIndicator = highlightText,
		.icon = windowText,
		.activeIcon = highlightText,
		.disabledIcon = windowText,
		.focusBorder = highlightText,
		.highContrast = true,
	};
}

CActivityBar::CActivityBar(ToggleRequestCallback onToggleRequest)
	: m_onToggleRequest(std::move(onToggleRequest))
{
}

CActivityBar::~CActivityBar()
{
	m_accessibilityLifetime->Invalidate();
	Destroy();
}

bool CActivityBar::Create(HWND parent, HINSTANCE instance)
{
	if (m_destroyed || m_window != nullptr || parent == nullptr || instance == nullptr || !EnsureWindowClass(instance)) return false;
	m_window = ::CreateWindowExW(0, kActivityBarClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_window == nullptr) return false;

	m_tooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
		WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		m_window, nullptr, instance, nullptr);
	if (m_tooltip != nullptr) {
		::SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, ScaleDip(240, m_model.GetDpi()));
		for (std::size_t index = 0; index < m_model.GetButtonCount(); ++index) {
			const auto item = static_cast<ActivityBarItem>(index);
			TOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			tool.uFlags = TTF_SUBCLASS;
			tool.hwnd = m_window;
			tool.uId = static_cast<UINT_PTR>(index + 1);
			tool.lpszText = const_cast<wchar_t*>(ActivityBarItemName(item));
			::SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
		}
	}
	UpdateClientLayout(static_cast<unsigned int>(::GetDpiForWindow(m_window)));
	return true;
}

bool CActivityBar::Create(HWND parent, HINSTANCE instance, ToggleRequestCallback onToggleRequest)
{
	m_onToggleRequest = std::move(onToggleRequest);
	return Create(parent, instance);
}

void CActivityBar::Destroy() noexcept
{
	if (m_destroyed || m_destroying) return;
	m_destroying = true;
	m_captureItem.reset();
	if (m_iconFont != nullptr) {
		::DeleteObject(m_iconFont);
		m_iconFont = nullptr;
	}
	if (m_tooltip != nullptr && ::IsWindow(m_tooltip)) ::DestroyWindow(m_tooltip);
	m_tooltip = nullptr;
	if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
	m_window = nullptr;
	m_destroyed = true;
	m_destroying = false;
}

void CActivityBar::Layout(const RECT& bounds, unsigned int dpi)
{
	if (m_destroyed) return;
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	m_model.SetViewport(width, height, dpi);
	if (m_window != nullptr) {
		::SetWindowPos(m_window, nullptr, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	}
	UpdateTooltipRects();
	Invalidate();
}

void CActivityBar::SetPalette(const ActivityBarPalette& palette) noexcept
{
	m_palette = palette;
	Invalidate();
}

void CActivityBar::SetSelectedItem(std::optional<ActivityBarItem> item) noexcept
{
	m_model.SetSelectedItem(item);
	Invalidate();
}

void CActivityBar::SetPressed(std::optional<ActivityBarItem> item) noexcept
{
	m_model.SetPressedItem(item);
	Invalidate();
}

void CActivityBar::SetItemEnabled(ActivityBarItem item, bool enabled) noexcept
{
	const bool oldEnabled = m_model.IsEnabled(item);
	m_model.SetEnabled(item, enabled);
	if (oldEnabled != enabled) accessibility::RaiseEnabledChanged(*this, static_cast<int>(item), oldEnabled, enabled);
	Invalidate();
}

bool CActivityBar::Invoke(ActivityBarItem item) noexcept
{
	const auto requested = m_model.Invoke(item);
	if (requested) accessibility::RaiseInvoked(*this, static_cast<int>(*requested));
	return InvokeRequest(requested);
}

bool CActivityBar::PreTranslateMessage(MSG& message) noexcept
{
	if (m_destroyed || m_window == nullptr || message.hwnd != m_window) return false;
	if (message.message == WM_KEYDOWN) return HandleNavigationKey(message.wParam);
	if (message.message == WM_CHAR && (message.wParam == VK_SPACE || message.wParam == VK_RETURN)) return true;
	return false;
}

LRESULT CALLBACK CActivityBar::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* activityBar = static_cast<CActivityBar*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(activityBar));
		if (activityBar != nullptr) activityBar->m_window = window;
	}
	auto* activityBar = reinterpret_cast<CActivityBar*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (activityBar == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCDESTROY) {
		activityBar->m_accessibilityLifetime->Invalidate();
		activityBar->m_window = nullptr;
		activityBar->m_tooltip = nullptr;
		activityBar->m_captureItem.reset();
		if (!activityBar->m_destroying) activityBar->m_destroyed = true;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	return activityBar->HandleMessage(message, wParam, lParam);
}

LRESULT CActivityBar::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_GETOBJECT:
		if (lParam == static_cast<LPARAM>(UiaRootObjectId) || lParam == static_cast<LPARAM>(OBJID_CLIENT)) {
			return accessibility::HandleGetObject(*this, wParam, lParam);
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_SIZE:
		UpdateClientLayout(m_model.GetDpi());
		return 0;
	case WM_DPICHANGED:
		UpdateClientLayout(HIWORD(wParam));
		return 0;
	case WM_PAINT:
		Paint();
		return 0;
	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
	case WM_SETFOCUS:
		if (!m_model.GetFocusedItem()) static_cast<void>(m_model.MoveFocus(1));
		if (const auto item = m_model.GetFocusedItem()) accessibility::RaiseFocusChanged(*this, static_cast<int>(*item));
		Invalidate();
		return 0;
	case WM_KILLFOCUS:
		m_model.SetFocusedItem(std::nullopt);
		Invalidate();
		return 0;
	case WM_KEYDOWN:
		if (HandleNavigationKey(wParam)) return 0;
		break;
	case WM_CHAR:
		if (wParam == VK_SPACE || wParam == VK_RETURN) return 0;
		break;
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		SetHoverFromPoint(point);
		if (m_captureItem) m_model.SetPressedItem(m_model.HitTest(point.x, point.y) == m_captureItem ? m_captureItem : std::nullopt);
		if (!m_trackingMouseLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
			m_trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		Invalidate();
		return 0;
	}
	case WM_MOUSELEAVE:
		m_trackingMouseLeave = false;
		m_model.SetHoveredItem(std::nullopt);
		if (!m_captureItem) m_model.SetPressedItem(std::nullopt);
		Invalidate();
		return 0;
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const auto item = m_model.HitTest(point.x, point.y);
		if (!item) break;
		::SetFocus(m_window);
		m_model.SetFocusedItem(item);
		accessibility::RaiseFocusChanged(*this, static_cast<int>(*item));
		m_model.SetPressedItem(item);
		m_captureItem = item;
		::SetCapture(m_window);
		Invalidate();
		return 0;
	}
	case WM_LBUTTONUP: {
		if (!m_captureItem) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const auto captured = m_captureItem;
		m_captureItem.reset();
		m_model.SetPressedItem(std::nullopt);
		if (::GetCapture() == m_window) ::ReleaseCapture();
		SetHoverFromPoint(point);
		Invalidate();
	const auto invoked = m_model.HitTest(point.x, point.y) == captured ? captured : std::nullopt;
	if (invoked) accessibility::RaiseInvoked(*this, static_cast<int>(*invoked));
	static_cast<void>(InvokeRequest(invoked));
		return 0;
	}
	case WM_CAPTURECHANGED:
		m_captureItem.reset();
		m_model.SetPressedItem(std::nullopt);
		Invalidate();
		return 0;
	default:
		break;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CActivityBar::UpdateClientLayout(unsigned int dpi) noexcept
{
	if (m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	m_model.SetViewport(std::max(0L, client.right - client.left), std::max(0L, client.bottom - client.top), dpi);
	if (m_iconFont != nullptr) {
		::DeleteObject(m_iconFont);
		m_iconFont = nullptr;
	}
	UpdateTooltipRects();
	Invalidate();
}

void CActivityBar::UpdateTooltipRects() noexcept
{
	if (m_tooltip == nullptr || !::IsWindow(m_tooltip)) return;
	::SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, ScaleDip(240, m_model.GetDpi()));
	for (std::size_t index = 0; index < m_model.GetButtonCount(); ++index) {
		const auto bounds = m_model.GetButton(index).bounds;
		TOOLINFOW tool{};
		tool.cbSize = sizeof(tool);
		tool.hwnd = m_window;
		tool.uId = static_cast<UINT_PTR>(index + 1);
		tool.rect = { bounds.left, bounds.top, bounds.right, bounds.bottom };
		::SendMessageW(m_tooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
	}
}

void CActivityBar::EnsureIconFont() noexcept
{
	if (m_iconFont != nullptr) return;
	LOGFONTW font{};
	font.lfHeight = -ScaleDip(icons::kActivityIconDip, m_model.GetDpi());
	font.lfWeight = FW_NORMAL;
	font.lfCharSet = DEFAULT_CHARSET;
	font.lfQuality = CLEARTYPE_QUALITY;
	::wcscpy_s(font.lfFaceName, L"Segoe Fluent Icons");
	m_iconFont = ::CreateFontIndirectW(&font);
}

void CActivityBar::Paint() noexcept
{
	PAINTSTRUCT paint{};
	const HDC target = ::BeginPaint(m_window, &paint);
	if (target == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	if (width == 0 || height == 0) {
		::EndPaint(m_window, &paint);
		return;
	}

	const HDC buffer = ::CreateCompatibleDC(target);
	const HBITMAP bitmap = buffer == nullptr ? nullptr : ::CreateCompatibleBitmap(target, width, height);
	if (buffer == nullptr || bitmap == nullptr) {
		if (bitmap != nullptr) ::DeleteObject(bitmap);
		if (buffer != nullptr) ::DeleteDC(buffer);
		const HBRUSH fallbackBrush = ::CreateSolidBrush(m_palette.background);
		::FillRect(target, &client, fallbackBrush);
		::DeleteObject(fallbackBrush);
		::EndPaint(m_window, &paint);
		return;
	}
	const auto previousBitmap = static_cast<HBITMAP>(::SelectObject(buffer, bitmap));
	const HBRUSH background = ::CreateSolidBrush(m_palette.background);
	::FillRect(buffer, &client, background);
	::DeleteObject(background);

	::SetBkMode(buffer, TRANSPARENT);
	for (std::size_t index = 0; index < m_model.GetButtonCount(); ++index) {
		const auto button = m_model.GetButton(index);
		RECT bounds{ button.bounds.left, button.bounds.top, button.bounds.right, button.bounds.bottom };
		if (bounds.bottom <= bounds.top || bounds.right <= bounds.left) continue;
		const COLORREF buttonColour = button.pressed ? m_palette.pressedBackground
			: button.hovered ? m_palette.hoverBackground
			: button.selected ? m_palette.selectedBackground : m_palette.background;
		if (buttonColour != m_palette.background) {
			const HBRUSH brush = ::CreateSolidBrush(buttonColour);
			::FillRect(buffer, &bounds, brush);
			::DeleteObject(brush);
		}
		if (button.selected) {
			RECT indicator = bounds;
			indicator.right = std::min(indicator.right, indicator.left + std::max(1, ScaleDip(kIndicatorWidthDip, m_model.GetDpi())));
			const HBRUSH brush = ::CreateSolidBrush(m_palette.activeIndicator);
			::FillRect(buffer, &indicator, brush);
			::DeleteObject(brush);
		}
		const COLORREF iconColor = !button.enabled ? m_palette.disabledIcon
			: button.selected ? m_palette.activeIcon : m_palette.icon;
		const auto iconBounds = icons::CenteredIconBounds(
			{ bounds.left, bounds.top, bounds.right, bounds.bottom }, icons::kActivityIconDip, m_model.GetDpi());
		if (button.item == ActivityBarItem::SourceControl) {
			icons::codicons::DrawSourceControl(buffer, iconBounds, iconColor);
		} else if (button.item == ActivityBarItem::Extensions) {
			// Four compact tiles match the familiar Extensions activity affordance
			// without introducing another font/glyph dependency.
			const int gap = std::max(1, ScaleDip(2, m_model.GetDpi()));
			const int tileWidth = std::max(2, (iconBounds.right - iconBounds.left - gap) / 2);
			const int tileHeight = std::max(2, (iconBounds.bottom - iconBounds.top - gap) / 2);
			const HBRUSH tile = ::CreateSolidBrush(iconColor);
			for (int row = 0; row < 2; ++row) {
				for (int column = 0; column < 2; ++column) {
					RECT part{
						iconBounds.left + column * (tileWidth + gap),
						iconBounds.top + row * (tileHeight + gap),
						iconBounds.left + column * (tileWidth + gap) + tileWidth,
						iconBounds.top + row * (tileHeight + gap) + tileHeight,
					};
					::FillRect(buffer, &part, tile);
				}
			}
			::DeleteObject(tile);
		} else {
			icons::codicons::DrawFiles(buffer, iconBounds, iconColor);
		}
		if (button.focused) {
			const HPEN pen = ::CreatePen(PS_SOLID, 1, m_palette.focusBorder);
			const HGDIOBJ previousPen = ::SelectObject(buffer, pen);
			const HGDIOBJ previousBrush = ::SelectObject(buffer, ::GetStockObject(HOLLOW_BRUSH));
			RECT focus = bounds;
			::InflateRect(&focus, -2, -2);
			::Rectangle(buffer, focus.left, focus.top, focus.right, focus.bottom);
			::SelectObject(buffer, previousBrush);
			::SelectObject(buffer, previousPen);
			::DeleteObject(pen);
		}
	}
	::BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
	::SelectObject(buffer, previousBitmap);
	::DeleteObject(bitmap);
	::DeleteDC(buffer);
	::EndPaint(m_window, &paint);
}

void CActivityBar::Invalidate() const noexcept
{
	if (m_window != nullptr && ::IsWindow(m_window)) ::InvalidateRect(m_window, nullptr, FALSE);
}

bool CActivityBar::InvokeRequest(std::optional<ActivityBarItem> item) noexcept
{
	if (!item || !m_onToggleRequest) return false;
	try {
		m_onToggleRequest(*item);
		return true;
	} catch (...) {
		return false;
	}
}

bool CActivityBar::HandleNavigationKey(WPARAM key) noexcept
{
	std::optional<ActivityBarItem> invoked;
	bool focusChanged = false;
	switch (key) {
	case VK_TAB:
		static_cast<void>(m_model.MoveFocus((::GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1));
		focusChanged = true;
		break;
	case VK_UP:
	case VK_LEFT:
		static_cast<void>(m_model.MoveFocus(-1));
		focusChanged = true;
		break;
	case VK_DOWN:
	case VK_RIGHT:
		static_cast<void>(m_model.MoveFocus(1));
		focusChanged = true;
		break;
	case VK_HOME:
		m_model.SetFocusedItem(ActivityBarItem::Explorer);
		if (!m_model.GetFocusedItem()) static_cast<void>(m_model.MoveFocus(1));
		focusChanged = true;
		break;
	case VK_END:
		m_model.SetFocusedItem(ActivityBarItem::Extensions);
		if (!m_model.GetFocusedItem()) static_cast<void>(m_model.MoveFocus(-1));
		focusChanged = true;
		break;
	case VK_RETURN:
	case VK_SPACE:
		invoked = m_model.InvokeFocused();
		break;
	default:
		return false;
	}
	Invalidate();
	if (focusChanged) {
		if (const auto item = m_model.GetFocusedItem()) accessibility::RaiseFocusChanged(*this, static_cast<int>(*item));
	}
	if (invoked) accessibility::RaiseInvoked(*this, static_cast<int>(*invoked));
	static_cast<void>(InvokeRequest(invoked));
	return true;
}

int CActivityBar::AccessibilityChildCount(int parentId) const noexcept
{
	return parentId == -1 ? static_cast<int>(m_model.GetButtonCount()) : 0;
}

int CActivityBar::AccessibilityChildAt(int parentId, int index) const noexcept
{
	return parentId == -1 && index >= 0 && index < static_cast<int>(m_model.GetButtonCount()) ? index : -1;
}

int CActivityBar::AccessibilityParent(int nodeId) const noexcept
{
	return nodeId >= 0 && nodeId < static_cast<int>(m_model.GetButtonCount()) ? -1 : -2;
}

accessibility::CustomUiAutomationNode CActivityBar::AccessibilityNode(int nodeId) const
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetButtonCount())) return {};
	const auto button = m_model.GetButton(static_cast<std::size_t>(nodeId));
	const auto item = static_cast<ActivityBarItem>(nodeId);
	return {
		nodeId,
		ActivityBarItemName(item),
		std::wstring(L"Sakura.ActivityBar.") + ActivityBarItemName(item),
		UIA_ButtonControlTypeId,
		{ button.bounds.left, button.bounds.top, button.bounds.right, button.bounds.bottom },
		button.enabled,
		button.focused,
		true,
	};
}

int CActivityBar::AccessibilityFocusedNode() const noexcept
{
	const auto focused = m_model.GetFocusedItem();
	return focused ? static_cast<int>(*focused) : -1;
}

bool CActivityBar::AccessibilityInvoke(int nodeId) noexcept
{
	return nodeId >= 0 && nodeId < static_cast<int>(m_model.GetButtonCount()) && Invoke(static_cast<ActivityBarItem>(nodeId));
}

void CActivityBar::AccessibilitySetFocus(int nodeId) noexcept
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetButtonCount())) return;
	m_model.SetFocusedItem(static_cast<ActivityBarItem>(nodeId));
	Invalidate();
}

void CActivityBar::SetHoverFromPoint(POINT point) noexcept
{
	m_model.SetHoveredItem(m_model.HitTest(point.x, point.y));
}

} // namespace workbench
