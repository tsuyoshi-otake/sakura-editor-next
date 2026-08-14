/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/CWorkbenchBannerHost.h"

#include "workbench/WorkbenchBannerLayout.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/LabelRunPainter.h"
#include "workbench/icons/ThemeIconResolver.h"

#include <windowsx.h>

#include <algorithm>

namespace workbench {
namespace {

constexpr wchar_t kBannerHostClass[] = L"SakuraWorkbenchBannerHost";
constexpr int kDefaultDpi = 96;
//! Must match `WorkbenchBannerLayout.cpp`'s private `kIconSideDip`. That
//! constant is not exported -- the layout header only exposes measured pixel
//! widths, never DIP tokens -- so this side has to agree with it by convention
//! rather than by a shared symbol. Every other DPI-scaling helper in this
//! codebase is likewise duplicated per translation unit (see root CLAUDE.md).
constexpr int kIconSideDip = 16;

[[nodiscard]] int ScaleDip(int dip, unsigned int dpi) noexcept
{
	const int effective = dpi == 0 ? kDefaultDpi : static_cast<int>(dpi);
	return std::max(0, (dip * effective + kDefaultDpi / 2) / kDefaultDpi);
}

[[nodiscard]] RECT ToRect(const WorkbenchRect& rect) noexcept
{
	return RECT{ rect.left, rect.top, rect.right, rect.bottom };
}

[[nodiscard]] bool EnsureWindowClass(HINSTANCE instance)
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
	windowClass.lpfnWndProc = CWorkbenchBannerHost::WindowProc;
	windowClass.lpszClassName = kBannerHostClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

} // namespace

CWorkbenchBannerHost::~CWorkbenchBannerHost()
{
	Close();
}

bool CWorkbenchBannerHost::Create(HWND parent, HINSTANCE instance)
{
	if (m_closed || m_window != nullptr || parent == nullptr || instance == nullptr) return false;
	if (!EnsureWindowClass(instance)) return false;
	m_window = ::CreateWindowExW(0, kBannerHostClass, L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	return m_window != nullptr;
}

void CWorkbenchBannerHost::SetContent(std::wstring message, std::vector<WorkbenchBannerAction> actions)
{
	// Message and actions are replaced together (see the header comment on
	// this method): a stale hot/pressed index pointing at an action that no
	// longer exists would either draw an underline on the wrong text or, worse,
	// invoke the wrong command on the next mouse-up.
	m_message = std::move(message);
	m_actions = std::move(actions);
	m_hotAction = -1;
	m_pressedAction = -1;
	if (m_window != nullptr) {
		RecalculateLayout();
		::InvalidateRect(m_window, nullptr, TRUE);
	}
}

void CWorkbenchBannerHost::SetPalette(const theme::ThemePalette& palette)
{
	m_palette = palette;
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, TRUE);
}

int CWorkbenchBannerHost::PreferredHeightPixels(unsigned int dpi)
{
	// Empty content reserves nothing: a caller that always asked this strip
	// for a height, even with no message and no actions, would otherwise get
	// VS Code's kMinimumHeightDip floor -- CalculateWorkbenchBannerLayout has
	// no notion of "there is nothing to show", only "the strip has no room".
	if (m_message.empty() && m_actions.empty()) return 0;

	// This intentionally updates m_dpi/m_font as a side effect. The header
	// documents this as the value CEditWnd feeds into the layout request
	// *before* Layout() runs, so the normal caller sequence is "ask the
	// preferred height, then call Layout with that same dpi" -- by the time
	// Layout() arrives, EnsureFont() below has already done its work and
	// Layout()'s own EnsureFont() call is a no-op.
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	EnsureFont();

	const HDC dc = m_window != nullptr ? ::GetDC(m_window) : ::GetDC(nullptr);
	const HGDIOBJ previousFont = (dc != nullptr && m_font.Get() != nullptr) ? ::SelectObject(dc, m_font.Get()) : nullptr;
	int textHeight = 0;
	if (dc != nullptr) {
		TEXTMETRICW metrics{};
		if (::GetTextMetricsW(dc, &metrics) != FALSE) textHeight = static_cast<int>(metrics.tmHeight);
		if (previousFont != nullptr) ::SelectObject(dc, previousFont);
		if (m_window != nullptr) ::ReleaseDC(m_window, dc); else ::ReleaseDC(nullptr, dc);
	}

	// The pure layout's height is a function of dpi and text height alone --
	// width only affects how the content is arranged inside that height -- so
	// a zero-width probe input still returns the real answer.
	WorkbenchBannerLayoutInput input;
	input.dpi = m_dpi;
	input.textHeightPixels = textHeight;
	return CalculateWorkbenchBannerLayout(input).height;
}

void CWorkbenchBannerHost::Layout(const RECT& bounds, unsigned int dpi)
{
	if (m_closed) return;
	m_bounds = bounds;
	m_dpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_window != nullptr) {
		// SWP_NOCOPYBITS: the default bit-copy smears the old client content
		// across the moved rectangle before WM_PAINT arrives (Issue #17,
		// documented in the root CLAUDE.md's stale-pixel section).
		::SetWindowPos(m_window, nullptr, bounds.left, bounds.top,
			std::max(0L, bounds.right - bounds.left), std::max(0L, bounds.bottom - bounds.top),
			SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	}
	RecalculateLayout();
}

void CWorkbenchBannerHost::Show()
{
	if (m_closed || m_window == nullptr || m_visible) return;
	m_visible = true;
	::ShowWindow(m_window, SW_SHOWNA);
}

void CWorkbenchBannerHost::Hide()
{
	if (m_closed || m_window == nullptr || !m_visible) return;
	m_visible = false;
	::ShowWindow(m_window, SW_HIDE);
}

void CWorkbenchBannerHost::Close()
{
	if (m_closed) return;
	m_closed = true;
	m_visible = false;
	if (::GetCapture() == m_window) ::ReleaseCapture();
	if (m_window != nullptr) {
		::DestroyWindow(m_window);
		m_window = nullptr;
	}
}

void CWorkbenchBannerHost::EnsureFont()
{
	if (m_font.Dpi() != m_dpi) {
		(void)m_font.Recreate(theme::ThemeFontKind::Chrome, m_dpi);
	}
}

void CWorkbenchBannerHost::RecalculateLayout()
{
	EnsureFont();

	const HDC dc = m_window != nullptr ? ::GetDC(m_window) : ::GetDC(nullptr);
	const HGDIOBJ previousFont = (dc != nullptr && m_font.Get() != nullptr) ? ::SelectObject(dc, m_font.Get()) : nullptr;

	int textHeight = 0;
	if (dc != nullptr) {
		TEXTMETRICW metrics{};
		if (::GetTextMetricsW(dc, &metrics) != FALSE) textHeight = static_cast<int>(metrics.tmHeight);
	}

	const std::wstring_view faceName = icons::CCodiconFont::Instance().FaceName();
	const int iconSide = ScaleDip(kIconSideDip, m_dpi);

	WorkbenchBannerLayoutInput input;
	input.widthPixels = static_cast<int>(m_bounds.right - m_bounds.left);
	input.dpi = m_dpi;
	input.textHeightPixels = textHeight;

	if (dc != nullptr) {
		// Against a null contributed-icon registry: the banner is chrome, not
		// an extension surface, and its message/action strings are either
		// this product's own literals or a `security.workspace.trust.banner`
		// contribution's plain text -- there is no per-extension icon
		// namespace to resolve here, only the built-in codicon vocabulary.
		const auto messageRuns = icons::ParseLabelWithIcons(m_message, faceName);
		input.messageWidthPixels = icons::MeasureLabelRuns(dc, messageRuns, iconSide);
		input.actionWidthPixels.reserve(m_actions.size());
		for (const auto& action : m_actions) {
			const auto actionRuns = icons::ParseLabelWithIcons(action.label, faceName);
			input.actionWidthPixels.push_back(icons::MeasureLabelRuns(dc, actionRuns, iconSide));
		}
	} else {
		input.actionWidthPixels.assign(m_actions.size(), 0);
	}

	if (previousFont != nullptr) ::SelectObject(dc, previousFont);
	if (dc != nullptr) { if (m_window != nullptr) ::ReleaseDC(m_window, dc); else ::ReleaseDC(nullptr, dc); }

	m_layout = CalculateWorkbenchBannerLayout(input);
}

void CWorkbenchBannerHost::InvokeAction(int index)
{
	// Out-of-range indices are ignored rather than asserted: SetContent()
	// resets both indices, but a click message already queued for a
	// just-replaced action can still arrive after that reset.
	if (index < 0 || static_cast<std::size_t>(index) >= m_actions.size()) return;
	const WorkbenchBannerAction& action = m_actions[static_cast<std::size_t>(index)];
	switch (action.kind) {
	case EWorkbenchBannerActionKind::Command:
		if (m_executeCommand) m_executeCommand(action.commandId);
		break;
	case EWorkbenchBannerActionKind::Dismiss:
		if (m_dismiss) m_dismiss();
		break;
	}
}

void CWorkbenchBannerHost::SetHotAction(int index)
{
	if (index == m_hotAction) return;
	m_hotAction = index;
	if (m_window != nullptr) ::InvalidateRect(m_window, nullptr, FALSE);
}

void CWorkbenchBannerHost::Paint()
{
	PAINTSTRUCT paint{};
	const HDC dc = ::BeginPaint(m_window, &paint);
	if (dc == nullptr) return;

	RECT client{};
	::GetClientRect(m_window, &client);
	const int width = client.right - client.left;
	const int height = client.bottom - client.top;

	// Double-buffered per the root CLAUDE.md stale-pixel guidance, following
	// CMainStatusBar::PaintStatusBar's shape: painting straight to `dc` risks
	// one composited frame showing the previous frame's bits during a resize
	// or content swap (the Issue #17 defect class).
	const HDC buffer = ::CreateCompatibleDC(dc);
	const HBITMAP bitmap = buffer == nullptr ? nullptr
		: ::CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height));
	const HGDIOBJ oldBitmap = bitmap == nullptr ? nullptr : ::SelectObject(buffer, bitmap);
	const HDC target = oldBitmap == nullptr ? dc : buffer;

	const HBRUSH background = ::CreateSolidBrush(m_palette.bannerBackground.ToColorRef());
	::FillRect(target, &client, background);
	::DeleteObject(background);

	::SetBkMode(target, TRANSPARENT);
	const HGDIOBJ previousFont = m_font.Get() != nullptr ? ::SelectObject(target, m_font.Get()) : nullptr;

	const std::wstring_view faceName = icons::CCodiconFont::Instance().FaceName();
	const int iconSide = ScaleDip(kIconSideDip, m_dpi);
	const icons::SLabelRunFontProvider glyphFonts = icons::OwnedGlyphFontProvider();
	const COLORREF foreground = m_palette.bannerForeground.ToColorRef();

	if (m_layout.message.Width() > 0 && m_layout.message.Height() > 0) {
		const auto runs = icons::ParseLabelWithIcons(m_message, faceName);
		icons::DrawLabelRuns(target, runs, ToRect(m_layout.message), iconSide, foreground, glyphFonts);
	}

	for (std::size_t index = 0; index < m_actions.size() && index < m_layout.actions.size(); ++index) {
		const WorkbenchRect& actionRect = m_layout.actions[index];
		if (actionRect.Width() <= 0 || actionRect.Height() <= 0) continue;
		const RECT rect = ToRect(actionRect);
		const auto runs = icons::ParseLabelWithIcons(m_actions[index].label, faceName);
		icons::DrawLabelRuns(target, runs, rect, iconSide, foreground, glyphFonts);

		// VS Code's `.monaco-banner-actions .monaco-link:hover` rule adds only
		// `text-decoration: underline`, with no color change -- and
		// ThemePalette publishes no bannerHover* role at all (see
		// theme/CLAUDE.md's Banner Part color roles). A manual underline
		// under the hot action reproduces that exactly, instead of inventing
		// a hover color no theme could actually change.
		if (static_cast<int>(index) == m_hotAction) {
			const int lineY = rect.bottom - 1;
			const HPEN pen = ::CreatePen(PS_SOLID, 1, foreground);
			const HGDIOBJ previousPen = ::SelectObject(target, pen);
			::MoveToEx(target, rect.left, lineY, nullptr);
			::LineTo(target, rect.right, lineY);
			::SelectObject(target, previousPen);
			::DeleteObject(pen);
		}
	}

	if (previousFont != nullptr) ::SelectObject(target, previousFont);

	if (oldBitmap != nullptr) {
		::BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
		::SelectObject(buffer, oldBitmap);
	}
	if (bitmap != nullptr) ::DeleteObject(bitmap);
	if (buffer != nullptr) ::DeleteDC(buffer);

	::EndPaint(m_window, &paint);
}

LRESULT CALLBACK CWorkbenchBannerHost::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* host = static_cast<CWorkbenchBannerHost*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
		if (host != nullptr) host->m_window = window;
	}
	auto* host = reinterpret_cast<CWorkbenchBannerHost*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
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

LRESULT CWorkbenchBannerHost::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		Paint();
		return 0;
	case WM_MOUSEMOVE: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		SetHotAction(WorkbenchBannerActionAtPoint(m_layout, point.x, point.y));
		if (!m_trackingMouseLeave) {
			TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, m_window, 0 };
			m_trackingMouseLeave = ::TrackMouseEvent(&track) != FALSE;
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		m_trackingMouseLeave = false;
		SetHotAction(-1);
		return 0;
	case WM_SETCURSOR: {
		if (reinterpret_cast<HWND>(wParam) != m_window) break;
		POINT point{};
		if (!::GetCursorPos(&point) || !::ScreenToClient(m_window, &point)) break;
		const bool overAction = WorkbenchBannerActionAtPoint(m_layout, point.x, point.y) >= 0;
		::SetCursor(::LoadCursorW(nullptr, overAction ? IDC_HAND : IDC_ARROW));
		return TRUE;
	}
	case WM_LBUTTONDOWN: {
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const int index = WorkbenchBannerActionAtPoint(m_layout, point.x, point.y);
		if (index < 0) break;
		m_pressedAction = index;
		::SetCapture(m_window);
		return 0;
	}
	case WM_LBUTTONUP: {
		if (m_pressedAction < 0) break;
		const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		const int pressed = m_pressedAction;
		m_pressedAction = -1;
		if (::GetCapture() == m_window) ::ReleaseCapture();
		// Fires only when the release lands on the same action the press
		// started on -- a press that drags off the link before release must
		// not fire it (see the header comment on m_pressedAction).
		if (WorkbenchBannerActionAtPoint(m_layout, point.x, point.y) == pressed) InvokeAction(pressed);
		return 0;
	}
	case WM_CAPTURECHANGED:
		m_pressedAction = -1;
		return 0;
	default:
		break;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

} // namespace workbench
