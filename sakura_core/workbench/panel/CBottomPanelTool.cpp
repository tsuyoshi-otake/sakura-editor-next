/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/panel/CBottomPanelTool.h"
#include "CSelectLang.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include <CommCtrl.h>
#include <Richedit.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>

namespace workbench::panel {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraBottomPanel";
constexpr UINT_PTR kTerminalButton = 100;
constexpr UINT_PTR kProblemsButton = 101;
constexpr UINT_PTR kOutputButton = 102;
constexpr UINT_PTR kProblemsList = 105;
constexpr UINT_PTR kOutputSelector = 106;
constexpr UINT_PTR kOutputText = 107;
constexpr UINT_PTR kPanelMaximizeButton = 108;
constexpr UINT_PTR kPanelCloseButton = 109;
constexpr unsigned int kDefaultDpi = 96;
//! Panel header metrics in DIP. Layout and paint must read the same values:
//! the header rule is the header row's last device rows, so a painter that
//! derives the row from its own copy of the metric can miss the row entirely.
constexpr int kPanelHeaderHeightDip = 34;
constexpr int kOutputSelectorHeightDip = 28;
constexpr int kHeaderRuleThicknessDip = 1;

bool IsPanelTabId(UINT_PTR id) noexcept
{
	return id == kTerminalButton || id == kProblemsButton || id == kOutputButton;
}

bool IsPanelActionId(UINT_PTR id) noexcept
{
	return id == kPanelMaximizeButton || id == kPanelCloseButton;
}

int Scale(int value, unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi ? dpi : kDefaultDpi), kDefaultDpi);
}

//! Fills the header's bottom rule across one cell of the header row.
//!
//! VS Code paints that edge as part of the title row and lets
//! `panelTitle.activeBorder` replace it under the active tab. The Panel host is
//! a WS_CLIPCHILDREN window and every tab and action button is one of its
//! children, so a rule filled once across the parent's client area is clipped
//! out of every child rectangle and survives only in the gaps between them.
//! Each painter therefore fills its own span, and all of them derive the row
//! from the same clamped header height so the spans meet exactly.
void FillHeaderRule(HDC dc, const RECT& cell, int headerHeight, unsigned int dpi, COLORREF color)
{
	if (dc == nullptr || cell.right <= cell.left) return;
	const int thickness = std::min(std::max(0, headerHeight),
		std::max(1, Scale(kHeaderRuleThicknessDip, dpi)));
	if (thickness <= 0) return;
	RECT rule{ cell.left, cell.bottom - thickness, cell.right, cell.bottom };
	rule.top = std::max(rule.top, cell.top);
	if (rule.top >= rule.bottom) return;
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush == nullptr) return;
	::FillRect(dc, &rule, brush);
	::DeleteObject(brush);
}

std::string_view ContainerIdForButtonId(UINT_PTR id) noexcept
{
	switch (id) {
	case kProblemsButton: return containerIds::Problems;
	case kOutputButton: return containerIds::Output;
	case kTerminalButton: return containerIds::Terminal;
	default: return {};
	}
}

std::optional<std::string_view> ContainerIdForTab(const BottomPanelTab tab) noexcept
{
	switch (tab) {
	case BottomPanelTab::Problems: return containerIds::Problems;
	case BottomPanelTab::Output: return containerIds::Output;
	case BottomPanelTab::Terminal: return containerIds::Terminal;
	}
	return std::nullopt;
}

std::optional<BottomPanelTab> TabForContainerId(const std::string_view containerId) noexcept
{
	if (containerId == containerIds::Problems) return BottomPanelTab::Problems;
	if (containerId == containerIds::Output) return BottomPanelTab::Output;
	if (containerId == containerIds::Terminal) return BottomPanelTab::Terminal;
	return std::nullopt;
}

std::optional<std::string_view> CanonicalSupportedContainerId(
	const std::string_view containerId) noexcept
{
	if (containerId == containerIds::Problems) return containerIds::Problems;
	if (containerId == containerIds::Output) return containerIds::Output;
	if (containerId == containerIds::Terminal) return containerIds::Terminal;
	return std::nullopt;
}

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	windowClass.lpfnWndProc = CBottomPanelTool::WindowProc;
	windowClass.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

const wchar_t* SeverityText(const win32::EProblemsPanelSeverity severity) noexcept
{
	switch (severity) {
	case win32::EProblemsPanelSeverity::Error: return LS(STR_WORKBENCH_PANEL_SEVERITY_ERROR);
	case win32::EProblemsPanelSeverity::Warning: return LS(STR_WORKBENCH_PANEL_SEVERITY_WARNING);
	case win32::EProblemsPanelSeverity::Information: return LS(STR_WORKBENCH_PANEL_SEVERITY_INFO);
	case win32::EProblemsPanelSeverity::Hint: return LS(STR_WORKBENCH_PANEL_SEVERITY_HINT);
	}
	return L"";
}

constexpr std::array<UINT, 4> kProblemsColumnTextIds{
	STR_WORKBENCH_PANEL_COLUMN_SEVERITY,
	STR_WORKBENCH_PANEL_COLUMN_MESSAGE,
	STR_WORKBENCH_PANEL_COLUMN_FILE,
	STR_WORKBENCH_PANEL_COLUMN_SOURCE,
};

void RefreshProblemsColumnTitles(HWND list) noexcept
{
	if (list == nullptr) return;
	for (int index = 0; index < static_cast<int>(kProblemsColumnTextIds.size()); ++index) {
		LVCOLUMNW column{};
		column.mask = LVCF_TEXT;
		column.pszText = const_cast<wchar_t*>(LS(kProblemsColumnTextIds[static_cast<std::size_t>(index)]));
		(void)ListView_SetColumn(list, index, &column);
	}
}

} // namespace

struct CBottomPanelTool::Impl {
	explicit Impl(terminal::TerminalTabManagerDependencies dependencies,
		std::shared_ptr<viewcontainer::IViewContainerPageHostService> retainedPages)
		: terminal(std::make_unique<terminal::CTerminalTool>(std::move(dependencies)))
		, sharedPages(std::move(retainedPages))
	{
	}

	std::unique_ptr<terminal::CTerminalTool> terminal;
	std::shared_ptr<viewcontainer::IViewContainerPageHostService> sharedPages;
	HWND window = nullptr;
	HWND terminalButton = nullptr;
	HWND problemsButton = nullptr;
	HWND outputButton = nullptr;
	HWND problemsList = nullptr;
	HWND outputSelector = nullptr;
	HWND outputText = nullptr;
	HWND maximizeButton = nullptr;
	HWND closeButton = nullptr;
	RECT terminalHeaderBounds{};
	RECT bounds{};
	unsigned int dpi = kDefaultDpi;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont font;
	HBRUSH panelBrush = nullptr;
	HBRUSH raisedBrush = nullptr;
	rendering::CGdiBackBuffer backBuffer;
	std::string activeContainerId{ containerIds::Terminal };
	std::optional<std::string> attachedContainerId;
	ProblemActivationCallback problemActivation;
	OutputChannelSelectionCallback outputChannelSelection;
	ContainerSelectionCallback containerSelection;
	win32::ProblemsPanelSnapshot problems;
	win32::OutputPanelSnapshot outputs;
	std::optional<std::string> selectedOutputChannelId;
	CBottomPanelTool::PanelActions panelActions;
	bool closed = false;

	void ApplyFont(HWND control) const
	{
		if (control && font.Get()) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), FALSE);
	}

	void DestroyBrushes() noexcept
	{
		if (panelBrush) ::DeleteObject(panelBrush);
		if (raisedBrush) ::DeleteObject(raisedBrush);
		panelBrush = nullptr;
		raisedBrush = nullptr;
	}

	void RecreateBrushes()
	{
		DestroyBrushes();
		panelBrush = ::CreateSolidBrush(palette.bottomPanel.ToColorRef());
		raisedBrush = ::CreateSolidBrush(palette.raised.ToColorRef());
	}

	void ApplyControlPalette()
	{
		if (problemsList) {
			ListView_SetBkColor(problemsList, palette.bottomPanel.ToColorRef());
			ListView_SetTextBkColor(problemsList, palette.bottomPanel.ToColorRef());
			ListView_SetTextColor(problemsList, palette.primaryText.ToColorRef());
			::InvalidateRect(problemsList, nullptr, FALSE);
		}
		if (outputText) {
			::SendMessageW(outputText, EM_SETBKGNDCOLOR, TRUE, palette.bottomPanel.ToColorRef());
			::InvalidateRect(outputText, nullptr, FALSE);
		}
		if (outputSelector) ::InvalidateRect(outputSelector, nullptr, FALSE);
	}

	void DrawOwnerButton(const DRAWITEMSTRUCT& item) const
	{
		if (item.hDC == nullptr) return;
		const UINT_PTR id = static_cast<UINT_PTR>(item.CtlID);
		const bool action = IsPanelActionId(id);
		const bool tab = IsPanelTabId(id);
		const std::string_view buttonContainerId = ContainerIdForButtonId(id);
		const bool disabled = (item.itemState & ODS_DISABLED) != 0;
		const bool activeTab = tab && activeContainerId == buttonContainerId;
		const bool pressed = (item.itemState & ODS_SELECTED) != 0;
		const HBRUSH background = (activeTab || pressed) ? raisedBrush : panelBrush;
		if (background) ::FillRect(item.hDC, &item.rcItem, background);
		// The button's own height is the header height: LayoutChildren places
		// every header cell at the top of the row with the row's full height.
		FillHeaderRule(item.hDC, item.rcItem, item.rcItem.bottom - item.rcItem.top, dpi,
			palette.border.ToColorRef());

		if (action) {
			const int centerX = (item.rcItem.left + item.rcItem.right) / 2;
			const int centerY = (item.rcItem.top + item.rcItem.bottom) / 2;
			const int half = Scale(5, dpi);
			const COLORREF iconColor = palette.secondaryText.ToColorRef();
			const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scale(1, dpi)), iconColor);
			const HGDIOBJ previousPen = pen ? ::SelectObject(item.hDC, pen) : nullptr;
			if (id == kPanelCloseButton) {
				::MoveToEx(item.hDC, centerX - half, centerY - half, nullptr);
				::LineTo(item.hDC, centerX + half + 1, centerY + half + 1);
				::MoveToEx(item.hDC, centerX + half, centerY - half, nullptr);
				::LineTo(item.hDC, centerX - half - 1, centerY + half + 1);
			} else {
				const bool maximized = panelActions.isMaximized && panelActions.isMaximized();
				const RECT outer{ centerX - half, centerY - half, centerX + half + 1, centerY + half + 1 };
				if (maximized) {
					::Rectangle(item.hDC, outer.left + Scale(2, dpi), outer.top,
						outer.right, outer.bottom - Scale(2, dpi));
					::Rectangle(item.hDC, outer.left, outer.top + Scale(2, dpi),
						outer.right - Scale(2, dpi), outer.bottom);
				} else {
					::Rectangle(item.hDC, outer.left, outer.top, outer.right, outer.bottom);
				}
			}
			if (previousPen) ::SelectObject(item.hDC, previousPen);
			if (pen) ::DeleteObject(pen);
			return;
		}

		// The terminal button is the stable Panel tab label. The active session
		// title belongs to the terminal's own tab/list presentation, not to the
		// physical Panel Part label.
		wchar_t fallback[128]{};
		::GetWindowTextW(item.hwndItem, fallback, static_cast<int>(std::size(fallback)));
		const std::wstring label = fallback;
		::SetBkMode(item.hDC, TRANSPARENT);
		::SetTextColor(item.hDC, disabled ? palette.disabledText.ToColorRef()
			: activeTab ? palette.primaryText.ToColorRef() : palette.secondaryText.ToColorRef());
		RECT text = item.rcItem;
		text.left += Scale(12, dpi);
		text.right -= Scale(8, dpi);
		::DrawTextW(item.hDC, label.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
		if (activeTab) {
			RECT underline = item.rcItem;
			underline.left += Scale(8, dpi);
			underline.right -= Scale(8, dpi);
			underline.top = std::max(underline.top, underline.bottom - Scale(2, dpi));
			const HBRUSH accentBrush = ::CreateSolidBrush(palette.accent.ToColorRef());
			if (accentBrush) {
				::FillRect(item.hDC, &underline, accentBrush);
				::DeleteObject(accentBrush);
			}
		}
		if ((item.itemState & ODS_FOCUS) != 0) {
			RECT focus = item.rcItem;
			::InflateRect(&focus, -Scale(2, dpi), -Scale(2, dpi));
			::DrawFocusRect(item.hDC, &focus);
		}
	}

	void DrawOutputItem(const DRAWITEMSTRUCT& item) const
	{
		if (item.hDC == nullptr) return;
		const bool selected = (item.itemState & ODS_SELECTED) != 0;
		const HBRUSH background = selected ? raisedBrush : panelBrush;
		if (background) ::FillRect(item.hDC, &item.rcItem, background);
		wchar_t label[256]{};
		int itemIndex = item.itemID == static_cast<UINT>(-1)
			? static_cast<int>(::SendMessageW(outputSelector, CB_GETCURSEL, 0, 0))
			: static_cast<int>(item.itemID);
		if (itemIndex >= 0) {
			::SendMessageW(outputSelector, CB_GETLBTEXT, itemIndex, reinterpret_cast<LPARAM>(label));
		}
		::SetBkMode(item.hDC, TRANSPARENT);
		::SetTextColor(item.hDC, palette.primaryText.ToColorRef());
		RECT text = item.rcItem;
		text.left += Scale(8, dpi);
		text.right -= Scale(24, dpi);
		::DrawTextW(item.hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
	}

	[[nodiscard]] bool IsActivePageAttached() const noexcept
	{
		if (!attachedContainerId || *attachedContainerId != activeContainerId) return false;
		return !IsSharedPage(activeContainerId) || OwnsSharedPage(activeContainerId);
	}

	void DeactivatePage(const std::string_view containerId) noexcept
	{
		if (containerId == containerIds::Terminal) terminal->Deactivate();
		else if (IsSharedPage(containerId)) sharedPages->DeactivatePage(containerId);
	}

	[[nodiscard]] bool IsSharedPage(const std::string_view containerId) const noexcept
	{
		return sharedPages != nullptr && sharedPages->IsUsable()
			&& sharedPages->SupportsLocation(containerId,
				layout::EViewContainerLocation::Panel);
	}

	[[nodiscard]] bool SupportsContainer(const std::string_view containerId) const noexcept
	{
		return CanonicalSupportedContainerId(containerId).has_value()
			|| IsSharedPage(containerId);
	}

	[[nodiscard]] bool OwnsSharedPage(const std::string_view containerId) const noexcept
	{
		return IsSharedPage(containerId) && window != nullptr
			&& sharedPages->AttachedHost(containerId) == window;
	}

	[[nodiscard]] std::optional<viewcontainer::ViewContainerPageHost> SharedPageHost() const noexcept
	{
		if (window == nullptr || !::IsWindow(window)) return std::nullopt;
		try {
			return viewcontainer::ViewContainerPageHost{
				std::string(layout::ids::part::Panel),
				layout::EViewContainerLocation::Panel,
				reinterpret_cast<viewcontainer::ViewContainerNativeHandle>(window),
			};
		} catch (...) {
			return std::nullopt;
		}
	}

	[[nodiscard]] bool AttachSharedPage(const std::string_view containerId) noexcept
	{
		const auto host = SharedPageHost();
		if (!host || !IsSharedPage(containerId)) return false;
		const auto result = sharedPages->Attach(containerId, *host);
		return result.Succeeded() && OwnsSharedPage(containerId);
	}

	[[nodiscard]] bool DetachSharedPage(const std::string_view containerId) noexcept
	{
		if (!OwnsSharedPage(containerId)) return true;
		(void)sharedPages->Detach(containerId);
		return !OwnsSharedPage(containerId);
	}

	bool ApplyActiveContainer(const std::string_view requestedContainerId)
	{
		if (closed) return false;
		if (!SupportsContainer(requestedContainerId)) return false;
		if (activeContainerId != requestedContainerId) activeContainerId = requestedContainerId;
		const bool showTerminal = activeContainerId == containerIds::Terminal;
		::SendMessageW(terminalButton, BM_SETSTATE, showTerminal, 0);
		::SendMessageW(problemsButton, BM_SETSTATE,
			activeContainerId == containerIds::Problems, 0);
		::SendMessageW(outputButton, BM_SETSTATE,
			activeContainerId == containerIds::Output, 0);
		LayoutChildren();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
		return true;
	}

	EBottomPanelPageAttachStatus AttachActivePage() noexcept
	{
		if (closed) return EBottomPanelPageAttachStatus::Closed;
		if (IsActivePageAttached()) return EBottomPanelPageAttachStatus::AlreadyAttached;
		std::optional<std::string> previous;
		try {
			previous = attachedContainerId;
		} catch (...) {
			return EBottomPanelPageAttachStatus::Failed;
		}
		if (previous) {
			DeactivatePage(*previous);
			if (IsSharedPage(*previous) && !DetachSharedPage(*previous)) {
				return EBottomPanelPageAttachStatus::Failed;
			}
			attachedContainerId.reset();
		}
		if (IsSharedPage(activeContainerId) && !AttachSharedPage(activeContainerId)) {
			if (previous) {
				const bool restored = !IsSharedPage(*previous) || AttachSharedPage(*previous);
				if (restored) attachedContainerId = std::move(*previous);
			}
			LayoutChildren();
			return EBottomPanelPageAttachStatus::Failed;
		}
		try {
			attachedContainerId = activeContainerId;
		} catch (...) {
			if (IsSharedPage(activeContainerId)) (void)DetachSharedPage(activeContainerId);
			return EBottomPanelPageAttachStatus::Failed;
		}
		LayoutChildren();
		return EBottomPanelPageAttachStatus::Attached;
	}

	EBottomPanelPageDetachStatus DetachActivePage() noexcept
	{
		if (closed) return EBottomPanelPageDetachStatus::Closed;
		if (!attachedContainerId) return EBottomPanelPageDetachStatus::AlreadyDetached;
		DeactivatePage(*attachedContainerId);
		if (IsSharedPage(*attachedContainerId) && !DetachSharedPage(*attachedContainerId)) {
			return EBottomPanelPageDetachStatus::Failed;
		}
		attachedContainerId.reset();
		LayoutChildren();
		return EBottomPanelPageDetachStatus::Detached;
	}

	void LayoutChildren()
	{
		if (!window) return;
		const int width = std::max(0L, bounds.right - bounds.left);
		const int height = std::max(0L, bounds.bottom - bounds.top);
		const auto vertical = CalculateBottomPanelVerticalLayout(
			height, Scale(kPanelHeaderHeightDip, dpi), Scale(kOutputSelectorHeightDip, dpi));
		const int headerHeight = vertical.headerHeight;
		const int actionWidth = Scale(30, dpi);
		::SetWindowPos(window, nullptr, bounds.left, bounds.top, width, height,
			SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS);
		struct Placement {
			HWND child{};
			int left{};
			int top{};
			int width{};
			int height{};
			bool visible = true;
		};
		std::array<Placement, 9> placements{};
		std::size_t placementCount = 0;
		const auto place = [&placements, &placementCount](HWND child, int left, int top, int childWidth,
			int childHeight, bool visible = true) {
			if (child == nullptr || placementCount >= placements.size()) return;
			placements[placementCount++] = {
				child, left, top, std::max(0, childWidth), std::max(0, childHeight), visible };
		};

		int commonActionLeft = width;
		const auto placeAction = [&](HWND button, bool visible) {
			if (!button) return;
			if (visible) {
				commonActionLeft = std::max(0, commonActionLeft - actionWidth);
				place(button, commonActionLeft, 0,
					std::min(actionWidth, std::max(0, width - commonActionLeft)), headerHeight, true);
			} else {
				place(button, 0, 0, 0, 0, false);
			}
		};
		placeAction(closeButton, static_cast<bool>(panelActions.closePanel));
		placeAction(maximizeButton, static_cast<bool>(panelActions.toggleMaximize));

		// Keep the terminal actions in the same physical row as the view-container
		// tabs. The toolbar gets a bounded trailing region; the tabs retain a compact
		// intrinsic width and leave the middle as intentional breathing room.
		constexpr std::array desiredTabWidthsDip{ 82, 70, 78 };
		constexpr int desiredTabWidthDipTotal = 230;
		const int minimumTabArea = std::min(commonActionLeft, Scale(250, dpi));
		const int desiredToolbarWidth = Scale(260, dpi);
		const int toolbarWidth = std::min(desiredToolbarWidth,
			std::max(0, commonActionLeft - minimumTabArea));
		const int toolbarLeft = std::max(0, commonActionLeft - toolbarWidth);
		terminalHeaderBounds = toolbarWidth > 0 && headerHeight > 0
			? RECT{ toolbarLeft, 0, commonActionLeft, headerHeight } : RECT{};

		// VS Code's Panel view-container order, matching desiredTabWidthsDip.
		const std::array tabButtons{
			problemsButton, outputButton, terminalButton,
		};
		const int compactTabArea = std::min(toolbarLeft, Scale(desiredTabWidthDipTotal, dpi));
		int remainingWidth = compactTabArea;
		int remainingTabs = static_cast<int>(tabButtons.size());
		int tabLeft = 0;
		for (std::size_t index = 0; index < tabButtons.size(); ++index) {
			const int desired = Scale(desiredTabWidthsDip[index], dpi);
			const int tabWidth = toolbarLeft >= Scale(desiredTabWidthDipTotal, dpi)
				? desired
				: remainingTabs > 0 ? remainingWidth / remainingTabs : 0;
			place(tabButtons[index], tabLeft, 0, std::max(0, tabWidth), headerHeight);
			tabLeft += tabWidth;
			remainingWidth = std::max(0, remainingWidth - tabWidth);
			--remainingTabs;
		}

		const auto pageLayout = CalculateBottomPanelPageLayout(width, vertical);
		const RECT content = pageLayout.wrapperBounds;
		if (sharedPages && sharedPages->IsUsable()) {
			for (const auto& containerId : sharedPages->PageIds()) {
				if (sharedPages->AttachedHost(containerId) != window) continue;
				if (IsActivePageAttached() && containerId == activeContainerId) continue;
				sharedPages->SetPageVisible(containerId, false);
			}
		}
		place(problemsList, 0, vertical.contentTop, width, vertical.contentHeight,
			IsActivePageAttached() && activeContainerId == containerIds::Problems);
		place(outputSelector, 0, vertical.contentTop, width, vertical.outputSelectorHeight,
			IsActivePageAttached() && activeContainerId == containerIds::Output);
		place(outputText, 0, vertical.contentTop + vertical.outputSelectorHeight, width,
			vertical.contentHeight - vertical.outputSelectorHeight,
			IsActivePageAttached() && activeContainerId == containerIds::Output);
		// CTerminalTool::Layout uses SWP_SHOWWINDOW for its own root. Keep an
		// inactive terminal hidden in this transaction and only invoke that layout
		// after the chrome transaction when Terminal is the committed tab.
		if (!IsActivePageAttached() || activeContainerId != containerIds::Terminal) {
			place(terminal->GetHwnd(), 0, vertical.contentTop, width, vertical.contentHeight, false);
		}
		const UINT positionFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOCOPYBITS;
		HDWP positions = ::BeginDeferWindowPos(static_cast<int>(placementCount));
		bool committed = false;
		if (positions != nullptr) {
			bool buildSucceeded = true;
			for (std::size_t index = 0; index < placementCount; ++index) {
				const auto& item = placements[index];
				const UINT visibility = item.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
				positions = ::DeferWindowPos(positions, item.child, nullptr, item.left, item.top,
					item.width, item.height, positionFlags | visibility);
				if (positions == nullptr) {
					buildSucceeded = false;
					break;
				}
			}
			if (buildSucceeded) committed = ::EndDeferWindowPos(positions) != FALSE;
		}
		if (!committed) {
			// A failed HDWP is not a partially committed layout. Reapply every
			// final geometry/visibility with redraw suppressed, then publish one
			// invalidation below. This keeps the fallback out of the paint path.
			for (std::size_t index = 0; index < placementCount; ++index) {
				const auto& item = placements[index];
				const UINT visibility = item.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
				::SetWindowPos(item.child, nullptr, item.left, item.top, item.width, item.height,
					positionFlags | visibility);
			}
		}
		if (IsActivePageAttached() && activeContainerId == containerIds::Terminal) {
			terminal->Layout(content, dpi);
		} else if (IsActivePageAttached() && IsSharedPage(activeContainerId)) {
			sharedPages->LayoutPageProjection(activeContainerId,
				pageLayout.wrapperBounds, pageLayout.contentBounds, dpi);
			sharedPages->SetPageVisible(activeContainerId, true);
			sharedPages->NotifyPageLayout(activeContainerId);
		}
		::RedrawWindow(window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
	}

	void RefreshProblems()
	{
		ListView_DeleteAllItems(problemsList);
		for (std::size_t index = 0; index < problems.entries.size(); ++index) {
			const auto& problem = problems.entries[index];
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(index);
			item.pszText = const_cast<wchar_t*>(SeverityText(problem.severity));
			ListView_InsertItem(problemsList, &item);
			ListView_SetItemText(problemsList, static_cast<int>(index), 1,
				const_cast<wchar_t*>(problem.message.c_str()));
			ListView_SetItemText(problemsList, static_cast<int>(index), 2,
				const_cast<wchar_t*>(problem.location.c_str()));
			ListView_SetItemText(problemsList, static_cast<int>(index), 3,
				const_cast<wchar_t*>(problem.source.c_str()));
		}
		std::wstring label = LS(STR_WORKBENCH_PANEL_PROBLEMS);
		if (!problems.entries.empty()) label += L" (" + std::to_wstring(problems.entries.size()) + L")";
		::SetWindowTextW(problemsButton, label.c_str());
	}

	void SelectOutput(int index)
	{
		if (index < 0 || static_cast<std::size_t>(index) >= outputs.channels.size()) {
			selectedOutputChannelId.reset();
			::SetWindowTextW(outputText, L"");
			return;
		}
		selectedOutputChannelId = outputs.channels[static_cast<std::size_t>(index)].channelId;
		::SendMessageW(outputSelector, CB_SETCURSEL, index, 0);
		::SetWindowTextW(outputText, outputs.channels[static_cast<std::size_t>(index)].projectedText.c_str());
	}

	void SelectOutputForSnapshot()
	{
		const auto findChannel = [this](const std::string& channelId) {
			return std::find_if(outputs.channels.begin(), outputs.channels.end(), [&](const auto& channel) {
				return channel.channelId == channelId;
			});
		};
		if (outputs.activeChannelId && findChannel(*outputs.activeChannelId) != outputs.channels.end()) {
			selectedOutputChannelId = outputs.activeChannelId;
			return;
		}
		if (selectedOutputChannelId && findChannel(*selectedOutputChannelId) != outputs.channels.end()) return;
		if (!outputs.channels.empty()) {
			selectedOutputChannelId = outputs.channels.front().channelId;
		} else {
			selectedOutputChannelId.reset();
		}
	}

	void RefreshOutputs()
	{
		::SendMessageW(outputSelector, CB_RESETCONTENT, 0, 0);
		int selected = -1;
		for (std::size_t index = 0; index < outputs.channels.size(); ++index) {
			::SendMessageW(outputSelector, CB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(outputs.channels[index].label.c_str()));
			if (selectedOutputChannelId && outputs.channels[index].channelId == *selectedOutputChannelId) {
				selected = static_cast<int>(index);
			}
		}
		SelectOutput(selected);
	}

	void RestoreOutputSelection()
	{
		if (window && !closed) RefreshOutputs();
	}

	bool RequestOutputSelection(const std::string& channelId) noexcept
	{
		const auto selected = std::find_if(outputs.channels.begin(), outputs.channels.end(), [&](const auto& channel) {
			return channel.channelId == channelId;
		});
		if (selected == outputs.channels.end()) {
			RestoreOutputSelection();
			return false;
		}
		if (selectedOutputChannelId && *selectedOutputChannelId == channelId) {
			RestoreOutputSelection();
			return true;
		}
		if (outputChannelSelection) {
			try {
				if (!outputChannelSelection(channelId)) {
					RestoreOutputSelection();
					return false;
				}
			}
			catch (...) {
				RestoreOutputSelection();
				return false;
			}
		}
		// The callback is the model owner. Its accepted request is projected back
		// through SetOutputSnapshot; applying here would create a second, optimistic
		// authority and could briefly display a value the model rejected later.
		if (!outputChannelSelection) {
			SelectOutput(static_cast<int>(std::distance(outputs.channels.begin(), selected)));
		}
		return true;
	}

	void Refresh()
	{
		if (!window || closed) return;
		RefreshProblems();
		RefreshOutputs();
	}
};

CBottomPanelTool::CBottomPanelTool(terminal::TerminalTabManagerDependencies terminalDependencies,
	std::shared_ptr<viewcontainer::IViewContainerPageHostService> sharedPages)
	: m_impl(std::make_unique<Impl>(std::move(terminalDependencies), std::move(sharedPages))) {}
CBottomPanelTool::~CBottomPanelTool() { Close(); }

bool CBottomPanelTool::Create(HWND parent)
{
	if (!m_impl || m_impl->closed || m_impl->window || !parent) return false;
	const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (!EnsureClass(instance)) return false;
	m_impl->window = ::CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		0, 0, 0, 0, parent, nullptr, instance, this);
	if (!m_impl->window) return false;
	if (!m_impl->terminal->Create(m_impl->window)) {
		::DestroyWindow(m_impl->window);
		m_impl->window = nullptr;
		return false;
	}
	m_impl->terminal->SetPanelHeaderHost(m_impl->window);
	(void)m_impl->AttachActivePage();
	return true;
}

void CBottomPanelTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->bounds = contentRect;
	m_impl->dpi = dpi ? dpi : kDefaultDpi;
	if (m_impl->font.Dpi() != m_impl->dpi) {
		(void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
		m_impl->ApplyFont(m_impl->terminalButton);
		m_impl->ApplyFont(m_impl->problemsButton);
		m_impl->ApplyFont(m_impl->outputButton);
		m_impl->ApplyFont(m_impl->problemsList);
		m_impl->ApplyFont(m_impl->outputSelector);
		m_impl->ApplyFont(m_impl->outputText);
		m_impl->ApplyFont(m_impl->maximizeButton);
		m_impl->ApplyFont(m_impl->closeButton);
	}
	m_impl->LayoutChildren();
}

void CBottomPanelTool::Activate()
{
	if (!m_impl || m_impl->closed || !m_impl->IsActivePageAttached()) return;
	if (m_impl->activeContainerId == containerIds::Terminal) m_impl->terminal->Activate();
	else if (m_impl->activeContainerId == containerIds::Problems) ::SetFocus(m_impl->problemsList);
	else if (m_impl->activeContainerId == containerIds::Output) ::SetFocus(m_impl->outputText);
	else if (m_impl->IsSharedPage(m_impl->activeContainerId)) {
		m_impl->sharedPages->ActivatePage(m_impl->activeContainerId);
	}
}

void CBottomPanelTool::Deactivate()
{
	if (m_impl && !m_impl->closed && m_impl->IsActivePageAttached()) {
		m_impl->DeactivatePage(m_impl->activeContainerId);
	}
}

bool CBottomPanelTool::PreTranslateMessage(MSG& message)
{
	if (!m_impl || m_impl->closed || !m_impl->IsActivePageAttached()) return false;
	if (m_impl->activeContainerId == containerIds::Terminal) {
		return m_impl->terminal->PreTranslateMessage(message);
	}
	return m_impl->IsSharedPage(m_impl->activeContainerId)
		&& m_impl->sharedPages->PreTranslatePage(m_impl->activeContainerId, message);
}

void CBottomPanelTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	if (m_impl->attachedContainerId) {
		m_impl->DeactivatePage(*m_impl->attachedContainerId);
		if (m_impl->IsSharedPage(*m_impl->attachedContainerId)) {
			(void)m_impl->DetachSharedPage(*m_impl->attachedContainerId);
		}
	}
	m_impl->attachedContainerId.reset();
	m_impl->closed = true;
	m_impl->problemActivation = {};
	m_impl->outputChannelSelection = {};
	m_impl->containerSelection = {};
	m_impl->problems = {};
	m_impl->outputs = {};
	m_impl->selectedOutputChannelId.reset();
	m_impl->panelActions = {};
	m_impl->terminal->SetPanelHeaderHost(nullptr);
	m_impl->terminal->Close();
	if (m_impl->window) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->backBuffer.Reset();
	m_impl->DestroyBrushes();
}

terminal::CTerminalTool* CBottomPanelTool::Terminal() noexcept
{
	return m_impl ? m_impl->terminal.get() : nullptr;
}

void CBottomPanelTool::SetPalette(const theme::ThemePalette& palette)
{
	if (!m_impl) return;
	m_impl->palette = palette;
	m_impl->RecreateBrushes();
	m_impl->terminal->SetPalette(palette);
	m_impl->ApplyControlPalette();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CBottomPanelTool::SetProblemsSnapshot(win32::ProblemsPanelSnapshot snapshot)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->problems = std::move(snapshot);
	m_impl->Refresh();
}

void CBottomPanelTool::SetOutputSnapshot(win32::OutputPanelSnapshot snapshot)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->outputs = std::move(snapshot);
	m_impl->SelectOutputForSnapshot();
	m_impl->Refresh();
}

void CBottomPanelTool::SetProblemActivationCallback(ProblemActivationCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->problemActivation = std::move(callback);
}

void CBottomPanelTool::SetOutputChannelSelectionCallback(OutputChannelSelectionCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->outputChannelSelection = std::move(callback);
}

void CBottomPanelTool::SetContainerSelectionCallback(ContainerSelectionCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->containerSelection = std::move(callback);
}

void CBottomPanelTool::SetTabSelectionCallback(TabSelectionCallback callback)
{
	if (!m_impl || m_impl->closed) return;
	if (!callback) {
		m_impl->containerSelection = {};
		return;
	}
	m_impl->containerSelection = [callback = std::move(callback)](
		const std::string_view containerId) {
		const auto tab = TabForContainerId(containerId);
		return tab && callback(*tab);
	};
}

void CBottomPanelTool::SetPanelActions(PanelActions actions)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->panelActions = std::move(actions);
	m_impl->LayoutChildren();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CBottomPanelTool::Refresh() { if (m_impl) m_impl->Refresh(); }
void CBottomPanelTool::RefreshStrings()
{
	if (!m_impl || m_impl->closed) return;
	if (m_impl->terminalButton) ::SetWindowTextW(m_impl->terminalButton, LS(STR_WORKBENCH_PANEL_TERMINAL));
	if (m_impl->outputButton) ::SetWindowTextW(m_impl->outputButton, LS(STR_WORKBENCH_PANEL_OUTPUT));
	RefreshProblemsColumnTitles(m_impl->problemsList);
	m_impl->RefreshProblems();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}
bool CBottomPanelTool::ApplyActiveContainer(const std::string_view containerId)
{
	return m_impl && m_impl->ApplyActiveContainer(containerId);
}

bool CBottomPanelTool::SupportsContainer(const std::string_view containerId) const noexcept
{
	return m_impl && !m_impl->closed && m_impl->SupportsContainer(containerId);
}

bool CBottomPanelTool::RequestContainerSelection(const std::string_view containerId) noexcept
{
	if (!m_impl || m_impl->closed) return false;
	if (!m_impl->SupportsContainer(containerId)) return false;
	if (m_impl->containerSelection) {
		try {
			// A valid user request is delivered exactly once. Selection, attachment,
			// activation, and focus wait for their distinct committed paths.
			return m_impl->containerSelection(containerId);
		}
		catch (...) {
			return false;
		}
	}
	try {
		return m_impl->ApplyActiveContainer(containerId);
	}
	catch (...) {
		return false;
	}
}

std::string_view CBottomPanelTool::ActiveContainerId() const noexcept
{
	return m_impl ? m_impl->activeContainerId : std::string_view{};
}

EBottomPanelPageAttachStatus CBottomPanelTool::AttachActivePage() noexcept
{
	return m_impl ? m_impl->AttachActivePage() : EBottomPanelPageAttachStatus::Closed;
}

EBottomPanelPageDetachStatus CBottomPanelTool::DetachActivePage() noexcept
{
	return m_impl ? m_impl->DetachActivePage() : EBottomPanelPageDetachStatus::Closed;
}

std::optional<std::string_view> CBottomPanelTool::AttachedContainerId() const noexcept
{
	if (!m_impl || !m_impl->attachedContainerId) return std::nullopt;
	return std::string_view(*m_impl->attachedContainerId);
}

void CBottomPanelTool::SetActiveTab(const BottomPanelTab tab)
{
	if (const auto containerId = ContainerIdForTab(tab)) {
		(void)ApplyActiveContainer(*containerId);
	}
}

bool CBottomPanelTool::RequestTabSelection(const BottomPanelTab tab) noexcept
{
	const auto containerId = ContainerIdForTab(tab);
	return containerId && RequestContainerSelection(*containerId);
}

void CBottomPanelTool::ShowProblems() { (void)ApplyActiveContainer(containerIds::Problems); }
void CBottomPanelTool::ShowOutput() { (void)ApplyActiveContainer(containerIds::Output); }

BottomPanelTab CBottomPanelTool::ActiveTab() const noexcept
{
	const auto tab = m_impl ? TabForContainerId(m_impl->activeContainerId) : std::nullopt;
	return tab.value_or(BottomPanelTab::Terminal);
}

bool CBottomPanelTool::RequestOutputChannelSelection(const std::string& channelId) noexcept
{
	return m_impl && !m_impl->closed && m_impl->RequestOutputSelection(channelId);
}

std::optional<std::string> CBottomPanelTool::SelectedOutputChannelId() const
{
	return m_impl ? m_impl->selectedOutputChannelId : std::nullopt;
}

LRESULT CALLBACK CBottomPanelTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* self = reinterpret_cast<CBottomPanelTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<CBottomPanelTool*>(create->lpCreateParams);
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
		if (self && self->m_impl) self->m_impl->window = window;
	}
	if (!self || !self->m_impl) return ::DefWindowProcW(window, message, wParam, lParam);
	auto& impl = *self->m_impl;
	if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE
		|| message == WM_MOUSELEAVE || message == WM_CAPTURECHANGED || message == WM_SETCURSOR
		|| message == WM_RBUTTONUP)
		&& impl.terminal
		&& impl.terminal->HandlePanelHeaderMessage(message, wParam, lParam,
			impl.terminalHeaderBounds, impl.dpi)) {
		return 0;
	}
	switch (message) {
	case WM_CREATE: {
		const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(window, GWLP_HINSTANCE));
		impl.RecreateBrushes();
		const DWORD tabStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
		impl.terminalButton = ::CreateWindowExW(0, L"BUTTON", LS(STR_WORKBENCH_PANEL_TERMINAL), tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kTerminalButton), instance, nullptr);
		impl.problemsButton = ::CreateWindowExW(0, L"BUTTON", LS(STR_WORKBENCH_PANEL_PROBLEMS), tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsButton), instance, nullptr);
		impl.outputButton = ::CreateWindowExW(0, L"BUTTON", LS(STR_WORKBENCH_PANEL_OUTPUT), tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputButton), instance, nullptr);
		const DWORD actionStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
		impl.maximizeButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPanelMaximizeButton), instance, nullptr);
		impl.closeButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPanelCloseButton), instance, nullptr);
		impl.problemsList = ::CreateWindowExW(0, WC_LISTVIEWW, L"",
			WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsList), instance, nullptr);
		ListView_SetExtendedListViewStyle(impl.problemsList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		const std::array<std::pair<UINT, int>, 4> columns{
			std::pair{ STR_WORKBENCH_PANEL_COLUMN_SEVERITY, 90 },
			std::pair{ STR_WORKBENCH_PANEL_COLUMN_MESSAGE, 360 },
			std::pair{ STR_WORKBENCH_PANEL_COLUMN_FILE, 320 },
			std::pair{ STR_WORKBENCH_PANEL_COLUMN_SOURCE, 120 },
		};
		for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
			LVCOLUMNW column{};
			column.mask = LVCF_TEXT | LVCF_WIDTH;
			column.pszText = const_cast<wchar_t*>(LS(columns[static_cast<std::size_t>(index)].first));
			column.cx = columns[static_cast<std::size_t>(index)].second;
			ListView_InsertColumn(impl.problemsList, index, &column);
		}
		impl.outputSelector = ::CreateWindowExW(0, L"COMBOBOX", L"",
			WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputSelector), instance, nullptr);
		impl.outputText = ::CreateWindowExW(0, L"EDIT", L"",
			WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputText), instance, nullptr);
		impl.ApplyControlPalette();
		(void)impl.ApplyActiveContainer(containerIds::Terminal);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case kTerminalButton: (void)self->RequestContainerSelection(containerIds::Terminal); return 0;
		case kProblemsButton: (void)self->RequestContainerSelection(containerIds::Problems); return 0;
		case kOutputButton: (void)self->RequestContainerSelection(containerIds::Output); return 0;
		case kPanelMaximizeButton:
			if (HIWORD(wParam) == BN_CLICKED && impl.panelActions.toggleMaximize) {
				impl.panelActions.toggleMaximize();
			}
			return 0;
		case kPanelCloseButton:
			if (HIWORD(wParam) == BN_CLICKED && impl.panelActions.closePanel) {
				impl.panelActions.closePanel();
			}
			return 0;
		case kOutputSelector:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				const auto selected = static_cast<int>(::SendMessageW(impl.outputSelector, CB_GETCURSEL, 0, 0));
				if (selected >= 0 && static_cast<std::size_t>(selected) < impl.outputs.channels.size()) {
					(void)self->RequestOutputChannelSelection(impl.outputs.channels[static_cast<std::size_t>(selected)].channelId);
				} else {
					impl.RefreshOutputs();
				}
			}
			return 0;
		}
		break;
	case WM_DRAWITEM:
		if (lParam != 0) {
			const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
			const int saved = item.hDC ? ::SaveDC(item.hDC) : 0;
			if (IsPanelTabId(item.CtlID) || IsPanelActionId(item.CtlID)) {
				impl.DrawOwnerButton(item);
				if (saved != 0) ::RestoreDC(item.hDC, saved);
				return TRUE;
			}
			if (item.CtlID == kOutputSelector) {
				impl.DrawOutputItem(item);
				if (saved != 0) ::RestoreDC(item.hDC, saved);
				return TRUE;
			}
			if (saved != 0) ::RestoreDC(item.hDC, saved);
		}
		break;
	case WM_MEASUREITEM:
		if (wParam == kOutputSelector && lParam != 0) {
			auto& measure = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
			measure.itemHeight = static_cast<UINT>(Scale(26, impl.dpi));
			return TRUE;
		}
		break;
	case WM_CTLCOLORBTN:
	case WM_CTLCOLORLISTBOX:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetBkColor(dc, impl.palette.bottomPanel.ToColorRef());
		::SetTextColor(dc, impl.palette.primaryText.ToColorRef());
		return reinterpret_cast<LRESULT>(impl.panelBrush);
	}
	case WM_NOTIFY: {
		const auto* header = reinterpret_cast<const NMHDR*>(lParam);
		if (header && header->idFrom == kProblemsList && header->code == NM_DBLCLK && impl.problemActivation) {
			const int selected = ListView_GetNextItem(impl.problemsList, -1, LVNI_SELECTED);
			if (selected >= 0 && static_cast<std::size_t>(selected) < impl.problems.entries.size()) {
				impl.problemActivation(impl.problems.entries[static_cast<std::size_t>(selected)]);
			}
			return 0;
		}
		break;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC target = ::BeginPaint(window, &paint);
		if (target == nullptr) return 0;
		RECT client{};
		::GetClientRect(window, &client);
		const int width = std::max(0L, client.right - client.left);
		const int height = std::max(0L, client.bottom - client.top);
		const bool buffered = width > 0 && height > 0
			&& impl.backBuffer.Ensure(target, width, height);
		const HDC dc = buffered ? impl.backBuffer.Dc() : target;
		const auto paintSurface = [&](HDC surface, const RECT& fillRegion) {
			if (surface == nullptr) return;
			const int saved = ::SaveDC(surface);
			const HBRUSH brush = ::CreateSolidBrush(impl.palette.bottomPanel.ToColorRef());
			if (brush != nullptr) {
				::FillRect(surface, &fillRegion, brush);
				::DeleteObject(brush);
			}
			// Clamp with the same layout the children were placed with. The preferred
			// metric alone would draw the rule into the content area while the Panel
			// is shorter than its header during a live resize.
			const int headerHeight = CalculateBottomPanelVerticalLayout(client.bottom - client.top,
				Scale(kPanelHeaderHeightDip, impl.dpi),
				Scale(kOutputSelectorHeightDip, impl.dpi)).headerHeight;
			if (impl.terminalHeaderBounds.right > impl.terminalHeaderBounds.left) {
				impl.terminal->PaintPanelHeader(surface, impl.terminalHeaderBounds, impl.dpi);
			}
			// The host only reaches the gaps between children; each child fills the
			// rest of the rule from its own owner-draw pass.
			const RECT headerRow{ client.left, client.top, client.right, client.top + headerHeight };
			FillHeaderRule(surface, headerRow, headerHeight, impl.dpi, impl.palette.border.ToColorRef());
			if (saved != 0) ::RestoreDC(surface, saved);
		};
		paintSurface(dc, buffered ? client : paint.rcPaint);
		// The persistent target is repainted as a whole, but only the region
		// invalidated by BeginPaint needs to cross the target DC. This avoids a
		// full-client BitBlt on every child invalidation during live resize.
		if (buffered && !impl.backBuffer.Present(target, paint.rcPaint)) {
			// BitBlt can fail after a display/driver transition. The paint DC is
			// still valid here, so recover synchronously without exposing the
			// back-buffer failure as a stale frame.
			paintSurface(target, paint.rcPaint);
		}
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_NCDESTROY:
		impl.backBuffer.Reset();
		impl.DestroyBrushes();
		impl.window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::panel
