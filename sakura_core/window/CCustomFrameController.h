/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <memory>

#include "theme/CThemeService.h"
#include "window/CClientMenuBar.h"
#include "window/CCustomTitleBar.h"
#include "accessibility/CustomUiAutomationProvider.h"

//! All rectangles use client coordinates after WM_NCCALCSIZE extends the client to the top edge.
struct CustomFrameLayout {
	RECT title{};
	RECT systemMenu{};
	RECT menu{};
	RECT captionText{};
	RECT minimizeButton{};
	RECT maximizeButton{};
	RECT closeButton{};
};

[[nodiscard]] int ScaleCustomFrameDip(int value, UINT dpi) noexcept;
[[nodiscard]] CustomFrameLayout CalculateCustomFrameLayout(
	int clientWidth,
	UINT dpi,
	int preferredMenuWidth
) noexcept;
[[nodiscard]] LRESULT HitTestCustomFrame(
	const CustomFrameLayout& layout,
	POINT clientPoint,
	int clientWidth,
	int clientHeight,
	int resizeBorder,
	bool maximized
) noexcept;

//! DWM owns a processed non-client result. HTCLIENT/HTNOWHERE are the explicit
//! exception: the extended custom client must supply its own caption/menu hits.
[[nodiscard]] bool ShouldPreferDwmNonClientResult(UINT message, LRESULT dwmResult) noexcept;

//! Owns non-client extension, hit-testing, custom title/menu painting, and per-window DPI state.
class CCustomFrameController final : public accessibility::ICustomUiAutomationHost {
public:
	CCustomFrameController() = default;
	~CCustomFrameController();
	CCustomFrameController(const CCustomFrameController&) = delete;
	CCustomFrameController& operator=(const CCustomFrameController&) = delete;

	void Attach(HWND window, theme::ThemeMode savedMode) noexcept;
	void Detach() noexcept;
	[[nodiscard]] bool IsAttached() const noexcept { return m_window != nullptr; }

	//! Replaces but does not destroy the HMENU. Returns the previous menu to its owner.
	[[nodiscard]] HMENU ReplaceMenu(HMENU menu) noexcept;
	[[nodiscard]] HMENU GetMenu() const noexcept { return m_menuBar.GetMenu(); }
	void SetThemeMode(theme::ThemeMode savedMode) noexcept;
	[[nodiscard]] theme::ThemeMode GetThemeMode() const noexcept { return m_savedMode; }
	[[nodiscard]] UINT Dpi() const noexcept { return m_dpi; }
	[[nodiscard]] int TitleHeight() const noexcept { return ScaleCustomFrameDip(34, m_dpi); }

	//! May be used during WM_NCCREATE before CEditWnd's normal dispatch is enabled.
	[[nodiscard]] bool HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) noexcept;
	[[nodiscard]] bool PreTranslateMessage(MSG& message) noexcept;
	void Paint(HDC dc, const RECT& paintRect) noexcept;
	void InvalidateTitle() const noexcept;

private:
	[[nodiscard]] HWND AccessibilityWindow() const noexcept override { return m_window; }
	[[nodiscard]] std::shared_ptr<accessibility::CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept override { return m_accessibilityLifetime; }
	[[nodiscard]] std::wstring AccessibilityName() const override;
	[[nodiscard]] std::wstring AccessibilityAutomationId() const override { return L"Sakura.WindowChrome"; }
	[[nodiscard]] CONTROLTYPEID AccessibilityControlType() const noexcept override { return UIA_WindowControlTypeId; }
	[[nodiscard]] int AccessibilityChildCount(int parentId) const noexcept override;
	[[nodiscard]] int AccessibilityChildAt(int parentId, int index) const noexcept override;
	[[nodiscard]] int AccessibilityParent(int nodeId) const noexcept override;
	[[nodiscard]] accessibility::CustomUiAutomationNode AccessibilityNode(int nodeId) const override;
	[[nodiscard]] int AccessibilityFocusedNode() const noexcept override;
	[[nodiscard]] bool AccessibilityInvoke(int nodeId) noexcept override;
	void AccessibilitySetFocus(int nodeId) noexcept override;

	void RefreshMetrics() noexcept;
	void RefreshLayout() noexcept;
	[[nodiscard]] int ResizeBorder() const noexcept;
	[[nodiscard]] LRESULT HitTestScreenPoint(POINT screenPoint) noexcept;
	void SetHotHit(LRESULT hit) noexcept;
	void SetPressedHit(LRESULT hit) noexcept;
	void ClearAccessibilityFocus() noexcept;
	[[nodiscard]] bool IsCaptionButton(LRESULT hit) const noexcept;

	HWND m_window = nullptr;
	UINT m_dpi = 96;
	theme::ThemeMode m_savedMode = theme::ThemeMode::Dark;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	CustomFrameLayout m_layout{};
	CClientMenuBar m_menuBar;
	CCustomTitleBar m_titleBar;
	LRESULT m_hotHit = HTNOWHERE;
	LRESULT m_pressedHit = HTNOWHERE;
	bool m_active = true;
	bool m_trackingNonClientLeave = false;
	int m_accessibilityFocusedNode = -1;
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};
