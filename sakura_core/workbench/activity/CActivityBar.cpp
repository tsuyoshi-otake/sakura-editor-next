/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/activity/CActivityBar.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconsActivityIcons.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/layout/WorkbenchIds.h"
#include "util/string_ex.h"

#include <CommCtrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <string_view>

namespace workbench {
namespace {

constexpr wchar_t kActivityBarClass[] = L"SakuraWorkbenchActivityBar";
constexpr int kDefaultDpi = 96;
constexpr int kIndicatorWidthDip = 2;
// The activity indicator dot. Its diameter and insets place it on the glyph's
// bottom-right corner; the 24-DIP glyph is centred in the 48-DIP button, so its
// corner sits 12 DIP in from the button's right edge and 36 DIP down from its
// top. See PaintBadge for why this is a dot and not upstream's number pill.
constexpr int kBadgeDotDiameterDip = 8;
constexpr int kBadgeDotRightInsetDip = 10;
constexpr int kBadgeDotTopInsetDip = 28;

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

[[nodiscard]] wchar_t CodiconGlyph(std::wstring_view name) noexcept
{
	return icons::FindCodiconGlyph(name).value_or(L'\0');
}

[[nodiscard]] bool PaintFontGlyph(
	HDC dc,
	const icons::IconRect& box,
	HFONT font,
	wchar_t glyph,
	COLORREF color
) noexcept
{
	if (dc == nullptr || font == nullptr || glyph == L'\0' || box.Width() <= 0 || box.Height() <= 0) {
		return false;
	}
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	const HGDIOBJ oldFont = ::SelectObject(dc, font);
	if (oldFont == nullptr || oldFont == HGDI_ERROR) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, color);
	RECT glyphRect{ box.left, box.top, box.right, box.bottom };
	const wchar_t text[] = { glyph, L'\0' };
	const int drawn = ::DrawTextW(dc, text, 1, &glyphRect,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::RestoreDC(dc, saved);
	return drawn != 0;
}

/*
	The hand-drawn vector paths remain only as the explicit fallback for Sakura's own containers
	when codicon.ttf could not be registered. Keyed by container id rather than by an enum so
	that adding a container never means teaching this function a second identity system.
*/
[[nodiscard]] bool PaintBuiltinGlyph(
	HDC dc, const icons::IconRect& box, std::string_view containerId, COLORREF color) noexcept
{
	if (containerId == layout::ids::viewContainer::Explorer) {
		icons::codicons::DrawFiles(dc, box, color);
		return true;
	}
	if (containerId == layout::ids::viewContainer::SourceControl) {
		icons::codicons::DrawSourceControl(dc, box, color);
		return true;
	}
	return false;
}

/*
	Fallback for a built-in ViewContainer without a bundled glyph. A blank square is
	indistinguishable from a broken button, so the container's initial is drawn.
*/
[[nodiscard]] bool PaintInitialTile(
	HDC dc, const icons::IconRect& box, std::wstring_view label, COLORREF color, unsigned int dpi) noexcept
{
	if (dc == nullptr || label.empty() || box.Width() <= 0 || box.Height() <= 0) return false;
	LOGFONTW description{};
	description.lfHeight = -ScaleDip(14, dpi);
	description.lfWeight = FW_SEMIBOLD;
	description.lfCharSet = DEFAULT_CHARSET;
	description.lfQuality = CLEARTYPE_QUALITY;
	description.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	const HFONT font = ::CreateFontIndirectW(&description);
	if (font == nullptr) return false;
	const int saved = ::SaveDC(dc);
	if (saved == 0) {
		::DeleteObject(font);
		return false;
	}
	::SelectObject(dc, font);
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, color);
	RECT bounds{ box.left, box.top, box.right, box.bottom };
	const wchar_t initial[] = { static_cast<wchar_t>(::towupper(label.front())), L'\0' };
	const int drawn = ::DrawTextW(dc, initial, 1, &bounds,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::RestoreDC(dc, saved);
	::DeleteObject(font);
	return drawn != 0;
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
		.border = RGB(0xCD, 0xD2, 0xDB),
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
		.border = windowText,
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
	RebuildTooltips();
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
	m_captureItem.clear();
	m_dragging = false;
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

void CActivityBar::SetEntries(std::vector<ActivityBarEntry> entries)
{
	// A container the user was dragging may not exist any more.
	m_model.SetEntries(std::move(entries));
	if (!m_captureItem.empty() && !m_model.Contains(m_captureItem)) {
		m_captureItem.clear();
		m_dragging = false;
		if (m_window != nullptr && ::GetCapture() == m_window) ::ReleaseCapture();
	}
	RebuildTooltips();
	Invalidate();
}

void CActivityBar::SetSelectedItem(std::string_view containerId) noexcept
{
	m_model.SetSelectedItem(containerId);
	Invalidate();
}

void CActivityBar::SetPressed(std::string_view containerId) noexcept
{
	m_model.SetPressedItem(containerId);
	Invalidate();
}

void CActivityBar::SetItemEnabled(std::string_view containerId, bool enabled) noexcept
{
	const auto index = m_model.IndexOf(containerId);
	if (index == ActivityBarModel::kNoIndex) return;
	const bool oldEnabled = m_model.IsEnabled(containerId);
	m_model.SetEnabled(containerId, enabled);
	if (oldEnabled != enabled) {
		accessibility::RaiseEnabledChanged(*this, static_cast<int>(index), oldEnabled, enabled);
	}
	Invalidate();
}

void CActivityBar::SetItemVisible(std::string_view containerId, bool visible) noexcept
{
	if (m_model.IsVisible(containerId) == visible) return;
	if (!visible && m_captureItem == containerId) {
		m_captureItem.clear();
		m_dragging = false;
		if (::GetCapture() == m_window) ::ReleaseCapture();
	}
	m_model.SetItemVisible(containerId, visible);
	// Every remaining entry moved, so the tooltip rectangles are stale.
	UpdateTooltipRects();
	Invalidate();
}

void CActivityBar::SetViewContainerBadge(std::string_view containerId, std::optional<int> count)
{
	const auto previous = m_model.GetViewContainerBadge(containerId);
	std::optional<activity::ActivityBarNumberBadge> badge;
	if (count && *count > 0) badge = activity::ActivityBarNumberBadge{ .number = *count };
	if (previous == badge) return;
	m_model.SetViewContainerBadge(containerId, badge);
	Invalidate();
}

bool CActivityBar::Invoke(std::string_view containerId) noexcept
{
	const auto requested = m_model.Invoke(containerId);
	if (requested.empty()) return false;
	accessibility::RaiseInvoked(*this, static_cast<int>(m_model.IndexOf(requested)));
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
		activityBar->m_captureItem.clear();
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
		if (m_model.GetFocusedItem().empty()) static_cast<void>(m_model.MoveFocus(1));
		if (const auto item = m_model.GetFocusedItem(); !item.empty()) {
			accessibility::RaiseFocusChanged(*this, static_cast<int>(m_model.IndexOf(item)));
		}
		Invalidate();
		return 0;
	case WM_KILLFOCUS:
		m_model.SetFocusedItem({});
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
		if (!m_captureItem.empty() && BeginDragIfPastThreshold(point)) {
			// A drag has started, so the press affordance must stop pretending a click
			// is still pending.
			m_model.SetPressedItem({});
			m_model.SetHoveredItem({});
			Invalidate();
			return 0;
		}
		SetHoverFromPoint(point);
		if (!m_captureItem.empty()) {
			m_model.SetPressedItem(m_model.HitTest(point.x, point.y) == m_captureItem ? m_captureItem : std::string_view{});
		}
		if (!m_trackingMouseLeave) {
			TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
			m_trackingMouseLeave = ::TrackMouseEvent(&tracking) != FALSE;
		}
		Invalidate();
		return 0;
	}
	case WM_MOUSELEAVE:
		m_trackingMouseLeave = false;
		m_model.SetHoveredItem({});
		if (m_captureItem.empty()) m_model.SetPressedItem({});
		Invalidate();
		return 0;
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const std::string item(m_model.HitTest(point.x, point.y));
		if (item.empty()) break;
		::SetFocus(m_window);
		m_model.SetFocusedItem(item);
		accessibility::RaiseFocusChanged(*this, static_cast<int>(m_model.IndexOf(item)));
		m_model.SetPressedItem(item);
		m_captureItem = item;
		m_dragOrigin = point;
		m_dragging = false;
		::SetCapture(m_window);
		Invalidate();
		return 0;
	}
	case WM_SETCURSOR:
		if (m_dragging) {
			::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
			return TRUE;
		}
		break;
	case WM_LBUTTONUP: {
		if (m_captureItem.empty()) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const std::string captured = std::move(m_captureItem);
		const bool dragged = m_dragging;
		m_captureItem.clear();
		m_dragging = false;
		m_model.SetPressedItem({});
		if (::GetCapture() == m_window) ::ReleaseCapture();
		if (dragged) {
			// A completed drag is a move gesture, never the toggle it started as.
			Invalidate();
			static_cast<void>(FinishDrag(captured, point));
			return 0;
		}
		SetHoverFromPoint(point);
		Invalidate();
		if (m_model.HitTest(point.x, point.y) != captured) return 0;
		accessibility::RaiseInvoked(*this, static_cast<int>(m_model.IndexOf(captured)));
		const auto index = m_model.IndexOf(captured);
		if (index != ActivityBarModel::kNoIndex && m_model.GetButton(index).IsGlobalAction()) {
			static_cast<void>(InvokeGlobalAction(captured));
		} else {
			static_cast<void>(InvokeRequest(captured));
		}
		return 0;
	}
	case WM_CAPTURECHANGED:
		m_captureItem.clear();
		m_dragging = false;
		m_model.SetPressedItem({});
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

void CActivityBar::RebuildTooltips()
{
	if (m_tooltip == nullptr || !::IsWindow(m_tooltip)) return;
	// Tools are addressed by slot, so shrinking the strip must delete the tail rather than
	// leave a tool pointing at a label whose container is gone.
	for (std::size_t index = 0; index < m_tooltipLabels.size(); ++index) {
		TOOLINFOW tool{};
		tool.cbSize = sizeof(tool);
		tool.hwnd = m_window;
		tool.uId = static_cast<UINT_PTR>(index + 1);
		::SendMessageW(m_tooltip, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
	}

	m_tooltipLabels.clear();
	m_tooltipLabels.reserve(m_model.GetButtonCount());
	for (std::size_t index = 0; index < m_model.GetButtonCount(); ++index) {
		m_tooltipLabels.emplace_back(m_model.GetButton(index).label);
	}
	::SendMessageW(m_tooltip, TTM_SETMAXTIPWIDTH, 0, ScaleDip(240, m_model.GetDpi()));
	for (std::size_t index = 0; index < m_tooltipLabels.size(); ++index) {
		TOOLINFOW tool{};
		tool.cbSize = sizeof(tool);
		tool.uFlags = TTF_SUBCLASS;
		tool.hwnd = m_window;
		tool.uId = static_cast<UINT_PTR>(index + 1);
		tool.lpszText = m_tooltipLabels[index].data();
		::SendMessageW(m_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
	}
	UpdateTooltipRects();
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
	const auto faceName = icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) return;
	LOGFONTW font{};
	font.lfHeight = -ScaleDip(icons::kActivityIconDip, m_model.GetDpi());
	font.lfWeight = FW_NORMAL;
	font.lfCharSet = DEFAULT_CHARSET;
	font.lfOutPrecision = OUT_TT_PRECIS;
	font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	font.lfQuality = CLEARTYPE_QUALITY;
	font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), font.lfFaceName);
	font.lfFaceName[faceName.size()] = L'\0';
	m_iconFont = ::CreateFontIndirectW(&font);
}

/*!
	@brief Draws one ViewContainer's activity indicator.

	Documented divergence (owner request, 2026-08-20): upstream's
	`compositeBarActions.css` renders a `NumberBadge` as a pill carrying the count,
	and this paints a plain dot at the glyph's bottom-right corner instead. The
	count still decides *whether* the indicator appears -- upstream's own
	"hide at zero or below" rule -- so nothing is faked here; only the count's
	digits are dropped from the presentation. The dot uses the same
	`activityBarBadge.background` role the pill would have used.
*/
void CActivityBar::PaintBadge(HDC dc, const RECT& bounds, int number) noexcept
{
	if (dc == nullptr || number <= 0) return;
	const unsigned int dpi = m_model.GetDpi();
	const int diameter = std::max(2, ScaleDip(kBadgeDotDiameterDip, dpi));
	RECT dot{ bounds.right - ScaleDip(kBadgeDotRightInsetDip, dpi) - diameter,
		bounds.top + ScaleDip(kBadgeDotTopInsetDip, dpi), 0, 0 };
	dot.right = dot.left + diameter;
	dot.bottom = dot.top + diameter;
	if (dot.left < bounds.left) dot.left = bounds.left;
	const HBRUSH fill = ::CreateSolidBrush(m_palette.badgeBackground);
	const HPEN pen = ::CreatePen(PS_SOLID, 1, m_palette.badgeBackground);
	if (fill != nullptr && pen != nullptr) {
		const HGDIOBJ previousBrush = ::SelectObject(dc, fill);
		const HGDIOBJ previousPen = ::SelectObject(dc, pen);
		// `Ellipse` excludes the right/bottom edge, so ask for one past the dot to
		// get the requested diameter rather than one pixel less.
		::Ellipse(dc, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
		::SelectObject(dc, previousPen);
		::SelectObject(dc, previousBrush);
	}
	if (pen != nullptr) ::DeleteObject(pen);
	if (fill != nullptr) ::DeleteObject(fill);
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
	EnsureIconFont();
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
		// VS Code renders all built-in Activity Bar glyphs from codicon.ttf. Mixing
		// a GDI path for some entries with a font glyph for others makes their
		// anti-aliasing and optical weight visibly inconsistent. The 20-DIP bounds
		// and normal font weight stay unchanged; the vector paths remain only as
		// the explicit fallback when the embedded font could not be registered.
		if (!PaintFontGlyph(buffer, iconBounds, m_iconFont, CodiconGlyph(button.codicon), iconColor)
			&& !PaintBuiltinGlyph(buffer, iconBounds, button.id, iconColor)) {
			static_cast<void>(PaintInitialTile(buffer, iconBounds, button.label, iconColor, m_model.GetDpi()));
		}
		// The badge sits over the glyph, as upstream's absolutely-positioned
		// `.badge-content` does, and under the focus ring drawn next.
		if (button.badge) PaintBadge(buffer, bounds, button.badge->number);
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
	// Match VS Code's `activityBar.border`: a one-DIP Part edge against the
	// Primary Side Bar (or the editor when that Side Bar is hidden).
	const int borderWidth = std::max(1, ScaleDip(1, m_model.GetDpi()));
	RECT borderRect{ client.right - borderWidth, client.top, client.right, client.bottom };
	if (borderRect.left < client.left) borderRect.left = client.left;
	if (borderRect.right > borderRect.left) {
		const HBRUSH borderBrush = ::CreateSolidBrush(m_palette.border);
		::FillRect(buffer, &borderRect, borderBrush);
		::DeleteObject(borderBrush);
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

bool CActivityBar::InvokeRequest(std::string_view containerId) noexcept
{
	if (containerId.empty() || !m_onToggleRequest) return false;
	try {
		// The owner answers a toggle by re-projecting the entry list, which replaces the
		// storage `containerId` may point into. Hand the callback a copy it cannot outlive.
		const std::string requested(containerId);
		m_onToggleRequest(requested);
		return true;
	} catch (...) {
		return false;
	}
}

bool CActivityBar::InvokeGlobalAction(std::string_view actionId) noexcept
{
	if (actionId.empty() || !m_onGlobalAction || m_window == nullptr) return false;
	const auto index = m_model.IndexOf(actionId);
	if (index == ActivityBarModel::kNoIndex) return false;
	const auto bounds = m_model.GetButton(index).bounds;
	// Open to the right of the Activity Bar icon (vertical bar), matching VS Code's
	// GlobalCompositeBar popup alignment rather than the title-bar "below" placement.
	POINT screen{ bounds.right, bounds.top };
	if (::ClientToScreen(m_window, &screen) == FALSE) return false;
	try {
		const std::string requested(actionId);
		m_onGlobalAction(requested, screen);
		return true;
	} catch (...) {
		return false;
	}
}

bool CActivityBar::BeginDragIfPastThreshold(POINT point) noexcept
{
	if (m_dragging) return true;
	if (m_captureItem.empty() || !m_onContainerDrag || !m_model.IsDraggable(m_captureItem)) return false;
	const int dragX = std::max(1, ::GetSystemMetrics(SM_CXDRAG));
	const int dragY = std::max(1, ::GetSystemMetrics(SM_CYDRAG));
	if (std::abs(point.x - m_dragOrigin.x) < dragX && std::abs(point.y - m_dragOrigin.y) < dragY) return false;
	m_dragging = true;
	::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
	return true;
}

bool CActivityBar::FinishDrag(std::string_view containerId, POINT clientPoint) noexcept
{
	if (containerId.empty() || !m_onContainerDrag || m_window == nullptr) return false;
	POINT screenPoint = clientPoint;
	if (::ClientToScreen(m_window, &screenPoint) == FALSE) return false;
	try {
		// Same lifetime rule as InvokeRequest: a drop can move the container to another
		// part, and the resulting re-projection invalidates views into the entry list.
		const std::string dragged(containerId);
		m_onContainerDrag(dragged, screenPoint);
		return true;
	} catch (...) {
		return false;
	}
}

bool CActivityBar::HandleNavigationKey(WPARAM key) noexcept
{
	std::string invoked;
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
		// Home and End mean first and last rendered entry, not a particular container.
		static_cast<void>(m_model.FocusEdge(1));
		focusChanged = true;
		break;
	case VK_END:
		static_cast<void>(m_model.FocusEdge(-1));
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
		const auto focused = m_model.GetFocusedItem();
		if (!focused.empty()) accessibility::RaiseFocusChanged(*this, static_cast<int>(m_model.IndexOf(focused)));
	}
	if (!invoked.empty()) accessibility::RaiseInvoked(*this, static_cast<int>(m_model.IndexOf(invoked)));
	const auto index = m_model.IndexOf(invoked);
	if (index != ActivityBarModel::kNoIndex && m_model.GetButton(index).IsGlobalAction()) {
		static_cast<void>(InvokeGlobalAction(invoked));
	} else {
		static_cast<void>(InvokeRequest(invoked));
	}
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
	return {
		nodeId,
		std::wstring(button.label),
		// The automation id must stay keyed to the container id: a UI test that targets
		// "Sakura.ActivityBar.workbench.view.explorer" must not start matching a different
		// button because an extension inserted itself above Explorer.
		std::wstring(L"Sakura.ActivityBar.") + u8stowcs(button.id),
		UIA_ButtonControlTypeId,
		{ button.bounds.left, button.bounds.top, button.bounds.right, button.bounds.bottom },
		button.enabled,
		button.focused,
		true,
	};
}

int CActivityBar::AccessibilityFocusedNode() const noexcept
{
	const auto index = m_model.IndexOf(m_model.GetFocusedItem());
	return index == ActivityBarModel::kNoIndex ? -1 : static_cast<int>(index);
}

bool CActivityBar::AccessibilityInvoke(int nodeId) noexcept
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetButtonCount())) return false;
	return Invoke(m_model.IdAt(static_cast<std::size_t>(nodeId)));
}

void CActivityBar::AccessibilitySetFocus(int nodeId) noexcept
{
	if (nodeId < 0 || nodeId >= static_cast<int>(m_model.GetButtonCount())) return;
	m_model.SetFocusedItem(m_model.IdAt(static_cast<std::size_t>(nodeId)));
	Invalidate();
}

void CActivityBar::SetHoverFromPoint(POINT point) noexcept
{
	m_model.SetHoveredItem(m_model.HitTest(point.x, point.y));
}

} // namespace workbench
