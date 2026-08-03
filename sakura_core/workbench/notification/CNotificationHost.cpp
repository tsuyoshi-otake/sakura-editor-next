/*! @file
    @brief VS Code互換の非モーダル通知Toastを表示するWin32実装
*/
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/notification/CNotificationHost.h"
#include "workbench/IconMetrics.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace workbench::notification {
namespace {

constexpr wchar_t kWindowClassName[] = L"SakuraEditor.Next.NotificationHost";

ULONGLONG TimeoutFor(EExtensionNotificationSeverity severity) noexcept
{
	switch (severity) {
	case EExtensionNotificationSeverity::Warning:
		return 12'000;
	case EExtensionNotificationSeverity::Error:
		return 15'000;
	case EExtensionNotificationSeverity::Information:
	default:
		return 10'000;
	}
}

bool IsSticky(const SExtensionNotification& notification) noexcept
{
	// VS Code keeps errors and notifications with primary actions visible until
	// the user explicitly closes them or chooses an action. A button prompt must
	// never disappear while it is still waiting for that decision.
	return notification.severity == EExtensionNotificationSeverity::Error
		|| !notification.actions.empty();
}

int SafeWidth(int width) noexcept
{
	return (std::max)(80, width);
}

struct SeverityIconSpec final {
	workbench::icons::codicons::Icon icon;
	std::wstring_view glyphName;
};

[[nodiscard]] SeverityIconSpec SeverityIconFor(EExtensionNotificationSeverity severity) noexcept
{
	switch (severity) {
	case EExtensionNotificationSeverity::Warning:
		return { workbench::icons::codicons::Icon::Warning, L"warning" };
	case EExtensionNotificationSeverity::Error:
		return { workbench::icons::codicons::Icon::Error, L"error" };
	case EExtensionNotificationSeverity::Information:
	default:
		return { workbench::icons::codicons::Icon::Info, L"info" };
	}
}

[[nodiscard]] bool PaintFontGlyph(
	HDC dc,
	const workbench::icons::IconRect& box,
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

} // namespace

CNotificationHost::~CNotificationHost() noexcept
{
	Destroy();
}

ATOM CNotificationHost::RegisterWindowClass() noexcept
{
	static const ATOM atom = []() noexcept {
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.style = CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc = &CNotificationHost::WindowProc;
		windowClass.hInstance = ::GetModuleHandleW(nullptr);
		windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
		windowClass.lpszClassName = kWindowClassName;
		const ATOM registered = ::RegisterClassExW(&windowClass);
		if (registered != 0) return registered;
		if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) return static_cast<ATOM>(1);
		return static_cast<ATOM>(0);
	}();
	return atom;
}

bool CNotificationHost::Create(HWND owner) noexcept
{
	if (m_hwnd != nullptr) return m_owner == owner;
	if (owner == nullptr || ::IsWindow(owner) == FALSE) {
		return false;
	}
	const ATOM windowClass = RegisterWindowClass();
	if (windowClass == 0) {
		return false;
	}
	m_owner = owner;
	m_hwnd = ::CreateWindowExW(
		WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		kWindowClassName,
		L"",
		WS_POPUP,
		0, 0, 0, 0,
		owner,
		nullptr,
		::GetModuleHandleW(nullptr),
		this);
	if (m_hwnd == nullptr) {
		m_owner = nullptr;
		return false;
	}
	return true;
}

void CNotificationHost::Destroy() noexcept
{
	if (m_hwnd != nullptr) {
		::KillTimer(m_hwnd, kTimerId);
		::DestroyWindow(m_hwnd);
	}
	m_hwnd = nullptr;
	m_owner = nullptr;
	m_layouts.clear();
	m_notifications.clear();
	m_toasts.clear();
	m_knownNotificationIds.clear();
	m_hiddenToastIds.clear();
	m_unreadNotificationIds.clear();
	m_centerClose = {};
	m_centerContentHeight = 0;
	m_centerViewportHeight = 0;
	m_centerScrollOffset = 0;
	m_keyboardTarget.reset();
	m_centerVisible = false;
	m_trackingMouse = false;
	m_lastTimerTick = 0;
	m_font.Reset();
	ReleaseCodiconFont();
}

void CNotificationHost::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (m_hwnd != nullptr) ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CNotificationHost::SetResolveCallback(ResolveCallback callback)
{
	m_resolveCallback = std::move(callback);
}

void CNotificationHost::SetStatusChangedCallback(StatusChangedCallback callback)
{
	m_statusChangedCallback = std::move(callback);
	NotifyStatusChanged();
}

CNotificationHost::StatusSnapshot CNotificationHost::Status() const noexcept
{
	return { m_notifications.size(), m_unreadNotificationIds.size(), m_centerVisible };
}

void CNotificationHost::NotifyStatusChanged() noexcept
{
	if (!m_statusChangedCallback) return;
	try {
		m_statusChangedCallback(Status());
	} catch (...) {
		// Presentation observers must never break notification finalization.
	}
}

void CNotificationHost::SetNotifications(std::vector<SExtensionNotification> notifications)
{
	const auto now = ::GetTickCount64();
	std::set<std::uint64_t> currentIds;
	std::vector<SExtensionNotification> pending;
	pending.reserve(notifications.size());
	for (auto& notification : notifications) {
		if (notification.modal || notification.state != EExtensionNotificationState::Pending) continue;
		currentIds.insert(notification.id);
		if (!m_knownNotificationIds.contains(notification.id) && !m_centerVisible) {
			m_unreadNotificationIds.insert(notification.id);
		}
		pending.emplace_back(std::move(notification));
	}
	std::erase_if(m_hiddenToastIds, [&currentIds](std::uint64_t id) { return !currentIds.contains(id); });
	std::erase_if(m_unreadNotificationIds, [&currentIds](std::uint64_t id) { return !currentIds.contains(id); });
	m_knownNotificationIds = std::move(currentIds);
	m_notifications = std::move(pending);
	if (m_centerVisible) m_unreadNotificationIds.clear();
	RebuildPresentation(now);
	NotifyStatusChanged();
	if (m_hwnd == nullptr) return;
	Layout();
}

void CNotificationHost::RebuildPresentation(ULONGLONG now)
{
	std::vector<ToastState> old = std::move(m_toasts);
	std::vector<ToastState> next;
	next.reserve(m_centerVisible ? m_notifications.size() : (std::min)(kMaximumToasts, m_notifications.size()));
	// The model retains insertion order, while VS Code presents the newest toast first.
	for (auto it = m_notifications.rbegin(); it != m_notifications.rend(); ++it) {
		if (!m_centerVisible && (next.size() >= kMaximumToasts || m_hiddenToastIds.contains(it->id))) continue;
		ToastState state;
		const auto previous = std::find_if(old.begin(), old.end(), [id = it->id](const ToastState& value) {
			return value.notification.id == id;
		});
		if (previous != old.end()) {
			state = std::move(*previous);
			state.notification = *it;
		} else {
			state.notification = *it;
			state.deadline = m_centerVisible || IsSticky(*it) ? 0 : now + TimeoutFor(it->severity);
		}
		if (m_centerVisible || IsSticky(*it)) state.deadline = 0;
		next.emplace_back(std::move(state));
	}
	m_toasts = std::move(next);
}

void CNotificationHost::ShowCenter() noexcept
{
	if (m_centerVisible) return;
	m_centerVisible = true;
	m_unreadNotificationIds.clear();
	m_centerScrollOffset = 0;
	m_keyboardTarget = 0;
	RebuildPresentation(::GetTickCount64());
	NotifyStatusChanged();
	Layout();
	if (m_hwnd != nullptr) {
		::ShowWindow(m_hwnd, SW_SHOWNORMAL);
		::SetForegroundWindow(m_hwnd);
		::SetFocus(m_hwnd);
	}
}

void CNotificationHost::HideCenter() noexcept
{
	if (!m_centerVisible) return;
	for (const auto& notification : m_notifications) m_hiddenToastIds.insert(notification.id);
	m_centerVisible = false;
	m_centerScrollOffset = 0;
	m_keyboardTarget.reset();
	RebuildPresentation(::GetTickCount64());
	NotifyStatusChanged();
	Layout();
}

void CNotificationHost::ToggleCenter() noexcept
{
	if (m_centerVisible) HideCenter();
	else ShowCenter();
}

int CNotificationHost::Scale(int value, UINT dpi) const noexcept
{
	return (std::max)(1, ::MulDiv(value, static_cast<int>(dpi), 96));
}

int CNotificationHost::LineHeight(HDC dc) const noexcept
{
	TEXTMETRICW metrics{};
	if (dc != nullptr && ::GetTextMetricsW(dc, &metrics) != FALSE) {
		return (std::max)(16, static_cast<int>(metrics.tmHeight + 2));
	}
	return 18;
}

int CNotificationHost::WrappedTextHeight(
	HDC dc, std::wstring_view text, int width, int maximumHeight, int lineHeight) const noexcept
{
	if (dc == nullptr || text.empty()) return 0;
	const std::wstring value(text);
	RECT measured{ 0, 0, SafeWidth(width), 0 };
	::DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &measured,
		DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
	return (std::clamp)(static_cast<int>(measured.bottom - measured.top), lineHeight, maximumHeight);
}

COLORREF CNotificationHost::SeverityColor(EExtensionNotificationSeverity severity) const noexcept
{
	switch (severity) {
	case EExtensionNotificationSeverity::Warning:
		return m_palette.warning.ToColorRef();
	case EExtensionNotificationSeverity::Error:
		return m_palette.danger.ToColorRef();
	case EExtensionNotificationSeverity::Information:
	default:
		return m_palette.accent.ToColorRef();
	}
}

HFONT CNotificationHost::AcquireCodiconFont(int height) noexcept
{
	if (height <= 0) return nullptr;
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) {
		ReleaseCodiconFont();
		return nullptr;
	}
	if (m_codiconFont != nullptr && m_codiconFontHeight == height) return m_codiconFont;

	ReleaseCodiconFont();
	LOGFONTW logFont{};
	logFont.lfHeight = -height;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
	logFont.lfFaceName[faceName.size()] = L'\0';
	m_codiconFont = ::CreateFontIndirectW(&logFont);
	if (m_codiconFont != nullptr) m_codiconFontHeight = height;
	return m_codiconFont;
}

void CNotificationHost::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

std::wstring CNotificationHost::SourceText(std::wstring_view extensionId)
{
	if (extensionId.empty()) return {};
	return L"Source: " + std::wstring(extensionId);
}

void CNotificationHost::RebuildLayout(HDC dc, int width, UINT dpi) noexcept
{
	m_layouts.clear();
	const int padding = Scale(12, dpi);
	const int iconSize = Scale(20, dpi);
	const int gap = Scale(8, dpi);
	const int closeSize = Scale(18, dpi);
	const int headerHeight = m_centerVisible ? Scale(36, dpi) : 0;
	m_centerClose = m_centerVisible
		? RECT{ width - padding - closeSize, (headerHeight - closeSize) / 2,
			width - padding, (headerHeight - closeSize) / 2 + closeSize }
		: RECT{};
	if (m_toasts.empty()) {
		m_centerContentHeight = 0;
		return;
	}
	const int actionHeight = Scale(26, dpi);
	const int lineHeight = LineHeight(dc);
	const int textLeft = padding + iconSize + gap;
	const int textRight = (std::max)(textLeft + lineHeight * 4, width - padding - closeSize - gap);
	const int textWidth = (std::max)(80, textRight - textLeft);
	const int buttonGap = Scale(6, dpi);
	int top = headerHeight - (m_centerVisible ? m_centerScrollOffset : 0);
	const int contentStart = top;

	for (const auto& toast : m_toasts) {
		ToastLayout layout;
		layout.notification = toast.notification;
		int cursor = padding;
		layout.message = { textLeft, cursor, textRight, cursor +
			WrappedTextHeight(dc, toast.notification.message, textWidth, lineHeight * 3, lineHeight) };
		cursor = layout.message.bottom;

		if (!toast.notification.detail.empty()) {
			cursor += gap;
			layout.detail = { textLeft, cursor, textRight, cursor +
				WrappedTextHeight(dc, toast.notification.detail, textWidth, lineHeight * 3, lineHeight) };
			cursor = layout.detail.bottom;
		}

		const auto source = SourceText(toast.notification.extensionId);
		if (!source.empty()) {
			cursor += gap;
			layout.source = { textLeft, cursor, textRight, cursor + lineHeight };
			cursor = layout.source.bottom;
		}

		if (!toast.notification.actions.empty()) {
			cursor += gap;
			int buttonLeft = padding;
			int buttonTop = cursor;
			const std::size_t actionCount = (std::min<std::size_t>)(toast.notification.actions.size(), 4);
			for (std::size_t index = 0; index < actionCount; ++index) {
				SIZE extent{};
				const auto& title = toast.notification.actions[index];
				::GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &extent);
				const int buttonWidth = (std::clamp)(static_cast<int>(extent.cx) + padding * 2,
					Scale(64, dpi), width - padding * 2);
				if (buttonLeft != padding && buttonLeft + buttonWidth > width - padding) {
					buttonLeft = padding;
					buttonTop += actionHeight + buttonGap;
				}
				layout.actions.push_back({ buttonLeft, buttonTop, buttonLeft + buttonWidth, buttonTop + actionHeight });
				buttonLeft += buttonWidth + buttonGap;
			}
			cursor = buttonTop + actionHeight;
		}

		const int height = cursor + padding;
		layout.bounds = { 0, top, width, top + height };
		layout.close = { width - padding - closeSize, padding / 2, width - padding, padding / 2 + closeSize };
		const auto offsetContentRect = [top](RECT& rect) noexcept {
			if (rect.right > rect.left && rect.bottom > rect.top) {
				::OffsetRect(&rect, 0, top);
			}
		};
		offsetContentRect(layout.message);
		offsetContentRect(layout.detail);
		offsetContentRect(layout.source);
		offsetContentRect(layout.close);
		for (auto& action : layout.actions) offsetContentRect(action);
		m_layouts.emplace_back(std::move(layout));
		top += height + gap;
	}
	m_centerContentHeight = (std::max)(0, top - contentStart - gap);
}

void CNotificationHost::Layout() noexcept
{
	if (m_hwnd == nullptr) return;
	if ((!m_centerVisible && m_toasts.empty()) || m_owner == nullptr || ::IsWindow(m_owner) == FALSE ||
		::IsWindowVisible(m_owner) == FALSE || ::IsIconic(m_owner) != FALSE) {
		::KillTimer(m_hwnd, kTimerId);
		m_lastTimerTick = 0;
		::ShowWindow(m_hwnd, SW_HIDE);
		return;
	}

	RECT client{};
	if (::GetClientRect(m_owner, &client) == FALSE) return;
	const UINT dpi = (std::max)(96u, ::GetDpiForWindow(m_owner));
	const int clientWidth = client.right - client.left;
	const int width = (std::min)(Scale(450, dpi), (std::max)(Scale(220, dpi), clientWidth - Scale(16, dpi)));
	HDC dc = ::GetDC(m_hwnd);
	if (dc == nullptr) return;
	(void)m_font.RecreateForWindow(theme::ThemeFontKind::Chrome, m_hwnd);
	const HGDIOBJ oldFont = m_font.Get() != nullptr ? ::SelectObject(dc, m_font.Get()) : nullptr;
	RebuildLayout(dc, width, dpi);
	const int headerHeight = m_centerVisible ? Scale(36, dpi) : 0;
	const int emptyHeight = m_centerVisible && m_layouts.empty() ? Scale(72, dpi) : 0;
	const int maximumCenterHeight = (std::max)(Scale(120, dpi),
		static_cast<int>(client.bottom - client.top) - Scale(48, dpi));
	int height = m_centerVisible
		? (std::min)(maximumCenterHeight, headerHeight + (std::max)(m_centerContentHeight, emptyHeight))
		: (m_layouts.empty() ? 0 : static_cast<int>(m_layouts.back().bounds.bottom));
	if (m_centerVisible) {
		m_centerViewportHeight = (std::max)(0, height - headerHeight);
		const int maximumScroll = (std::max)(0, m_centerContentHeight - m_centerViewportHeight);
		const int clampedScroll = (std::clamp)(m_centerScrollOffset, 0, maximumScroll);
		if (clampedScroll != m_centerScrollOffset) {
			m_centerScrollOffset = clampedScroll;
			RebuildLayout(dc, width, dpi);
		}
	} else {
		m_centerViewportHeight = 0;
	}
	if (oldFont != nullptr) ::SelectObject(dc, oldFont);
	::ReleaseDC(m_hwnd, dc);
	if (height <= 0) return;
	const int bottomMargin = Scale(34, dpi);
	POINT bottomRight{ client.right, client.bottom };
	::ClientToScreen(m_owner, &bottomRight);
	int x = bottomRight.x - width - Scale(12, dpi);
	int y = bottomRight.y - bottomMargin - height;
	MONITORINFO monitor{ sizeof(monitor) };
	if (const HMONITOR handle = ::MonitorFromWindow(m_owner, MONITOR_DEFAULTTONEAREST);
		handle != nullptr && ::GetMonitorInfoW(handle, &monitor) != FALSE) {
		x = (std::max)(static_cast<int>(monitor.rcWork.left) + Scale(8, dpi), x);
		y = (std::max)(static_cast<int>(monitor.rcWork.top) + Scale(8, dpi), y);
	}
	const UINT positionFlags = (m_centerVisible ? 0 : SWP_NOACTIVATE) | SWP_SHOWWINDOW;
	::SetWindowPos(m_hwnd, HWND_TOP, x, y, width, height, positionFlags);
	if (::IsWindowVisible(m_hwnd) == FALSE) {
		::ShowWindow(m_hwnd, m_centerVisible ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE);
	}
	if (m_centerVisible) {
		::KillTimer(m_hwnd, kTimerId);
		m_lastTimerTick = 0;
	} else {
		if (m_lastTimerTick == 0) m_lastTimerTick = ::GetTickCount64();
		::SetTimer(m_hwnd, kTimerId, kTimerPeriodMs, nullptr);
	}
	::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CNotificationHost::OnPaint(HDC dc) noexcept
{
	const UINT dpi = (std::max)(96u, m_hwnd != nullptr ? ::GetDpiForWindow(m_hwnd) : 96u);
	const int padding = Scale(12, dpi);
	const int iconSize = Scale(20, dpi);
	const int closeSize = Scale(18, dpi);
	const HGDIOBJ oldFont = m_font.Get() != nullptr ? ::SelectObject(dc, m_font.Get()) : nullptr;
	::SetBkMode(dc, TRANSPARENT);
	RECT client{};
	::GetClientRect(m_hwnd, &client);
	if (m_centerVisible) {
		HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef());
		if (background != nullptr) {
			::FillRect(dc, &client, background);
			::DeleteObject(background);
		}
		const int headerHeight = Scale(36, dpi);
		RECT header{ client.left, client.top, client.right, headerHeight };
		HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef());
		if (border != nullptr) {
			RECT line{ header.left, header.bottom - 1, header.right, header.bottom };
			::FillRect(dc, &line, border);
			::DeleteObject(border);
		}
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		RECT title{ Scale(12, dpi), 0, m_centerClose.left - Scale(8, dpi), headerHeight };
		::DrawTextW(dc, L"Notifications", -1, &title,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		const auto closeGlyph = workbench::icons::FindCodiconGlyph(L"close");
		const workbench::icons::IconRect closeBox{
			m_centerClose.left, m_centerClose.top, m_centerClose.right, m_centerClose.bottom };
		if (!PaintFontGlyph(dc, closeBox, AcquireCodiconFont(closeSize), closeGlyph.value_or(L'\0'),
			m_palette.secondaryText.ToColorRef())) {
			workbench::icons::codicons::Draw(dc, closeBox, workbench::icons::codicons::Icon::Close,
				m_palette.secondaryText.ToColorRef());
		}
		if (m_layouts.empty()) {
			::SetTextColor(dc, m_palette.descriptionText.ToColorRef());
			RECT empty{ client.left + Scale(12, dpi), headerHeight,
				client.right - Scale(12, dpi), client.bottom };
			::DrawTextW(dc, L"No Notifications", -1, &empty,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		}
		::IntersectClipRect(dc, client.left, headerHeight, client.right, client.bottom);
	}
	for (const auto& layout : m_layouts) {
		if (m_centerVisible && (layout.bounds.bottom <= Scale(36, dpi) || layout.bounds.top >= client.bottom)) continue;
		HBRUSH background = ::CreateSolidBrush(m_palette.panel.ToColorRef());
		HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef());
		if (background != nullptr) {
			::FillRect(dc, &layout.bounds, background);
			::DeleteObject(background);
		}
		if (border != nullptr) {
			::FrameRect(dc, &layout.bounds, border);
			::DeleteObject(border);
		}

		RECT icon = layout.bounds;
		icon.left += padding;
		icon.top += padding;
		icon.right = icon.left + iconSize;
		icon.bottom = icon.top + iconSize;
		const auto severityIcon = SeverityIconFor(layout.notification.severity);
		const auto severityGlyph = workbench::icons::FindCodiconGlyph(severityIcon.glyphName);
		const workbench::icons::IconRect iconBox{
			static_cast<int>(icon.left), static_cast<int>(icon.top),
			static_cast<int>(icon.right), static_cast<int>(icon.bottom) };
		if (!PaintFontGlyph(dc, iconBox, AcquireCodiconFont(iconSize), severityGlyph.value_or(L'\0'),
			SeverityColor(layout.notification.severity))) {
			workbench::icons::codicons::Draw(dc, iconBox, severityIcon.icon,
				SeverityColor(layout.notification.severity));
		}

		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		RECT message = layout.message;
		::DrawTextW(dc, layout.notification.message.c_str(), -1, &message,
			DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
		if (layout.detail.bottom > layout.detail.top) {
			::SetTextColor(dc, m_palette.secondaryText.ToColorRef());
			RECT detail = layout.detail;
			::DrawTextW(dc, layout.notification.detail.c_str(), -1, &detail,
				DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
		}
		if (layout.source.bottom > layout.source.top) {
			::SetTextColor(dc, m_palette.descriptionText.ToColorRef());
			const auto source = SourceText(layout.notification.extensionId);
			RECT sourceRect = layout.source;
			::DrawTextW(dc, source.c_str(), -1, &sourceRect, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}

		RECT close = layout.close;
		::SetTextColor(dc, m_palette.secondaryText.ToColorRef());
		const auto closeGlyph = workbench::icons::FindCodiconGlyph(L"close");
		const workbench::icons::IconRect closeBox{
			static_cast<int>(close.left), static_cast<int>(close.top),
			static_cast<int>(close.right), static_cast<int>(close.bottom) };
		if (!PaintFontGlyph(dc, closeBox, AcquireCodiconFont(closeSize), closeGlyph.value_or(L'\0'),
			m_palette.secondaryText.ToColorRef())) {
			workbench::icons::codicons::Draw(dc, closeBox,
				workbench::icons::codicons::Icon::Close, m_palette.secondaryText.ToColorRef());
		}
		for (std::size_t index = 0; index < layout.actions.size(); ++index) {
			HBRUSH actionBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
			if (actionBrush != nullptr) {
				::FillRect(dc, &layout.actions[index], actionBrush);
				::DeleteObject(actionBrush);
			}
			HBRUSH actionBorder = ::CreateSolidBrush(m_palette.border.ToColorRef());
			if (actionBorder != nullptr) {
				::FrameRect(dc, &layout.actions[index], actionBorder);
				::DeleteObject(actionBorder);
			}
			RECT action = layout.actions[index];
			::SetTextColor(dc, m_palette.primaryText.ToColorRef());
			::DrawTextW(dc, layout.notification.actions[index].c_str(), -1, &action,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
	}
	if (m_centerVisible) {
		::SelectClipRgn(dc, nullptr);
		if (m_keyboardTarget && *m_keyboardTarget == 0) {
			RECT focus = m_centerClose;
			::InflateRect(&focus, 2, 2);
			::DrawFocusRect(dc, &focus);
		}
	}
	if (oldFont != nullptr) ::SelectObject(dc, oldFont);
}

void CNotificationHost::OnTimer() noexcept
{
	if (m_toasts.empty()) return;
	const ULONGLONG now = ::GetTickCount64();
	POINT cursor{};
	RECT window{};
	const bool pointerInside = ::GetCursorPos(&cursor) != FALSE &&
		::GetWindowRect(m_hwnd, &window) != FALSE && ::PtInRect(&window, cursor) != FALSE;
	const HWND foreground = ::GetForegroundWindow();
	const bool ownerInactive = foreground != m_owner && foreground != m_hwnd &&
		(foreground == nullptr || ::GetWindow(foreground, GW_OWNER) != m_owner);
	AdvanceTimer(now, pointerInside || ownerInactive);
}

void CNotificationHost::AdvanceTimer(ULONGLONG now, bool pause) noexcept
{
	const ULONGLONG elapsed = m_lastTimerTick == 0 ? kTimerPeriodMs : now - m_lastTimerTick;
	m_lastTimerTick = now;
	if (pause) {
		for (auto& toast : m_toasts) {
			if (toast.deadline != 0) toast.deadline += elapsed;
		}
		return;
	}

	bool hidden = false;
	for (auto it = m_toasts.begin(); it != m_toasts.end();) {
		if (it->deadline != 0 && it->deadline <= now) {
			m_hiddenToastIds.insert(it->notification.id);
			it = m_toasts.erase(it);
			hidden = true;
		} else {
			++it;
		}
	}
	// A timeout retracts only the toast. The pending request remains available
	// in the notification center and is completed only by close/action.
	if (hidden) Layout();
}

void CNotificationHost::ResolveAt(POINT point) noexcept
{
	if (m_centerVisible && ::PtInRect(&m_centerClose, point) != FALSE) {
		HideCenter();
		return;
	}
	std::uint64_t id = 0;
	std::optional<std::size_t> selected;
	for (const auto& layout : m_layouts) {
		if (layout.close.left <= point.x && point.x < layout.close.right &&
			layout.close.top <= point.y && point.y < layout.close.bottom) {
			id = layout.notification.id;
			break;
		}
		for (std::size_t index = 0; index < layout.actions.size(); ++index) {
			if (::PtInRect(&layout.actions[index], point) != FALSE) {
				id = layout.notification.id;
				selected = index;
				break;
			}
		}
		if (id != 0) break;
	}
	if (id == 0) return;
	m_notifications.erase(std::remove_if(m_notifications.begin(), m_notifications.end(), [id](const auto& notification) {
		return notification.id == id;
	}), m_notifications.end());
	m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(), [id](const ToastState& toast) {
		return toast.notification.id == id;
	}), m_toasts.end());
	m_knownNotificationIds.erase(id);
	m_hiddenToastIds.erase(id);
	m_unreadNotificationIds.erase(id);
	if (m_resolveCallback) m_resolveCallback(id, selected);
	NotifyStatusChanged();
	Layout();
}

void CNotificationHost::MoveKeyboardTarget(bool backwards) noexcept
{
	if (!m_centerVisible) return;
	std::size_t count = 1; // center close button
	for (const auto& layout : m_layouts) count += 1 + layout.actions.size();
	if (count == 0) return;
	const std::size_t current = m_keyboardTarget.value_or(0);
	m_keyboardTarget = backwards ? (current == 0 ? count - 1 : current - 1) : (current + 1) % count;
	::InvalidateRect(m_hwnd, nullptr, FALSE);
}

void CNotificationHost::ActivateKeyboardTarget() noexcept
{
	if (!m_centerVisible || !m_keyboardTarget) return;
	std::size_t cursor = 0;
	if (*m_keyboardTarget == cursor) {
		HideCenter();
		return;
	}
	++cursor;
	for (const auto& layout : m_layouts) {
		if (*m_keyboardTarget == cursor) {
			ResolveAt({ (layout.close.left + layout.close.right) / 2,
				(layout.close.top + layout.close.bottom) / 2 });
			return;
		}
		++cursor;
		for (const auto& action : layout.actions) {
			if (*m_keyboardTarget == cursor) {
				ResolveAt({ (action.left + action.right) / 2, (action.top + action.bottom) / 2 });
				return;
			}
			++cursor;
		}
	}
}

void CNotificationHost::TrackMouse() noexcept
{
	if (m_trackingMouse || m_hwnd == nullptr) return;
	TRACKMOUSEEVENT event{ sizeof(event), TME_LEAVE, m_hwnd, 0 };
	if (::TrackMouseEvent(&event) != FALSE) m_trackingMouse = true;
}

void CNotificationHost::UpdateCursor() noexcept
{
	if (m_hwnd == nullptr) return;
	POINT cursor{};
	if (::GetCursorPos(&cursor) == FALSE || ::ScreenToClient(m_hwnd, &cursor) == FALSE) return;
	bool hand = false;
	for (const auto& layout : m_layouts) {
		if (::PtInRect(&layout.close, cursor) != FALSE) {
			hand = true;
			break;
		}
		if (std::any_of(layout.actions.begin(), layout.actions.end(), [cursor](const RECT& rect) {
			return ::PtInRect(&rect, cursor) != FALSE;
		})) {
			hand = true;
			break;
		}
	}
	::SetCursor(::LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
}

LRESULT CALLBACK CNotificationHost::WindowProc(
	HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	CNotificationHost* self = reinterpret_cast<CNotificationHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = create != nullptr ? static_cast<CNotificationHost*>(create->lpCreateParams) : nullptr;
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (self == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);
	try {
		const LRESULT result = self->HandleMessage(hwnd, message, wParam, lParam);
		if (message == WM_NCDESTROY) ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return result;
	} catch (...) {
		if (message == WM_NCDESTROY) ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return 0;
	}
}

LRESULT CNotificationHost::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	switch (message) {
	case WM_NCHITTEST:
		return HTCLIENT;
	case WM_MOUSEACTIVATE:
		return m_centerVisible ? MA_ACTIVATE : MA_NOACTIVATE;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(m_hwnd, &paint);
		if (dc != nullptr) {
			OnPaint(dc);
			::EndPaint(m_hwnd, &paint);
		}
		return 0;
	}
	case WM_TIMER:
		if (wParam == kTimerId) OnTimer();
		return 0;
	case WM_MOUSEMOVE:
		TrackMouse();
		UpdateCursor();
		return 0;
	case WM_MOUSELEAVE:
		m_trackingMouse = false;
		::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
		return 0;
	case WM_SETCURSOR:
		UpdateCursor();
		return TRUE;
	case WM_LBUTTONUP: {
		const POINT point{
			static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
		ResolveAt(point);
		return 0;
	}
	case WM_MOUSEWHEEL:
		if (m_centerVisible && m_centerContentHeight > m_centerViewportHeight) {
			const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
			const UINT dpi = (std::max)(96u, ::GetDpiForWindow(m_hwnd));
			m_centerScrollOffset -= (delta / WHEEL_DELTA) * Scale(48, dpi);
			Layout();
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (m_centerVisible) {
			switch (wParam) {
			case VK_ESCAPE:
				HideCenter();
				return 0;
			case VK_TAB:
				MoveKeyboardTarget((::GetKeyState(VK_SHIFT) & 0x8000) != 0);
				return 0;
			case VK_RETURN:
			case VK_SPACE:
				ActivateKeyboardTarget();
				return 0;
			default:
				break;
			}
		}
		break;
	case WM_DPICHANGED:
		Layout();
		return 0;
	case WM_CLOSE:
		if (m_centerVisible) HideCenter();
		else ::ShowWindow(m_hwnd, SW_HIDE);
		return 0;
	case WM_DESTROY:
		::KillTimer(m_hwnd, kTimerId);
		m_lastTimerTick = 0;
		return 0;
	default:
		// WM_NCCREATE/WM_CREATE arrive before CreateWindowExW returns and before
		// m_hwnd is assigned. Use the message's HWND for the default procedure.
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace workbench::notification
