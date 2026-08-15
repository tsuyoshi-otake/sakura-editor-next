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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench {

using activity::ActivityBarButtonInfo;
using activity::ActivityBarEntry;
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
	//! VS Code `activityBar.border`: the Part edge against the Primary Side Bar
	//! (or the editor when the Side Bar is hidden).
	COLORREF border = RGB(0x45, 0x45, 0x45);
	bool highContrast = false;

	[[nodiscard]] static ActivityBarPalette Dark() noexcept;
	[[nodiscard]] static ActivityBarPalette Light() noexcept;
	[[nodiscard]] static ActivityBarPalette HighContrast(COLORREF window, COLORREF windowText,
		COLORREF highlight, COLORREF highlightText) noexcept;
};

//! A native vertical workbench switcher. Selection is controlled by its owner; activation emits a toggle request.
class CActivityBar final : public accessibility::ICustomUiAutomationHost {
public:
	//! The argument is the ViewContainer id of the clicked entry.
	using ToggleRequestCallback = std::function<void(std::string_view containerId)>;
	//! GlobalCompositeBar actions (`workbench.actions.accounts` / `workbench.actions.manage`).
	//! The point is the button's bottom-left in screen coordinates for popup anchoring.
	using GlobalActionCallback = std::function<void(std::string_view actionId, POINT screenPoint)>;
	//! Raised when the user drops a dragged Activity Bar entry. VS Code treats both the
	//! Activity Bar icon and the side-bar title as composite drag handles, and the drop
	//! target decides whether the ViewContainer changes location. The point is in screen
	//! coordinates because the target is usually a different window.
	using ContainerDragCallback = std::function<void(std::string_view containerId, POINT screenPoint)>;

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
	void SetGlobalActionCallback(GlobalActionCallback callback) { m_onGlobalAction = std::move(callback); }
	void SetContainerDragCallback(ContainerDragCallback callback) { m_onContainerDrag = std::move(callback); }

	//! Replaces the rendered ViewContainers, including any an extension contributed.
	//! Tooltips and accessibility follow, so this is the only entry point a composition needs.
	void SetEntries(std::vector<ActivityBarEntry> entries);
	[[nodiscard]] const std::vector<ActivityBarEntry>& GetEntries() const noexcept { return m_model.Entries(); }

	void SetSelectedItem(std::string_view containerId) noexcept;
	void SetSelected(std::string_view containerId) noexcept { SetSelectedItem(containerId); }
	void SetPressed(std::string_view containerId) noexcept;
	void SetItemEnabled(std::string_view containerId, bool enabled) noexcept;
	//! Adds or removes the entry for a ViewContainer that left the Primary Side Bar.
	void SetItemVisible(std::string_view containerId, bool visible) noexcept;
	[[nodiscard]] bool IsItemVisible(std::string_view containerId) const noexcept { return m_model.IsVisible(containerId); }
	[[nodiscard]] int GetPreferredWidthPixels() const noexcept { return m_model.GetPreferredWidthPixels(); }
	[[nodiscard]] unsigned int GetDpi() const noexcept { return m_model.GetDpi(); }
	[[nodiscard]] HWND GetHwnd() const noexcept { return m_window; }

	// Provider-ready logical API. Bounds and state stay valid even before a window is created.
	[[nodiscard]] std::size_t GetButtonCount() const noexcept { return m_model.GetButtonCount(); }
	[[nodiscard]] ActivityBarButtonInfo GetButton(std::size_t index) const noexcept { return m_model.GetButton(index); }
	[[nodiscard]] std::string_view HitTest(int x, int y) const noexcept { return m_model.HitTest(x, y); }
	[[nodiscard]] std::string_view GetFocusedItem() const noexcept { return m_model.GetFocusedItem(); }
	//! Emits the callback for an enabled item; it does not change selected state optimistically.
	[[nodiscard]] bool Invoke(std::string_view containerId) noexcept;
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
	[[nodiscard]] bool InvokeRequest(std::string_view containerId) noexcept;
	[[nodiscard]] bool InvokeGlobalAction(std::string_view actionId) noexcept;
	[[nodiscard]] bool HandleNavigationKey(WPARAM key) noexcept;
	void SetHoverFromPoint(POINT point) noexcept;
	//! True once the pointer has left the system drag threshold while a button is held.
	[[nodiscard]] bool BeginDragIfPastThreshold(POINT point) noexcept;
	//! Delivers the drop to the owner. Returns true when a drag consumed the click.
	[[nodiscard]] bool FinishDrag(std::string_view containerId, POINT clientPoint) noexcept;
	//! Rebuilds the tooltip tools after the entry list changed. Tooltips are keyed by index,
	//! so a new container must not inherit the label of whoever held that slot before.
	void RebuildTooltips();

	ActivityBarModel m_model;
	ActivityBarPalette m_palette = ActivityBarPalette::Dark();
	ToggleRequestCallback m_onToggleRequest;
	GlobalActionCallback m_onGlobalAction;
	ContainerDragCallback m_onContainerDrag;
	HWND m_window = nullptr;
	HWND m_tooltip = nullptr;
	HFONT m_iconFont = nullptr;
	//! ViewContainer id under the pressed pointer, empty while nothing is captured. It is a
	//! copy rather than a view because the entry list can be replaced between messages.
	std::string m_captureItem;
	//! Tooltip labels must outlive TTM_ADDTOOLW, which stores the pointer it was given.
	std::vector<std::wstring> m_tooltipLabels;
	POINT m_dragOrigin{};
	bool m_dragging = false;
	bool m_trackingMouseLeave = false;
	bool m_destroyed = false;
	bool m_destroying = false;
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};

} // namespace workbench
