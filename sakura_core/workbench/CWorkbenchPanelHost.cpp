/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/CWorkbenchPanelHost.h"

#include <algorithm>

namespace workbench {
namespace {

constexpr wchar_t kPanelHostClass[] = L"SakuraWorkbenchPanelHost";
constexpr int kDefaultDpi = 96;
constexpr int kHeaderHeightDip = 30;

BOOL CALLBACK ApplyChromeFont(HWND window, LPARAM parameter)
{
	::SendMessageW(window, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
	return TRUE;
}

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? kDefaultDpi : dpi) + kDefaultDpi / 2) / kDefaultDpi);
}

[[nodiscard]] bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CWorkbenchPanelHost::WindowProc;
	windowClass.lpszClassName = kPanelHostClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

CWorkbenchPanelHost::CWorkbenchPanelHost(WorkbenchEdge edge, int extentDip, PersistExtentCallback persistExtent)
	: m_edge(edge)
	, m_extentDip(ClampExtent(extentDip))
	, m_pendingExtentDip(m_extentDip)
	, m_persistExtent(std::move(persistExtent))
{
}

CWorkbenchPanelHost::~CWorkbenchPanelHost()
{
	Close();
}

bool CWorkbenchPanelHost::Create(HWND parent, HINSTANCE instance, std::unique_ptr<IWorkbenchTool> tool)
{
	if (m_closed || m_window != nullptr || parent == nullptr || instance == nullptr || !tool) return false;
	if (!EnsureWindowClass(instance)) return false;

	m_window = ::CreateWindowExW(0, kPanelHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_window == nullptr) return false;
	m_tool = std::move(tool);
	if (!m_tool->Create(m_window)) {
		m_tool.reset();
		::DestroyWindow(m_window);
		m_window = nullptr;
		return false;
	}
	return true;
}

void CWorkbenchPanelHost::Layout(const RECT& bounds, unsigned int dpi)
{
	if (m_closed) return;
	m_bounds = bounds;
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_font.Dpi() != m_dpi) {
		(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
	}
	if (m_window != nullptr) {
		::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			std::max(0L, bounds.right - bounds.left), std::max(0L, bounds.bottom - bounds.top),
			SWP_NOACTIVATE | SWP_NOZORDER);
	}
	LayoutTool();
}

void CWorkbenchPanelHost::Show()
{
	if (m_closed || m_window == nullptr || m_state != WorkbenchPanelState::Hidden) return;
	m_state = WorkbenchPanelState::Visible;
	::ShowWindow(m_window, SW_SHOWNA);
	LayoutTool();
}

void CWorkbenchPanelHost::Hide()
{
	if (m_closed || m_window == nullptr || m_state == WorkbenchPanelState::Hidden) return;
	m_state = WorkbenchPanelState::Hidden;
	m_pendingExtentDip = m_extentDip;
	m_tool->Deactivate();
	::ShowWindow(m_window, SW_HIDE);
}

void CWorkbenchPanelHost::ActivateTool()
{
	if (m_closed || m_window == nullptr || m_tool == nullptr || m_state == WorkbenchPanelState::Hidden) return;
	::SetFocus(m_window);
	m_tool->Activate();
}

void CWorkbenchPanelHost::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, TRUE);
}

void CWorkbenchPanelHost::ApplyExtentDip(int extentDip)
{
	if (m_closed || m_state == WorkbenchPanelState::DragResizing) return;
	m_extentDip = ClampExtent(extentDip);
	m_pendingExtentDip = m_extentDip;
}

void CWorkbenchPanelHost::BeginResize()
{
	if (!m_closed && m_state == WorkbenchPanelState::Visible) {
		m_pendingExtentDip = m_extentDip;
		m_state = WorkbenchPanelState::DragResizing;
	}
}

void CWorkbenchPanelHost::UpdateResize(int extentDip)
{
	if (m_state == WorkbenchPanelState::DragResizing) m_pendingExtentDip = ClampExtent(extentDip);
}

void CWorkbenchPanelHost::CommitResize()
{
	if (m_state != WorkbenchPanelState::DragResizing) return;
	m_state = WorkbenchPanelState::Visible;
	if (m_extentDip == m_pendingExtentDip) return;
	m_extentDip = m_pendingExtentDip;
	if (m_persistExtent) m_persistExtent(m_edge, m_extentDip);
}

void CWorkbenchPanelHost::CancelResize()
{
	if (m_state == WorkbenchPanelState::DragResizing) {
		m_pendingExtentDip = m_extentDip;
		m_state = WorkbenchPanelState::Visible;
	}
}

bool CWorkbenchPanelHost::PreTranslateMessage(MSG& message)
{
	return !m_closed && m_tool != nullptr && m_state != WorkbenchPanelState::Hidden && m_tool->PreTranslateMessage(message);
}

void CWorkbenchPanelHost::Close()
{
	if (m_closed) return;
	m_closed = true;
	if (m_tool) {
		m_tool->Close();
		m_tool.reset();
	}
	if (m_window != nullptr) {
		::DestroyWindow(m_window);
		m_window = nullptr;
	}
	m_state = WorkbenchPanelState::Hidden;
}

int CWorkbenchPanelHost::GetHeaderHeightPixels() const noexcept
{
	return ScaleDip(kHeaderHeightDip, m_dpi);
}

LRESULT CALLBACK CWorkbenchPanelHost::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* host = static_cast<CWorkbenchPanelHost*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
		if (host != nullptr) host->m_window = window;
	}
	auto* host = reinterpret_cast<CWorkbenchPanelHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (host != nullptr) {
		if (message == WM_NCDESTROY) {
			host->m_window = nullptr;
			::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return host->HandleMessage(message, wParam, lParam);
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CWorkbenchPanelHost::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		Paint();
		return 0;
	case WM_DPICHANGED:
		m_dpi = HIWORD(wParam);
		LayoutTool();
		::InvalidateRect(m_window, nullptr, TRUE);
		return 0;
	case WM_SETFOCUS:
		if (!m_closed && m_tool && m_state != WorkbenchPanelState::Hidden) m_tool->Activate();
		return 0;
	case WM_KILLFOCUS:
		// Focus normally moves from this host to the hosted control. That remains an
		// active panel; only deactivate once focus leaves the complete subtree.
		if (!m_closed && m_tool && m_state != WorkbenchPanelState::Hidden
			&& reinterpret_cast<HWND>(wParam) != m_window
			&& !::IsChild(m_window, reinterpret_cast<HWND>(wParam))) {
			m_tool->Deactivate();
		}
		return 0;
	default:
		return ::DefWindowProcW(m_window, message, wParam, lParam);
	}
}

void CWorkbenchPanelHost::LayoutTool()
{
	if (m_closed || m_tool == nullptr || m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	client.top = std::min(client.bottom, client.top + GetHeaderHeightPixels());
	m_tool->Layout(client, m_dpi);
	if (m_font.Get() != nullptr) {
		::EnumChildWindows(m_window, ApplyChromeFont, reinterpret_cast<LPARAM>(m_font.Get()));
	}
}

void CWorkbenchPanelHost::Paint()
{
	PAINTSTRUCT paint{};
	const HDC dc = ::BeginPaint(m_window, &paint);
	if (dc == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	RECT header = client;
	header.bottom = std::min(header.bottom, header.top + GetHeaderHeightPixels());
	const HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef());
	const HBRUSH headerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
	::FillRect(dc, &client, background);
	::FillRect(dc, &header, headerBrush);
	::DeleteObject(background);
	::DeleteObject(headerBrush);
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, m_palette.primaryText.ToColorRef());
	const HGDIOBJ previousFont = m_font.Get() == nullptr ? nullptr : ::SelectObject(dc, m_font.Get());
	const wchar_t* title = m_edge == WorkbenchEdge::Left ? L"EXPLORER" : m_edge == WorkbenchEdge::Right ? L"OUTLINE" : L"TERMINAL";
	::InflateRect(&header, -ScaleDip(8, m_dpi), 0);
	::DrawTextW(dc, title, -1, &header, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	::EndPaint(m_window, &paint);
}

int CWorkbenchPanelHost::ClampExtent(int extentDip) noexcept
{
	return std::clamp(extentDip, 0, 10000);
}

} // namespace workbench
