/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/CEmptyEditorSurface.h"

#include "config/system_constants.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

#include <windowsx.h>

#include <algorithm>

namespace workbench::editor {
namespace {

constexpr wchar_t kEmptyEditorSurfaceClass[] = L"SakuraWorkbenchEmptyEditorSurface";
constexpr unsigned int kDefaultDpi = 96;
constexpr int kFocusInsetDip = 2;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? kDefaultDpi : dpi) + 48) / 96);
}

[[nodiscard]] bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	// VS Code opens a pinned Untitled editor when the user double-clicks an
	// empty editor group.  Win32 only emits WM_LBUTTONDBLCLK for classes that
	// opt in with CS_DBLCLKS.
	windowClass.style = CS_DBLCLKS;
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CEmptyEditorSurface::WindowProc;
	windowClass.lpszClassName = kEmptyEditorSurfaceClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

[[nodiscard]] const wchar_t* AutomationActionName(EmptyEditorSurfaceAction action) noexcept
{
	switch (action) {
	case EmptyEditorSurfaceAction::NewFile: return L"NewFile";
	case EmptyEditorSurfaceAction::OpenFile: return L"OpenFile";
	case EmptyEditorSurfaceAction::OpenFolder: return L"OpenFolder";
	case EmptyEditorSurfaceAction::ShowAllCommands: return L"ShowAllCommands";
	case EmptyEditorSurfaceAction::OpenSettings: return L"OpenSettings";
	case EmptyEditorSurfaceAction::Count: break;
	}
	return L"";
}

[[nodiscard]] std::wstring LocalizedActionLabel(EmptyEditorSurfaceAction action, const wchar_t* fallback)
{
	UINT resourceId = 0;
	switch (action) {
	case EmptyEditorSurfaceAction::NewFile: resourceId = STR_WORKBENCH_COMMAND_NEW_FILE; break;
	case EmptyEditorSurfaceAction::OpenFile: resourceId = STR_WORKBENCH_COMMAND_OPEN_FILE; break;
	case EmptyEditorSurfaceAction::OpenFolder: resourceId = STR_WORKBENCH_COMMAND_OPEN_FOLDER; break;
	case EmptyEditorSurfaceAction::ShowAllCommands: resourceId = STR_WORKBENCH_COMMAND_SHOW_COMMANDS; break;
	case EmptyEditorSurfaceAction::OpenSettings: resourceId = STR_WORKBENCH_COMMAND_OPEN_SETTINGS; break;
	case EmptyEditorSurfaceAction::Count: break;
	}
	if (resourceId != 0) {
		const auto localized = CSelectLang::LoadStringW(resourceId);
		if (!localized.empty()) return std::wstring(localized);
	}
	return fallback != nullptr ? std::wstring(fallback) : std::wstring();
}

} // namespace

CEmptyEditorSurface::CEmptyEditorSurface(CommandCallback onCommand)
	: m_onCommand(std::move(onCommand))
{
}

CEmptyEditorSurface::~CEmptyEditorSurface()
{
	m_accessibilityLifetime->Invalidate();
	Destroy();
}

bool CEmptyEditorSurface::Create(HWND parent, HINSTANCE instance)
{
	if (m_destroyed || m_window != nullptr || parent == nullptr || instance == nullptr || !EnsureWindowClass(instance)) return false;
	m_window = ::CreateWindowExW(0, kEmptyEditorSurfaceClass, L"", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (m_window == nullptr) return false;
	m_instance = instance;
	UpdateClientLayout(static_cast<unsigned int>(::GetDpiForWindow(m_window)));
	return true;
}

bool CEmptyEditorSurface::Create(HWND parent, HINSTANCE instance, CommandCallback onCommand)
{
	m_onCommand = std::move(onCommand);
	return Create(parent, instance);
}

void CEmptyEditorSurface::Destroy() noexcept
{
	if (m_destroyed || m_destroying) return;
	m_destroying = true;
	m_captureAction.reset();
	if (m_window != nullptr && ::IsWindow(m_window)) ::DestroyWindow(m_window);
	m_window = nullptr;
	m_font.Reset();
	ReleaseLetterpress();
	m_instance = nullptr;
	m_destroyed = true;
	m_destroying = false;
}

HICON CEmptyEditorSurface::EnsureLetterpress(int side) noexcept
{
	if (m_instance == nullptr || side <= 0) return nullptr;
	if (m_letterpress != nullptr && m_letterpressSide == side) return m_letterpress;
	// LoadImageW realizes the closest stored frame at the requested size, so the logo stays
	// crisp across DPI changes instead of being stretched from one cached bitmap.
	const auto realized = static_cast<HICON>(::LoadImageW(m_instance, MAKEINTRESOURCEW(ICON_DEFAULT_APP),
		IMAGE_ICON, side, side, LR_DEFAULTCOLOR));
	if (realized == nullptr) return m_letterpress;
	ReleaseLetterpress();
	m_letterpress = realized;
	m_letterpressSide = side;
	return m_letterpress;
}

void CEmptyEditorSurface::ReleaseLetterpress() noexcept
{
	if (m_letterpress != nullptr) ::DestroyIcon(m_letterpress);
	m_letterpress = nullptr;
	m_letterpressSide = 0;
}

void CEmptyEditorSurface::Layout(const RECT& bounds, unsigned int dpi)
{
	if (m_destroyed) return;
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	m_model.SetViewport(width, height, dpi);
	if (m_font.Dpi() != m_model.GetDpi()) (void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_model.GetDpi());
	if (m_window != nullptr) ::SetWindowPos(m_window, nullptr, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	Invalidate();
}

void CEmptyEditorSurface::Show() noexcept
{
	if (!m_destroyed && m_window != nullptr) ::ShowWindow(m_window, SW_SHOWNA);
}

void CEmptyEditorSurface::Hide() noexcept
{
	if (!m_destroyed && m_window != nullptr) ::ShowWindow(m_window, SW_HIDE);
}

void CEmptyEditorSurface::Focus() noexcept
{
	if (!m_destroyed && m_window != nullptr && ::IsWindowVisible(m_window)) ::SetFocus(m_window);
}

void CEmptyEditorSurface::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	Invalidate();
}

void CEmptyEditorSurface::SetActionEnabled(EmptyEditorSurfaceAction action, bool enabled) noexcept
{
	const bool oldEnabled = m_model.IsEnabled(action);
	m_model.SetEnabled(action, enabled);
	if (!enabled && m_captureAction == action) {
		m_captureAction.reset();
		if (::GetCapture() == m_window) ::ReleaseCapture();
	}
	if (oldEnabled != enabled) accessibility::RaiseEnabledChanged(*this, static_cast<int>(action), oldEnabled, enabled);
	Invalidate();
}

bool CEmptyEditorSurface::Invoke(EmptyEditorSurfaceAction action) noexcept
{
	const auto invoked = m_model.Invoke(action);
	if (invoked) accessibility::RaiseInvoked(*this, static_cast<int>(*invoked));
	return InvokeRequest(invoked);
}

bool CEmptyEditorSurface::PreTranslateMessage(MSG& message) noexcept
{
	if (m_destroyed || m_window == nullptr || message.hwnd != m_window) return false;
	if (message.message == WM_KEYDOWN) return HandleNavigationKey(message.wParam);
	return message.message == WM_CHAR && (message.wParam == VK_SPACE || message.wParam == VK_RETURN);
}

LRESULT CALLBACK CEmptyEditorSurface::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* surface = static_cast<CEmptyEditorSurface*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(surface));
		if (surface != nullptr) surface->m_window = window;
	}
	auto* surface = reinterpret_cast<CEmptyEditorSurface*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (surface == nullptr) return ::DefWindowProcW(window, message, wParam, lParam);
	if (message == WM_NCDESTROY) {
		surface->m_accessibilityLifetime->Invalidate();
		surface->m_window = nullptr;
		surface->m_captureAction.reset();
		if (!surface->m_destroying) surface->m_destroyed = true;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	return surface->HandleMessage(message, wParam, lParam);
}

LRESULT CEmptyEditorSurface::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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
		if (!m_model.GetFocused()) static_cast<void>(m_model.MoveFocus(1));
		if (const auto action = m_model.GetFocused()) accessibility::RaiseFocusChanged(*this, static_cast<int>(*action));
		Invalidate();
		return 0;
	case WM_KILLFOCUS:
		m_model.SetFocused(std::nullopt);
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
		const auto previousHovered = m_model.GetHovered();
		const auto previousPressed = m_model.GetPressed();
		SetHoverFromPoint(point);
		if (m_captureAction) m_model.SetPressed(m_model.HitTest(point.x, point.y) == m_captureAction ? m_captureAction : std::nullopt);
		if (!m_trackingMouseLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
			m_trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		// Pointer motion that changes no visual state must not repaint. Invalidating
		// on every WM_MOUSEMOVE redrew the whole welcome surface at pointer rate.
		if (m_model.GetHovered() != previousHovered || m_model.GetPressed() != previousPressed) Invalidate();
		return 0;
	}
	case WM_MOUSELEAVE: {
		m_trackingMouseLeave = false;
		const bool changed = m_model.GetHovered().has_value()
			|| (!m_captureAction && m_model.GetPressed().has_value());
		m_model.SetHovered(std::nullopt);
		if (!m_captureAction) m_model.SetPressed(std::nullopt);
		if (changed) Invalidate();
		return 0;
	}
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const auto action = m_model.HitTest(point.x, point.y);
		if (!action) break;
		::SetFocus(m_window);
		m_model.SetFocused(action);
		accessibility::RaiseFocusChanged(*this, static_cast<int>(*action));
		m_model.SetPressed(action);
		m_captureAction = action;
		::SetCapture(m_window);
		Invalidate();
		return 0;
	}
	case WM_LBUTTONUP: {
		if (!m_captureAction) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const auto captured = m_captureAction;
		m_captureAction.reset();
		m_model.SetPressed(std::nullopt);
		if (::GetCapture() == m_window) ::ReleaseCapture();
		SetHoverFromPoint(point);
		const auto invoked = m_model.HitTest(point.x, point.y) == captured ? captured : std::nullopt;
		Invalidate();
		if (invoked) accessibility::RaiseInvoked(*this, static_cast<int>(*invoked));
		static_cast<void>(InvokeRequest(invoked));
		return 0;
	}
	case WM_LBUTTONDBLCLK:
		// Match EditorGroupView.registerContainerListeners(): the entire empty
		// editor group, not just the watermark action rows, creates an Untitled
		// editor on double-click.  This surface is shown only while the group has
		// no active input, and its stable command callback owns the transition.
		static_cast<void>(Invoke(EmptyEditorSurfaceAction::NewFile));
		return 0;
	case WM_CAPTURECHANGED:
		m_captureAction.reset();
		m_model.SetPressed(std::nullopt);
		Invalidate();
		return 0;
	default:
		break;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CEmptyEditorSurface::UpdateClientLayout(unsigned int dpi) noexcept
{
	if (m_window == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	m_model.SetViewport(std::max(0L, client.right - client.left), std::max(0L, client.bottom - client.top), dpi);
	if (m_font.Dpi() != m_model.GetDpi()) (void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_model.GetDpi());
	Invalidate();
}

void CEmptyEditorSurface::Paint() noexcept
{
	PAINTSTRUCT paint{};
	const HDC target = ::BeginPaint(m_window, &paint);
	if (target == nullptr) return;
	RECT client{};
	::GetClientRect(m_window, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);

	// Compose into a back buffer. Filling the canvas and then drawing the wordmark
	// and action rows straight onto the window DC made every repaint visibly blank
	// the surface first, which the user sees as flicker.
	const HDC memory = width > 0 && height > 0 ? ::CreateCompatibleDC(target) : nullptr;
	const HBITMAP buffer = memory != nullptr ? ::CreateCompatibleBitmap(target, width, height) : nullptr;
	const HGDIOBJ previousBitmap = buffer != nullptr ? ::SelectObject(memory, buffer) : nullptr;
	const HDC surface = buffer != nullptr ? memory : target;
	const auto releaseBuffer = [&]() noexcept {
		if (buffer != nullptr) {
			::SelectObject(memory, previousBitmap);
			::DeleteObject(buffer);
		}
		if (memory != nullptr) ::DeleteDC(memory);
	};

	const HBRUSH canvas = ::CreateSolidBrush(m_palette.canvas.ToColorRef());
	::FillRect(surface, &client, canvas);
	::DeleteObject(canvas);
	if (width <= 0 || height <= 0) {
		releaseBuffer();
		::EndPaint(m_window, &paint);
		return;
	}

	PaintContent(surface);
	if (buffer != nullptr) {
		::BitBlt(target, client.left, client.top, width, height, memory, client.left, client.top, SRCCOPY);
	}
	releaseBuffer();
	::EndPaint(m_window, &paint);
}

void CEmptyEditorSurface::PaintContent(HDC target) noexcept
{
	const HGDIOBJ previousFont = m_font.Get() != nullptr ? ::SelectObject(target, m_font.Get()) : nullptr;
	::SetBkMode(target, TRANSPARENT);

	// VS Code's watermark is the product letterpress alone; the caption text lives in the
	// accessible name instead of being painted, so the surface stays a single centered column.
	const auto letterpress = m_model.GetLetterpressBounds();
	const int letterpressSide = std::min(letterpress.Width(), letterpress.Height());
	if (letterpressSide > 0) {
		if (const HICON logo = EnsureLetterpress(letterpressSide); logo != nullptr) {
			(void)::DrawIconEx(target, letterpress.left, letterpress.top, logo,
				letterpressSide, letterpressSide, 0, nullptr, DI_NORMAL);
		}
	} else {
		ReleaseLetterpress();
	}

	for (std::size_t index = 0; index < m_model.GetActionCount(); ++index) {
		const auto action = m_model.GetAction(index);
		if (action.bounds.Width() <= 0 || action.bounds.Height() <= 0) continue;
		RECT bounds{ action.bounds.left, action.bounds.top, action.bounds.right, action.bounds.bottom };
		if (action.pressed || action.hovered) {
			const HBRUSH background = ::CreateSolidBrush((action.pressed ? m_palette.accent : m_palette.raised).ToColorRef());
			::FillRect(target, &bounds, background);
			::DeleteObject(background);
		}
		// VS Code colors the whole watermark definition list, both the label and its keybinding,
		// with descriptionForeground. Only the pressed row inverts onto the accent fill.
		const auto textColor = action.pressed ? m_palette.highlightText
			: (action.enabled ? m_palette.descriptionText : m_palette.disabledText);
		::SetTextColor(target, textColor.ToColorRef());
		const int padding = ScaleDip(8, m_model.GetDpi());
		RECT label{ bounds.left + padding, bounds.top, std::max(bounds.left + padding, bounds.right - padding), bounds.bottom };
		const auto localizedLabel = LocalizedActionLabel(action.action, action.label);
		::DrawTextW(target, localizedLabel.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		::DrawTextW(target, action.shortcut, -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		if (action.focused) {
			RECT focus = bounds;
			const int inset = ScaleDip(kFocusInsetDip, m_model.GetDpi());
			::InflateRect(&focus, -inset, -inset);
			const HBRUSH focusBrush = ::CreateSolidBrush(m_palette.accent.ToColorRef());
			::FrameRect(target, &focus, focusBrush);
			::DeleteObject(focusBrush);
		}
	}
	if (previousFont != nullptr) ::SelectObject(target, previousFont);
}

void CEmptyEditorSurface::Invalidate() const noexcept
{
	if (m_window != nullptr && ::IsWindow(m_window)) ::InvalidateRect(m_window, nullptr, FALSE);
}

bool CEmptyEditorSurface::InvokeRequest(std::optional<EmptyEditorSurfaceAction> action) noexcept
{
	if (!action || !m_onCommand) return false;
	try {
		m_onCommand(EmptyEditorSurfaceModel::CommandId(*action));
		return true;
	} catch (...) {
		return false;
	}
}

bool CEmptyEditorSurface::HandleNavigationKey(WPARAM key) noexcept
{
	std::optional<EmptyEditorSurfaceAction> invoked;
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
	case VK_RETURN:
	case VK_SPACE:
		invoked = m_model.InvokeFocused();
		break;
	default:
		return false;
	}
	Invalidate();
	if (focusChanged) {
		if (const auto action = m_model.GetFocused()) accessibility::RaiseFocusChanged(*this, static_cast<int>(*action));
	}
	if (invoked) accessibility::RaiseInvoked(*this, static_cast<int>(*invoked));
	static_cast<void>(InvokeRequest(invoked));
	return true;
}

int CEmptyEditorSurface::AccessibilityChildCount(int parentId) const noexcept
{
	return parentId == -1 ? static_cast<int>(m_model.GetActionCount()) : 0;
}

int CEmptyEditorSurface::AccessibilityChildAt(int parentId, int index) const noexcept
{
	return parentId == -1 && index >= 0 && index < static_cast<int>(m_model.GetActionCount()) ? index : -1;
}

int CEmptyEditorSurface::AccessibilityParent(int nodeId) const noexcept
{
	return nodeId >= 0 && nodeId < static_cast<int>(m_model.GetActionCount()) ? -1 : -2;
}

accessibility::CustomUiAutomationNode CEmptyEditorSurface::AccessibilityNode(int nodeId) const
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetActionCount())) return {};
	const auto action = m_model.GetAction(static_cast<std::size_t>(nodeId));
	const auto localizedLabel = LocalizedActionLabel(action.action, action.label);
	return {
		nodeId,
		localizedLabel,
		std::wstring(L"Sakura.EmptyEditorSurface.") + AutomationActionName(action.action),
		UIA_ButtonControlTypeId,
		{ action.bounds.left, action.bounds.top, action.bounds.right, action.bounds.bottom },
		action.enabled,
		action.focused,
		true,
	};
}

int CEmptyEditorSurface::AccessibilityFocusedNode() const noexcept
{
	const auto focused = m_model.GetFocused();
	return focused ? static_cast<int>(*focused) : -1;
}

bool CEmptyEditorSurface::AccessibilityInvoke(int nodeId) noexcept
{
	return nodeId >= 0 && nodeId < static_cast<int>(m_model.GetActionCount()) && Invoke(static_cast<EmptyEditorSurfaceAction>(nodeId));
}

void CEmptyEditorSurface::AccessibilitySetFocus(int nodeId) noexcept
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetActionCount())) return;
	m_model.SetFocused(static_cast<EmptyEditorSurfaceAction>(nodeId));
	Focus();
	Invalidate();
}

void CEmptyEditorSurface::SetHoverFromPoint(POINT point) noexcept
{
	m_model.SetHovered(m_model.HitTest(point.x, point.y));
}

} // namespace workbench::editor
