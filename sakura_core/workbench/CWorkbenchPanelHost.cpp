/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/CWorkbenchPanelHost.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

namespace workbench {
namespace {

constexpr wchar_t kPanelHostClass[] = L"SakuraWorkbenchPanelHost";
constexpr wchar_t kPanelSashClass[] = L"SakuraWorkbenchPanelSash";
constexpr int kDefaultDpi = 96;
constexpr int kHeaderHeightDip = 30;
constexpr int kSashHitTargetDip = 4;
constexpr int kHeaderActionSideDip = 26;
constexpr int kHeaderActionTrailingDip = 4;
constexpr int kHeaderMenuControlId = 0x7f01;
constexpr UINT_PTR kHeaderMenuSubclassId = 1;

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

[[nodiscard]] bool EnsureSashWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_SIZEWE);
	windowClass.lpfnWndProc = CWorkbenchPanelHost::SashWindowProc;
	windowClass.lpszClassName = kPanelSashClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

CWorkbenchPanelHost::CWorkbenchPanelHost(WorkbenchEdge edge, int extentDip, CommitExtentCallback commitExtent)
	: m_edge(edge)
	, m_extentDip(ClampExtent(extentDip))
	, m_pendingExtentDip(m_extentDip)
	, m_commitExtent(std::move(commitExtent))
{
}

CWorkbenchPanelHost::~CWorkbenchPanelHost()
{
	Close();
}

bool CWorkbenchPanelHost::Create(HWND parent, HINSTANCE instance, std::unique_ptr<IWorkbenchTool> tool)
{
	if (m_closed || m_window != nullptr || parent == nullptr || instance == nullptr || !tool) return false;
	if (!EnsureWindowClass(instance) || !EnsureSashWindowClass(instance)) return false;

	m_window = ::CreateWindowExW(0, kPanelHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_window == nullptr) return false;
	m_headerMenuButton = ::CreateWindowExW(0, L"BUTTON", L"More Actions...",
		WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 0, 0, m_window,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(kHeaderMenuControlId)), instance, nullptr);
	if (m_headerMenuButton == nullptr
		|| ::SetWindowSubclass(m_headerMenuButton, HeaderMenuButtonSubclassProc,
			kHeaderMenuSubclassId, reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
		if (m_headerMenuButton != nullptr) ::DestroyWindow(m_headerMenuButton);
		m_headerMenuButton = nullptr;
		::DestroyWindow(m_window);
		m_window = nullptr;
		return false;
	}
	m_tool = std::move(tool);
	if (!m_tool->Create(m_window)) {
		m_tool.reset();
		::DestroyWindow(m_window);
		m_window = nullptr;
		m_headerMenuButton = nullptr;
		return false;
	}
	m_sashWindow = ::CreateWindowExW(WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY,
		kPanelSashClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_sashWindow == nullptr) {
		m_tool->Close();
		m_tool.reset();
		::DestroyWindow(m_window);
		m_window = nullptr;
		m_headerMenuButton = nullptr;
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
		// SWP_NOCOPYBITS: the default bit-copy smears the old client content
		// across the moved rectangle before WM_PAINT arrives, which reads as
		// ghost pixels whenever a sash commit or toggle relocates this host.
		::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			std::max(0L, bounds.right - bounds.left), std::max(0L, bounds.bottom - bounds.top),
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
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
	if (m_sashWindow != nullptr) ::ShowWindow(m_sashWindow, SW_HIDE);
}

void CWorkbenchPanelHost::ActivateTool()
{
	if (m_closed || m_window == nullptr || m_tool == nullptr || m_state == WorkbenchPanelState::Hidden) return;
	const HWND focus = ::GetFocus();
	if (focus == m_window || (focus != nullptr && ::IsChild(m_window, focus))) {
		m_tool->Activate();
		return;
	}
	::SetFocus(m_window);
	// WM_SETFOCUS synchronously owns normal activation. If focus could not enter
	// the host subtree, retain one explicit fallback instead of silently doing
	// nothing. This keeps each successful focus transition single-dispatch.
	const HWND resultingFocus = ::GetFocus();
	if (resultingFocus != m_window && (resultingFocus == nullptr || !::IsChild(m_window, resultingFocus))) {
		m_tool->Activate();
	}
}

void CWorkbenchPanelHost::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
	if (m_headerMenuButton != nullptr) ::InvalidateRect(m_headerMenuButton, nullptr, FALSE);
}

void CWorkbenchPanelHost::SetTitle(std::wstring title)
{
	m_title = std::move(title);
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
}

void CWorkbenchPanelHost::SetHeaderMenu(std::vector<HeaderMenuItem> items)
{
	m_headerMenu = std::move(items);
	LayoutHeaderMenuButton();
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
}

void CWorkbenchPanelHost::ApplyExtentDip(int extentDip)
{
	if (m_closed || m_state == WorkbenchPanelState::DragResizing) return;
	m_extentDip = ClampExtent(extentDip);
	m_pendingExtentDip = m_extentDip;
}

void CWorkbenchPanelHost::LayoutSash(const RECT& visibleBoundary)
{
	if (m_closed || m_sashWindow == nullptr) return;
	RECT hit = visibleBoundary;
	const bool vertical = m_edge != WorkbenchEdge::Bottom;
	const int visibleExtent = vertical
		? static_cast<int>(hit.right - hit.left)
		: static_cast<int>(hit.bottom - hit.top);
	if (m_state == WorkbenchPanelState::Hidden || visibleExtent <= 0
		|| hit.right <= hit.left || hit.bottom <= hit.top) {
		::ShowWindow(m_sashWindow, SW_HIDE);
		return;
	}

	const int targetExtent = std::max(1, ScaleDip(kSashHitTargetDip, m_dpi));
	const int extra = std::max(0, targetExtent - visibleExtent);
	if (vertical) {
		hit.left -= extra / 2;
		hit.right += extra - extra / 2;
	} else {
		hit.top -= extra / 2;
		hit.bottom += extra - extra / 2;
	}
	RECT parentClient{};
	const HWND parent = ::GetParent(m_sashWindow);
	if (parent == nullptr || !::GetClientRect(parent, &parentClient)) {
		::ShowWindow(m_sashWindow, SW_HIDE);
		return;
	}
	hit.left = std::clamp(hit.left, parentClient.left, parentClient.right);
	hit.right = std::clamp(hit.right, hit.left, parentClient.right);
	hit.top = std::clamp(hit.top, parentClient.top, parentClient.bottom);
	hit.bottom = std::clamp(hit.bottom, hit.top, parentClient.bottom);
	if (hit.right <= hit.left || hit.bottom <= hit.top) {
		::ShowWindow(m_sashWindow, SW_HIDE);
		return;
	}
	::SetWindowPos(m_sashWindow, HWND_TOP, hit.left, hit.top,
		hit.right - hit.left, hit.bottom - hit.top,
		SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW);
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

bool CWorkbenchPanelHost::CommitResize()
{
	if (m_state != WorkbenchPanelState::DragResizing) return false;
	m_state = WorkbenchPanelState::Visible;
	if (m_extentDip == m_pendingExtentDip) return true;

	const int requestedExtentDip = m_pendingExtentDip;
	bool accepted = true;
	if (m_commitExtent) {
		try {
			accepted = m_commitExtent(m_edge, requestedExtentDip);
		}
		catch (...) {
			accepted = false;
		}
	}
	if (!accepted) {
		m_pendingExtentDip = m_extentDip;
		return false;
	}
	m_extentDip = requestedExtentDip;
	m_pendingExtentDip = m_extentDip;
	return true;
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
	m_headerPressed = false;
	m_headerDragging = false;
	if (m_sashWindow != nullptr) {
		::DestroyWindow(m_sashWindow);
		m_sashWindow = nullptr;
	}
	if (m_tool) {
		m_tool->Close();
		m_tool.reset();
	}
	if (m_headerMenuButton != nullptr && ::IsWindow(m_headerMenuButton)) {
		::DestroyWindow(m_headerMenuButton);
	}
	m_headerMenuButton = nullptr;
	if (m_window != nullptr) {
		::DestroyWindow(m_window);
		m_window = nullptr;
	}
	m_headerMenu.clear();
	m_headerMenuHovered = false;
	m_headerMenuTrackingMouse = false;
	ReleaseCodiconFont();
	m_backBuffer.Reset();
	m_state = WorkbenchPanelState::Hidden;
}

LRESULT CWorkbenchPanelHost::HandleSashMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		::BeginPaint(m_sashWindow, &paint);
		::EndPaint(m_sashWindow, &paint);
		return 0;
	}
	case WM_SETCURSOR:
		::SetCursor(::LoadCursor(nullptr, m_edge == WorkbenchEdge::Bottom ? IDC_SIZENS : IDC_SIZEWE));
		return TRUE;
	case WM_LBUTTONDOWN: {
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const HWND parent = ::GetParent(m_sashWindow);
		if (parent != nullptr) {
			(void)::MapWindowPoints(m_sashWindow, parent, &point, 1);
			return ::SendMessageW(parent, WM_LBUTTONDOWN, wParam, MAKELPARAM(point.x, point.y));
		}
		return 0;
	}
	default:
		return ::DefWindowProcW(m_sashWindow, message, wParam, lParam);
	}
}

LRESULT CALLBACK CWorkbenchPanelHost::SashWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(window, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	auto* self = reinterpret_cast<CWorkbenchPanelHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (self == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCCREATE) self->m_sashWindow = window;
	if (message == WM_NCDESTROY) {
		self->m_sashWindow = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	return self->HandleSashMessage(message, wParam, lParam);
}

LRESULT CALLBACK CWorkbenchPanelHost::HeaderMenuButtonSubclassProc(HWND window,
	UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
	auto* self = reinterpret_cast<CWorkbenchPanelHost*>(data);
	if (message == WM_NCDESTROY) {
		(void)::RemoveWindowSubclass(window, HeaderMenuButtonSubclassProc, id);
		if (self != nullptr && self->m_headerMenuButton == window) self->m_headerMenuButton = nullptr;
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (self == nullptr) return ::DefSubclassProc(window, message, wParam, lParam);
	const auto paintButton = [self, window](HDC dc) {
		RECT bounds{};
		::GetClientRect(window, &bounds);
		const LRESULT buttonState = ::SendMessageW(window, BM_GETSTATE, 0, 0);
		UINT state = 0;
		if ((buttonState & BST_PUSHED) != 0) state |= ODS_SELECTED;
		if ((buttonState & BST_FOCUS) != 0) state |= ODS_FOCUS;
		if (::IsWindowEnabled(window) == FALSE) state |= ODS_DISABLED;
		if ((::SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0) {
			state |= ODS_NOFOCUSRECT;
		}
		const DRAWITEMSTRUCT draw{ ODT_BUTTON, kHeaderMenuControlId, 0, ODA_DRAWENTIRE,
			state, window, dc, bounds, 0 };
		self->PaintHeaderMenuButton(draw);
	};
	if (message == WM_ERASEBKGND) return 1;
	if (message == WM_PAINT) {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if (dc != nullptr) paintButton(dc);
		::EndPaint(window, &paint);
		return 0;
	}
	if (message == WM_PRINTCLIENT) {
		paintButton(reinterpret_cast<HDC>(wParam));
		return 0;
	}
	if (message == WM_MOUSEMOVE) {
		if (!self->m_headerMenuHovered) {
			self->m_headerMenuHovered = true;
			::InvalidateRect(window, nullptr, FALSE);
		}
		if (!self->m_headerMenuTrackingMouse) {
			TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
			self->m_headerMenuTrackingMouse = ::TrackMouseEvent(&track) != FALSE;
		}
	} else if (message == WM_MOUSELEAVE) {
		self->m_headerMenuHovered = false;
		self->m_headerMenuTrackingMouse = false;
		::InvalidateRect(window, nullptr, FALSE);
	}
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE) {
		::InvalidateRect(window, nullptr, FALSE);
	}
	return result;
}

int CWorkbenchPanelHost::GetHeaderHeightPixels() const noexcept
{
	if (m_edge == WorkbenchEdge::Bottom) return 0;
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
	case WM_DRAWITEM: {
		const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
		if (draw != nullptr && draw->CtlID == kHeaderMenuControlId
			&& draw->hwndItem == m_headerMenuButton) {
			PaintHeaderMenuButton(*draw);
			return TRUE;
		}
		break;
	}
	case WM_COMMAND:
		if (LOWORD(wParam) == kHeaderMenuControlId && HIWORD(wParam) == BN_CLICKED) {
			ShowHeaderMenu();
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		Paint();
		return 0;
	case WM_DPICHANGED:
		m_dpi = HIWORD(wParam);
		LayoutTool();
		::InvalidateRect(m_window, nullptr, FALSE);
		return 0;
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (!m_headerDrag || !IsHeaderPoint(point)) break;
		m_headerPressed = true;
		m_headerDragging = false;
		m_headerDragOrigin = point;
		::SetCapture(m_window);
		return 0;
	}
	case WM_MOUSEMOVE: {
		if (!m_headerPressed) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (!m_headerDragging) {
			const int dragX = std::max(1, ::GetSystemMetrics(SM_CXDRAG));
			const int dragY = std::max(1, ::GetSystemMetrics(SM_CYDRAG));
			if (std::abs(point.x - m_headerDragOrigin.x) < dragX
				&& std::abs(point.y - m_headerDragOrigin.y) < dragY) {
				return 0;
			}
			m_headerDragging = true;
		}
		::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
		return 0;
	}
	case WM_LBUTTONUP: {
		if (!m_headerPressed) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		EndHeaderDrag(m_headerDragging, point);
		return 0;
	}
	case WM_CAPTURECHANGED:
		EndHeaderDrag(false, POINT{});
		return 0;
	case WM_SETCURSOR:
		if (m_headerDragging) {
			::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
			return TRUE;
		}
		break;
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
		break;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

bool CWorkbenchPanelHost::IsHeaderPoint(POINT clientPoint) const noexcept
{
	const int headerHeight = GetHeaderHeightPixels();
	if (headerHeight <= 0 || m_window == nullptr) return false;
	if (m_headerMenuButton != nullptr && ::IsWindowVisible(m_headerMenuButton)) {
		RECT action{};
		if (::GetWindowRect(m_headerMenuButton, &action)) {
			::MapWindowPoints(nullptr, m_window, reinterpret_cast<POINT*>(&action), 2);
			if (::PtInRect(&action, clientPoint)) return false;
		}
	}
	RECT client{};
	::GetClientRect(m_window, &client);
	return clientPoint.y >= client.top && clientPoint.y < client.top + headerHeight
		&& clientPoint.x >= client.left && clientPoint.x < client.right;
}

void CWorkbenchPanelHost::EndHeaderDrag(bool deliver, POINT clientPoint)
{
	if (!m_headerPressed) return;
	m_headerPressed = false;
	m_headerDragging = false;
	if (::GetCapture() == m_window) ::ReleaseCapture();
	if (!deliver || !m_headerDrag || m_window == nullptr) return;
	POINT screenPoint = clientPoint;
	if (::ClientToScreen(m_window, &screenPoint) == FALSE) return;
	try {
		m_headerDrag(m_edge, screenPoint);
	}
	catch (...) {
		// A rejected move must never destabilize the host that raised it.
	}
}

void CWorkbenchPanelHost::LayoutTool()
{
	if (m_closed || m_tool == nullptr || m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	LayoutHeaderMenuButton();
	client.top = std::min(client.bottom, client.top + GetHeaderHeightPixels());
	// Font metrics are layout input. Applying WM_SETFONT after a tool has laid out
	// its children lets native controls reset geometry such as EDIT's formatting
	// rectangle after the tool established it.
	if (m_font.Get() != nullptr) {
		::EnumChildWindows(m_window, ApplyChromeFont, reinterpret_cast<LPARAM>(m_font.Get()));
	}
	m_tool->Layout(client, m_dpi);
}

void CWorkbenchPanelHost::LayoutHeaderMenuButton()
{
	if (m_headerMenuButton == nullptr || m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const int headerHeight = GetHeaderHeightPixels();
	const bool visible = headerHeight > 0 && !m_headerMenu.empty();
	if (!visible) {
		::ShowWindow(m_headerMenuButton, SW_HIDE);
		return;
	}
	const int side = std::min(headerHeight, ScaleDip(kHeaderActionSideDip, m_dpi));
	const int trailing = ScaleDip(kHeaderActionTrailingDip, m_dpi);
	const int left = std::max<LONG>(client.left, client.right - trailing - side);
	const int top = client.top + std::max(0, (headerHeight - side) / 2);
	::SetWindowPos(m_headerMenuButton, HWND_TOP, left, top, side, side,
		SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW);
	::InvalidateRect(m_headerMenuButton, nullptr, FALSE);
}

HFONT CWorkbenchPanelHost::AcquireCodiconFont(int height) noexcept
{
	if (height <= 0) return nullptr;
	if (m_codiconFont != nullptr && m_codiconFontHeight == height) return m_codiconFont;
	ReleaseCodiconFont();
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty()) return nullptr;
	m_codiconFont = workbench::icons::CreateLabelRunGlyphFont(faceName, height);
	if (m_codiconFont != nullptr) m_codiconFontHeight = height;
	return m_codiconFont;
}

void CWorkbenchPanelHost::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

void CWorkbenchPanelHost::Paint()
{
	PAINTSTRUCT paint{};
	const HDC target = ::BeginPaint(m_window, &paint);
	if (target == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	const bool buffered = width > 0 && height > 0 && m_backBuffer.Ensure(target, width, height);
	const HDC dc = buffered ? m_backBuffer.Dc() : target;
	RECT header = client;
	const int headerHeight = GetHeaderHeightPixels();
	header.bottom = std::min(header.bottom, header.top + headerHeight);
	const theme::ThemeColor surface = m_edge == WorkbenchEdge::Bottom
		? m_palette.bottomPanel
		: m_edge == WorkbenchEdge::Left ? m_palette.sideBar : m_palette.panel;
	const HBRUSH background = ::CreateSolidBrush(surface.ToColorRef());
	::FillRect(dc, &client, background);
	::DeleteObject(background);
	if (headerHeight > 0) {
		const HBRUSH headerBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
		::FillRect(dc, &header, headerBrush);
		::DeleteObject(headerBrush);
		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		const HGDIOBJ previousFont = m_font.Get() == nullptr ? nullptr : ::SelectObject(dc, m_font.Get());
		const wchar_t* title = !m_title.empty() ? m_title.c_str()
			: m_edge == WorkbenchEdge::Left ? L"EXPLORER" : L"OUTLINE";
		::InflateRect(&header, -ScaleDip(8, m_dpi), 0);
		if (m_headerMenuButton != nullptr && ::IsWindowVisible(m_headerMenuButton)) {
			RECT action{};
			if (::GetWindowRect(m_headerMenuButton, &action)) {
				::MapWindowPoints(nullptr, m_window, reinterpret_cast<POINT*>(&action), 2);
				header.right = std::max(header.left, action.left - ScaleDip(4, m_dpi));
			}
		}
		::DrawTextW(dc, title, -1, &header, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	}
	if (buffered) (void)m_backBuffer.Present(target, client);
	::EndPaint(m_window, &paint);
}

void CWorkbenchPanelHost::PaintHeaderMenuButton(const DRAWITEMSTRUCT& draw)
{
	if (draw.hDC == nullptr) return;
	const bool enabled = (draw.itemState & ODS_DISABLED) == 0;
	const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
	const bool focused = (draw.itemState & ODS_FOCUS) != 0
		&& (draw.itemState & ODS_NOFOCUSRECT) == 0;
	const COLORREF background = enabled && (pressed || m_headerMenuHovered)
		? m_palette.listHoverBackground.ToColorRef() : m_palette.raised.ToColorRef();
	const COLORREF foreground = enabled
		? m_palette.primaryText.ToColorRef() : m_palette.disabledText.ToColorRef();
	const HBRUSH fill = ::CreateSolidBrush(background);
	if (fill != nullptr) {
		::FillRect(draw.hDC, &draw.rcItem, fill);
		::DeleteObject(fill);
	}
	RECT glyphBounds = draw.rcItem;
	if (pressed) ::OffsetRect(&glyphBounds, 0, 1);
	const auto glyph = workbench::icons::FindCodiconGlyph(L"ellipsis");
	const HFONT font = AcquireCodiconFont(ScaleDip(16, m_dpi));
	if (glyph.has_value() && font != nullptr) {
		const wchar_t text[] = { *glyph, L'\0' };
		const HGDIOBJ previousFont = ::SelectObject(draw.hDC, font);
		const int previousMode = ::SetBkMode(draw.hDC, TRANSPARENT);
		const COLORREF previousColor = ::SetTextColor(draw.hDC, foreground);
		::DrawTextW(draw.hDC, text, 1, &glyphBounds,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		::SetTextColor(draw.hDC, previousColor);
		::SetBkMode(draw.hDC, previousMode);
		if (previousFont != nullptr) ::SelectObject(draw.hDC, previousFont);
	}
	if (!focused) return;
	RECT focus = draw.rcItem;
	::InflateRect(&focus, -ScaleDip(2, m_dpi), -ScaleDip(2, m_dpi));
	const HBRUSH border = ::CreateSolidBrush(m_palette.listFocusAndSelectionOutline.ToColorRef());
	if (border != nullptr) {
		::FrameRect(draw.hDC, &focus, border);
		::DeleteObject(border);
	}
}

void CWorkbenchPanelHost::ShowHeaderMenu()
{
	if (m_headerMenuButton == nullptr || m_headerMenu.empty()) return;
	const HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return;
	for (std::size_t index = 0; index < m_headerMenu.size(); ++index) {
		const auto& item = m_headerMenu[index];
		const UINT flags = MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED);
		(void)::AppendMenuW(menu, flags, static_cast<UINT_PTR>(index + 1), item.title.c_str());
	}
	RECT anchor{};
	::GetWindowRect(m_headerMenuButton, &anchor);
	const UINT chosen = ::TrackPopupMenuEx(menu,
		TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_RIGHTALIGN,
		anchor.right, anchor.bottom, m_window, nullptr);
	::DestroyMenu(menu);
	if (chosen == 0 || static_cast<std::size_t>(chosen) > m_headerMenu.size()) return;
	const auto& item = m_headerMenu[static_cast<std::size_t>(chosen) - 1];
	if (!item.enabled || !item.invoke) return;
	auto invoke = item.invoke;
	try {
		invoke();
	}
	catch (...) {
		// A contributed title command cannot unwind through the Part window proc.
	}
}

int CWorkbenchPanelHost::ClampExtent(int extentDip) noexcept
{
	return std::clamp(extentDip, 0,
		static_cast<int>(layout::kMaximumWorkbenchLayoutCommittedExtentDip));
}

} // namespace workbench
