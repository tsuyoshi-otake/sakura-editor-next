/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "window/CCustomFrameController.h"
#include "CSelectLang.h"
#include "func/Funccode.h"
#include "workbench/WorkbenchLayout.h"
#include "workbench/WorkbenchZoom.h"

#include <algorithm>

#include <dwmapi.h>
#include <stdio.h>

namespace {

constexpr wchar_t kResizeOverlayClass[] = L"SakuraCustomFrameResizeOverlay";

// DWM window attributes. The SDK this project builds against does not declare the
// Windows 11 members of DWMWINDOWATTRIBUTE, so their documented values are used
// directly; DwmSetWindowAttribute simply fails on an older system.
constexpr DWORD kDwmUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmBorderColor = 34;          // DWMWA_BORDER_COLOR

[[nodiscard]] bool EnsureResizeOverlayWindowClass(HINSTANCE instance) noexcept
{
	static ATOM atom = 0;
	if (atom != 0) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.lpfnWndProc = CCustomFrameController::ResizeOverlayWindowProc;
	windowClass.lpszClassName = kResizeOverlayClass;
	atom = ::RegisterClassExW(&windowClass);
	if (atom != 0) return true;
	if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
		atom = 1;
		return true;
	}
	return false;
}

constexpr int kMenuBarNode = 1000;
constexpr int kMenuItemBaseNode = 1100;
constexpr int kMinimizeNode = 1200;
constexpr int kMaximizeNode = 1201;
constexpr int kCloseNode = 1202;
constexpr int kTitleControlNodeBase = 1300;
constexpr UINT kManageShowCommandPalette = 0x5A01;
constexpr UINT kManageOpenSettings = 0x5A02;
constexpr UINT kManageOpenKeyboardShortcuts = 0x5A04;
constexpr UINT kManageSelectColorTheme = 0x5A05;
// Group `7_update`. Only the four actionable upstream entries need a command id;
// the in-progress ones are contributed with `precondition: false` and are appended
// greyed, so `TrackPopupMenu` can never return them.
constexpr UINT kManageUpdateCheck = 0x5A07;
constexpr UINT kManageUpdateDownload = 0x5A08;
constexpr UINT kManageUpdateInstall = 0x5A09;
constexpr UINT kManageUpdateRestart = 0x5A0A;

// The Update indicator is a button drawn on the title bar, not a title-bar cell: it keeps
// a margin on each side and stands at the button height instead of the full caption
// height. Both the measured action width and the painted pill derive from these, so they
// can never describe two different buttons.
constexpr int kUpdateIndicatorMarginDip = 4;
constexpr int kUpdateIndicatorHeightDip = 22;

constexpr int kPopupAnchorGapDip = 4;

bool Contains(const RECT& rect, POINT point) noexcept
{
	return point.x >= rect.left && point.x < rect.right
		&& point.y >= rect.top && point.y < rect.bottom;
}

RECT MakeRect(int left, int top, int right, int bottom) noexcept
{
	return { left, top, std::max(left, right), std::max(top, bottom) };
}

void ClampPopupRectToWorkArea(RECT& popup, const RECT& workArea) noexcept
{
	if (::IsRectEmpty(&workArea) || ::IsRectEmpty(&popup)) return;
	const int popupWidth = popup.right - popup.left;
	const int popupHeight = popup.bottom - popup.top;
	if (popupWidth <= 0 || popupHeight <= 0) return;

	// A menu larger than the work area cannot be made fully visible. Keep its native
	// extent and pin its origin to the work-area edge; TrackPopupMenuEx will apply the
	// same best-effort policy for that impossible case.
	const int workLeft = static_cast<int>(workArea.left);
	const int workTop = static_cast<int>(workArea.top);
	const int workRight = static_cast<int>(workArea.right);
	const int workBottom = static_cast<int>(workArea.bottom);
	const int maxLeft = std::max(workLeft, workRight - popupWidth);
	const int maxTop = std::max(workTop, workBottom - popupHeight);
	popup.left = static_cast<LONG>(std::clamp(static_cast<int>(popup.left), workLeft, maxLeft));
	popup.top = static_cast<LONG>(std::clamp(static_cast<int>(popup.top), workTop, maxTop));
	popup.right = popup.left + popupWidth;
	popup.bottom = popup.top + popupHeight;
}

RECT TitleControlRect(const CustomFrameLayout& layout, CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::Layout: return layout.layoutButton;
	case CustomFrameControl::PrimarySidebar: return layout.primarySidebarButton;
	case CustomFrameControl::BottomPanel: return layout.bottomPanelButton;
	case CustomFrameControl::SecondarySidebar: return layout.secondarySidebarButton;
	case CustomFrameControl::Update: return layout.updateButton;
	case CustomFrameControl::Account: return layout.accountButton;
	case CustomFrameControl::Manage: return layout.manageButton;
	case CustomFrameControl::None: break;
	}
	return {};
}

//! Every title control is kept in this order so hit testing, painting, popup
//! invocation, and accessibility all consume the same left-to-right run. The
//! Account/Manage rectangles are empty while the Activity Bar is vertical.
constexpr std::array<CustomFrameControl, 7> kTitleControls = {
	CustomFrameControl::Layout,
	CustomFrameControl::PrimarySidebar,
	CustomFrameControl::BottomPanel,
	CustomFrameControl::SecondarySidebar,
	CustomFrameControl::Update,
	CustomFrameControl::Account,
	CustomFrameControl::Manage,
};

CustomFrameControl TitleControlFromNode(int nodeId) noexcept
{
	const int value = nodeId - kTitleControlNodeBase + 1;
	return value >= static_cast<int>(CustomFrameControl::Layout)
		&& value <= static_cast<int>(CustomFrameControl::Manage)
		? static_cast<CustomFrameControl>(value)
		: CustomFrameControl::None;
}

int TitleControlNode(CustomFrameControl control) noexcept
{
	return control == CustomFrameControl::None
		? -1
		: kTitleControlNodeBase + static_cast<int>(control) - 1;
}

CustomFrameManageAction ManageActionFromMenuCommand(UINT command) noexcept
{
	switch (command) {
	case kManageShowCommandPalette: return CustomFrameManageAction::ShowCommandPalette;
	case kManageOpenSettings: return CustomFrameManageAction::OpenSettings;
	case kManageOpenKeyboardShortcuts: return CustomFrameManageAction::OpenKeyboardShortcuts;
	case kManageSelectColorTheme: return CustomFrameManageAction::SelectColorTheme;
	case kManageUpdateCheck: return CustomFrameManageAction::CheckForUpdates;
	case kManageUpdateDownload: return CustomFrameManageAction::DownloadUpdate;
	case kManageUpdateInstall: return CustomFrameManageAction::InstallUpdate;
	case kManageUpdateRestart: return CustomFrameManageAction::RestartToUpdate;
	default: return CustomFrameManageAction::None;
	}
}

//! A popup menu item text composed from a localized label and, optionally, VS Code's
//! keybinding hint. The hint is a key sequence rather than prose, so it stays in code
//! while only the label comes from the message resource. The buffer is owned by the
//! caller because `LS` hands back a rotating static buffer that the next `LS` call
//! overwrites; copying here is what makes several labels safe to build in one menu.
struct MenuItemText
{
	wchar_t text[192]{};

	[[nodiscard]] const wchar_t* c_str() const noexcept { return text; }
};

//! Owns a popup until it is successfully attached to another menu. DestroyMenu
//! recursively releases attached submenus, while an unattached submenu remains
//! owned by this guard on every AppendMenuW failure path.
class ScopedPopupMenu final {
public:
	explicit ScopedPopupMenu(HMENU menu = nullptr) noexcept : m_menu(menu) {}
	~ScopedPopupMenu()
	{
		if (m_menu != nullptr) ::DestroyMenu(m_menu);
	}
	ScopedPopupMenu(const ScopedPopupMenu&) = delete;
	ScopedPopupMenu& operator=(const ScopedPopupMenu&) = delete;

	[[nodiscard]] HMENU get() const noexcept { return m_menu; }
	[[nodiscard]] HMENU release() noexcept { return std::exchange(m_menu, nullptr); }

private:
	HMENU m_menu = nullptr;
};

[[nodiscard]] MenuItemText MakeMenuItemText(int stringId, const wchar_t* accelerator = nullptr) noexcept
{
	MenuItemText item{};
	const wchar_t* const label = LS(stringId);
	if (accelerator == nullptr || accelerator[0] == L'\0') {
		(void)::_snwprintf_s(item.text, _TRUNCATE, L"%s", label);
	} else {
		(void)::_snwprintf_s(item.text, _TRUNCATE, L"%s\t%s", label, accelerator);
	}
	return item;
}

//! Appends VS Code's `MenuId.GlobalActivity` group `7_update`
//! (`contrib/update/browser/update.ts`). Upstream registers eight items there, each
//! gated on `CONTEXT_UPDATE_STATE == '<state>'`, so at most one is visible at a time;
//! `None` is the `disabled`/`uninitialized` case where upstream contributes nothing and
//! the menu must therefore not grow a stray separator either. Upstream localizes these
//! titles through its language packs, so the labels come from the message resource here
//! rather than staying as English literals.
bool AppendUpdateMenuGroup(HMENU menu, CustomFrameUpdateMenuEntry entry) noexcept
{
	UINT command = 0;
	int labelId = 0;
	switch (entry) {
	case CustomFrameUpdateMenuEntry::None: return true;
	case CustomFrameUpdateMenuEntry::Check:
		command = kManageUpdateCheck; labelId = STR_WORKBENCH_MANAGE_UPDATE_CHECK; break;
	case CustomFrameUpdateMenuEntry::Checking: labelId = STR_WORKBENCH_MANAGE_UPDATE_CHECKING; break;
	case CustomFrameUpdateMenuEntry::DownloadNow:
		command = kManageUpdateDownload; labelId = STR_WORKBENCH_MANAGE_UPDATE_DOWNLOAD; break;
	case CustomFrameUpdateMenuEntry::Downloading: labelId = STR_WORKBENCH_MANAGE_UPDATE_DOWNLOADING; break;
	case CustomFrameUpdateMenuEntry::Install:
		command = kManageUpdateInstall; labelId = STR_WORKBENCH_MANAGE_UPDATE_INSTALL; break;
	case CustomFrameUpdateMenuEntry::Updating: labelId = STR_WORKBENCH_MANAGE_UPDATE_UPDATING; break;
	case CustomFrameUpdateMenuEntry::Cancelling: labelId = STR_WORKBENCH_MANAGE_UPDATE_CANCELLING; break;
	case CustomFrameUpdateMenuEntry::Restart:
		command = kManageUpdateRestart; labelId = STR_WORKBENCH_MANAGE_UPDATE_RESTART; break;
	}
	if (labelId == 0) return true;
	const MenuItemText label = MakeMenuItemText(labelId);
	return ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) != FALSE
		&& ::AppendMenuW(menu, command == 0 ? MF_STRING | MF_GRAYED : MF_STRING, command, label.c_str()) != FALSE;
}

[[nodiscard]] RECT PopupWorkAreaForPoint(POINT point) noexcept
{
	RECT workArea{};
	const HMONITOR monitor = ::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo{ sizeof(monitorInfo) };
	if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitorInfo) != FALSE) {
		return monitorInfo.rcWork;
	}
	if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) != FALSE) {
		return workArea;
	}
	workArea = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
	return workArea;
}

//! Native popup menu sizes are not available until the menu is displayed. This
//! conservative estimate is used only to make the explicit work-area clamp agree
//! with the eventual TrackPopupMenuEx placement; the system still performs its own
//! final menu placement using TPM_WORKAREA.
[[nodiscard]] SIZE EstimatePopupMenuSize(HMENU menu, HWND owner, HFONT font, UINT dpi) noexcept
{
	const int effectiveDpi = dpi == 0 ? 96 : static_cast<int>(dpi);
	const int rowHeight = std::max(
		ScaleCustomFrameDip(24, static_cast<UINT>(effectiveDpi)),
		::GetSystemMetricsForDpi(SM_CYMENU, static_cast<UINT>(effectiveDpi)));
	const int separatorHeight = std::max(1, ScaleCustomFrameDip(8, static_cast<UINT>(effectiveDpi)));
	const int horizontalPadding = ScaleCustomFrameDip(48, static_cast<UINT>(effectiveDpi));
	const int acceleratorGap = ScaleCustomFrameDip(20, static_cast<UINT>(effectiveDpi));
	const int submenuArrow = ScaleCustomFrameDip(18, static_cast<UINT>(effectiveDpi));
	SIZE size{
		ScaleCustomFrameDip(220, static_cast<UINT>(effectiveDpi)),
		ScaleCustomFrameDip(32, static_cast<UINT>(effectiveDpi)),
	};
	if (menu == nullptr) return size;

	HDC dc = owner == nullptr ? ::GetDC(nullptr) : ::GetDC(owner);
	HGDIOBJ previous = nullptr;
	if (dc != nullptr && font != nullptr) previous = ::SelectObject(dc, font);
	int measuredHeight = 0;
	int measuredWidth = 0;
	const int itemCount = std::max(0, ::GetMenuItemCount(menu));
	for (int index = 0; index < itemCount; ++index) {
		wchar_t text[256]{};
		MENUITEMINFOW item{ sizeof(item) };
		item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
		item.dwTypeData = text;
		item.cch = static_cast<UINT>(sizeof(text) / sizeof(text[0]) - 1);
		if (::GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &item) == FALSE) {
			measuredHeight += rowHeight;
			continue;
		}
		if ((item.fType & MFT_SEPARATOR) != 0) {
			measuredHeight += separatorHeight;
			continue;
		}

		int labelWidth = 0;
		int acceleratorWidth = 0;
		const wchar_t* const tab = ::wcschr(text, L'\t');
		const int labelLength = tab == nullptr
			? static_cast<int>(::wcslen(text))
			: static_cast<int>(tab - text);
		if (dc != nullptr && labelLength > 0) {
			SIZE extent{};
			if (::GetTextExtentPoint32W(dc, text, labelLength, &extent) != FALSE) {
				labelWidth = extent.cx;
			}
		}
		if (dc != nullptr && tab != nullptr && tab[1] != L'\0') {
			SIZE extent{};
			if (::GetTextExtentPoint32W(dc, tab + 1,
				static_cast<int>(::wcslen(tab + 1)), &extent) != FALSE) {
				acceleratorWidth = extent.cx;
			}
		}
		measuredWidth = std::max(measuredWidth,
			labelWidth + (acceleratorWidth == 0 ? 0 : acceleratorGap + acceleratorWidth)
				+ horizontalPadding + (item.hSubMenu == nullptr ? 0 : submenuArrow));
		measuredHeight += rowHeight;
	}
	if (previous != nullptr) ::SelectObject(dc, previous);
	if (dc != nullptr) {
		if (owner == nullptr) ::ReleaseDC(nullptr, dc);
		else ::ReleaseDC(owner, dc);
	}
	size.cx = static_cast<LONG>(std::max(static_cast<int>(size.cx), measuredWidth));
	size.cy = static_cast<LONG>(std::max(static_cast<int>(size.cy), measuredHeight));
	return size;
}

} // namespace

int ScaleCustomFrameDip(int value, UINT dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi == 0 ? 96 : dpi), 96);
}

RECT CalculateCustomFrameClientRect(
	const RECT& systemFrameClient,
	LONG windowTop,
	bool maximized,
	int resizeHandleHeight
) noexcept
{
	RECT client = systemFrameClient;
	client.top = windowTop;
	if (maximized) {
		// A maximized window's rectangle is one resize handle larger than the work
		// area on every edge, so the restored top must give that handle back. Without
		// it the title bar would be laid out above the top of the monitor.
		client.top += std::max(0, resizeHandleHeight);
	}
	client.top = std::min(client.top, client.bottom);
	return client;
}

std::array<RECT, static_cast<size_t>(CustomFrameResizeEdge::Count)>
CalculateCustomFrameResizeOverlayBounds(
	int clientWidth,
	int clientHeight,
	int resizeBorder,
	bool maximized
) noexcept
{
	std::array<RECT, static_cast<size_t>(CustomFrameResizeEdge::Count)> bounds{};
	if (maximized) return bounds;
	const int width = std::max(0, clientWidth);
	const int height = std::max(0, clientHeight);
	const int border = std::clamp(resizeBorder, 0, std::min(width, height));
	if (border == 0) return bounds;
	// Only the top edge lies inside the client after WM_NCCALCSIZE, so it is the only
	// edge whose initial press a child control can take away from the frame. It owns
	// the whole strip including both corner squares. The left, right, and bottom
	// targets live in the system frame outside the client, where the top-level window
	// already receives WM_NCHITTEST first and no child exists to intercept anything.
	bounds[static_cast<size_t>(CustomFrameResizeEdge::Top)] = MakeRect(0, 0, width, border);
	return bounds;
}

CustomFrameLayout CalculateCustomFrameLayout(
	int clientWidth,
	UINT dpi,
	int preferredMenuWidth,
	int updateButtonWidth,
	bool showActivityBarGlobalActions) noexcept
{
	const int width = std::max(0, clientWidth);
	const int titleHeight = ScaleCustomFrameDip(34, dpi);
	const int systemWidth = ScaleCustomFrameDip(42, dpi);
	const int buttonWidth = ScaleCustomFrameDip(46, dpi);
	const int closeWidth = ScaleCustomFrameDip(48, dpi);
	const int captionPadding = ScaleCustomFrameDip(10, dpi);
	const int titleControlWidth = ScaleCustomFrameDip(30, dpi);
	// Layout, Primary Side Bar, Panel, and Secondary Side Bar are the fixed-width
	// controls. Accounts and Manage are the Activity Bar's GlobalCompositeBar actions;
	// they use the same compact width only when the Activity Bar is projected into the
	// title bar. The Update indicator is measured separately because its width comes
	// from its label, and it is absent entirely unless the update state is actionable.
	const int globalActionCount = showActivityBarGlobalActions ? 2 : 0;
	const int titleControlCount = 4 + globalActionCount;
	const int updateWidth = std::max(0, updateButtonWidth);
	const int buttonsWidth = buttonWidth * 2 + closeWidth;
	const int buttonLeft = std::max(0, width - buttonsWidth);
	const int menuLeft = std::min(width, systemWidth);
	const int titleControlsWidth = titleControlWidth * titleControlCount + updateWidth;
	// Never partially draw a title control or let it overlap the system menu. When
	// an ordinary four-control run would crowd out a readable centred caption, reclaim
	// the run as a whole. Extremely narrow widths cannot provide that caption even after
	// reclaiming the controls, so retain every physically fitting control in that case.
	// An actionable update indicator keeps its own visibility contract: its complete
	// run appears whenever it fits physically.
	const int titleControlsCaptionClearance = ScaleCustomFrameDip(80, dpi);
	const bool titleControlsFit = buttonLeft >= systemWidth + titleControlsWidth;
	const auto centeredCaptionWidthWithControlsAt = [&](int controlsLeft) noexcept {
		const int prospectiveMaximumMenuRight = std::max(menuLeft, controlsLeft - titleControlsCaptionClearance);
		const int prospectiveMenuRight = std::min(
			prospectiveMaximumMenuRight, menuLeft + std::max(0, preferredMenuWidth));
		const int prospectiveCaptionSafeLeft = std::clamp(prospectiveMenuRight + captionPadding, 0, width);
		const int prospectiveCaptionSafeRight = std::clamp(controlsLeft - captionPadding, 0, width);
		const int prospectiveCenteredLeft = std::max(prospectiveCaptionSafeLeft, width - prospectiveCaptionSafeRight);
		const int prospectiveCenteredRight = std::min(prospectiveCaptionSafeRight, width - prospectiveCaptionSafeLeft);
		return std::max(0, prospectiveCenteredRight - prospectiveCenteredLeft);
	};
	const int minimumCenteredCaptionWidth = titleControlWidth + captionPadding * 2;
	const bool reclaimControlsForCaption = updateWidth == 0
		&& titleControlsFit
		&& buttonLeft < systemWidth + titleControlsWidth + titleControlsCaptionClearance
		&& centeredCaptionWidthWithControlsAt(buttonLeft) >= minimumCenteredCaptionWidth;
	const bool showTitleControls = titleControlsFit && !reclaimControlsForCaption;
	const int titleControlsLeft = showTitleControls ? buttonLeft - titleControlsWidth : buttonLeft;
	const int maximumMenuRight = std::max(menuLeft, titleControlsLeft - titleControlsCaptionClearance);
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
		const auto nextControl = [&](int controlWidth) noexcept {
			const RECT rect = MakeRect(controlLeft, 0, controlLeft + controlWidth, titleHeight);
			controlLeft += controlWidth;
			return rect;
		};
		layout.layoutButton = nextControl(titleControlWidth);
		layout.primarySidebarButton = nextControl(titleControlWidth);
		layout.bottomPanelButton = nextControl(titleControlWidth);
		layout.secondarySidebarButton = nextControl(titleControlWidth);
		// A zero measured width means the update state is not actionable. `nextControl`
		// would still produce a degenerate rectangle at the current offset, and an empty
		// RECT is what every hit-test/paint/accessibility path treats as absent, so skip
		// it outright rather than emitting one that starts and ends at the same x.
		if (updateWidth > 0) layout.updateButton = nextControl(updateWidth);
		if (showActivityBarGlobalActions) {
			layout.accountButton = nextControl(titleControlWidth);
			layout.manageButton = nextControl(titleControlWidth);
		}
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

CustomFrameLayout CalculateCustomFrameLayout(
	int clientWidth,
	UINT dpi,
	int preferredMenuWidth,
	bool showActivityBarGlobalActions,
	int updateButtonWidth) noexcept
{
	return CalculateCustomFrameLayout(
		clientWidth, dpi, preferredMenuWidth, updateButtonWidth,
		showActivityBarGlobalActions);
}

int MeasureCustomFrameUpdateButtonWidth(HDC dc, UINT dpi) noexcept
{
	// The label plus VS Code's horizontal action padding on each side, plus the margin
	// that keeps the painted pill clear of the neighbouring compact controls. A minimum
	// keeps the button a button when the caption font cannot be measured at all.
	const int padding = ScaleCustomFrameDip(8, dpi);
	const int margins = ScaleCustomFrameDip(kUpdateIndicatorMarginDip, dpi) * 2;
	const int minimumWidth = ScaleCustomFrameDip(56, dpi) + margins;
	const wchar_t* const label = CustomFrameControlName(CustomFrameControl::Update);
	SIZE extent{};
	if (dc == nullptr
		|| ::GetTextExtentPoint32W(dc, label, static_cast<int>(::wcslen(label)), &extent) == FALSE) {
		return minimumWidth;
	}
	return std::max(minimumWidth, static_cast<int>(extent.cx) + padding * 2 + margins);
}

RECT CustomFrameUpdateIndicatorPillRect(const RECT& actionRect, UINT dpi) noexcept
{
	const int width = actionRect.right - actionRect.left;
	const int height = actionRect.bottom - actionRect.top;
	if (width <= 0 || height <= 0) return {};
	// Inset horizontally by the margin `MeasureCustomFrameUpdateButtonWidth` reserved, and
	// vertically to the button height so the indicator reads as a button sitting on the
	// title bar. A title bar shorter than the button keeps the full height rather than
	// collapsing the pill to nothing.
	const int horizontalMargin = std::min(
		ScaleCustomFrameDip(kUpdateIndicatorMarginDip, dpi), width / 2);
	const int pillHeight = std::min(height, ScaleCustomFrameDip(kUpdateIndicatorHeightDip, dpi));
	const int verticalMargin = (height - pillHeight) / 2;
	const RECT pill = MakeRect(
		actionRect.left + horizontalMargin,
		actionRect.top + verticalMargin,
		actionRect.right - horizontalMargin,
		actionRect.top + verticalMargin + pillHeight
	);
	return pill.right > pill.left && pill.bottom > pill.top ? pill : RECT{};
}

CustomFramePopupPlacement CalculateCustomFramePopupPlacement(
	const RECT& anchorScreen,
	const SIZE& popupSize,
	const RECT& workArea,
	UINT dpi,
	CustomFramePopupPlacementKind kind,
	bool rightAlign
) noexcept
{
	constexpr UINT commonFlags =
		TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_VERTICAL | TPM_WORKAREA;
	const int gap = std::max(1, ScaleCustomFrameDip(kPopupAnchorGapDip, dpi));
	const int width = std::max(0, static_cast<int>(popupSize.cx));
	const int height = std::max(0, static_cast<int>(popupSize.cy));

	CustomFramePopupPlacement placement{};
	placement.flags = commonFlags;
	RECT popup{};
	if (kind == CustomFramePopupPlacementKind::ActivityBar) {
		// GlobalCompositeBar's vertical action is anchored at its right/bottom corner.
		// The horizontal gap separates the menu from the bar while the shared bottom
		// edge keeps the popup visually attached to the action that opened it.
		popup = MakeRect(anchorScreen.right + gap, anchorScreen.bottom - height,
			anchorScreen.right + gap + width, anchorScreen.bottom);
		bool alignRight = false;
		if (!::IsRectEmpty(&workArea) && width > 0) {
			const int leftCandidate = anchorScreen.left - gap - width;
			const bool fitsRight = popup.right <= workArea.right;
			const bool fitsLeft = leftCandidate >= workArea.left;
			if (!fitsRight && fitsLeft) {
				// A vertical bar on the opposite edge still needs a visible menu. Keep
				// the anchor corner and reverse only the horizontal alignment.
				alignRight = true;
				popup.left = leftCandidate;
				popup.right = leftCandidate + width;
			}
		}
		if (alignRight) placement.flags |= TPM_RIGHTALIGN;
		else placement.flags |= TPM_LEFTALIGN;
		placement.flags |= TPM_BOTTOMALIGN;
		ClampPopupRectToWorkArea(popup, workArea);
		placement.point = {
			alignRight ? popup.right : popup.left,
			popup.bottom,
		};
	} else {
		// Title-bar actions are laid out horizontally. Account and Manage share this
		// alignment, as they do in VS Code's GlobalCompositeBar.
		popup = rightAlign
			? MakeRect(anchorScreen.right - gap - width, anchorScreen.bottom + gap,
				anchorScreen.right - gap, anchorScreen.bottom + gap + height)
			: MakeRect(anchorScreen.left + gap, anchorScreen.bottom + gap,
				anchorScreen.left + gap + width, anchorScreen.bottom + gap + height);
		placement.flags |= rightAlign ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
		placement.flags |= TPM_TOPALIGN;
		ClampPopupRectToWorkArea(popup, workArea);
		placement.point = {
			rightAlign ? popup.right : popup.left,
			popup.top,
		};
	}
	placement.bounds = popup;
	return placement;
}

std::wstring SanitizeCustomFrameAccountMenuText(std::wstring_view text)
{
	std::wstring sanitized;
	sanitized.reserve(text.size());
	bool pendingSpace = false;
	for (const wchar_t character : text) {
		if (character == L'\0') {
			// AppendMenuW consumes a null-terminated string. Treat an embedded NUL
			// as a separator so data after it cannot silently merge into the row.
			if (!sanitized.empty()) pendingSpace = true;
			continue;
		}
		if (character == L' ' || character < 0x20
			|| (character >= 0x7F && character <= 0x9F)
			|| character == L'\u2028' || character == L'\u2029') {
			if (!sanitized.empty()) pendingSpace = true;
			continue;
		}
		if (pendingSpace) {
			sanitized.push_back(L' ');
			pendingSpace = false;
		}
		if (character == L'&') {
			// Account labels are data, not menu markup. Win32 interprets a single
			// ampersand as a mnemonic marker, so double it before AppendMenuW.
			sanitized.append(L"&&");
		} else {
			sanitized.push_back(character);
		}
	}
	return sanitized;
}

CustomFrameAccountMenuProjection ProjectCustomFrameAccountMenu(
	const CustomFrameAccountMenuModel& model)
{
	CustomFrameAccountMenuProjection projection{};
	projection.state = model.state;
	switch (model.state) {
	case CustomFrameAccountMenuState::Absent:
		projection.fallbackLabel = SanitizeCustomFrameAccountMenuText(model.absentFallback);
		return projection;
	case CustomFrameAccountMenuState::Loading:
		projection.fallbackLabel = SanitizeCustomFrameAccountMenuText(model.loadingFallback);
		return projection;
	case CustomFrameAccountMenuState::Unavailable:
		projection.fallbackLabel = SanitizeCustomFrameAccountMenuText(model.unavailableFallback);
		return projection;
	case CustomFrameAccountMenuState::Available:
		break;
	default:
		// A value outside the closed enum is malformed producer data. Preserve an
		// explicit terminal state and do not treat it as an available snapshot.
		projection.state = CustomFrameAccountMenuState::Unavailable;
		projection.fallbackLabel = SanitizeCustomFrameAccountMenuText(model.unavailableFallback);
		return projection;
	}

	projection.parents.reserve(model.parents.size());
	for (const auto& parent : model.parents) {
		CustomFrameAccountMenuParent projectedParent;
		projectedParent.label = SanitizeCustomFrameAccountMenuText(parent.label);
		if (projectedParent.label.empty()) continue;
		projectedParent.detailRows.reserve(parent.detailRows.size());
		for (const auto& detail : parent.detailRows) {
			const std::wstring sanitizedDetail = SanitizeCustomFrameAccountMenuText(detail);
			if (!sanitizedDetail.empty()) projectedParent.detailRows.push_back(sanitizedDetail);
		}
		// A parent without a read-only detail row is not a useful Account menu
		// surface. Drop it instead of manufacturing an enabled or empty action.
		if (!projectedParent.detailRows.empty()) {
			projection.parents.push_back(std::move(projectedParent));
		}
	}
	if (!projection.parents.empty()) return projection;

	// An Available snapshot with no complete parent is not an empty success. Make
	// the loss of provider data explicit and use the producer's unavailable copy.
	projection.state = CustomFrameAccountMenuState::Unavailable;
	projection.fallbackLabel = SanitizeCustomFrameAccountMenuText(model.unavailableFallback);
	return projection;
}

CustomFrameControl HitTestCustomFrameControl(const CustomFrameLayout& layout, POINT point) noexcept
{
	for (const CustomFrameControl control : kTitleControls) {
		if (Contains(TitleControlRect(layout, control), point)) return control;
	}
	return CustomFrameControl::None;
}

UINT CustomFrameControlCommand(CustomFrameControl control) noexcept
{
	switch (control) {
	case CustomFrameControl::PrimarySidebar: return F_TOGGLE_LEFT_EXPLORER;
	case CustomFrameControl::BottomPanel: return F_TOGGLE_BOTTOM_PANEL;
	// VS Code's title-bar control toggles the physical Auxiliary Bar
	// (workbench.action.toggleAuxiliaryBar). F_TOGGLE_RIGHT_OUTLINE only expands the
	// Outline View nested in the Primary Side Bar and is not this surface.
	case CustomFrameControl::SecondarySidebar: return F_TOGGLE_SECONDARY_SIDEBAR;
	case CustomFrameControl::None:
	case CustomFrameControl::Layout:
	// The Update indicator has no legacy function code by design: which command it
	// runs depends on the current update state, so it is resolved by the composition
	// root through `WorkbenchCommandRegistry::ResolveUpdateIndicatorCommand` rather
	// than being pinned to one `EFunctionCode` here.
	case CustomFrameControl::Update:
	case CustomFrameControl::Account:
	case CustomFrameControl::Manage:
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
		// The system frame survives WM_NCCALCSIZE on the left, right, and bottom, so
		// those resize targets lie entirely outside the client and arrive here as
		// negative or past-the-edge coordinates. Only the top edge is client-owned,
		// which is why it is the one edge whose band reaches inside. Claiming an inner
		// band on the other three as well would take the outermost pixels of the
		// Activity Bar, the editor, and the status bar, which VS Code leaves fully
		// clickable.
		const bool outerLeft = point.x < 0 && point.x >= -border;
		const bool outerRight = point.x >= width && point.x < width + border;
		const bool outerBottom = point.y >= height && point.y < height + border;
		const bool top = point.y >= -border && point.y < border;
		// A corner target may reach inside horizontally: at the top the caption owns
		// those pixels anyway, and at the bottom the point is already past the client
		// edge.
		const bool cornerLeft = point.x < border && point.x >= -border;
		const bool cornerRight = point.x >= width - border && point.x < width + border;
		if (top && cornerLeft) return HTTOPLEFT;
		if (top && cornerRight) return HTTOPRIGHT;
		if (outerBottom && cornerLeft) return HTBOTTOMLEFT;
		if (outerBottom && cornerRight) return HTBOTTOMRIGHT;
		if (outerLeft) return HTLEFT;
		if (outerRight) return HTRIGHT;
		if (top) return HTTOP;
		if (outerBottom) return HTBOTTOM;
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
	DestroyResizeOverlays();
	m_accessibilityLifetime->Invalidate();
}

void CCustomFrameController::Attach(HWND window, theme::ThemeMode savedMode) noexcept
{
	DestroyResizeOverlays();
	if (!m_accessibilityLifetime->IsAlive()) m_accessibilityLifetime = std::make_shared<accessibility::CustomUiAutomationLifetime>();
	m_window = window;
	for (auto& slot : m_resizeOverlays) {
		slot.owner = this;
		slot.edge = CustomFrameResizeEdge::Top;
	}
	m_savedMode = savedMode;
	m_active = window == nullptr || ::GetActiveWindow() == window;
	RefreshMetrics();
	ApplyDwmFrameAppearance();
}

void CCustomFrameController::Detach() noexcept
{
	DestroyResizeOverlays();
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
	ApplyDwmFrameAppearance();
	InvalidateTitle();
}

void CCustomFrameController::ApplyDwmFrameAppearance() noexcept
{
	if (m_window == nullptr) return;
	const BOOL dark = m_savedMode == theme::ThemeMode::Dark;
	::DwmSetWindowAttribute(m_window, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
	// DWM paints the window border itself, inside the non-client frame that survives
	// WM_NCCALCSIZE. Left alone it uses the system border, which reads brighter than
	// the workbench chrome; the theme's own border color keeps the outline subdued and
	// consistent with every seam the window draws for itself.
	const COLORREF border = m_palette.border.ToColorRef();
	::DwmSetWindowAttribute(m_window, kDwmBorderColor, &border, sizeof(border));
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
	m_layout = CalculateCustomFrameLayout(
		client.right - client.left, m_dpi, menuWidth, MeasureUpdateIndicatorWidth(),
		m_activityBarGlobalActionsInTitleBar);
	m_menuBar.SetBounds(m_layout.menu);
	m_menuBar.UpdateItemLayout(m_window, m_menuFont.Get());
	// A resize can collapse the complete compact run without going through one of
	// the visibility setters. Do not retain hover/press/UIA state for a rectangle
	// that the newly committed layout no longer exposes.
	const auto titleControlVisible = [&](CustomFrameControl control) noexcept {
		const RECT bounds = TitleControlRect(m_layout, control);
		return !::IsRectEmpty(&bounds);
	};
	if (m_hotControl != CustomFrameControl::None && !titleControlVisible(m_hotControl)) {
		m_hotControl = CustomFrameControl::None;
	}
	if (m_pressedControl != CustomFrameControl::None && !titleControlVisible(m_pressedControl)) {
		m_pressedControl = CustomFrameControl::None;
	}
	const CustomFrameControl focusedControl = TitleControlFromNode(m_accessibilityFocusedNode);
	if (focusedControl != CustomFrameControl::None && !titleControlVisible(focusedControl)) {
		ClearAccessibilityFocus();
	}
}

int CCustomFrameController::MeasureUpdateIndicatorWidth() const noexcept
{
	if (!m_updateIndicatorVisible || m_window == nullptr) return 0;
	// The indicator is drawn with the caption font, so it must be measured with the
	// same font; a width measured against the DC's default face would ellipsize or
	// leave a gap the moment the caption font is not the stock system font.
	const HDC dc = ::GetDC(m_window);
	if (dc == nullptr) return MeasureCustomFrameUpdateButtonWidth(nullptr, m_dpi);
	const HFONT previous = static_cast<HFONT>(::SelectObject(dc, m_font.Get()));
	const int measured = MeasureCustomFrameUpdateButtonWidth(dc, m_dpi);
	if (previous != nullptr) (void)::SelectObject(dc, previous);
	::ReleaseDC(m_window, dc);
	return measured;
}

void CCustomFrameController::SetUpdateIndicatorVisible(bool visible) noexcept
{
	if (m_updateIndicatorVisible == visible) return;
	m_updateIndicatorVisible = visible;
	// The indicator disappearing while it is hot or pressed would otherwise leave the
	// frame reporting a control that no longer has a rectangle.
	if (!visible) {
		if (m_hotControl == CustomFrameControl::Update) SetHotControl(CustomFrameControl::None);
		if (m_pressedControl == CustomFrameControl::Update) SetPressedControl(CustomFrameControl::None);
		if (TitleControlFromNode(m_accessibilityFocusedNode) == CustomFrameControl::Update) {
			ClearAccessibilityFocus();
		}
	}
	RefreshLayout();
	InvalidateTitle();
}

void CCustomFrameController::SetActivityBarGlobalActionsInTitleBar(bool visible) noexcept
{
	if (m_activityBarGlobalActionsInTitleBar == visible) return;
	m_activityBarGlobalActionsInTitleBar = visible;
	// A placement change can remove both rectangles while the pointer or the UIA
	// focus still names one of them. Clear those transient states before publishing
	// the new layout so every input and accessibility path agrees on visibility.
	if (!visible) {
		if (m_hotControl == CustomFrameControl::Account
			|| m_hotControl == CustomFrameControl::Manage) {
			SetHotControl(CustomFrameControl::None);
		}
		if (m_pressedControl == CustomFrameControl::Account
			|| m_pressedControl == CustomFrameControl::Manage) {
			SetPressedControl(CustomFrameControl::None);
		}
		const CustomFrameControl focused = TitleControlFromNode(m_accessibilityFocusedNode);
		if (focused == CustomFrameControl::Account || focused == CustomFrameControl::Manage) {
			ClearAccessibilityFocus();
		}
	}
	RefreshLayout();
	InvalidateTitle();
}

void CCustomFrameController::SetActivityBarLocation(
	workbench::ActivityBarLocation location) noexcept
{
	SetActivityBarGlobalActionsInTitleBar(
		location == workbench::ActivityBarLocation::Top
		|| location == workbench::ActivityBarLocation::Bottom);
}

void CCustomFrameController::CreateResizeOverlays() noexcept
{
	if (m_window == nullptr) return;
	const HINSTANCE instance = reinterpret_cast<HINSTANCE>(
		::GetWindowLongPtrW(m_window, GWLP_HINSTANCE));
	if (instance == nullptr || !EnsureResizeOverlayWindowClass(instance)) return;
	for (auto& slot : m_resizeOverlays) {
		if (slot.window != nullptr) continue;
		(void)::CreateWindowExW(
			WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY,
			kResizeOverlayClass,
			L"",
			WS_CHILD | WS_CLIPSIBLINGS,
			0, 0, 0, 0,
			m_window,
			nullptr,
			instance,
			&slot);
	}
}

void CCustomFrameController::LayoutResizeOverlays() noexcept
{
	if (m_window == nullptr) return;
	CreateResizeOverlays();
	RECT client{};
	if (!::GetClientRect(m_window, &client)) return;
	const auto bounds = CalculateCustomFrameResizeOverlayBounds(
		client.right - client.left,
		client.bottom - client.top,
		ResizeBorder(),
		::IsZoomed(m_window) != FALSE);
	for (size_t i = 0; i < m_resizeOverlays.size(); ++i) {
		const HWND overlay = m_resizeOverlays[i].window;
		if (overlay == nullptr) continue;
		const RECT& rect = bounds[i];
		if (rect.right <= rect.left || rect.bottom <= rect.top) {
			::ShowWindow(overlay, SW_HIDE);
			continue;
		}
		::SetWindowPos(overlay, HWND_TOP, rect.left, rect.top,
			rect.right - rect.left, rect.bottom - rect.top,
			SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
}

void CCustomFrameController::DestroyResizeOverlays() noexcept
{
	for (auto& slot : m_resizeOverlays) {
		if (slot.window != nullptr) {
			::DestroyWindow(slot.window);
			slot.window = nullptr;
		}
		slot.owner = nullptr;
	}
}

int CCustomFrameController::ResizeBorder() const noexcept
{
	if (m_window == nullptr) {
		return 0;
	}
	return ::GetSystemMetricsForDpi(SM_CXFRAME, m_physicalDpi)
		+ ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_physicalDpi);
}

int CCustomFrameController::ResizeBorderHeight() const noexcept
{
	if (m_window == nullptr) {
		return 0;
	}
	return ::GetSystemMetricsForDpi(SM_CYFRAME, m_physicalDpi)
		+ ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, m_physicalDpi);
}

LRESULT CALLBACK CCustomFrameController::ResizeOverlayWindowProc(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam)
{
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* slot = static_cast<ResizeOverlaySlot*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(slot));
		if (slot != nullptr) slot->window = window;
	}
	auto* slot = reinterpret_cast<ResizeOverlaySlot*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (slot == nullptr || slot->owner == nullptr) {
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	if (message == WM_NCDESTROY) {
		slot->window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
	const auto resizeCursor = [](LRESULT hit) noexcept -> LPCWSTR {
		switch (hit) {
		case HTTOPLEFT:
		case HTBOTTOMRIGHT: return IDC_SIZENWSE;
		case HTTOPRIGHT:
		case HTBOTTOMLEFT: return IDC_SIZENESW;
		case HTTOP:
		case HTBOTTOM: return IDC_SIZENS;
		case HTLEFT:
		case HTRIGHT: return IDC_SIZEWE;
		default: return IDC_ARROW;
		}
	};
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		::BeginPaint(window, &paint);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_SETCURSOR: {
		POINT screen{};
		const LRESULT hit = ::GetCursorPos(&screen)
			? slot->owner->HitTestScreenPoint(screen)
			: HTNOWHERE;
		::SetCursor(::LoadCursorW(nullptr, resizeCursor(hit)));
		return TRUE;
	}
	case WM_LBUTTONDOWN: {
		POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		(void)::ClientToScreen(window, &screen);
		const LRESULT hit = slot->owner->HitTestScreenPoint(screen);
		if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) {
			return ::SendMessageW(slot->owner->m_window, WM_NCLBUTTONDOWN, hit,
				MAKELPARAM(screen.x, screen.y));
		}
		return 0;
	}
	default:
		return ::DefWindowProcW(window, message, wParam, lParam);
	}
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
	if (control == CustomFrameControl::Update) {
		// A press while the indicator has no rectangle is not reachable through the
		// mouse path, but the accessibility Invoke path can still arrive here.
		if (!m_updateIndicatorVisible || !m_updateIndicatorCallback) return;
		try {
			m_updateIndicatorCallback();
		} catch (...) {
			::OutputDebugStringW(L"Sakura Editor NEXT: Update indicator callback threw.\n");
		}
		return;
	}
	const RECT anchor = TitleControlRect(m_layout, control);
	if (control == CustomFrameControl::Layout) {
		ShowLayoutMenu(anchor);
	} else if (control == CustomFrameControl::Account) {
		ShowAccountMenu(anchor);
	} else if (control == CustomFrameControl::Manage) {
		ShowManageMenu(anchor);
	}
}

void CCustomFrameController::ShowLayoutMenu(const RECT& anchor) noexcept
{
	if (m_window == nullptr || ::IsRectEmpty(&anchor)) return;
	const HMENU menu = ::CreatePopupMenu();
	if (menu == nullptr) return;
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_LEFT_EXPLORER,
		MakeMenuItemText(STR_WORKBENCH_LAYOUT_TOGGLE_PRIMARY_SIDEBAR).c_str());
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_BOTTOM_PANEL,
		MakeMenuItemText(STR_WORKBENCH_LAYOUT_TOGGLE_PANEL).c_str());
	::AppendMenuW(menu, MF_STRING, F_TOGGLE_SECONDARY_SIDEBAR,
		MakeMenuItemText(STR_WORKBENCH_LAYOUT_TOGGLE_SECONDARY_SIDEBAR).c_str());
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
	POINT point{ anchor.right, anchor.bottom };
	::ClientToScreen(m_window, &point);
	ShowAccountMenuAt(point, true);
}

void CCustomFrameController::ShowAccountMenuAt(POINT screenPoint) noexcept
{
	ShowAccountMenuAt(screenPoint, false);
}

void CCustomFrameController::ShowAccountMenuAt(POINT screenPoint, bool titleBar) noexcept
{
	if (m_window == nullptr) return;
	CustomFrameAccountMenuModel model;
	try {
		if (m_accountMenuModelCallback) {
			model = m_accountMenuModelCallback();
		} else {
			model = m_accountMenuModel;
		}
	}
	catch (...) {
		// A provider callback is an optional presentation source. Its failure must
		// not escape this noexcept UI path or turn into an implied sign-in action.
		::OutputDebugStringW(L"Sakura Editor NEXT: Account menu callback threw.\n");
		model = {};
	}

	CustomFrameAccountMenuProjection projection;
	try {
		projection = ProjectCustomFrameAccountMenu(model);
	}
	catch (...) {
		// Allocation failure while copying provider presentation data fails closed
		// to the explicit no-provider row below.
		::OutputDebugStringW(L"Sakura Editor NEXT: Account menu projection failed.\n");
		projection = {};
	}

	ScopedPopupMenu menu(::CreatePopupMenu());
	if (menu.get() == nullptr) return;
	bool menuBuilt = true;
	if (projection.parents.empty()) {
		const wchar_t* fallback = projection.fallbackLabel.empty()
			? LS(STR_WORKBENCH_ACCOUNT_NO_PROVIDER)
			: projection.fallbackLabel.c_str();
		if (fallback == nullptr) {
			// The legacy no-provider resource remains the final localized fallback
			// for an absent or malformed snapshot. Loading/unavailable producers pass
			// their own localized text through the model above.
			fallback = L"";
		}
		menuBuilt = ::AppendMenuW(menu.get(), MF_STRING | MF_GRAYED, 0, fallback) != FALSE;
	} else {
		for (const auto& parent : projection.parents) {
			ScopedPopupMenu submenu(::CreatePopupMenu());
			if (submenu.get() == nullptr) {
				menuBuilt = false;
				break;
			}
			for (const auto& detail : parent.detailRows) {
				// Account rows are descriptive only. They intentionally have no
				// command id and cannot initiate sign-in/sign-out or other actions.
				if (::AppendMenuW(submenu.get(), MF_STRING | MF_GRAYED, 0,
					detail.c_str()) == FALSE) {
					menuBuilt = false;
					break;
				}
			}
			if (!menuBuilt) break;
			if (::AppendMenuW(menu.get(), MF_POPUP,
				reinterpret_cast<UINT_PTR>(submenu.get()), parent.label.c_str()) == FALSE) {
				menuBuilt = false;
				break;
			}
			// Ownership transfers to the parent menu only after AppendMenuW succeeds.
			(void)submenu.release();
		}
	}
	if (!menuBuilt) return;

	const SIZE popupSize = EstimatePopupMenuSize(menu.get(), m_window, m_menuFont.Get(), m_dpi);
	const RECT anchor{ screenPoint.x, screenPoint.y, screenPoint.x, screenPoint.y };
	const auto placement = CalculateCustomFramePopupPlacement(
		anchor, popupSize, PopupWorkAreaForPoint(screenPoint), m_dpi,
		titleBar ? CustomFramePopupPlacementKind::TitleBar : CustomFramePopupPlacementKind::ActivityBar,
		titleBar);
	(void)::TrackPopupMenuEx(menu.get(), placement.flags,
		placement.point.x, placement.point.y, m_window, nullptr);
}

void CCustomFrameController::ShowManageMenu(const RECT& anchor) noexcept
{
	if (m_window == nullptr || ::IsRectEmpty(&anchor)) return;
	POINT point{ anchor.right, anchor.bottom };
	::ClientToScreen(m_window, &point);
	ShowManageMenuAt(point, true);
}

void CCustomFrameController::ShowManageMenuAt(POINT screenPoint, bool rightAlign) noexcept
{
	if (m_window == nullptr) return;
	const HMENU menu = ::CreatePopupMenu();
	const HMENU themes = ::CreatePopupMenu();
	if (menu == nullptr || themes == nullptr) {
		if (themes != nullptr) ::DestroyMenu(themes);
		if (menu != nullptr) ::DestroyMenu(menu);
		return;
	}
	// Each label is copied into its own `MenuItemText` before the next one is built,
	// because `LS` returns a rotating static buffer that a later call would overwrite.
	const bool menuItemsAppended = ::AppendMenuW(menu, MF_STRING, kManageShowCommandPalette,
		MakeMenuItemText(STR_WORKBENCH_MANAGE_COMMAND_PALETTE, L"Ctrl+Shift+P").c_str()) != FALSE
		&& ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) != FALSE
		&& ::AppendMenuW(menu, MF_STRING, kManageOpenSettings,
			MakeMenuItemText(STR_WORKBENCH_MANAGE_SETTINGS, L"Ctrl+,").c_str()) != FALSE
		&& ::AppendMenuW(menu, MF_STRING, kManageOpenKeyboardShortcuts,
			MakeMenuItemText(STR_WORKBENCH_MANAGE_KEYBOARD_SHORTCUTS, L"Ctrl+K Ctrl+S").c_str()) != FALSE
		&& ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr) != FALSE
		&& ::AppendMenuW(themes, MF_STRING, kManageSelectColorTheme,
			MakeMenuItemText(STR_WORKBENCH_COLOR_THEME, L"Ctrl+K Ctrl+T").c_str()) != FALSE;
	const bool themesAttached = menuItemsAppended
		&& ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themes),
			MakeMenuItemText(STR_WORKBENCH_MANAGE_THEMES).c_str()) != FALSE;
	if (!themesAttached || !AppendUpdateMenuGroup(menu, m_updateMenuEntry)) {
		// An attached submenu is owned by its parent, so destroying it again after
		// `DestroyMenu(menu)` would be a double free. Only an unattached one is ours.
		if (!themesAttached) ::DestroyMenu(themes);
		::DestroyMenu(menu);
		return;
	}
	const SIZE popupSize = EstimatePopupMenuSize(menu, m_window, m_menuFont.Get(), m_dpi);
	const RECT anchor{ screenPoint.x, screenPoint.y, screenPoint.x, screenPoint.y };
	const auto placement = CalculateCustomFramePopupPlacement(
		anchor, popupSize, PopupWorkAreaForPoint(screenPoint), m_dpi,
		rightAlign ? CustomFramePopupPlacementKind::TitleBar : CustomFramePopupPlacementKind::ActivityBar,
		rightAlign);
	const UINT command = ::TrackPopupMenuEx(
		menu, placement.flags, placement.point.x, placement.point.y, m_window, nullptr);
	::DestroyMenu(menu);
	const auto action = ManageActionFromMenuCommand(command);
	if (action == CustomFrameManageAction::None || !m_manageMenuActionCallback) return;
	try {
		m_manageMenuActionCallback(action);
	}
	catch (...) {
		::OutputDebugStringW(L"Sakura Editor NEXT: Manage menu action callback threw.\n");
	}
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
	case WM_NCCALCSIZE: {
		// The client is extended over the caption only. Everything else stays with the
		// system frame, because DWM paints the window border and rounds the corners
		// inside the frame region that survives here; a client sized to the whole
		// window rectangle leaves no frame and therefore neither border nor corners.
		if (lParam == 0) {
			result = 0;
			return true;
		}
		RECT* const client = wParam != FALSE
			? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0]
			: reinterpret_cast<RECT*>(lParam);
		const LONG windowTop = client->top;
		const LRESULT systemResult = ::DefWindowProcW(m_window, message, wParam, lParam);
		if (systemResult != 0) {
			// A non-zero result carries client-preservation flags the system owns; the
			// rectangles it produced then belong to it, not to this extension.
			result = systemResult;
			return true;
		}
		*client = CalculateCustomFrameClientRect(
			*client, windowTop, ::IsZoomed(m_window) != FALSE, ResizeBorderHeight());
		result = 0;
		return true;
	}
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
		LayoutResizeOverlays();
		InvalidateTitle();
		result = 0;
		return true;
	case WM_SIZE:
		RefreshLayout();
		LayoutResizeOverlays();
		InvalidateTitle();
		return false;
	case WM_SHOWWINDOW:
		// The border color does not survive the window's first appearance when it is
		// only asked for at WM_CREATE time, so ask again as the window is shown.
		if (wParam != FALSE) ApplyDwmFrameAppearance();
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
		// A system theme change resets the DWM attributes to their defaults, so dark
		// mode and the border color have to be asked for again.
		ApplyDwmFrameAppearance();
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
	return 3 + VisibleTitleControlCount() + (m_menuBar.AccessibilityItemCount() > 0 ? 1 : 0);
}

int CCustomFrameController::VisibleTitleControlCount() const noexcept
{
	// Counted from the rectangles rather than from a fixed number, because the Update
	// indicator is present only while the update state is actionable and the whole set
	// collapses on a narrow window.
	int count = 0;
	for (const CustomFrameControl control : kTitleControls) {
		const RECT bounds = TitleControlRect(m_layout, control);
		if (!::IsRectEmpty(&bounds)) ++count;
	}
	return count;
}

CustomFrameControl CCustomFrameController::VisibleTitleControlAt(int index) const noexcept
{
	if (index < 0) return CustomFrameControl::None;
	int remaining = index;
	for (const CustomFrameControl control : kTitleControls) {
		const RECT bounds = TitleControlRect(m_layout, control);
		if (::IsRectEmpty(&bounds)) continue;
		if (remaining == 0) return control;
		--remaining;
	}
	return CustomFrameControl::None;
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
	const int titleControlCount = VisibleTitleControlCount();
	if (childIndex < titleControlCount) {
		return TitleControlNode(VisibleTitleControlAt(childIndex));
	}
	childIndex -= titleControlCount;
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
