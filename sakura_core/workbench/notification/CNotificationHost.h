/*! @file
    @brief VS Code互換の非モーダル通知Toastを表示するWin32ホスト
*/
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionNotificationCenter.h"
#include "theme/CThemeService.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::notification {

//! Owns the window-local presentation of pending non-modal extension notifications.
class CNotificationHost final {
public:
	using ResolveCallback = std::function<void(
		std::uint64_t, std::optional<std::size_t>)>;
	struct StatusSnapshot final {
		std::size_t pendingCount = 0;
		std::size_t unreadCount = 0;
		bool centerVisible = false;
		[[nodiscard]] bool operator==(const StatusSnapshot&) const noexcept = default;
	};
	using StatusChangedCallback = std::function<void(const StatusSnapshot&)>;

	CNotificationHost() noexcept = default;
	~CNotificationHost() noexcept;
	CNotificationHost(const CNotificationHost&) = delete;
	CNotificationHost& operator=(const CNotificationHost&) = delete;

	[[nodiscard]] bool Create(HWND owner) noexcept;
	void Destroy() noexcept;

	void SetPalette(const theme::ThemePalette& palette) noexcept;
	void SetResolveCallback(ResolveCallback callback);
	void SetStatusChangedCallback(StatusChangedCallback callback);
	void SetNotifications(std::vector<SExtensionNotification> notifications);
	void ShowCenter() noexcept;
	void HideCenter() noexcept;
	void ToggleCenter() noexcept;
	void Layout() noexcept;
	[[nodiscard]] StatusSnapshot Status() const noexcept;

	[[nodiscard]] bool IsVisible() const noexcept
	{
		return m_hwnd != nullptr && ::IsWindowVisible(m_hwnd) != FALSE;
	}

private:
	friend struct CNotificationHostTestPeer;

	struct ToastState {
		SExtensionNotification notification;
		ULONGLONG deadline = 0;
	};

	struct ToastLayout {
		SExtensionNotification notification;
		RECT bounds{};
		RECT message{};
		RECT detail{};
		RECT source{};
		RECT close{};
		std::vector<RECT> actions;
	};

	static constexpr UINT_PTR kTimerId = 1;
	static constexpr UINT kTimerPeriodMs = 250;
	static constexpr std::size_t kMaximumToasts = 3;

	static ATOM RegisterWindowClass() noexcept;
	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
	LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

	void OnPaint(HDC dc) noexcept;
	void OnTimer() noexcept;
	void AdvanceTimer(ULONGLONG now, bool pause) noexcept;
	void ResolveAt(POINT point) noexcept;
	void ActivateKeyboardTarget() noexcept;
	void MoveKeyboardTarget(bool backwards) noexcept;
	void RebuildPresentation(ULONGLONG now);
	void NotifyStatusChanged() noexcept;
	void UpdateCursor() noexcept;
	void TrackMouse() noexcept;
	void RebuildLayout(HDC dc, int width, UINT dpi) noexcept;
	[[nodiscard]] HFONT AcquireCodiconFont(int height) noexcept;
	void ReleaseCodiconFont() noexcept;

	[[nodiscard]] int Scale(int value, UINT dpi) const noexcept;
	[[nodiscard]] int LineHeight(HDC dc) const noexcept;
	[[nodiscard]] int WrappedTextHeight(HDC dc, std::wstring_view text,
		int width, int maximumHeight, int lineHeight) const noexcept;
	[[nodiscard]] COLORREF SeverityColor(EExtensionNotificationSeverity severity) const noexcept;
	[[nodiscard]] static std::wstring SourceText(std::wstring_view extensionId);

	HWND m_owner = nullptr;
	HWND m_hwnd = nullptr;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	ResolveCallback m_resolveCallback;
	StatusChangedCallback m_statusChangedCallback;
	std::vector<SExtensionNotification> m_notifications;
	std::vector<ToastState> m_toasts;
	std::vector<ToastLayout> m_layouts;
	std::set<std::uint64_t> m_knownNotificationIds;
	std::set<std::uint64_t> m_hiddenToastIds;
	std::set<std::uint64_t> m_unreadNotificationIds;
	RECT m_centerClose{};
	int m_centerContentHeight = 0;
	int m_centerViewportHeight = 0;
	int m_centerScrollOffset = 0;
	std::optional<std::size_t> m_keyboardTarget;
	bool m_centerVisible = false;
	bool m_trackingMouse = false;
	ULONGLONG m_lastTimerTick = 0;
	HFONT m_codiconFont = nullptr;
	int m_codiconFontHeight = 0;
};

} // namespace workbench::notification
