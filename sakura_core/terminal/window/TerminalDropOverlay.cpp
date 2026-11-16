/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/window/TerminalDropOverlay.h"

namespace terminal {

namespace {

constexpr wchar_t kTerminalDropOverlayClass[] = L"SakuraTerminalDropOverlay";

LRESULT CALLBACK DropOverlayProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	switch( message ) {
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		if( const HDC dc = ::BeginPaint(window, &paint) ) {
			const auto color = static_cast<COLORREF>(::GetWindowLongPtrW(window, GWLP_USERDATA));
			if( const HBRUSH brush = ::CreateSolidBrush(color) ) {
				::FillRect(dc, &paint.rcPaint, brush);
				::DeleteObject(brush);
			}
			::EndPaint(window, &paint);
		}
		return 0;
	}
	default:
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
}

bool EnsureDropOverlayClass( HINSTANCE instance )
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.lpfnWndProc = DropOverlayProc;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = kTerminalDropOverlayClass;
	if( ::RegisterClassExW(&windowClass) != 0 ) return true;
	return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

TerminalDropOverlay::~TerminalDropOverlay()
{
	Destroy();
}

bool TerminalDropOverlay::Show( HWND anchor, HINSTANCE instance, const RECT& screenRect, theme::ThemeColor color )
{
	if( screenRect.right <= screenRect.left || screenRect.bottom <= screenRect.top || color.alpha == 0 ) {
		Hide();
		return true;
	}
	if( !m_window ) {
		if( !anchor || !EnsureDropOverlayClass(instance) ) return false;
		const HWND owner = ::GetAncestor(anchor, GA_ROOT);
		m_window = ::CreateWindowExW(
			WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
			kTerminalDropOverlayClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, nullptr);
		if( !m_window ) return false;
		m_alpha = 0;
		m_color = CLR_INVALID;
	}
	const COLORREF rgb = color.ToColorRef();
	if( rgb != m_color ) {
		m_color = rgb;
		::SetWindowLongPtrW(m_window, GWLP_USERDATA, static_cast<LONG_PTR>(rgb));
		::InvalidateRect(m_window, nullptr, TRUE);
	}
	if( color.alpha != m_alpha ) {
		m_alpha = color.alpha;
		::SetLayeredWindowAttributes(m_window, 0, m_alpha, LWA_ALPHA);
	}
	if( ::EqualRect(&m_shown, &screenRect) != FALSE && IsVisible() ) return true;
	m_shown = screenRect;
	::SetWindowPos(m_window, HWND_TOP, screenRect.left, screenRect.top,
		screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
		SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
	return true;
}

void TerminalDropOverlay::Hide() noexcept
{
	m_shown = {};
	if( m_window && ::IsWindowVisible(m_window) ) ::ShowWindow(m_window, SW_HIDE);
}

void TerminalDropOverlay::Destroy() noexcept
{
	m_shown = {};
	if( m_window ) ::DestroyWindow(m_window);
	m_window = nullptr;
}

bool TerminalDropOverlay::IsVisible() const noexcept
{
	return m_window && ::IsWindowVisible(m_window) != FALSE;
}

HWND TerminalDropOverlay::GetHwnd() const noexcept
{
	return m_window;
}

RECT TerminalDropOverlay::ShownRect() const noexcept
{
	return m_shown;
}

} // namespace terminal
