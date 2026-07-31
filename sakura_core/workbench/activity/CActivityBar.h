/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/activity/ActivityBarModel.h"
#include "accessibility/CustomUiAutomationProvider.h"

#include <Windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace workbench {

using activity::ActivityBarButtonInfo;
using activity::ActivityBarItem;
using activity::ActivityBarItemName;
using activity::ActivityBarModel;

//! Caller-supplied colours. High contrast is explicit so callers can inject their system palette.
struct ActivityBarPalette {
	COLORREF background = RGB(24, 24, 24);
	COLORREF hoverBackground = RGB(42, 42, 42);
	COLORREF pressedBackground = RGB(57, 57, 57);
	COLORREF selectedBackground = RGB(37, 37, 37);
	COLORREF activeIndicator = RGB(0, 120, 212);
	//! Inactive glyphs are deliberately secondary; selection promotes only the active glyph.
	COLORREF icon = RGB(170, 177, 188);
	COLORREF activeIcon = RGB(232, 235, 240);
	COLORREF disabledIcon = RGB(105, 105, 105);
	COLORREF focusBorder = RGB(255, 255, 255);
	bool highContrast = false;

	[[nodiscard]] static ActivityBarPalette Dark() noexcept;
	[[nodiscard]] static ActivityBarPalette Light() noexcept;
	[[nodiscard]] static ActivityBarPalette HighContrast(COLORREF window, COLORREF windowText,
		COLORREF highlight, COLORREF highlightText) noexcept;
};

//! A native vertical workbench switcher. Selection is controlled by its owner; activation emits a toggle request.
class CActivityBar final : public accessibility::ICustomUiAutomationHost {
public:
	using ToggleRequestCallback = std::function<void(ActivityBarItem item)>;
	//! Raised when the user drops a dragged Activity Bar entry. VS Code treats both the
	//! Activity Bar icon and the side-bar title as composite drag handles, and the drop
	//! target decides whether the ViewContainer changes location. The point is in screen
	//! coordinates because the target is usually a different window.
	using ContainerDragCallback = std::function<void(ActivityBarItem item, POINT screenPoint)>;

	explicit CActivityBar(ToggleRequestCallback onToggleRequest = {});
	~CActivityBar();
	CActivityBar(const CActivityBar&) = delete;
	CActivityBar& operator=(const CActivityBar&) = delete;

	//! Creates a WS_CHILD/WS_TABSTOP window below parent. The caller owns positioning through Layout().
	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance);
	[[nodiscard]] bool Create(HWND parent, HINSTANCE instance, ToggleRequestCallback onToggleRequest);
	//! Safe to call repeatedly, including after a parent has already destroyed the child.
	void Destroy() noexcept;
	void Layout(const RECT& bounds, unsigned int dpi);
	void SetPalette(const ActivityBarPalette& palette) noexcept;
	[[nodiscard]] const ActivityBarPalette& GetPalette() const noexcept { return m_palette; }
	void SetToggleRequestCallback(ToggleRequestCallback callback) { m_onToggleRequest = std::move(callback); }
	void SetContainerDragCallback(ContainerDragCallback callback) { m_onContainerDrag = std::move(callback); }

	void SetSelectedItem(std::optional<ActivityBarItem> item) noexcept;
	void SetSelected(std::optional<ActivityBarItem> item) noexcept { SetSelectedItem(item); }
	void SetPressed(std::optional<ActivityBarItem> item) noexcept;
	void SetItemEnabled(ActivityBarItem item, bool enabled) noexcept;
	//! Adds or removes the entry for a ViewContainer that left the Primary Side Bar.
	void SetItemVisible(ActivityBarItem item, bool visible) noexcept;
	[[nodiscard]] bool IsItemVisible(ActivityBarItem item) const noexcept { return m_model.IsVisible(item); }
	[[nodiscard]] int GetPreferredWidthPixels() const noexcept { return m_model.GetPreferredWidthPixels(); }
	[[nodiscard]] unsigned int GetDpi() const noexcept { return m_model.GetDpi(); }
	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }

	// Provider-ready logical API. Bounds and state stay valid even before a window is created.
	[[nodiscard]] std::size_t GetButtonCount() const noexcept { return m_model.GetButtonCount(); }
	[[nodiscard]] ActivityBarButtonInfo GetButton(std::size_t index) const noexcept { return m_model.GetButton(index); }
	[[nodiscard]] std::optional<ActivityBarItem> HitTest(int x, int y) const noexcept { return m_model.HitTest(x, y); }
	[[nodiscard]] std::optional<ActivityBarItem> GetFocusedItem() const noexcept { return m_model.GetFocusedItem(); }
	//! Emits the callback for an enabled item; it does not change selected state optimistically.
	[[nodiscard]] bool Invoke(ActivityBarItem item) noexcept;
	//! Routes keyboard messages before the application's accelerator table.
	[[nodiscard]] bool PreTranslateMessage(MSG& message) noexcept;
	[[nodiscard]] bool PreTranslate(MSG& message) noexcept { return PreTranslateMessage(message); }
	void Close() noexcept { Destroy(); }

	static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
	[[nodiscard]] HWND AccessibilityWindow() const noexcept override { return m_window; }
	[[nodiscard]] std::shared_ptr<accessibility::CustomUiAutomationLifetime> AccessibilityLifetime() const noexcept override { return m_accessibilityLifetime; }
	[[nodiscard]] std::wstring AccessibilityName() const override { return L"Activity Bar"; }
	[[nodiscard]] std::wstring AccessibilityAutomationId() const override { return L"Sakura.ActivityBar"; }
	[[nodiscard]] CONTROLTYPEID AccessibilityControlType() const noexcept override { return UIA_ToolBarControlTypeId; }
	[[nodiscard]] int AccessibilityChildCount(int parentId) const noexcept override;
	[[nodiscard]] int AccessibilityChildAt(int parentId, int index) const noexcept override;
	[[nodiscard]] int AccessibilityParent(int nodeId) const noexcept override;
	[[nodiscard]] accessibility::CustomUiAutomationNode AccessibilityNode(int nodeId) const override;
	[[nodiscard]] int AccessibilityFocusedNode() const noexcept override;
	[[nodiscard]] bool AccessibilityInvoke(int nodeId) noexcept override;
	void AccessibilitySetFocus(int nodeId) noexcept override;

	[[nodiscard]] LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
	void UpdateClientLayout(unsigned int dpi) noexcept;
	void UpdateTooltipRects() noexcept;
	void EnsureIconFont() noexcept;
	void Paint() noexcept;
	void Invalidate() const noexcept;
	[[nodiscard]] bool InvokeRequest(std::optional<ActivityBarItem> item) noexcept;
	[[nodiscard]] bool HandleNavigationKey(WPARAM key) noexcept;
	void SetHoverFromPoint(POINT point) noexcept;
	//! True once the pointer has left the system drag threshold while a button is held.
	[[nodiscard]] bool BeginDragIfPastThreshold(POINT point) noexcept;
	//! Delivers the drop to the owner. Returns true when a drag consumed the click.
	[[nodiscard]] bool FinishDrag(ActivityBarItem item, POINT clientPoint) noexcept;

	ActivityBarModel m_model;
	ActivityBarPalette m_palette = ActivityBarPalette::Dark();
	ToggleRequestCallback m_onToggleRequest;
	ContainerDragCallback m_onContainerDrag;
	HWND m_window = nullptr;
	HWND m_tooltip = nullptr;
	HFONT m_iconFont = nullptr;
	std::optional<ActivityBarItem> m_captureItem;
	POINT m_dragOrigin{};
	bool m_dragging = false;
	bool m_trackingMouseLeave = false;
	bool m_destroyed = false;
	bool m_destroying = false;
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};

} // namespace workbench
