/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/explorer/CExplorerOutlineTool.h"

#include <algorithm>
#include <windowsx.h>

namespace workbench::explorer {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraExplorerOutlineTool";
constexpr int kDefaultDpi = 96;
constexpr int kOutlineHeaderHeightDip = 24;
constexpr int kOutlinePreferredHeightDip = 180;
constexpr int kOutlineMinimumHeightDip = 96;

int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return ::MulDiv(dip, static_cast<int>(dpi == 0 ? kDefaultDpi : dpi), kDefaultDpi);
}

bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CExplorerOutlineTool::WindowProc;
	windowClass.lpszClassName = kWindowClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

CExplorerOutlineTool::CExplorerOutlineTool(CDlgFuncList& dialog, OutlineExpandedCallback callback)
	: m_explorer(std::make_unique<CExplorerTool>())
	, m_outline(std::make_unique<outline::COutlineWorkbenchTool>(dialog))
	, m_scm(std::make_unique<scm::CScmWorkbenchTool>())
	, m_callback(std::move(callback))
{
}

CExplorerOutlineTool::~CExplorerOutlineTool()
{
	Close();
}

bool CExplorerOutlineTool::Create(HWND parent)
{
	if (m_closed || m_window != nullptr || parent == nullptr) return false;
	m_instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (m_instance == nullptr) m_instance = ::GetModuleHandleW(nullptr);
	if (!EnsureWindowClass(m_instance)) return false;
	m_window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, m_instance, this);
	if (m_window == nullptr) return false;
	if (!m_explorer->Create(m_window) || !m_outline->Create(m_window) || !m_scm->Create(m_window)) {
		Close();
		return false;
	}
	m_outline->SetVisible(m_outlineExpanded);
	m_scm->SetVisible(false);
	return true;
}

void CExplorerOutlineTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (m_closed || m_window == nullptr) return;
	m_bounds = contentRect;
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_font.Dpi() != m_dpi) (void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
	::SetWindowPos(m_window, nullptr, contentRect.left, contentRect.top,
		std::max(0L, contentRect.right - contentRect.left), std::max(0L, contentRect.bottom - contentRect.top),
		SWP_NOACTIVATE | SWP_NOZORDER);
	LayoutChildren();
}

void CExplorerOutlineTool::Activate()
{
	if (m_sourceControlVisible && m_scm) m_scm->Activate();
	else if (m_explorer) m_explorer->Activate();
}

void CExplorerOutlineTool::Deactivate()
{
	if (m_explorer) m_explorer->Deactivate();
	if (m_outline) m_outline->Deactivate();
	if (m_scm) m_scm->Deactivate();
}

bool CExplorerOutlineTool::PreTranslateMessage(MSG& message)
{
	if (m_sourceControlVisible) return m_scm && m_scm->PreTranslateMessage(message);
	return (m_outlineExpanded && m_outline && m_outline->PreTranslateMessage(message))
		|| (m_explorer && m_explorer->PreTranslateMessage(message));
}

void CExplorerOutlineTool::Close()
{
	if (m_closed) return;
	m_closed = true;
	if (m_outline) m_outline->Close();
	if (m_scm) m_scm->Close();
	if (m_explorer) m_explorer->Close();
	if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
	m_window = nullptr;
}

void CExplorerOutlineTool::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_explorer) {
		m_explorer->SetPalette({ palette.panel.ToColorRef(), palette.primaryText.ToColorRef(),
			palette.border.ToColorRef(), palette.accent.ToColorRef() });
	}
	if (m_outline) m_outline->SetPalette(palette);
	if (m_scm) m_scm->SetPalette(palette);
	if (m_window) ::InvalidateRect(m_window, nullptr, TRUE);
}

void CExplorerOutlineTool::SetOutlineExpanded(bool expanded)
{
	if (m_outlineExpanded == expanded) return;
	m_outlineExpanded = expanded;
	if (m_outline) m_outline->SetVisible(expanded);
	LayoutChildren();
	if (m_window) ::InvalidateRect(m_window, nullptr, TRUE);
}

bool CExplorerOutlineTool::RequestOutlineExpanded(bool expanded) noexcept
{
	if (m_closed || m_outlineExpanded == expanded) return !m_closed;
	if (m_callback) {
		try {
			if (!m_callback(expanded)) return false;
		}
		catch (...) {
			return false;
		}
	}
	SetOutlineExpanded(expanded);
	return true;
}

void CExplorerOutlineTool::FocusOutline()
{
	ShowSourceControl(false);
	// Focus is a projection/activation operation, not a user expansion request.
	SetOutlineExpanded(true);
	if (m_outline) m_outline->Activate();
}

void CExplorerOutlineTool::ShowSourceControl(bool show)
{
	if (m_sourceControlVisible == show) return;
	m_sourceControlVisible = show;
	if (m_scm) m_scm->SetVisible(show);
	LayoutChildren();
	if (m_window) ::InvalidateRect(m_window, nullptr, TRUE);
}

int CExplorerOutlineTool::OutlineHeaderHeightPixels(unsigned int dpi) noexcept
{
	return ScaleDip(kOutlineHeaderHeightDip, dpi);
}

LRESULT CALLBACK CExplorerOutlineTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		auto* self = static_cast<CExplorerOutlineTool*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		if (self) self->m_window = window;
	}
	auto* self = reinterpret_cast<CExplorerOutlineTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (!self) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCDESTROY) {
		self->m_window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	return self->HandleMessage(message, wParam, lParam);
}

LRESULT CExplorerOutlineTool::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_PAINT:
		Paint();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_LBUTTONUP: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (IsOutlineHeaderPoint(point)) {
			(void)RequestOutlineExpanded(!m_outlineExpanded);
			return 0;
		}
		break;
	}
	case WM_SETCURSOR: {
		POINT point{};
		::GetCursorPos(&point);
		::ScreenToClient(m_window, &point);
		if (IsOutlineHeaderPoint(point)) {
			::SetCursor(::LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CExplorerOutlineTool::LayoutChildren()
{
	if (m_window == nullptr || !m_explorer || !m_outline || !m_scm) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	if (m_sourceControlVisible) {
		::ShowWindow(m_explorer->GetHwnd(), SW_HIDE);
		m_outline->SetVisible(false);
		m_scm->Layout(client, m_dpi);
		m_scm->SetVisible(true);
		m_outlineHeader = {};
		return;
	}
	m_scm->SetVisible(false);
	::ShowWindow(m_explorer->GetHwnd(), SW_SHOW);
	const int headerHeight = OutlineHeaderHeightPixels(m_dpi);
	const int available = std::max(0L, client.bottom - client.top);
	int outlineHeight = 0;
	if (m_outlineExpanded) {
		outlineHeight = std::clamp(ScaleDip(kOutlinePreferredHeightDip, m_dpi),
			ScaleDip(kOutlineMinimumHeightDip, m_dpi), std::max(ScaleDip(kOutlineMinimumHeightDip, m_dpi), available / 2));
		outlineHeight = std::min(outlineHeight, std::max(0, available - headerHeight));
	}
	m_outlineHeader = { client.left, client.bottom - outlineHeight - headerHeight, client.right, client.bottom - outlineHeight };
	RECT explorerBounds{ client.left, client.top, client.right, m_outlineHeader.top };
	RECT outlineBounds{ client.left, m_outlineHeader.bottom, client.right, client.bottom };
	m_explorer->Layout(explorerBounds, m_dpi);
	m_outline->Layout(outlineBounds, m_dpi);
	m_outline->SetVisible(m_outlineExpanded);
	::InvalidateRect(m_window, &m_outlineHeader, TRUE);
}

void CExplorerOutlineTool::Paint()
{
	PAINTSTRUCT paint{};
	const HDC dc = ::BeginPaint(m_window, &paint);
	const HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef());
	::FillRect(dc, &paint.rcPaint, background);
	::DeleteObject(background);
	if (!::IsRectEmpty(&m_outlineHeader)) {
		const HPEN border = ::CreatePen(PS_SOLID, 1, m_palette.border.ToColorRef());
		const HGDIOBJ oldPen = ::SelectObject(dc, border);
		::MoveToEx(dc, m_outlineHeader.left, m_outlineHeader.top, nullptr);
		::LineTo(dc, m_outlineHeader.right, m_outlineHeader.top);
		::SelectObject(dc, oldPen);
		::DeleteObject(border);
		if (m_font.Get()) ::SelectObject(dc, m_font.Get());
		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		RECT chevron = m_outlineHeader;
		chevron.left += ScaleDip(6, m_dpi);
		chevron.right = chevron.left + ScaleDip(12, m_dpi);
		::DrawTextW(dc, m_outlineExpanded ? L"\x2304" : L"\x203A", -1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		RECT text = m_outlineHeader;
		text.left += ScaleDip(22, m_dpi);
		::DrawTextW(dc, L"OUTLINE", -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
	::EndPaint(m_window, &paint);
}

bool CExplorerOutlineTool::IsOutlineHeaderPoint(POINT point) const noexcept
{
	return ::PtInRect(&m_outlineHeader, point) != FALSE;
}

} // namespace workbench::explorer
