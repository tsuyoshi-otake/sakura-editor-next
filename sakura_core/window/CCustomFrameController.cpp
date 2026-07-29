/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CCustomFrameController.h"
#include "func/Funccode.h"
#include "workbench/WorkbenchZoom.h"

#include <algorithm>

#include <dwmapi.h>

namespace {

constexpr int kMenuBarNode = 1000;
constexpr int kMenuItemBaseNode = 1100;
constexpr int kMinimizeNode = 1200;
constexpr int kMaximizeNode = 1201;
constexpr int kCloseNode = 1202;
constexpr int kTitleControlNodeBase = 1300;

bool Contains(const RECT& rect, POINT point) noexcept
{
	return point.x >= rect.left && point.x < rect.right
		&& point.y >= rect.top && point.y < rect.bottom;
}

RECT MakeRect(int left, int top, int right, int bottom) noexcept
{
	return { left, top, std::max(left, right), std::max(top, bottom) };
}

RECT TitleControlRect(const CustomFrameLayout& layout, CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return layout.layoutButton;
	case CustomFrameControl::PrimarySidebar: return layout.primarySidebarButton;
	case CustomFrameControl::BottomPanel: return layout.bottomPanelButton;
	case CustomFrameControl::SecondarySidebar: return layout.secondarySidebarButton;
	case CustomFrameControl::Account: return layout.accountButton;
	case CustomFrameControl::Settings: return layout.settingsButton;
	case CustomFrameControl::None: break;
	}
	return {};
}

CustomFrameControl TitleControlFromNode(int nodeId) noexcept
{
	const int value = nodeId - kTitleControlNodeBase + 1;
	return value >= static_cast<int>(CustomFrameControl::Layout)
		&& value <= static_cast<int>(CustomFrameControl::Settings)
		? static_cast<CustomFrameControl>(value)
		: CustomFrameControl::None;
}

int TitleControlNode(CustomFrameControl control) noexcept
{
	return control == CustomFrameControl::None
		? -1
		: kTitleControlNodeBase + static_cast<int>(control) - 1;
}

} // namespace

int ScaleCustomFrameDip(int value, UINT dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

CustomFrameLayout CalculateCustomFrameLayout(int clientWidth, UINT dpi, int preferredMenuWidth) noexcept
{
	const int width = std::max(0, clientWidth);
	const int titleHeight = ScaleCustomFrameDip(34, dpi);
	const int systemWidth = ScaleCustomFrameDip(42, dpi);
	const int buttonWidth = ScaleCustomFrameDip(46, dpi);
	const int closeWidth = ScaleCustomFrameDip(48, dpi);
	const int captionPadding = ScaleCustomFrameDip(10, dpi);
	const int titleControlWidth = ScaleCustomFrameDip(30, dpi);
	const int titleControlCount = 6;
	const int buttonsWidth = buttonWidth * 2 + closeWidth;
	const int buttonLeft = std::max(0, width - buttonsWidth);
	const int menuLeft = std::min(width, systemWidth);
	const int titleControlsWidth = titleControlWidth * titleControlCount;
	// Never partially draw a title control or let it overlap the system menu. On a
	// narrow window all six collapse together, preserving caption drag and native buttons.
	const bool showTitleControls = buttonLeft >= systemWidth + titleControlsWidth;
	const int titleControlsLeft = showTitleControls ? buttonLeft - titleControlsWidth : buttonLeft;
	const int maximumMenuRight = std::max(menuLeft, titleControlsLeft - ScaleCustomFrameDip(80, dpi));
	const int menuRight = std::min(maximumMenuRight, menuLeft + std::max(0, preferredMenuWidth));
	const int captionSafeLeft = std::clamp(menuRight + captionPadding, 0, width);
	const int captionSafeRight = std::clamp(titleControlsLeft - captionPadding, 0, width);
	// Reserve identical physical space on both sides of the window centre. This keeps
	// the title visually centred even when the menu and compact controls have different
	// widths. On extremely narrow windows, retain the remaining safe area for an
	// ellipsized title instead of intersecting the menu or caption controls.
	const int centeredCaptionLeft = std::max(captionSafeLeft, width - captionSafeRight);
	const int centeredCaptionRight = std::min(captionSafeRight, width - captionSafeLeft);
	const bool hasCenteredCaption = centeredCaptionLeft < centeredCaptionRight;

	CustomFrameLayout layout{};
	layout.title = MakeRect(0, 0, width, titleHeight);
	layout.systemMenu = MakeRect(0, 0, std::min(systemWidth, width), titleHeight);
	layout.menu = MakeRect(menuLeft, 0, menuRight, titleHeight);
	layout.captionText = MakeRect(
		hasCenteredCaption ? centeredCaptionLeft : captionSafeLeft,
		0,
		hasCenteredCaption ? centeredCaptionRight : captionSafeRight,
		titleHeight
	);
	if (showTitleControls) {
		int controlLeft = titleControlsLeft;
		const auto nextControl = [&]() noexcept {
			const RECT rect = MakeRect(controlLeft, 0, controlLeft + titleControlWidth, titleHeight);
			controlLeft += titleControlWidth;
			return rect;
		};
		layout.layoutButton = nextControl();
		layout.primarySidebarButton = nextControl();
		layout.bottomPanelButton = nextControl();
		layout.secondarySidebarButton = nextControl();
		layout.accountButton = nextControl();
		layout.settingsButton = nextControl();
	}
	layout.minimizeButton = MakeRect(buttonLeft, 0, std::min(width, buttonLeft + buttonWidth), titleHeight);
	layout.maximizeButton = MakeRect(
		std::min(width, buttonLeft + buttonWidth),
		0,
		std::min(width, buttonLeft + buttonWidth * 2),
		titleHeight
	);
	layout.closeButton = MakeRect(std::min(width, buttonLeft + buttonWidth * 2), 0, width, titleHeight);
	return layout;
}

CustomFrameControl HitTestCustomFrameControl(const CustomFrameLayout& layout, POINT point) noexcept
{
	for (const CustomFrameControl control : {
		CustomFrameControl::Layout,
		CustomFrameControl::PrimarySidebar,
		CustomFrameControl::BottomPanel,
		CustomFrameControl::SecondarySidebar,
		CustomFrameControl::Account,
		CustomFrameControl::Settings,
	}) {
		if (Contains(TitleControlRect(layout, control), point)) return control;
	}
	return CustomFrameControl::None;
}

UINT CustomFrameControlCommand(CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::PrimarySidebar: return F_TOGGLE_LEFT_EXPLORER;
	case CustomFrameControl::BottomPanel: return F_TOGGLE_BOTTOM_PANEL;
	case CustomFrameControl::SecondarySidebar: return F_TOGGLE_RIGHT_OUTLINE;
	case CustomFrameControl::Settings: return F_OPTION;
	case CustomFrameControl::None:
	case CustomFrameControl::Layout:
	case CustomFrameControl::Account:
		return 0;
	}
	return 0;
}

accessibility::CustomUiAutomationNode CustomFrameControlAccessibilityNode(
	CustomFrameControl control,
	const CustomFrameLayout& layout,
	bool focused
)
{
	if (control == CustomFrameControl::None) return {};
	return {
		TitleControlNode(control),
		CustomFrameControlName(control),
		CustomFrameControlAutomationId(control),
		UIA_ButtonControlTypeId,
		TitleControlRect(layout, control),
		true,
		focused,
		true,
	};
}

LRESULT HitTestCustomFrame(
	const CustomFrameLayout& layout,
	POINT point,
	int clientWidth,
	int clientHeight,
	int resizeBorder,
	bool maximized
) noexcept
{
	const int width = std::max(0, clientWidth);
	const int height = std::max(0, clientHeight);
	const int border = std::max(0, resizeBorder);
	if (!maximized && border > 0) {
		const bool left = point.x >= 0 && point.x < border;
		const bool right = point.x < width && point.x >= width - border;
		const bool top = point.y >= 0 && point.y < border;
		const bool bottom = point.y < height && point.y >= height - border;
		if (top && left) return HTTOPLEFT;
		if (top && right) return HTTOPRIGHT;
		if (bottom && left) return HTBOTTOMLEFT;
		if (bottom && right) return HTBOTTOMRIGHT;
		if (left) return HTLEFT;
		if (right) return HTRIGHT;
		if (top) return HTTOP;
		if (bottom) return HTBOTTOM;
	}
	if (Contains(layout.closeButton, point)) return HTCLOSE;
	if (Contains(layout.maximizeButton, point)) return HTMAXBUTTON;
	if (Contains(layout.minimizeButton, point)) return HTMINBUTTON;
	if (Contains(layout.systemMenu, point)) return HTSYSMENU;
	if (Contains(layout.menu, point)) return HTCLIENT;
	if (HitTestCustomFrameControl(layout, point) != CustomFrameControl::None) return HTCLIENT;
	if (Contains(layout.title, point)) return HTCAPTION;
	return HTCLIENT;
}

bool ShouldPreferDwmNonClientResult(UINT message, LRESULT dwmResult) noexcept
{
	if (message == WM_NCHITTEST) return dwmResult != HTCLIENT && dwmResult != HTNOWHERE;
	switch (message) {
	case WM_NCMOUSEMOVE:
	case WM_NCMOUSELEAVE:
	case WM_NCLBUTTONDOWN:
	case WM_NCLBUTTONUP:
	case WM_NCLBUTTONDBLCLK:
	case WM_NCRBUTTONDOWN:
	case WM_NCRBUTTONUP:
	case WM_NCRBUTTONDBLCLK:
		return true;
	default:
		return false;
	}
}

UINT CaptionButtonSystemCommand(LRESULT hit, bool maximized) noexcept
{
	switch (hit) {
	case HTMINBUTTON:
		return SC_MINIMIZE;
	case HTMAXBUTTON:
		return maximized ? SC_RESTORE : SC_MAXIMIZE;
	case HTCLOSE:
		return SC_CLOSE;
	default:
		return 0;
	}
}

CCustomFrameController::~CCustomFrameController()
{
	m_accessibilityLifetime->Invalidate();
}

void CCustomFrameController::Attach(HWND window, theme::ThemeMode savedMode) noexcept
{
	if (!m_accessibilityLifetime->IsAlive()) m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
	m_window = window;
	m_savedMode = savedMode;
	m_active = window == nullptr || ::GetActiveWindow() == window;
	RefreshMetrics();
	if (m_window != nullptr) {
		const BOOL dark = m_savedMode == theme::ThemeMode::Dark;
		constexpr DWORD kUseImmersiveDarkMode = 20;
		::DwmSetWindowAttribute(m_window, kUseImmersiveDarkMode, &dark, sizeof(dark));
	}
}

void CCustomFrameController::Detach() noexcept
{
	m_accessibilityLifetime->Invalidate();
	m_menuBar.SetMenu(nullptr);
	m_font.Reset();
	m_menuFont.Reset();
	m_window = nullptr;
	m_hotHit = HTNOWHERE;
	m_pressedHit = HTNOWHERE;
	m_hotControl = CustomFrameControl::None;
	m_pressedControl = CustomFrameControl::None;
	m_trackingNonClientLeave = false;
	m_trackingTitleControlLeave = false;
	m_accessibilityFocusedNode = -1;
}

HMENU CCustomFrameController::ReplaceMenu(HMENU menu) noexcept
{
	const HMENU previous = m_menuBar.GetMenu();
	m_menuBar.SetMenu(menu);
	RefreshLayout();
	InvalidateTitle();
	return previous;
}

void CCustomFrameController::SetThemeMode(theme::ThemeMode savedMode) noexcept
{
	m_savedMode = savedMode;
	m_palette = theme::CThemeService::EffectivePalette(savedMode);
	if (m_window != nullptr) {
		const BOOL dark = savedMode == theme::ThemeMode::Dark;
		constexpr DWORD kUseImmersiveDarkMode = 20;
		::DwmSetWindowAttribute(m_window, kUseImmersiveDarkMode, &dark, sizeof(dark));
	}
	InvalidateTitle();
}

void CCustomFrameController::SetUiScalePercent(int percent) noexcept
{
	percent = std::clamp(percent, workbench::kMinimumZoomPercent, workbench::kMaximumZoomPercent);
	if (m_uiScalePercent == percent) return;
	m_uiScalePercent = percent;
	RefreshMetrics();
	InvalidateTitle();
}

void CCustomFrameController::RefreshMetrics() noexcept
{
	m_physicalDpi = m_window == nullptr ? 96 : std::max<UINT>(96, ::GetDpiForWindow(m_window));
	m_dpi = workbench::ScaleDpi(m_physicalDpi, m_uiScalePercent);
	m_palette = theme::CThemeService::EffectivePalette(m_savedMode);
	(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
	(void)m_menuFont.Recreate(theme::ThemeFontKind::Chrome, m_dpi, 8);
	RefreshLayout();
}

void CCustomFrameController::RefreshLayout() noexcept
{
	if (m_window == nullptr) {
		m_layout = {};
		return;
	}
	RECT client{};
	::GetClientRect(m_window, &client);
	const int menuWidth = m_menuBar.MeasurePreferredWidth(m_window, m_menuFont.Get(), m_dpi);
	m_layout = CalculateCustomFrameLayout(client.right - client.left, m_dpi, menuWidth);
	m_menuBar.SetBounds(m_layout.menu);
	m_menuBar.UpdateItemLayout(m_window, m_menuFont.Get());
}

int CCustomFrameController::ResizeBorder() const noexcept
{
	if (m_window == nullptr) {
		return 0;
	}
	return ::GetSystemMetricsForDpi(SM_CXFRAME, m_dpi)
		+ ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_dpi);
}

bool CCustomFrameController::IsCaptionButton(LRESULT hit) const noexcept
{
	return hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE;
}

LRESULT CCustomFrameController::HitTestScreenPoint(POINT screenPoint) noexcept
{
	if (m_window == nullptr) {
		return HTNOWHERE;
	}
	RefreshLayout();
	POINT clientPoint = screenPoint;
	::ScreenToClient(m_window, &clientPoint);
	RECT client{};
	::GetClientRect(m_window, &client);
	return HitTestCustomFrame(
		m_layout,
		clientPoint,
		client.right - client.left,
		client.bottom - client.top,
		ResizeBorder(),
		::IsZoomed(m_window) != FALSE
	);
}

void CCustomFrameController::SetHotHit(LRESULT hit) noexcept
{
	if (m_hotHit != hit) {
		m_hotHit = hit;
		InvalidateTitle();
	}
}

void CCustomFrameController::SetPressedHit(LRESULT hit) noexcept
{
	if (m_pressedHit != hit) {
		m_pressedHit = hit;
		InvalidateTitle();
	}
}

void CCustomFrameController::SetHotControl(CustomFrameControl control) noexcept
{
	if (m_hotControl != control) {
		m_hotControl = control;
		InvalidateTitle();
	}
}

void CCustomFrameController::SetPressedControl(CustomFrameControl control) noexcept
{
	if (m_pressedControl != control) {
		m_pressedControl = control;
		InvalidateTitle();
	}
}

bool CCustomFrameController::HandleTitleControlMouseMessage(
	UINT message,
	[[maybe_unused]] WPARAM wParam,
	LPARAM lParam,
	LRESULT& result
) noexcept
{
	if (m_window == nullptr) return false;
	const auto controlAtCursor = [&]() noexcept {
		return HitTestCustomFrameControl(m_layout, { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
	};
	switch (message) {
	case WM_MOUSEMOVE: {
		const CustomFrameControl control = controlAtCursor();
		SetHotControl(control);
		if (control != CustomFrameControl::None && !m_trackingTitleControlLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
			m_trackingTitleControlLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		return control != CustomFrameControl::None || m_pressedControl != CustomFrameControl::None;
	}
	case WM_MOUSELEAVE:
		m_trackingTitleControlLeave = false;
		SetHotControl(CustomFrameControl::None);
		return false;
	case WM_LBUTTONDOWN: {
		const CustomFrameControl control = controlAtCursor();
		if (control == CustomFrameControl::None) return false;
		SetPressedControl(control);
		::SetCapture(m_window);
		result = 0;
		return true;
	}
	case WM_LBUTTONUP: {
		const CustomFrameControl pressed = m_pressedControl;
		if (pressed == CustomFrameControl::None) return false;
		const CustomFrameControl released = controlAtCursor();
		if (::GetCapture() == m_window) ::ReleaseCapture();
		SetPressedControl(CustomFrameControl::None);
		SetHotControl(released);
		if (released == pressed) InvokeTitleControl(released);
		result = 0;
		return true;
	}
	case WM_CAPTURECHANGED:
		if (m_pressedControl == CustomFrameControl::None) return false;
		SetPressedControl(CustomFrameControl::None);
		SetHotControl(CustomFrameControl::None);
		return true;
	default:
		return false;
	}
}

void CCustomFrameController::InvokeTitleControl(CustomFrameControl control) noexcept
{
	if (m_window == nullptr || control == CustomFrameControl::None) return;
	const UINT command = CustomFrameControlCommand(control);
	if (command != 0) {
		::SendMessageW(m_window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
		return;
	}
	const RECT anchor = TitleControlRect(m_layout, control);
	if (control == CustomFrameControl::Layout) {
		ShowLayoutMenu(anchor);
	} else if (control == CustomFrameControl::Account) {
		ShowAccountMenu(anchor);
	}
}

void CCustomFrameController::ShowLayoutMenu(const RECT& anchor) noexcept
{
	if (m_window == nullptr || ::IsRectEmpty(&anchor)) return;
	const HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return;
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_LEFT_EXPLORER, L"Toggle Primary Side Bar");
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_BOTTOM_PANEL, L"Toggle Bottom Panel");
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_RIGHT_OUTLINE, L"Toggle Secondary Side Bar");
	POINT point{ anchor.left, anchor.bottom };
	::ClientToScreen(m_window, &point);
	const UINT command = ::TrackPopupMenu(
		menu,
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_RETURNCMD | TPM_VERTICAL,
		point.x, point.y, 0, m_window, nullptr
	);
	::DestroyMenu(menu);
	if (command != 0) ::SendMessageW(m_window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

void CCustomFrameController::ShowAccountMenu(const RECT& anchor) noexcept
{
	if (m_window == nullptr || ::IsRectEmpty(&anchor)) return;
	const HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return;
	// Authentication is not configured in Sakura Editor; keep this explicit rather than implying sign-in works.
	::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No account provider configured");
	POINT point{ anchor.left, anchor.bottom };
	::ClientToScreen(m_window, &point);
	(void)::TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON | TPM_VERTICAL,
		point.x, point.y, 0, m_window, nullptr);
	::DestroyMenu(menu);
}

void CCustomFrameController::ClearAccessibilityFocus() noexcept
{
	if (m_accessibilityFocusedNode < 0) return;
	const int previous = m_accessibilityFocusedNode;
	m_accessibilityFocusedNode = -1;
	m_menuBar.SetAccessibilityFocusedItem(m_window, -1);
	accessibility::RaiseFocusCleared(*this, previous);
	InvalidateTitle();
}

bool CCustomFrameController::HandleWindowMessage(
	UINT message,
	WPARAM wParam,
	LPARAM lParam,
	LRESULT& result
) noexcept
{
	if (m_window == nullptr) {
		return false;
	}
	LRESULT dwmResult = 0;
	const BOOL dwmHandled = ::DwmDefWindowProc(m_window, message, wParam, lParam, &dwmResult);
	switch (message) {
	case WM_GETOBJECT:
		if (lParam == static_cast<LPARAM>(UiaRootObjectId) || lParam == static_cast<LPARAM>(OBJID_CLIENT)) {
			result = accessibility::HandleGetObject(*this, wParam, lParam);
			return true;
		}
		return false;
	case WM_NCCALCSIZE:
		if (wParam != FALSE && lParam != 0) {
			auto* parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
			if (::IsZoomed(m_window)) {
				MONITORINFO monitor{ sizeof(monitor) };
				if (::GetMonitorInfoW(::MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor)) {
					parameters->rgrc[0].left = std::max(parameters->rgrc[0].left, monitor.rcWork.left);
					parameters->rgrc[0].top = std::max(parameters->rgrc[0].top, monitor.rcWork.top);
					parameters->rgrc[0].right = std::min(parameters->rgrc[0].right, monitor.rcWork.right);
					parameters->rgrc[0].bottom = std::min(parameters->rgrc[0].bottom, monitor.rcWork.bottom);
				}
			}
		}
		result = 0;
		return true;
	case WM_NCHITTEST: {
		// DWM must retain ownership of a processed non-client target (notably
		// HTMAXBUTTON, which enables Windows 11 Snap Layouts). DWM returns
		// HTCLIENT over the extended custom client, where our title/menu/resize
		// geometry is the deliberate exception and supplies the hit instead.
		if (dwmHandled && ShouldPreferDwmNonClientResult(message, dwmResult)) {
			result = dwmResult;
			return true;
		}
		const POINT screenPoint{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const LRESULT hit = HitTestScreenPoint(screenPoint);
		result = hit;
		return true;
	}
	case WM_NCMOUSEMOVE:
		SetHotHit(IsCaptionButton(static_cast<LRESULT>(wParam)) ? static_cast<LRESULT>(wParam) : HTNOWHERE);
		if (!m_trackingNonClientLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE | TME_NONCLIENT, m_window, 0 };
			m_trackingNonClientLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		if (dwmHandled) {
			result = dwmResult;
			return true;
		}
		return false;
	case WM_NCMOUSELEAVE:
		m_trackingNonClientLeave = false;
		SetHotHit(HTNOWHERE);
		SetPressedHit(HTNOWHERE);
		if (dwmHandled) {
			result = dwmResult;
			return true;
		}
		return false;
	case WM_NCLBUTTONDOWN:
		if (IsCaptionButton(static_cast<LRESULT>(wParam))) {
			SetPressedHit(static_cast<LRESULT>(wParam));
			// The top edge is client-extended, so DWM supplies Snap hover but does
			// not dispatch the command for Sakura-owned caption buttons.
			result = 0;
			return true;
		}
		if (dwmHandled) {
			result = dwmResult;
			return true;
		}
		return false;
	case WM_NCLBUTTONUP:
	{
		const LRESULT releasedHit = static_cast<LRESULT>(wParam);
		const bool invoke = IsCaptionButton(releasedHit) && m_pressedHit == releasedHit;
		SetPressedHit(HTNOWHERE);
		if (IsCaptionButton(releasedHit)) {
			if (invoke) {
				const UINT command = CaptionButtonSystemCommand(releasedHit, ::IsZoomed(m_window) != FALSE);
				if (command != 0) {
					::SendMessageW(m_window, WM_SYSCOMMAND, command, 0);
				}
			}
			result = 0;
			return true;
		}
		if (dwmHandled) {
			result = dwmResult;
			return true;
		}
		return false;
	}
	case WM_NCLBUTTONDBLCLK:
	case WM_NCRBUTTONDOWN:
	case WM_NCRBUTTONUP:
	case WM_NCRBUTTONDBLCLK:
		if (dwmHandled && ShouldPreferDwmNonClientResult(message, dwmResult)) {
			result = dwmResult;
			return true;
		}
		return false;
	case WM_MOUSEMOVE:
	case WM_MOUSELEAVE:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (HandleTitleControlMouseMessage(message, wParam, lParam, result)) return true;
		return m_menuBar.HandleMouseMessage(m_window, message, wParam, lParam, result);
	case WM_DPICHANGED:
		m_physicalDpi = HIWORD(wParam) == 0 ? 96 : HIWORD(wParam);
		m_dpi = workbench::ScaleDpi(m_physicalDpi, m_uiScalePercent);
		(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
		(void)m_menuFont.Recreate(theme::ThemeFontKind::Chrome, m_dpi, 8);
		if (lParam != 0) {
			const RECT suggested = *reinterpret_cast<const RECT*>(lParam);
			::SetWindowPos(m_window, nullptr, suggested.left, suggested.top,
				suggested.right - suggested.left, suggested.bottom - suggested.top,
				SWP_NOACTIVATE | SWP_NOZORDER);
		}
		RefreshLayout();
		InvalidateTitle();
		result = 0;
		return true;
	case WM_SIZE:
		RefreshLayout();
		InvalidateTitle();
		return false;
	case WM_NCACTIVATE:
	case WM_ACTIVATE:
		m_active = message == WM_NCACTIVATE ? wParam != FALSE : LOWORD(wParam) != WA_INACTIVE;
		InvalidateTitle();
		return false;
	case WM_KILLFOCUS:
		// The virtual chrome no longer owns keyboard focus when focus moves to an
		// editor child or another top-level window.
		ClearAccessibilityFocus();
		return false;
	case WM_SETTEXT:
		InvalidateTitle();
		return false;
	case WM_THEMECHANGED:
	case WM_SETTINGCHANGE:
		m_palette = theme::CThemeService::EffectivePalette(m_savedMode);
		InvalidateTitle();
		return false;
	case WM_NCDESTROY:
		Detach();
		return false;
	default:
		return false;
	}
}

bool CCustomFrameController::PreTranslateMessage(MSG& message) noexcept
{
	if (m_window == nullptr) return false;
	const int before = m_menuBar.AccessibilityFocusedItem();
	const bool handled = m_menuBar.PreTranslateMessage(m_window, message);
	const int focused = m_menuBar.AccessibilityFocusedItem();
	if (focused >= 0 && focused != before) {
		m_accessibilityFocusedNode = kMenuItemBaseNode + focused;
		accessibility::RaiseFocusChanged(*this, m_accessibilityFocusedNode);
	}
	return handled;
}

void CCustomFrameController::Paint(HDC dc, [[maybe_unused]] const RECT& paintRect) noexcept
{
	if (m_window == nullptr || dc == nullptr) {
		return;
	}
	RefreshLayout();
	m_titleBar.Paint(m_window, dc, m_layout, m_palette, m_font.Get(), m_active, m_hotHit, m_pressedHit,
		m_hotControl, m_pressedControl, TitleControlFromNode(m_accessibilityFocusedNode));
	m_menuBar.Paint(m_window, dc, m_menuFont.Get(), m_palette, m_active);
}

void CCustomFrameController::InvalidateTitle() const noexcept
{
	if (m_window != nullptr && !::IsRectEmpty(&m_layout.title)) {
		::InvalidateRect(m_window, &m_layout.title, FALSE);
	}
}

std::wstring CCustomFrameController::AccessibilityName() const
{
	if (m_window == nullptr) return L"Sakura Editor NEXT";
	const int length = ::GetWindowTextLengthW(m_window);
	if (length <= 0) return L"Sakura Editor NEXT";
	std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
	::GetWindowTextW(m_window, title.data(), length + 1);
	title.resize(static_cast<std::size_t>(length));
	return title;
}

int CCustomFrameController::AccessibilityChildCount(int parentId) const noexcept
{
	if (parentId == kMenuBarNode) return m_menuBar.AccessibilityItemCount();
	if (parentId != -1) return 0;
	const bool hasTitleControls = !::IsRectEmpty(&m_layout.layoutButton);
	return 3 + (hasTitleControls ? 6 : 0) + (m_menuBar.AccessibilityItemCount() > 0 ? 1 : 0);
}

int CCustomFrameController::AccessibilityChildAt(int parentId, int index) const noexcept
{
	if (parentId == kMenuBarNode) {
		return index >= 0 && index < m_menuBar.AccessibilityItemCount() ? kMenuItemBaseNode + index : -1;
	}
	if (parentId != -1 || index < 0) return -1;
	const bool hasMenu = m_menuBar.AccessibilityItemCount() > 0;
	int childIndex = index;
	if (hasMenu) {
		if (childIndex == 0) return kMenuBarNode;
		--childIndex;
	}
	if (!::IsRectEmpty(&m_layout.layoutButton)) {
		if (childIndex >= 0 && childIndex < 6) {
			return TitleControlNode(static_cast<CustomFrameControl>(childIndex + 1));
		}
		childIndex -= 6;
	}
	const int captionIndex = childIndex;
	switch (captionIndex) {
	case 0: return kMinimizeNode;
	case 1: return kMaximizeNode;
	case 2: return kCloseNode;
	default: return -1;
	}
}

int CCustomFrameController::AccessibilityParent(int nodeId) const noexcept
{
	if (nodeId == kMenuBarNode || nodeId == kMinimizeNode || nodeId == kMaximizeNode || nodeId == kCloseNode) return -1;
	if (const CustomFrameControl control = TitleControlFromNode(nodeId); control != CustomFrameControl::None) {
		const RECT bounds = TitleControlRect(m_layout, control);
		if (!::IsRectEmpty(&bounds)) return -1;
	}
	if (nodeId >= kMenuItemBaseNode && nodeId < kMenuItemBaseNode + m_menuBar.AccessibilityItemCount()) return kMenuBarNode;
	return -2;
}

accessibility::CustomUiAutomationNode CCustomFrameController::AccessibilityNode(int nodeId) const
{
	if (nodeId == kMenuBarNode) {
		return { nodeId, L"Application menu", L"Sakura.MenuBar", UIA_MenuBarControlTypeId,
			m_layout.menu, true, false, false };
	}
	if (nodeId >= kMenuItemBaseNode && nodeId < kMenuItemBaseNode + m_menuBar.AccessibilityItemCount()) {
		const int index = nodeId - kMenuItemBaseNode;
		return { nodeId, accessibility::StripMenuMnemonics(m_menuBar.AccessibilityItemText(index)),
			L"Sakura.Menu." + std::to_wstring(index), UIA_MenuItemControlTypeId,
			m_menuBar.AccessibilityItemBounds(index), m_menuBar.AccessibilityItemEnabled(index),
			m_accessibilityFocusedNode == nodeId, true };
	}
	if (const CustomFrameControl control = TitleControlFromNode(nodeId); control != CustomFrameControl::None) {
		const RECT bounds = TitleControlRect(m_layout, control);
		if (!::IsRectEmpty(&bounds)) {
			return CustomFrameControlAccessibilityNode(control, m_layout, m_accessibilityFocusedNode == nodeId);
		}
	}
	const LONG_PTR style = m_window == nullptr ? 0 : ::GetWindowLongPtrW(m_window, GWL_STYLE);
	if (nodeId == kMinimizeNode) {
		return { nodeId, L"Minimize", L"Sakura.Caption.Minimize", UIA_ButtonControlTypeId,
			m_layout.minimizeButton, (style & WS_MINIMIZEBOX) != 0,
			m_accessibilityFocusedNode == nodeId, true };
	}
	if (nodeId == kMaximizeNode) {
		const bool maximized = m_window != nullptr && ::IsZoomed(m_window) != FALSE;
		return { nodeId, maximized ? L"Restore" : L"Maximize", L"Sakura.Caption.Maximize", UIA_ButtonControlTypeId,
			m_layout.maximizeButton, (style & WS_MAXIMIZEBOX) != 0,
			m_accessibilityFocusedNode == nodeId, true };
	}
	if (nodeId == kCloseNode) {
		return { nodeId, L"Close", L"Sakura.Caption.Close", UIA_ButtonControlTypeId,
			m_layout.closeButton, (style & WS_SYSMENU) != 0,
			m_accessibilityFocusedNode == nodeId, true };
	}
	return {};
}

int CCustomFrameController::AccessibilityFocusedNode() const noexcept
{
	return m_accessibilityFocusedNode;
}

bool CCustomFrameController::AccessibilityInvoke(int nodeId) noexcept
{
	if (m_window == nullptr || !::IsWindow(m_window)) return false;
	if (AccessibilityParent(nodeId) == -2 || !AccessibilityNode(nodeId).enabled) return false;
	// The invoked event must be emitted before a command such as Close can destroy this window.
	accessibility::RaiseInvoked(*this, nodeId);
	bool invoked = false;
	if (nodeId >= kMenuItemBaseNode && nodeId < kMenuItemBaseNode + m_menuBar.AccessibilityItemCount()) {
		invoked = m_menuBar.InvokeAccessibilityItem(m_window, nodeId - kMenuItemBaseNode);
	} else if (const CustomFrameControl control = TitleControlFromNode(nodeId); control != CustomFrameControl::None) {
		InvokeTitleControl(control);
		invoked = true;
	} else if (nodeId == kMinimizeNode && AccessibilityNode(nodeId).enabled) {
		::SendMessageW(m_window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
		invoked = true;
	} else if (nodeId == kMaximizeNode && AccessibilityNode(nodeId).enabled) {
		::SendMessageW(m_window, WM_SYSCOMMAND, ::IsZoomed(m_window) ? SC_RESTORE : SC_MAXIMIZE, 0);
		invoked = true;
	} else if (nodeId == kCloseNode && AccessibilityNode(nodeId).enabled) {
		::SendMessageW(m_window, WM_SYSCOMMAND, SC_CLOSE, 0);
		invoked = true;
	}
	return invoked;
}

void CCustomFrameController::AccessibilitySetFocus(int nodeId) noexcept
{
	if (AccessibilityParent(nodeId) == -2 || !AccessibilityNode(nodeId).enabled) return;
	m_accessibilityFocusedNode = nodeId;
	if (nodeId >= kMenuItemBaseNode && nodeId < kMenuItemBaseNode + m_menuBar.AccessibilityItemCount()) {
		m_menuBar.SetAccessibilityFocusedItem(m_window, nodeId - kMenuItemBaseNode);
	}
	InvalidateTitle();
}
