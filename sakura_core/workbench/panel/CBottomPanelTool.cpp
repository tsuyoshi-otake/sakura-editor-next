/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/panel/CBottomPanelTool.h"
#include "CSelectLang.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/explorer/ExplorerFileIcon.h"
#include "workbench/icons/CSetiFont.h"
#include "workbench/icons/SetiFileIcon.h"
#include "workbench/icons/SetiIconPainter.h"
#include "workbench/rendering/CGdiBackBuffer.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/viewcontainer/ViewPaneChrome.h"

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
constexpr UINT_PTR kProblemsTree = 105;
constexpr UINT_PTR kOutputSelector = 106;
constexpr UINT_PTR kOutputText = 107;
constexpr UINT_PTR kPanelMaximizeButton = 108;
constexpr UINT_PTR kPanelCloseButton = 109;
constexpr UINT_PTR kProblemsFilterEdit = 110;
constexpr UINT_PTR kProblemsFilterBadge = 111;
constexpr UINT_PTR kProblemsFilterButton = 112;
constexpr UINT_PTR kProblemsCollapseAllButton = 113;
constexpr UINT_PTR kProblemsEmptyMessage = 114;
constexpr UINT_PTR kProblemsClearFilters = 115;
constexpr unsigned int kDefaultDpi = 96;
//! Panel header metrics in DIP. Layout and paint must read the same values:
//! the header rule is the header row's last device rows, so a painter that
//! derives the row from its own copy of the metric can miss the row entirely.
constexpr int kPanelHeaderHeightDip = 34;
constexpr int kOutputSelectorHeightDip = 28;
constexpr int kHeaderRuleThicknessDip = 1;
//! VS Code's Problems tree row height (VirtualDelegate.LINE_HEIGHT) and the
//! `.viewpane-filter-container` body row height when the filter leaves the title.
constexpr int kProblemsRowHeightDip = 22;
constexpr int kProblemsFilterRowHeightDip = 44;
constexpr int kProblemsFilterWidthDipThreshold = 600;
constexpr int kProblemsFilterHeightDipThreshold = 100;

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

bool IsProblemsViewActionId(UINT_PTR id) noexcept
{
	return id == kProblemsFilterButton || id == kProblemsCollapseAllButton;
}

//! Row identity stored on each HTREEITEM's lParam: index into Impl::problemsRows.
enum class ProblemsRowKind : std::uint8_t { Resource, Marker, RelatedInformation };
struct ProblemsRowRef {
	HTREEITEM item{};
	ProblemsRowKind kind{ ProblemsRowKind::Resource };
	std::size_t resource{};
	std::size_t marker{};
	std::size_t related{};
};

[[nodiscard]] COLORREF SeverityIconColor(const theme::ThemePalette& palette,
	const win32::EProblemsPanelSeverity severity) noexcept
{
	switch (severity) {
	case win32::EProblemsPanelSeverity::Error: return palette.danger.ToColorRef();
	case win32::EProblemsPanelSeverity::Warning: return palette.warning.ToColorRef();
	case win32::EProblemsPanelSeverity::Information: return palette.accent.ToColorRef();
	case win32::EProblemsPanelSeverity::Hint: return palette.secondaryText.ToColorRef();
	}
	return palette.secondaryText.ToColorRef();
}

[[nodiscard]] std::wstring_view SeverityCodicon(const win32::EProblemsPanelSeverity severity) noexcept
{
	switch (severity) {
	case win32::EProblemsPanelSeverity::Error: return L"error";
	case win32::EProblemsPanelSeverity::Warning: return L"warning";
	case win32::EProblemsPanelSeverity::Information: return L"info";
	case win32::EProblemsPanelSeverity::Hint: return L"info";
	}
	return L"info";
}

[[nodiscard]] std::wstring MarkerLocationText(const win32::ProblemsPanelRange& range)
{
	return L"[Ln " + std::to_wstring(static_cast<std::uint64_t>(range.startLine) + 1)
		+ L", Col " + std::to_wstring(static_cast<std::uint64_t>(range.startColumn) + 1) + L"]";
}

void DrawTextRuns(HDC dc, RECT bounds, const std::wstring& primary, COLORREF primaryColor,
	const std::wstring& secondary, COLORREF secondaryColor, int gapPx)
{
	if (dc == nullptr || bounds.right <= bounds.left) return;
	const auto previousColor = ::SetTextColor(dc, primaryColor);
	RECT left = bounds;
	if (!secondary.empty()) {
		SIZE primarySize{}, secondarySize{};
		::GetTextExtentPoint32W(dc, primary.data(), static_cast<int>(primary.size()), &primarySize);
		::GetTextExtentPoint32W(dc, secondary.data(), static_cast<int>(secondary.size()), &secondarySize);
		const int available = bounds.right - bounds.left;
		const int primaryWidth = std::max(0, std::min(static_cast<int>(primarySize.cx),
			available - std::min(static_cast<int>(secondarySize.cx) + gapPx, available / 3)));
		left.right = left.left + primaryWidth;
	}
	::DrawTextW(dc, primary.data(), static_cast<int>(primary.size()), &left,
		DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (secondary.empty() || left.right >= bounds.right) {
		::SetTextColor(dc, previousColor);
		return;
	}
	::SetTextColor(dc, secondaryColor);
	RECT right = bounds;
	right.left = std::min(bounds.right, left.right + gapPx);
	::DrawTextW(dc, secondary.data(), static_cast<int>(secondary.size()), &right,
		DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
	::SetTextColor(dc, previousColor);
}

//! Replaces `{0}` and `{1}` in a localized message.
[[nodiscard]] std::wstring FormatProblemsMessage(std::wstring text, std::wstring_view first,
	std::wstring_view second)
{
	const auto replace = [&text](std::wstring_view token, std::wstring_view value) {
		const std::size_t position = text.find(token);
		if (position != std::wstring::npos) text.replace(position, token.size(), value);
	};
	replace(L"{0}", first);
	replace(L"{1}", second);
	return text;
}

//! VS Code's count badge: a rounded pill drawn right after the resource label.
int DrawCountBadge(HDC dc, int left, int top, int rowHeight, const std::wstring& text,
	const theme::ThemePalette& palette, HFONT font)
{
	if (dc == nullptr || text.empty()) return left;
	const auto previousFont = font ? ::SelectObject(dc, font) : nullptr;
	SIZE size{};
	::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
	const int height = std::max(1, rowHeight - 6);
	const int width = std::max(height, static_cast<int>(size.cx) + 12);
	RECT pill{ left, top + (rowHeight - height) / 2, left + width, top + (rowHeight + height) / 2 };
	const HBRUSH brush = ::CreateSolidBrush(palette.raised.ToColorRef());
	const HBRUSH previousBrush = brush ? static_cast<HBRUSH>(::SelectObject(dc, brush)) : nullptr;
	const HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border.ToColorRef());
	const HPEN previousPen = pen ? static_cast<HPEN>(::SelectObject(dc, pen)) : nullptr;
	::RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, height, height);
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, palette.primaryText.ToColorRef());
	::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	if (previousPen) ::SelectObject(dc, previousPen);
	if (pen) ::DeleteObject(pen);
	if (previousBrush) ::SelectObject(dc, previousBrush);
	if (brush) ::DeleteObject(brush);
	if (previousFont) ::SelectObject(dc, previousFont);
	return pill.right;
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
	HWND problemsTree = nullptr;
	HWND problemsFilterEdit = nullptr;
	HWND problemsFilterBadge = nullptr;
	HWND problemsFilterButton = nullptr;
	HWND problemsCollapseAllButton = nullptr;
	HWND problemsEmptyMessage = nullptr;
	HWND problemsClearFilters = nullptr;
	HWND problemsTooltip = nullptr;
	HWND outputSelector = nullptr;
	HWND outputText = nullptr;
	HWND maximizeButton = nullptr;
	HWND closeButton = nullptr;
	RECT terminalHeaderBounds{};
	RECT problemsHeaderBounds{};
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
	win32::ProblemsViewTree problemsView;
	win32::ProblemsViewFilterState problemsFilter;
	controls::COverlayScrollbar problemsScrollbar;
	std::vector<ProblemsRowRef> problemsRows;
	std::optional<std::wstring> problemsActiveResource;
	bool problemsAutoReveal = true;
	//! `problems.action.showMultilineMessage`/`showSinglelineMessage` state;
	//! upstream's MarkersView multiline default is true.
	bool problemsMultiline = true;
	bool problemsFilterInHeader = true;
	//! Set while the tool itself rewrites the filter input, so the resulting
	//! EN_CHANGE is not mistaken for the user typing.
	bool problemsFilterUpdating = false;
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
		if (problemsTree) {
			TreeView_SetBkColor(problemsTree, palette.bottomPanel.ToColorRef());
			TreeView_SetTextColor(problemsTree, palette.primaryText.ToColorRef());
			problemsScrollbar.SetColors(controls::ResolveOverlayScrollbarColors(palette,
				palette.bottomPanel));
			problemsScrollbar.SetDpi(dpi);
			problemsScrollbar.Update();
			::InvalidateRect(problemsTree, nullptr, FALSE);
		}
		for (const HWND control : { problemsFilterEdit, problemsFilterBadge, problemsFilterButton,
			problemsCollapseAllButton, problemsEmptyMessage, problemsClearFilters }) {
			if (control) ::InvalidateRect(control, nullptr, FALSE);
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
		const bool viewAction = IsProblemsViewActionId(id);
		const bool activeTab = tab && activeContainerId == buttonContainerId;
		const bool pressed = (item.itemState & ODS_SELECTED) != 0;
		const HBRUSH background = (activeTab || pressed || viewAction) ? raisedBrush : panelBrush;
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

		if (viewAction) {
			const int centerX = (item.rcItem.left + item.rcItem.right) / 2;
			const int centerY = (item.rcItem.top + item.rcItem.bottom) / 2;
			const int side = Scale(16, dpi);
			const std::wstring_view glyph = id == kProblemsFilterButton ? L"filter" : L"collapse-all";
			viewcontainer::PaintViewPaneIcon(item.hDC,
				{ centerX - side / 2, centerY - side / 2, centerX + (side + 1) / 2, centerY + (side + 1) / 2 },
				glyph, disabled ? palette.disabledText.ToColorRef() : palette.secondaryText.ToColorRef());
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
		// VS Code activityBadge = NumberBadge(errors + warnings + infos), absent at
		// zero. The badge sits right after the "Problems" label.
		if (id == kProblemsButton && problemsView.totalMarkers > 0) {
			SIZE labelSize{};
			::GetTextExtentPoint32W(item.hDC, label.c_str(), -1, &labelSize);
			const auto countText = std::to_wstring(problemsView.totalMarkers);
			DrawCountBadge(item.hDC, text.left + std::min<int>(labelSize.cx, text.right - text.left)
				+ Scale(6, dpi), item.rcItem.top, item.rcItem.bottom - item.rcItem.top,
				countText, palette, font.Get());
		}
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
		std::array<Placement, 17> placements{};
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
		// VS Code's view-title area: the Problems view's filter widget and
		// Collapse All action share the same physical row as the tabs, using the
		// region the terminal toolbar uses when Terminal is the active page.
		problemsHeaderBounds = RECT{ toolbarLeft, 0, commonActionLeft, headerHeight };

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
		// Problems view chrome. Upstream's FilterViewPane keeps the filter widget
		// in the view title while the pane is wide enough; below 600x100 it drops
		// into a 44px row at the top of the body.
		const bool problemsActive = IsActivePageAttached() && activeContainerId == containerIds::Problems;
		problemsFilterInHeader = !(width < Scale(kProblemsFilterWidthDipThreshold, dpi)
			&& vertical.contentHeight > Scale(kProblemsFilterHeightDipThreshold, dpi));
		const int viewActionWidth = Scale(28, dpi);
		const int filterBadgeWidth = problemsView.visibleMarkers != problemsView.totalMarkers
			? Scale(110, dpi) : 0;
		const int filterRowHeight = Scale(kProblemsFilterRowHeightDip, dpi);
		const int filterRowTop = vertical.contentTop;
		const int treeTop = problemsFilterInHeader ? vertical.contentTop : vertical.contentTop + filterRowHeight;
		const int treeHeight = std::max(0, vertical.contentTop + vertical.contentHeight - treeTop);
		const bool problemsEmpty = problemsActive
			&& problemsView.emptyState != win32::EProblemsViewEmptyState::None;
		if (problemsActive && !::IsRectEmpty(&problemsHeaderBounds)) {
			// Collapse All is a view-title action (MenuId.ViewTitle, 'navigation').
			const int headerLeft = static_cast<int>(problemsHeaderBounds.left);
			int actionRight = static_cast<int>(problemsHeaderBounds.right);
			place(problemsCollapseAllButton, std::max(headerLeft, actionRight - viewActionWidth),
				0, std::min(viewActionWidth, actionRight - headerLeft), headerHeight, true);
			actionRight -= viewActionWidth;
			if (problemsFilterInHeader) {
				int widgetRight = actionRight;
				place(problemsFilterButton, std::max(headerLeft, widgetRight - viewActionWidth),
					0, viewActionWidth, headerHeight, true);
				widgetRight -= viewActionWidth;
				const int badgeWidth = std::min(filterBadgeWidth, std::max(0, widgetRight - headerLeft));
				place(problemsFilterBadge, widgetRight - badgeWidth, 0, badgeWidth, headerHeight,
					badgeWidth > 0);
				widgetRight -= badgeWidth;
				const int editHeight = Scale(24, dpi);
				const int editTop = std::max(0, (headerHeight - editHeight) / 2);
				place(problemsFilterEdit, headerLeft + Scale(4, dpi), editTop,
					std::max(0, widgetRight - headerLeft - Scale(8, dpi)), editHeight, true);
			} else {
				place(problemsFilterEdit, 0, 0, 0, 0, false);
				place(problemsFilterBadge, 0, 0, 0, 0, false);
				place(problemsFilterButton, 0, 0, 0, 0, false);
			}
		} else {
			place(problemsCollapseAllButton, 0, 0, 0, 0, false);
			place(problemsFilterEdit, 0, 0, 0, 0, false);
			place(problemsFilterBadge, 0, 0, 0, 0, false);
			place(problemsFilterButton, 0, 0, 0, 0, false);
		}
		if (problemsActive && !problemsFilterInHeader) {
			// The filter widget in the body row: input, badge, funnel (right-aligned).
			int widgetRight = width;
			const int editHeight = Scale(24, dpi);
			const int editTop = filterRowTop + std::max(0, (filterRowHeight - editHeight) / 2);
			place(problemsFilterButton, std::max(0, widgetRight - viewActionWidth), filterRowTop,
				viewActionWidth, filterRowHeight, true);
			widgetRight -= viewActionWidth + Scale(4, dpi);
			const int badgeWidth = std::min(filterBadgeWidth, widgetRight);
			place(problemsFilterBadge, std::max(0, widgetRight - badgeWidth), filterRowTop,
				badgeWidth, filterRowHeight, badgeWidth > 0);
			widgetRight -= badgeWidth;
			place(problemsFilterEdit, Scale(8, dpi), editTop, std::max(0, widgetRight - Scale(8, dpi)),
				editHeight, true);
		}
		place(problemsTree, 0, treeTop, width, treeHeight, problemsActive && !problemsEmpty);
		if (problemsActive) {
			// Empty-state message centered in the content region, with the
			// "Clear Filters" action under the filtered-variant text.
			const int messageHeight = Scale(24, dpi);
			const int centerY = vertical.contentTop + vertical.contentHeight / 2;
			place(problemsEmptyMessage, 0, centerY - messageHeight, width, messageHeight, problemsEmpty);
			place(problemsClearFilters,
				std::max(0, width / 2 - Scale(60, dpi)), centerY + Scale(4, dpi), Scale(120, dpi),
				Scale(26, dpi), problemsEmpty && problemsView.emptyState == win32::EProblemsViewEmptyState::Filtered);
		} else {
			place(problemsEmptyMessage, 0, 0, 0, 0, false);
			place(problemsClearFilters, 0, 0, 0, 0, false);
		}
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
		problemsScrollbar.Update();
		::RedrawWindow(window, nullptr, nullptr,
			RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN);
	}

	//! One committed problem row's identity. Activation/copy read the projection,
	//! never the tree control itself.
	[[nodiscard]] const win32::ProblemsPanelEntry* SelectedProblemsEntry() const noexcept
	{
		const auto item = TreeView_GetSelection(problemsTree);
		if (item == nullptr) return nullptr;
		TVITEMW query{};
		query.hItem = item;
		query.mask = TVIF_PARAM;
		if (!TreeView_GetItem(problemsTree, &query)) return nullptr;
		const auto index = static_cast<std::size_t>(query.lParam);
		if (index >= problemsRows.size()) return nullptr;
		const auto& row = problemsRows[index];
		if (row.kind != ProblemsRowKind::Marker) return nullptr;
		if (row.resource >= problemsView.resources.size()) return nullptr;
		const auto& markers = problemsView.resources[row.resource].markers;
		if (row.marker >= markers.size()) return nullptr;
		return &markers[row.marker].entry;
	}

	void ActivateSelectedProblem()
	{
		const auto* entry = SelectedProblemsEntry();
		if (entry && problemActivation) problemActivation(*entry);
	}

	void CopySelectedProblems(const bool messageOnly)
	{
		std::vector<const win32::ProblemsPanelEntry*> entries;
		const auto item = TreeView_GetSelection(problemsTree);
		if (item == nullptr) return;
		TVITEMW query{};
		query.hItem = item;
		query.mask = TVIF_PARAM;
		if (!TreeView_GetItem(problemsTree, &query)) return;
		const auto index = static_cast<std::size_t>(query.lParam);
		if (index >= problemsRows.size()) return;
		const auto& row = problemsRows[index];
		if (row.resource >= problemsView.resources.size()) return;
		const auto& resource = problemsView.resources[row.resource];
		if (row.kind == ProblemsRowKind::Resource) {
			for (const auto& marker : resource.markers) entries.push_back(&marker.entry);
		} else if (row.kind == ProblemsRowKind::Marker && row.marker < resource.markers.size()) {
			entries.push_back(&resource.markers[row.marker].entry);
		} else if (row.kind == ProblemsRowKind::RelatedInformation && row.marker < resource.markers.size()) {
			const auto& related = resource.markers[row.marker].entry.relatedInformation;
			if (row.related < related.size()) {
				const auto text = related[row.related].message;
				SetClipboardText(text);
			}
			return;
		}
		if (entries.empty()) return;
		std::wstring text;
		if (messageOnly) {
			for (std::size_t index2 = 0; index2 < entries.size(); ++index2) {
				if (index2) text += L'\n';
				text += entries[index2]->message;
			}
		} else {
			text = win32::BuildProblemsViewCopyText(entries);
		}
		SetClipboardText(text);
	}

	void SetClipboardText(const std::wstring& text) noexcept
	{
		if (!::OpenClipboard(window)) return;
		::EmptyClipboard();
		const auto bytes = static_cast<SIZE_T>((text.size() + 1) * sizeof(wchar_t));
		HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
		if (handle != nullptr) {
			if (void* memory = ::GlobalLock(handle)) {
				::memcpy(memory, text.c_str(), bytes);
				::GlobalUnlock(handle);
				::SetClipboardData(CF_UNICODETEXT, handle);
				handle = nullptr;
			}
		}
		if (handle != nullptr) ::GlobalFree(handle);
		::CloseClipboard();
	}

	void CollapseProblemsTree() noexcept
	{
		// This SDK predates TVE_COLLAPSERECURSE. Collect every expanded item
		// first (collapsing hides descendants mid-walk), then collapse all so
		// re-expanding a resource does not reveal still-expanded children.
		std::vector<HTREEITEM> expanded;
		for (auto item = TreeView_GetRoot(problemsTree); item;
			item = TreeView_GetNextItem(problemsTree, item, TVGN_NEXT)) {
			if (TreeView_GetChild(problemsTree, item)) expanded.push_back(item);
		}
		for (const auto item : expanded) {
			TreeView_Expand(problemsTree, item, TVE_COLLAPSE);
		}
		TreeView_SelectItem(problemsTree, nullptr);
		::SetFocus(problemsTree);
	}

	void ShowProblemsFilterMenu()
	{
		HMENU menu = ::CreatePopupMenu();
		if (menu == nullptr) return;
		auto checkItem = [&](UINT id, UINT textId, bool checked) {
			MENUITEMINFOW itemInfo{};
			itemInfo.cbSize = sizeof(itemInfo);
			itemInfo.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
			itemInfo.wID = id;
			itemInfo.dwTypeData = const_cast<wchar_t*>(LS(textId));
			itemInfo.fState = checked ? MFS_CHECKED : 0;
			::InsertMenuItemW(menu, static_cast<UINT>(-1), TRUE, &itemInfo);
		};
		checkItem(1, STR_WORKBENCH_PANEL_PROBLEMS_SHOW_ERRORS, problemsFilter.showErrors);
		checkItem(2, STR_WORKBENCH_PANEL_PROBLEMS_SHOW_WARNINGS, problemsFilter.showWarnings);
		checkItem(3, STR_WORKBENCH_PANEL_PROBLEMS_SHOW_INFOS, problemsFilter.showInfos);
		::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		checkItem(4, STR_WORKBENCH_PANEL_PROBLEMS_ACTIVE_FILE_ONLY, problemsFilter.activeFileOnly);
		RECT buttonRect{};
		::GetWindowRect(problemsFilterButton, &buttonRect);
		const UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON;
		const int selected = ::TrackPopupMenu(menu, flags | TPM_RETURNCMD | TPM_NONOTIFY,
			buttonRect.left, buttonRect.bottom, 0, window, nullptr);
		::DestroyMenu(menu);
		bool changed = true;
		switch (selected) {
		case 1: problemsFilter.showErrors = !problemsFilter.showErrors; break;
		case 2: problemsFilter.showWarnings = !problemsFilter.showWarnings; break;
		case 3: problemsFilter.showInfos = !problemsFilter.showInfos; break;
		case 4: problemsFilter.activeFileOnly = !problemsFilter.activeFileOnly; break;
		default: changed = false; break;
		}
		if (changed) RebuildProblemsView();
	}

	void ClearProblemsFilters()
	{
		problemsFilter.text.clear();
		problemsFilter.showErrors = true;
		problemsFilter.showWarnings = true;
		problemsFilter.showInfos = true;
		problemsFilter.activeFileOnly = false;
		if (problemsFilterEdit) {
			problemsFilterUpdating = true;
			::SetWindowTextW(problemsFilterEdit, L"");
			problemsFilterUpdating = false;
		}
		RebuildProblemsView();
		::SetFocus(problemsTree);
	}

	//! `problems.action.clearFilterText`: Esc clears only the text box; the
	//! More Filters toggles keep their state, matching upstream's command.
	void ClearProblemsFilterText()
	{
		if (problemsFilter.text.empty()) return;
		problemsFilter.text.clear();
		if (problemsFilterEdit) {
			problemsFilterUpdating = true;
			::SetWindowTextW(problemsFilterEdit, L"");
			problemsFilterUpdating = false;
		}
		RebuildProblemsView();
	}

	HTREEITEM InsertProblemsRow(HTREEITEM parent, const std::size_t resource,
		const std::size_t marker, const std::size_t related, const ProblemsRowKind kind,
		const int heightLines)
	{
		problemsRows.push_back({ nullptr, kind, resource, marker, related });
		const auto rowIndex = problemsRows.size() - 1;
		TVINSERTSTRUCTW insert{};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_PARAM;
		insert.item.lParam = static_cast<LPARAM>(rowIndex);
		const HTREEITEM item = TreeView_InsertItem(problemsTree, &insert);
		problemsRows.back().item = item;
		if (item && heightLines > 1) {
			// TVS_NONEVENHEIGHT: a per-item height is a multiple of the base row.
			::SendMessageW(problemsTree, TVM_SETITEMHEIGHT, heightLines,
				reinterpret_cast<LPARAM>(item));
		}
		return item;
	}

	void RebuildProblemsView()
	{
		problemsView = win32::ProjectProblemsViewTree(problems, problemsFilter, problemsActiveResource);
		if (problemsTree) {
			::SendMessageW(problemsTree, WM_SETREDRAW, FALSE, 0);
			TreeView_DeleteAllItems(problemsTree);
			problemsRows.clear();
			for (std::size_t r = 0; r < problemsView.resources.size(); ++r) {
				const auto& resource = problemsView.resources[r];
				const HTREEITEM resourceItem = InsertProblemsRow(TVI_ROOT, r, 0, 0,
					ProblemsRowKind::Resource, 1);
				for (std::size_t m = 0; m < resource.markers.size(); ++m) {
					const auto& marker = resource.markers[m];
					const HTREEITEM markerItem = InsertProblemsRow(resourceItem, r, m, 0,
						ProblemsRowKind::Marker,
						problemsMultiline
							? static_cast<int>(std::max<std::size_t>(1, marker.lines.size())) : 1);
					for (std::size_t n = 0; n < marker.entry.relatedInformation.size(); ++n) {
						InsertProblemsRow(markerItem, r, m, n,
							ProblemsRowKind::RelatedInformation, 1);
					}
					TreeView_Expand(problemsTree, markerItem, TVE_EXPAND);
				}
				TreeView_Expand(problemsTree, resourceItem, TVE_EXPAND);
			}
			::SendMessageW(problemsTree, WM_SETREDRAW, TRUE, 0);
			problemsScrollbar.Update();
			::InvalidateRect(problemsTree, nullptr, FALSE);
		}
		// The Problems tab keeps its plain "Problems" label; the count moves to a
		// NumberBadge drawn in DrawOwnerButton. A repaint is required here because
		// the badge count lives in the projection, not the window text.
		if (problemsButton) ::InvalidateRect(problemsButton, nullptr, FALSE);
		if (problemsFilterBadge) {
			// Upstream's filtered-count badge: "Showing {0} of {1}", hidden while
			// the filter changes nothing (the widget's badge only appears when
			// filtered != total).
			const std::wstring badge = problemsView.visibleMarkers != problemsView.totalMarkers
				? FormatProblemsMessage(LS(STR_WORKBENCH_PANEL_PROBLEMS_FILTER_BADGE),
					std::to_wstring(problemsView.visibleMarkers),
					std::to_wstring(problemsView.totalMarkers))
				: std::wstring{};
			::SetWindowTextW(problemsFilterBadge, badge.c_str());
		}
		if (problemsEmptyMessage) {
			UINT textId = 0;
			switch (problemsView.emptyState) {
			case win32::EProblemsViewEmptyState::Workspace:
				textId = STR_WORKBENCH_PANEL_PROBLEMS_EMPTY_WORKSPACE; break;
			case win32::EProblemsViewEmptyState::ActiveFile:
				textId = STR_WORKBENCH_PANEL_PROBLEMS_EMPTY_ACTIVE_FILE; break;
			case win32::EProblemsViewEmptyState::Filtered:
				textId = STR_WORKBENCH_PANEL_PROBLEMS_EMPTY_FILTERED; break;
			default: break;
			}
			if (textId != 0) ::SetWindowTextW(problemsEmptyMessage, LS(textId));
		}
		UpdateProblemsTooltips();
		RevealProblemsForActiveDocument();
		LayoutChildren();
	}

	//! Scrolls the tree so that `target` is its first visible row. A TreeView has
	//! no "set top row" message, so the row is reached by walking visible items.
	void SetProblemsFirstVisibleRow(int target)
	{
		if (problemsTree == nullptr) return;
		auto item = TreeView_GetRoot(problemsTree);
		for (int index = 0; item != nullptr && index < std::max(0, target); ++index) {
			item = TreeView_GetNextVisible(problemsTree, item);
		}
		if (item != nullptr) (void)TreeView_SelectSetFirstVisible(problemsTree, item);
	}

	//! The Problems tab's `title` is upstream's `Total {0} Problems`; the two
	//! view-title actions carry their command titles as tooltips.
	void UpdateProblemsTooltips()
	{
		if (problemsTooltip == nullptr) return;
		const auto setTip = [this](HWND control, std::wstring text) {
			if (control == nullptr) return;
			TOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			tool.uFlags = TTF_IDISHWND;
			tool.hwnd = window;
			tool.uId = reinterpret_cast<UINT_PTR>(control);
			tool.lpszText = text.data();
			::SendMessageW(problemsTooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
		};
		setTip(problemsButton, FormatProblemsMessage(LS(STR_WORKBENCH_PANEL_PROBLEMS_TAB_TOOLTIP),
			std::to_wstring(problemsView.totalMarkers), {}));
		setTip(problemsCollapseAllButton, LS(STR_WORKBENCH_PANEL_PROBLEMS_COLLAPSE_ALL));
		setTip(problemsFilterButton, LS(STR_WORKBENCH_PANEL_PROBLEMS_MORE_FILTERS));
	}

	//! `problems.action.open` / `problems.action.openToSide`. This fork has no
	//! SIDE_GROUP yet, so both ids take the same activation path; recorded in
	//! window/CLAUDE.md. On a non-marker row Enter toggles expansion, the same
	//! behavior the upstream workbench tree has for a non-leaf element.
	void OpenSelectedProblemsRow()
	{
		const auto* entry = SelectedProblemsEntry();
		if (entry != nullptr) {
			if (problemActivation) problemActivation(*entry);
			return;
		}
		const auto item = TreeView_GetSelection(problemsTree);
		if (item == nullptr) return;
		TVITEMW query{};
		query.mask = TVIF_STATE | TVIF_CHILDREN;
		query.hItem = item;
		query.stateMask = TVIS_EXPANDED;
		if (!TreeView_GetItem(problemsTree, &query) || query.cChildren <= 0) return;
		TreeView_Expand(problemsTree, item,
			(query.state & TVIS_EXPANDED) != 0 ? TVE_COLLAPSE : TVE_EXPAND);
	}

	void RevealProblemsForActiveDocument()
	{
		if (!problemsAutoReveal || problemsFilter.activeFileOnly || !problemsActiveResource
			|| !problemsTree) return;
		for (const auto& row : problemsRows) {
			if (row.kind != ProblemsRowKind::Resource) continue;
			const auto& resource = problemsView.resources[row.resource];
			if (resource.resourceUri == *problemsActiveResource) {
				TreeView_Expand(problemsTree, row.item, TVE_EXPAND);
				TreeView_EnsureVisible(problemsTree, row.item);
				return;
			}
		}
	}

	//! Executes one registered Problems-view command id (`problems.action.*`
	//! and the `workbench.actions.*.workbench.panel.markers.view.*` actions).
	//! `toggleProblems`/`showErrorsWarnings` are owned by the panel host, not
	//! this tool. Returns false where upstream's command needs a service this
	//! fork does not ship; the caller reports that as the typed unsupported
	//! terminal rather than silently succeeding.
	[[nodiscard]] bool ExecuteProblemsViewCommand(std::string_view commandId) noexcept
	{
		static constexpr std::string_view kMarkersView =
			"workbench.actions.workbench.panel.markers.view.";
		static constexpr std::string_view kMarkersTreeView =
			"workbench.actions.treeView.workbench.panel.markers.view.";
		static constexpr std::string_view kMarkersTableView =
			"workbench.actions.table.workbench.panel.markers.view.";
		const auto toggle = [this](bool& flag) { flag = !flag; RebuildProblemsView(); return true; };
		if (commandId == "problems.action.open") { OpenSelectedProblemsRow(); return true; }
		// "Open to the Side" needs a split editor group, which does not exist here.
		if (commandId == "problems.action.openToSide") return false;
		if (commandId == "problems.action.copy") { CopySelectedProblems(false); return true; }
		if (commandId == "problems.action.copyMessage"
			|| commandId == "problems.action.copyRelatedInformationMessage") {
			CopySelectedProblems(true);
			return true;
		}
		if (commandId == "problems.action.clearFilterText") { ClearProblemsFilterText(); return true; }
		if (commandId == "problems.action.focusFilter") {
			if (problemsFilterEdit) ::SetFocus(problemsFilterEdit);
			return problemsFilterEdit != nullptr;
		}
		if (commandId == "problems.action.focusProblemsFromFilter") {
			if (problemsTree) ::SetFocus(problemsTree);
			return problemsTree != nullptr;
		}
		if (commandId == "problems.action.showMultilineMessage") {
			problemsMultiline = true;
			RebuildProblemsView();
			return true;
		}
		if (commandId == "problems.action.showSinglelineMessage") {
			problemsMultiline = false;
			RebuildProblemsView();
			return true;
		}
		// Quick fixes need a code-action service this fork does not ship.
		if (commandId == "problems.action.showQuickFixes") return false;
		if (commandId.size() > kMarkersView.size() && commandId.starts_with(kMarkersView)) {
			const std::string_view name = commandId.substr(kMarkersView.size());
			if (name == "toggleErrors") return toggle(problemsFilter.showErrors);
			if (name == "toggleWarnings") return toggle(problemsFilter.showWarnings);
			if (name == "toggleInfos") return toggle(problemsFilter.showInfos);
			if (name == "toggleActiveFile") return toggle(problemsFilter.activeFileOnly);
			// Hiding excluded files needs a files-exclude configuration store.
			if (name == "toggleExcludedFiles") return false;
		}
		if (commandId.size() > kMarkersTreeView.size() && commandId.starts_with(kMarkersTreeView)) {
			if (commandId.substr(kMarkersTreeView.size()) == "collapseAll") {
				CollapseProblemsTree();
				return true;
			}
		}
		if (commandId.size() > kMarkersTableView.size() && commandId.starts_with(kMarkersTableView)) {
			const std::string_view name = commandId.substr(kMarkersTableView.size());
			// Tree is the only implemented view mode; `viewAsTable` reports the
			// unsupported terminal rather than switching to a nonexistent table.
			if (name == "viewAsTree") return true;
			if (name == "viewAsTable") return false;
		}
		return false;
	}

	void DrawProblemsItem(NMTVCUSTOMDRAW& draw)
	{
		const auto item = reinterpret_cast<HTREEITEM>(draw.nmcd.dwItemSpec);
		TVITEMW query{};
		query.hItem = item;
		query.mask = TVIF_PARAM;
		if (!TreeView_GetItem(problemsTree, &query)) return;
		const auto index = static_cast<std::size_t>(query.lParam);
		if (index >= problemsRows.size()) return;
		const auto& row = problemsRows[index];
		if (row.resource >= problemsView.resources.size()) return;
		const auto& resource = problemsView.resources[row.resource];

		RECT line{};
		if (!TreeView_GetItemRect(problemsTree, item, &line, FALSE)) return;
		RECT text{};
		if (!TreeView_GetItemRect(problemsTree, item, &text, TRUE)) return;
		RECT client{};
		::GetClientRect(problemsTree, &client);
		line.left = client.left;
		line.right = client.right;
		const bool selected = (draw.nmcd.uItemState & CDIS_SELECTED) != 0;
		const bool focused = ::GetFocus() == problemsTree;
		const HBRUSH background = ::CreateSolidBrush(
			(selected ? focused ? palette.listActiveSelectionBackground : palette.raised
				: palette.bottomPanel).ToColorRef());
		if (background) {
			::FillRect(draw.nmcd.hdc, &line, background);
			::DeleteObject(background);
		}
		const auto previousFont = ::SelectObject(draw.nmcd.hdc, font.Get());
		::SetBkMode(draw.nmcd.hdc, TRANSPARENT);
		const auto mainColor = selected && focused ? palette.listActiveSelectionForeground
			: palette.primaryText;
		const auto secondaryColor = selected && focused ? palette.listActiveSelectionForeground
			: palette.descriptionText;
		const int rowHeight = Scale(kProblemsRowHeightDip, dpi);
		const int iconSide = Scale(16, dpi);
		const int iconTop = line.top + std::max(0, (rowHeight - iconSide) / 2);
		text.right = std::max(text.left, client.right - Scale(10, dpi));
		if (row.kind == ProblemsRowKind::Resource) {
			// Twistie + file icon + basename + folder description + count badge.
			TVITEMW itemState{};
			itemState.mask = TVIF_STATE;
			itemState.hItem = item;
			itemState.stateMask = TVIS_EXPANDED;
			TreeView_GetItem(problemsTree, &itemState);
			const bool expanded = (itemState.state & TVIS_EXPANDED) != 0;
			viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc,
				{ text.left - iconSide, iconTop, text.left, iconTop + iconSide },
				expanded ? L"chevron-down" : L"chevron-right", secondaryColor.ToColorRef());
			const RECT iconBox{ text.left, iconTop, text.left + iconSide, iconTop + iconSide };
			if (icons::CSetiFont::Instance().IsAvailable()) {
				// The bundled `vs-seti` theme is VS Code's default file icon theme;
				// its `light` section applies to ColorThemeKind.Light alone.
				const auto icon = icons::seti::ResolveSetiFileIcon(resource.name, false,
					theme::CThemeService::IsActiveColorThemeLightKind()
						? icons::seti::EIconVariant::Light : icons::seti::EIconVariant::Dark);
				if (icon) {
					icons::seti::DrawSetiIcon(draw.nmcd.hdc, iconBox, icon->character,
						icon->color == icons::seti::kInheritColor
							? mainColor.ToColorRef() : icons::seti::ColorRefFromThemeRgb(icon->color));
					text.left += iconSide + Scale(6, dpi);
				}
			} else {
				// seti.ttf could not be registered: fall back to the first-party
				// Codicon association table rather than paint unrelated glyphs.
				viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc, iconBox,
					explorer::ResolveExplorerFileIconCodicon(resource.name, false, expanded, false),
					mainColor.ToColorRef());
				text.left += iconSide + Scale(6, dpi);
			}
			const int textEnd = text.right;
			SIZE nameSize{}, descriptionSize{};
			::SetTextColor(draw.nmcd.hdc, mainColor.ToColorRef());
			::GetTextExtentPoint32W(draw.nmcd.hdc, resource.name.data(),
				static_cast<int>(resource.name.size()), &nameSize);
			::GetTextExtentPoint32W(draw.nmcd.hdc, resource.description.data(),
				static_cast<int>(resource.description.size()), &descriptionSize);
			const int available = textEnd - text.left;
			const int nameWidth = std::max(0, std::min(static_cast<int>(nameSize.cx),
				available - std::min(static_cast<int>(descriptionSize.cx) + Scale(6, dpi), available / 3)));
			RECT name = text;
			name.right = name.left + nameWidth;
			::DrawTextW(draw.nmcd.hdc, resource.name.data(), static_cast<int>(resource.name.size()),
				&name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			int cursor = std::min(textEnd, static_cast<int>(name.right) + Scale(6, dpi));
			if (!resource.description.empty() && cursor < textEnd) {
				::SetTextColor(draw.nmcd.hdc, secondaryColor.ToColorRef());
				RECT descriptionRect{ cursor, text.top, textEnd, text.bottom };
				::DrawTextW(draw.nmcd.hdc, resource.description.data(),
					static_cast<int>(resource.description.size()), &descriptionRect,
					DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
				cursor += static_cast<int>(descriptionSize.cx);
			}
			// CountBadge = the number of *visible* markers under this resource.
			const auto count = std::to_wstring(resource.markers.size());
			DrawCountBadge(draw.nmcd.hdc, cursor + Scale(8, dpi), line.top, rowHeight,
				count, palette, font.Get());
		} else if (row.kind == ProblemsRowKind::Marker) {
			if (row.marker >= resource.markers.size()) return;
			const auto& marker = resource.markers[row.marker];
			// Twistie only when the marker owns related-information children.
			if (!marker.entry.relatedInformation.empty()) {
				TVITEMW itemState{};
				itemState.mask = TVIF_STATE;
				itemState.hItem = item;
				itemState.stateMask = TVIS_EXPANDED;
				TreeView_GetItem(problemsTree, &itemState);
				const bool expanded = (itemState.state & TVIS_EXPANDED) != 0;
				viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc,
					{ text.left - iconSide, iconTop, text.left, iconTop + iconSide },
					expanded ? L"chevron-down" : L"chevron-right", secondaryColor.ToColorRef());
			}
			viewcontainer::PaintViewPaneIcon(draw.nmcd.hdc,
				{ text.left, iconTop, text.left + iconSide, iconTop + iconSide },
				SeverityCodicon(marker.entry.severity),
				SeverityIconColor(palette, marker.entry.severity));
			text.left += iconSide + Scale(6, dpi);
			const int firstLineHeight = rowHeight;
			// Line 1 carries message + details ([Ln X, Col Y] + source + code).
			std::wstring details;
			if (!marker.entry.source.empty()) details += marker.entry.source;
			if (!marker.entry.code.empty()) {
				if (!details.empty()) details += L' ';
				details += marker.entry.code;
			}
			if (!details.empty()) details += L' ';
			details += MarkerLocationText(marker.entry.range);
			RECT firstLine{ text.left, line.top, text.right, line.top + firstLineHeight };
			DrawTextRuns(draw.nmcd.hdc, firstLine,
				marker.lines.empty() ? std::wstring{} : marker.lines[0],
				mainColor.ToColorRef(), details, secondaryColor.ToColorRef(), Scale(6, dpi));
			::SetTextColor(draw.nmcd.hdc, mainColor.ToColorRef());
			const std::size_t lineCount = problemsMultiline ? marker.lines.size() : 1;
			for (std::size_t index2 = 1; index2 < lineCount; ++index2) {
				RECT lineRect{ text.left, line.top + static_cast<int>(index2) * rowHeight,
					text.right, line.top + static_cast<int>(index2 + 1) * rowHeight };
				::DrawTextW(draw.nmcd.hdc, marker.lines[index2].data(),
					static_cast<int>(marker.lines[index2].size()), &lineRect,
					DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			}
		} else {
			// RelatedInformation: file basename + [Ln X, Col Y] + ':' + message.
			if (row.marker >= resource.markers.size()) return;
			const auto& related = resource.markers[row.marker].entry.relatedInformation;
			if (row.related >= related.size()) return;
			const auto& item2 = related[row.related];
			const auto separator = item2.resourceUri.find_last_of(L"/\\");
			std::wstring name = separator == std::wstring::npos ? item2.resourceUri
				: item2.resourceUri.substr(separator + 1);
			name += L' ';
			name += MarkerLocationText(item2.range);
			::SetTextColor(draw.nmcd.hdc, mainColor.ToColorRef());
			RECT nameRect = text;
			SIZE nameSize{};
			::GetTextExtentPoint32W(draw.nmcd.hdc, name.data(), static_cast<int>(name.size()), &nameSize);
			nameRect.right = nameRect.left + static_cast<int>(nameSize.cx);
			::DrawTextW(draw.nmcd.hdc, name.data(), static_cast<int>(name.size()), &nameRect,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			::SetTextColor(draw.nmcd.hdc, secondaryColor.ToColorRef());
			RECT sep{ nameRect.right, text.top, std::min(text.right, nameRect.right + Scale(6, dpi)), text.bottom };
			::DrawTextW(draw.nmcd.hdc, L":", 1, &sep, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
			::SetTextColor(draw.nmcd.hdc, mainColor.ToColorRef());
			RECT message{ sep.right, text.top, text.right, text.bottom };
			::DrawTextW(draw.nmcd.hdc, item2.message.data(), static_cast<int>(item2.message.size()),
				&message, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
		::SelectObject(draw.nmcd.hdc, previousFont);
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
		RebuildProblemsView();
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
		m_impl->ApplyFont(m_impl->problemsTree);
		m_impl->ApplyFont(m_impl->problemsFilterEdit);
		m_impl->ApplyFont(m_impl->problemsFilterBadge);
		m_impl->ApplyFont(m_impl->problemsEmptyMessage);
		m_impl->ApplyFont(m_impl->problemsClearFilters);
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
	else if (m_impl->activeContainerId == containerIds::Problems) ::SetFocus(m_impl->problemsTree);
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
	if (m_impl->activeContainerId == containerIds::Problems
		&& message.message == WM_KEYDOWN) {
		const HWND focus = ::GetFocus();
		const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		// problems.action.focusFilter: Ctrl+F while the view owns focus.
		if (ctrl && message.wParam == 'F'
			&& (focus == m_impl->problemsTree || focus == m_impl->problemsFilterEdit)) {
			::SetFocus(m_impl->problemsFilterEdit);
			::SendMessageW(m_impl->problemsFilterEdit, EM_SETSEL, 0, -1);
			return true;
		}
		// problems.action.clearFilterText: Esc in the filter input. The tree's
		// Esc arrives through TVN_KEYDOWN.
		if (message.wParam == VK_ESCAPE && focus == m_impl->problemsFilterEdit) {
			m_impl->ClearProblemsFilterText();
			return true;
		}
		// problems.action.focusProblemsFromFilter: Ctrl+Down moves from the
		// filter input to the tree, upstream's MarkerViewFilterFocus chord.
		if (ctrl && message.wParam == VK_DOWN && focus == m_impl->problemsFilterEdit) {
			::SetFocus(m_impl->problemsTree);
			return true;
		}
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
	m_impl->problemsScrollbar.Destroy();
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

void CBottomPanelTool::SetProblemsActiveDocument(std::optional<std::wstring> resourceUri)
{
	if (!m_impl || m_impl->closed) return;
	if (m_impl->problemsActiveResource == resourceUri) return;
	m_impl->problemsActiveResource = std::move(resourceUri);
	m_impl->RevealProblemsForActiveDocument();
}

bool CBottomPanelTool::ExecuteProblemsViewCommand(std::string_view commandId) noexcept
{
	if (m_impl->closed || m_impl->problemsTree == nullptr) return false;
	return m_impl->ExecuteProblemsViewCommand(commandId);
}

void CBottomPanelTool::SetProblemsAutoReveal(bool enabled)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->problemsAutoReveal = enabled;
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
	if (m_impl->problemsFilterEdit) {
		::SendMessageW(m_impl->problemsFilterEdit, EM_SETCUEBANNER, TRUE,
			reinterpret_cast<LPARAM>(LS(STR_WORKBENCH_PANEL_PROBLEMS_FILTER_PLACEHOLDER)));
	}
	if (m_impl->problemsClearFilters) {
		::SetWindowTextW(m_impl->problemsClearFilters, LS(STR_WORKBENCH_PANEL_PROBLEMS_CLEAR_FILTERS));
	}
	m_impl->Refresh();
	m_impl->UpdateProblemsTooltips();
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
		impl.problemsTree = ::CreateWindowExW(0, WC_TREEVIEWW, L"",
			WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS
				| TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_NOHSCROLL | TVS_NONEVENHEIGHT,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsTree), instance, nullptr);
		if (impl.problemsTree) {
			::SendMessageW(impl.problemsTree, TVM_SETEXTENDEDSTYLE,
				TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
			// VS Code's Problems rows are 22 DIP; a multiline marker is that many
			// lines tall (markersTreeViewer's renderElement count).
			TreeView_SetItemHeight(impl.problemsTree,
				static_cast<short>(std::max(1, Scale(kProblemsRowHeightDip, impl.dpi))));
			Impl* const owner = &impl;
			(void)impl.problemsScrollbar.Create(window, impl.problemsTree,
				[owner](int topRow) { owner->SetProblemsFirstVisibleRow(topRow); });
		}
		impl.problemsFilterEdit = ::CreateWindowExW(0, L"EDIT", L"",
			WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsFilterEdit), instance, nullptr);
		if (impl.problemsFilterEdit) {
			::SendMessageW(impl.problemsFilterEdit, EM_SETCUEBANNER, TRUE,
				reinterpret_cast<LPARAM>(LS(STR_WORKBENCH_PANEL_PROBLEMS_FILTER_PLACEHOLDER)));
		}
		impl.problemsFilterBadge = ::CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsFilterBadge), instance, nullptr);
		impl.problemsFilterButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsFilterButton), instance, nullptr);
		impl.problemsCollapseAllButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsCollapseAllButton), instance, nullptr);
		impl.problemsEmptyMessage = ::CreateWindowExW(0, L"STATIC", L"",
			WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsEmptyMessage), instance, nullptr);
		impl.problemsClearFilters = ::CreateWindowExW(0, L"BUTTON",
			LS(STR_WORKBENCH_PANEL_PROBLEMS_CLEAR_FILTERS), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsClearFilters), instance, nullptr);
		impl.problemsTooltip = ::CreateWindowExW(0, TOOLTIPS_CLASSW, L"",
			WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
		if (impl.problemsTooltip) {
			for (const HWND control : { impl.problemsButton, impl.problemsFilterButton,
				impl.problemsCollapseAllButton }) {
				if (control == nullptr) continue;
				TOOLINFOW tool{};
				tool.cbSize = sizeof(tool);
				tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
				tool.hwnd = window;
				tool.uId = reinterpret_cast<UINT_PTR>(control);
				tool.lpszText = LPSTR_TEXTCALLBACKW;
				::SendMessageW(impl.problemsTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
			}
			impl.UpdateProblemsTooltips();
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
		case kProblemsFilterEdit:
			if (HIWORD(wParam) == EN_CHANGE && !impl.problemsFilterUpdating) {
				const int length = ::GetWindowTextLengthW(impl.problemsFilterEdit);
				std::wstring text(static_cast<std::size_t>(std::max(0, length)), L'\0');
				if (!text.empty()) ::GetWindowTextW(impl.problemsFilterEdit, text.data(),
					static_cast<int>(text.size() + 1));
				impl.problemsFilter.text = text;
				impl.RebuildProblemsView();
			}
			return 0;
		case kProblemsFilterButton:
			if (HIWORD(wParam) == BN_CLICKED) impl.ShowProblemsFilterMenu();
			return 0;
		case kProblemsCollapseAllButton:
			if (HIWORD(wParam) == BN_CLICKED) impl.CollapseProblemsTree();
			return 0;
		case kProblemsClearFilters:
			if (HIWORD(wParam) == BN_CLICKED) impl.ClearProblemsFilters();
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
		if (header && header->idFrom == kProblemsTree) {
			if (header->code == NM_CUSTOMDRAW) {
				auto& draw = *reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
				switch (draw.nmcd.dwDrawStage) {
				case CDDS_PREPAINT:
					return CDRF_NOTIFYITEMDRAW;
				case CDDS_ITEMPREPAINT:
					impl.DrawProblemsItem(draw);
					return CDRF_SKIPDEFAULT;
				default:
					return CDRF_DODEFAULT;
				}
			}
			if (header->code == NM_DBLCLK) {
				// problems.action.open: double-click opens the marker, matching
				// upstream's tree 'open' behavior on a marker row.
				impl.ActivateSelectedProblem();
				return 0;
			}
			if (header->code == TVN_KEYDOWN) {
				const auto& key = *reinterpret_cast<const NMTVKEYDOWN*>(lParam);
				const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
				if (key.wVKey == VK_RETURN) {
					impl.OpenSelectedProblemsRow();
					return 0;
				}
				if (ctrl && key.wVKey == 'C') {
					impl.CopySelectedProblems(false);
					return 0;
				}
				if (ctrl && key.wVKey == 'F') {
					::SetFocus(impl.problemsFilterEdit);
					::SendMessageW(impl.problemsFilterEdit, EM_SETSEL, 0, -1);
					return 0;
				}
				if (key.wVKey == VK_ESCAPE) {
					impl.ClearProblemsFilterText();
					return 0;
				}
			}
			if (header->code == TVN_ITEMEXPANDED || header->code == TVN_SELCHANGED
				|| header->code == NM_CLICK) {
				impl.problemsScrollbar.Update();
			}
			break;
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
		impl.problemsScrollbar.Detach();
		impl.backBuffer.Reset();
		impl.DestroyBrushes();
		impl.window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::panel
