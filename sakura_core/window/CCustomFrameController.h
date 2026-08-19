/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>

#include <array>
#include <functional>
#include <memory>
#include <utility>

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
	RECT layoutButton{};
	RECT primarySidebarButton{};
	RECT bottomPanelButton{};
	RECT secondarySidebarButton{};
	//! Empty unless the update state is actionable. Unlike the glyph controls this
	//! one is a labelled button, so its width is measured from the caption font
	//! instead of being the fixed compact-control width.
	RECT updateButton{};
	RECT accountButton{};
	RECT manageButton{};
	RECT minimizeButton{};
	RECT maximizeButton{};
	RECT closeButton{};
};

[[nodiscard]] int ScaleCustomFrameDip(int value, UINT dpi) noexcept;
//! `updateButtonWidth` is the measured physical width of the Update indicator, or
//! zero when the update state is not actionable. Zero must leave every other
//! rectangle exactly where it would be without the indicator.
[[nodiscard]] CustomFrameLayout CalculateCustomFrameLayout(
	int clientWidth,
	UINT dpi,
	int preferredMenuWidth,
	int updateButtonWidth = 0
) noexcept;
//! Physical width the Update indicator needs for its label, including its padding and
//! the margin that separates the painted pill from its neighbours.
[[nodiscard]] int MeasureCustomFrameUpdateButtonWidth(HDC dc, UINT dpi) noexcept;
//! The filled pill painted inside the Update indicator's action rectangle. VS Code paints
//! the actionable indicator as a button inset in the title bar rather than as a full-height
//! caption cell, so the fill, the hover feedback, and the focus ring all belong to this
//! rectangle instead of to the whole action area. Returns an empty rectangle when the
//! action rectangle is empty or too small to hold a pill.
[[nodiscard]] RECT CustomFrameUpdateIndicatorPillRect(const RECT& actionRect, UINT dpi) noexcept;
[[nodiscard]] LRESULT HitTestCustomFrame(
	const CustomFrameLayout& layout,
	POINT clientPoint,
	int clientWidth,
	int clientHeight,
	int resizeBorder,
	bool maximized
) noexcept;
//! Returns the compact title-bar control at a client point, or None outside its bounds.
[[nodiscard]] CustomFrameControl HitTestCustomFrameControl(
	const CustomFrameLayout& layout,
	POINT clientPoint
) noexcept;
//! Maps a directly invokable title-bar control to its editor command; zero opens a local popup.
[[nodiscard]] UINT CustomFrameControlCommand(CustomFrameControl control) noexcept;
//! Builds the shared UIA/MSAA snapshot for one compact title-bar control.
[[nodiscard]] accessibility::CustomUiAutomationNode CustomFrameControlAccessibilityNode(
	CustomFrameControl control,
	const CustomFrameLayout& layout,
	bool focused
);

//! DWM owns a processed non-client result. HTCLIENT/HTNOWHERE are the explicit
//! exception: the extended custom client must supply its own caption/menu hits.
[[nodiscard]] bool ShouldPreferDwmNonClientResult(UINT message, LRESULT dwmResult) noexcept;
//! Maps a released custom caption button to the standard window command.
//! Returns zero for non-caption hits.
[[nodiscard]] UINT CaptionButtonSystemCommand(LRESULT hit, bool maximized) noexcept;

//! Actions available from the VS Code-compatible Manage menu.
enum class CustomFrameManageAction : unsigned char {
	None,
	ShowCommandPalette,
	OpenSettings,
	OpenKeyboardShortcuts,
	SelectColorTheme,
	//! Group `7_update`. Only the four actionable upstream entries can be chosen;
	//! the in-progress ones are contributed with `precondition: false` and are
	//! therefore drawn greyed and never produce an action.
	CheckForUpdates,
	DownloadUpdate,
	InstallUpdate,
	RestartToUpdate,
};

//! The single `7_update` entry the current update state makes visible. Upstream
//! registers eight items into `MenuId.GlobalActivity` group `7_update`, each gated
//! on `CONTEXT_UPDATE_STATE == '<state>'`, so at most one is visible at a time.
//! `None` is the `disabled`/`uninitialized` case, where upstream contributes nothing.
enum class CustomFrameUpdateMenuEntry : unsigned char {
	None,
	Check,       //!< `update.check`       "Check for Updates..."
	Checking,    //!< `update.checking`    "Checking for Updates..."  (disabled upstream)
	DownloadNow, //!< `update.downloadNow` "Download Update (1)"
	Downloading, //!< `update.downloading` "Downloading Update..."    (disabled upstream)
	Install,     //!< `update.install`     "Install Update... (1)"
	Updating,    //!< `update.updating`    "Installing Update..."     (disabled upstream)
	Cancelling,  //!< `update.cancelling`  "Cancelling Update..."     (disabled upstream)
	Restart,     //!< `update.restart`     "Restart to Update (1)"
};

//! Physical edge covered by an input-only child overlay. The custom client
//! fills the whole top-level window, so child controls would otherwise consume
//! the initial press before the frame can return an HT* resize result.
enum class CustomFrameResizeEdge : unsigned char {
	Top,
	Bottom,
	Left,
	Right,
	Count,
};

[[nodiscard]] std::array<RECT, static_cast<size_t>(CustomFrameResizeEdge::Count)>
CalculateCustomFrameResizeOverlayBounds(
	int clientWidth,
	int clientHeight,
	int resizeBorder,
	bool maximized
) noexcept;

using CustomFrameManageActionCallback = std::function<void(CustomFrameManageAction)>;

//! Invoked when the title-bar Update indicator is pressed. The frame does not know
//! which update command that means; the composition root resolves the current
//! update state to `update.downloadNow` / `update.install` / `update.restart`
//! exactly as upstream's `updateTitleBarEntry.ts` does.
using CustomFrameUpdateIndicatorCallback = std::function<void()>;

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
	void SetUiScalePercent(int percent) noexcept;
	//! Binds the window-local workbench command dispatcher used by the Manage popup.
	void SetManageMenuActionCallback(CustomFrameManageActionCallback callback) noexcept
	{
		m_manageMenuActionCallback = std::move(callback);
	}
	//! Binds the window-local dispatcher for the title-bar Update indicator.
	void SetUpdateIndicatorCallback(CustomFrameUpdateIndicatorCallback callback) noexcept
	{
		m_updateIndicatorCallback = std::move(callback);
	}
	//! Shows or hides the Update indicator. Upstream shows it only for the
	//! actionable states and only while `update.titleBar` is enabled; both
	//! decisions belong to the composition root, so this takes the answer.
	void SetUpdateIndicatorVisible(bool visible) noexcept;
	[[nodiscard]] bool IsUpdateIndicatorVisible() const noexcept { return m_updateIndicatorVisible; }
	//! Selects the `7_update` entry the Manage popup contributes. The popup is built
	//! on demand, so this needs no invalidation; `None` contributes nothing at all.
	void SetUpdateMenuEntry(CustomFrameUpdateMenuEntry entry) noexcept { m_updateMenuEntry = entry; }
	[[nodiscard]] CustomFrameUpdateMenuEntry UpdateMenuEntry() const noexcept { return m_updateMenuEntry; }
	[[nodiscard]] theme::ThemeMode GetThemeMode() const noexcept { return m_savedMode; }
	[[nodiscard]] UINT Dpi() const noexcept { return m_dpi; }
	[[nodiscard]] int TitleHeight() const noexcept { return ScaleCustomFrameDip(34, m_dpi); }

	//! May be used during WM_NCCREATE before CEditWnd's normal dispatch is enabled.
	[[nodiscard]] bool HandleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) noexcept;
	[[nodiscard]] bool PreTranslateMessage(MSG& message) noexcept;
	void Paint(HDC dc, const RECT& paintRect) noexcept;
	void InvalidateTitle() const noexcept;
	//! Reasserts the input-only edge overlays after sibling HWND layout.
	void LayoutResizeOverlays() noexcept;
	//! Activity Bar GlobalCompositeBar popup anchors (screen coordinates).
	void ShowAccountMenuAt(POINT screenPoint) noexcept;
	//! `rightAlign` matches a former title-bar Manage popup; Activity Bar uses left-align.
	void ShowManageMenuAt(POINT screenPoint, bool rightAlign = false) noexcept;
	static LRESULT CALLBACK ResizeOverlayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

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
	//! Zero while the indicator is hidden; the caption-font measurement otherwise.
	[[nodiscard]] int MeasureUpdateIndicatorWidth() const noexcept;
	//! Title controls that currently have a rectangle, in left-to-right order.
	[[nodiscard]] int VisibleTitleControlCount() const noexcept;
	[[nodiscard]] CustomFrameControl VisibleTitleControlAt(int index) const noexcept;
	void CreateResizeOverlays() noexcept;
	void DestroyResizeOverlays() noexcept;
	[[nodiscard]] int ResizeBorder() const noexcept;
	[[nodiscard]] LRESULT HitTestScreenPoint(POINT screenPoint) noexcept;
	void SetHotHit(LRESULT hit) noexcept;
	void SetPressedHit(LRESULT hit) noexcept;
	void SetHotControl(CustomFrameControl control) noexcept;
	void SetPressedControl(CustomFrameControl control) noexcept;
	void ClearAccessibilityFocus() noexcept;
	[[nodiscard]] bool IsCaptionButton(LRESULT hit) const noexcept;
	[[nodiscard]] bool HandleTitleControlMouseMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) noexcept;
	void InvokeTitleControl(CustomFrameControl control) noexcept;
	void ShowLayoutMenu(const RECT& anchor) noexcept;
	void ShowAccountMenu(const RECT& anchor) noexcept;
	void ShowManageMenu(const RECT& anchor) noexcept;
	struct ResizeOverlaySlot {
		CCustomFrameController* owner = nullptr;
		CustomFrameResizeEdge edge = CustomFrameResizeEdge::Top;
		HWND window = nullptr;
	};

	HWND m_window = nullptr;
	UINT m_dpi = 96;
	UINT m_physicalDpi = 96;
	int m_uiScalePercent = 100;
	theme::ThemeMode m_savedMode = theme::ThemeMode::Dark;
	theme::ThemePalette m_palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont m_font;
	theme::CThemeFont m_menuFont;
	CustomFrameLayout m_layout{};
	CClientMenuBar m_menuBar;
	CCustomTitleBar m_titleBar;
	CustomFrameManageActionCallback m_manageMenuActionCallback;
	CustomFrameUpdateIndicatorCallback m_updateIndicatorCallback;
	bool m_updateIndicatorVisible = false;
	CustomFrameUpdateMenuEntry m_updateMenuEntry = CustomFrameUpdateMenuEntry::None;
	LRESULT m_hotHit = HTNOWHERE;
	LRESULT m_pressedHit = HTNOWHERE;
	CustomFrameControl m_hotControl = CustomFrameControl::None;
	CustomFrameControl m_pressedControl = CustomFrameControl::None;
	bool m_active = true;
	bool m_trackingNonClientLeave = false;
	bool m_trackingTitleControlLeave = false;
	int m_accessibilityFocusedNode = -1;
	std::array<ResizeOverlaySlot, static_cast<size_t>(CustomFrameResizeEdge::Count)> m_resizeOverlays{};
	std::shared_ptr<accessibility::CustomUiAutomationLifetime> m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
};
