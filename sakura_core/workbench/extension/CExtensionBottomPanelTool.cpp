/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/extension/CExtensionBottomPanelTool.h"

#include <CommCtrl.h>
#include <Richedit.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>

namespace workbench::extension {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraExtensionBottomPanel";
constexpr UINT_PTR kTerminalButton = 100;
constexpr UINT_PTR kProblemsButton = 101;
constexpr UINT_PTR kOutputButton = 102;
constexpr UINT_PTR kPortsButton = 103;
constexpr UINT_PTR kDebugConsoleButton = 104;
constexpr UINT_PTR kProblemsList = 105;
constexpr UINT_PTR kOutputSelector = 106;
constexpr UINT_PTR kOutputText = 107;
constexpr UINT_PTR kPanelMaximizeButton = 108;
constexpr UINT_PTR kPanelCloseButton = 109;
constexpr unsigned int kDefaultDpi = 96;

bool IsPanelTabId(UINT_PTR id) noexcept
{
	return id == kTerminalButton || id == kProblemsButton || id == kOutputButton
		|| id == kPortsButton || id == kDebugConsoleButton;
}

bool IsPanelActionId(UINT_PTR id) noexcept
{
	return id == kPanelMaximizeButton || id == kPanelCloseButton;
}

int Scale(int value, unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi ? dpi : kDefaultDpi), kDefaultDpi);
}

bool IsSupportedTab(ExtensionBottomPanelTab tab) noexcept
{
	return tab == ExtensionBottomPanelTab::Terminal
		|| tab == ExtensionBottomPanelTab::Problems
		|| tab == ExtensionBottomPanelTab::Output;
}

ExtensionBottomPanelTab TabForButtonId(UINT_PTR id) noexcept
{
	switch (id) {
	case kProblemsButton: return ExtensionBottomPanelTab::Problems;
	case kOutputButton: return ExtensionBottomPanelTab::Output;
	case kTerminalButton: return ExtensionBottomPanelTab::Terminal;
	case kPortsButton: return ExtensionBottomPanelTab::Ports;
	case kDebugConsoleButton: return ExtensionBottomPanelTab::DebugConsole;
	default: return ExtensionBottomPanelTab::Terminal;
	}
}

bool EnsureClass(HINSTANCE instance)
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	windowClass.lpfnWndProc = CExtensionBottomPanelTool::WindowProc;
	windowClass.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

const wchar_t* SeverityText(const win32::EProblemsPanelSeverity severity) noexcept
{
	switch (severity) {
	case win32::EProblemsPanelSeverity::Error: return L"Error";
	case win32::EProblemsPanelSeverity::Warning: return L"Warning";
	case win32::EProblemsPanelSeverity::Information: return L"Info";
	case win32::EProblemsPanelSeverity::Hint: return L"Hint";
	}
	return L"";
}

} // namespace

struct CExtensionBottomPanelTool::Impl {
	std::unique_ptr<terminal::CTerminalTool> terminal = std::make_unique<terminal::CTerminalTool>();
	HWND window = nullptr;
	HWND terminalButton = nullptr;
	HWND problemsButton = nullptr;
	HWND outputButton = nullptr;
	HWND portsButton = nullptr;
	HWND debugConsoleButton = nullptr;
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
	ExtensionBottomPanelTab active = ExtensionBottomPanelTab::Terminal;
	ProblemActivationCallback problemActivation;
	OutputChannelSelectionCallback outputChannelSelection;
	TabSelectionCallback tabSelection;
	win32::ProblemsPanelSnapshot problems;
	win32::OutputPanelSnapshot outputs;
	std::optional<std::string> selectedOutputChannelId;
	CExtensionBottomPanelTool::PanelActions panelActions;
	bool closed = false;

	void ApplyFont(HWND control) const
	{
		if (control && font.Get()) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), TRUE);
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
			::InvalidateRect(problemsList, nullptr, TRUE);
		}
		if (outputText) {
			::SendMessageW(outputText, EM_SETBKGNDCOLOR, TRUE, palette.bottomPanel.ToColorRef());
			::InvalidateRect(outputText, nullptr, TRUE);
		}
		if (outputSelector) ::InvalidateRect(outputSelector, nullptr, TRUE);
	}

	void UpdateActionVisibility()
	{
		if (maximizeButton) {
			::ShowWindow(maximizeButton, panelActions.toggleMaximize ? SW_SHOW : SW_HIDE);
		}
		if (closeButton) {
			::ShowWindow(closeButton, panelActions.closePanel ? SW_SHOW : SW_HIDE);
		}
	}

	void DrawOwnerButton(const DRAWITEMSTRUCT& item) const
	{
		if (item.hDC == nullptr) return;
		const UINT_PTR id = static_cast<UINT_PTR>(item.CtlID);
		const bool action = IsPanelActionId(id);
		const bool tab = IsPanelTabId(id);
		const ExtensionBottomPanelTab buttonTab = TabForButtonId(id);
		const bool supportedTab = !tab || IsSupportedTab(buttonTab);
		const bool disabled = (item.itemState & ODS_DISABLED) != 0 || !supportedTab;
		const bool activeTab = tab && supportedTab && active == buttonTab;
		const bool pressed = (item.itemState & ODS_SELECTED) != 0;
		const HBRUSH background = (activeTab || pressed) ? raisedBrush : panelBrush;
		if (background) ::FillRect(item.hDC, &item.rcItem, background);

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

		wchar_t label[128]{};
		::GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
		::SetBkMode(item.hDC, TRANSPARENT);
		::SetTextColor(item.hDC, disabled ? palette.disabledText.ToColorRef()
			: activeTab ? palette.primaryText.ToColorRef() : palette.secondaryText.ToColorRef());
		RECT text = item.rcItem;
		text.left += Scale(12, dpi);
		text.right -= Scale(8, dpi);
		::DrawTextW(item.hDC, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
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

	void ApplyActiveTab(ExtensionBottomPanelTab tab)
	{
		if (closed || !IsSupportedTab(tab)) return;
		active = tab;
		const bool showTerminal = tab == ExtensionBottomPanelTab::Terminal;
		::ShowWindow(terminal->GetHwnd(), showTerminal ? SW_SHOW : SW_HIDE);
		::ShowWindow(problemsList, tab == ExtensionBottomPanelTab::Problems ? SW_SHOW : SW_HIDE);
		::ShowWindow(outputSelector, tab == ExtensionBottomPanelTab::Output ? SW_SHOW : SW_HIDE);
		::ShowWindow(outputText, tab == ExtensionBottomPanelTab::Output ? SW_SHOW : SW_HIDE);
		::SendMessageW(terminalButton, BM_SETSTATE, showTerminal, 0);
		::SendMessageW(problemsButton, BM_SETSTATE, tab == ExtensionBottomPanelTab::Problems, 0);
		::SendMessageW(outputButton, BM_SETSTATE, tab == ExtensionBottomPanelTab::Output, 0);
		::SendMessageW(portsButton, BM_SETSTATE, FALSE, 0);
		::SendMessageW(debugConsoleButton, BM_SETSTATE, FALSE, 0);
		if (!showTerminal) terminal->Deactivate();
		LayoutChildren();
		if (window) ::InvalidateRect(window, nullptr, FALSE);
	}

	void LayoutChildren()
	{
		if (!window) return;
		const int width = std::max(0L, bounds.right - bounds.left);
		const int height = std::max(0L, bounds.bottom - bounds.top);
		const auto vertical = CalculateExtensionBottomPanelVerticalLayout(
			height, Scale(34, dpi), Scale(28, dpi));
		const int headerHeight = vertical.headerHeight;
		const int actionWidth = Scale(30, dpi);
		::MoveWindow(window, bounds.left, bounds.top, width, height, TRUE);

		int commonActionLeft = width;
		const auto placeAction = [&](HWND button, bool visible) {
			if (!button) return;
			if (visible) {
				commonActionLeft = std::max(0, commonActionLeft - actionWidth);
				::MoveWindow(button, commonActionLeft, 0,
					std::min(actionWidth, std::max(0, width - commonActionLeft)), headerHeight, TRUE);
			} else {
				::MoveWindow(button, 0, 0, 0, 0, FALSE);
			}
		};
		placeAction(closeButton, static_cast<bool>(panelActions.closePanel));
		placeAction(maximizeButton, static_cast<bool>(panelActions.toggleMaximize));

		// Keep the terminal actions in the same physical row as the view-container
		// tabs. The toolbar gets a bounded trailing region; the tabs retain a compact
		// intrinsic width and leave the middle as intentional breathing room.
		constexpr std::array desiredTabWidthsDip{ 82, 70, 78, 58, 118 };
		constexpr int desiredTabWidthDipTotal = 406;
		const int minimumTabArea = std::min(commonActionLeft, Scale(250, dpi));
		const int desiredToolbarWidth = Scale(260, dpi);
		const int toolbarWidth = std::min(desiredToolbarWidth,
			std::max(0, commonActionLeft - minimumTabArea));
		const int toolbarLeft = std::max(0, commonActionLeft - toolbarWidth);
		terminalHeaderBounds = toolbarWidth > 0 && headerHeight > 0
			? RECT{ toolbarLeft, 0, commonActionLeft, headerHeight } : RECT{};

		constexpr std::array tabButtons{
			std::pair{ ExtensionBottomPanelTab::Problems, kProblemsButton },
			std::pair{ ExtensionBottomPanelTab::Output, kOutputButton },
			std::pair{ ExtensionBottomPanelTab::Terminal, kTerminalButton },
			std::pair{ ExtensionBottomPanelTab::Ports, kPortsButton },
			std::pair{ ExtensionBottomPanelTab::DebugConsole, kDebugConsoleButton },
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
			const HWND button = index == 0 ? problemsButton
				: index == 1 ? outputButton
				: index == 2 ? terminalButton
				: index == 3 ? portsButton : debugConsoleButton;
			::MoveWindow(button, tabLeft, 0, std::max(0, tabWidth), headerHeight, TRUE);
			tabLeft += tabWidth;
			remainingWidth = std::max(0, remainingWidth - tabWidth);
			--remainingTabs;
		}

		RECT content{ 0, vertical.contentTop, width, vertical.contentTop + vertical.contentHeight };
		terminal->Layout(content, dpi);
		::MoveWindow(problemsList, 0, vertical.contentTop, width, vertical.contentHeight, TRUE);
		::MoveWindow(outputSelector, 0, vertical.contentTop, width, vertical.outputSelectorHeight, TRUE);
		::MoveWindow(outputText, 0, vertical.contentTop + vertical.outputSelectorHeight, width,
			vertical.contentHeight - vertical.outputSelectorHeight, TRUE);
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
		std::wstring label = L"PROBLEMS";
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

CExtensionBottomPanelTool::CExtensionBottomPanelTool() : m_impl(std::make_unique<Impl>()) {}
CExtensionBottomPanelTool::~CExtensionBottomPanelTool() { Close(); }

bool CExtensionBottomPanelTool::Create(HWND parent)
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
	return true;
}

void CExtensionBottomPanelTool::Layout(const RECT& contentRect, unsigned int dpi)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->bounds = contentRect;
	m_impl->dpi = dpi ? dpi : kDefaultDpi;
	if (m_impl->font.Dpi() != m_impl->dpi) {
		(void)m_impl->font.Recreate(theme::ThemeFontKind::Chrome, m_impl->dpi);
		m_impl->ApplyFont(m_impl->terminalButton);
		m_impl->ApplyFont(m_impl->problemsButton);
		m_impl->ApplyFont(m_impl->outputButton);
		m_impl->ApplyFont(m_impl->portsButton);
		m_impl->ApplyFont(m_impl->debugConsoleButton);
		m_impl->ApplyFont(m_impl->problemsList);
		m_impl->ApplyFont(m_impl->outputSelector);
		m_impl->ApplyFont(m_impl->outputText);
		m_impl->ApplyFont(m_impl->maximizeButton);
		m_impl->ApplyFont(m_impl->closeButton);
	}
	m_impl->LayoutChildren();
}

void CExtensionBottomPanelTool::Activate()
{
	if (!m_impl || m_impl->closed) return;
	if (m_impl->active == ExtensionBottomPanelTab::Terminal) m_impl->terminal->Activate();
	else if (m_impl->active == ExtensionBottomPanelTab::Problems) ::SetFocus(m_impl->problemsList);
	else ::SetFocus(m_impl->outputText);
}

void CExtensionBottomPanelTool::Deactivate()
{
	if (m_impl && !m_impl->closed) m_impl->terminal->Deactivate();
}

bool CExtensionBottomPanelTool::PreTranslateMessage(MSG& message)
{
	return m_impl && !m_impl->closed && m_impl->active == ExtensionBottomPanelTab::Terminal &&
		m_impl->terminal->PreTranslateMessage(message);
}

void CExtensionBottomPanelTool::Close()
{
	if (!m_impl || m_impl->closed) return;
	m_impl->closed = true;
	m_impl->problemActivation = {};
	m_impl->outputChannelSelection = {};
	m_impl->tabSelection = {};
	m_impl->problems = {};
	m_impl->outputs = {};
	m_impl->selectedOutputChannelId.reset();
	m_impl->panelActions = {};
	m_impl->terminal->SetPanelHeaderHost(nullptr);
	m_impl->terminal->Close();
	if (m_impl->window) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->DestroyBrushes();
}

terminal::CTerminalTool* CExtensionBottomPanelTool::Terminal() noexcept
{
	return m_impl ? m_impl->terminal.get() : nullptr;
}

void CExtensionBottomPanelTool::SetPalette(const theme::ThemePalette& palette)
{
	if (!m_impl) return;
	m_impl->palette = palette;
	m_impl->RecreateBrushes();
	m_impl->terminal->SetPalette(palette);
	m_impl->ApplyControlPalette();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, TRUE);
}

void CExtensionBottomPanelTool::SetProblemsSnapshot(win32::ProblemsPanelSnapshot snapshot)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->problems = std::move(snapshot);
	m_impl->Refresh();
}

void CExtensionBottomPanelTool::SetOutputSnapshot(win32::OutputPanelSnapshot snapshot)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->outputs = std::move(snapshot);
	m_impl->SelectOutputForSnapshot();
	m_impl->Refresh();
}

void CExtensionBottomPanelTool::SetProblemActivationCallback(ProblemActivationCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->problemActivation = std::move(callback);
}

void CExtensionBottomPanelTool::SetOutputChannelSelectionCallback(OutputChannelSelectionCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->outputChannelSelection = std::move(callback);
}

void CExtensionBottomPanelTool::SetTabSelectionCallback(TabSelectionCallback callback)
{
	if (m_impl && !m_impl->closed) m_impl->tabSelection = std::move(callback);
}

void CExtensionBottomPanelTool::SetPanelActions(PanelActions actions)
{
	if (!m_impl || m_impl->closed) return;
	m_impl->panelActions = std::move(actions);
	m_impl->UpdateActionVisibility();
	m_impl->LayoutChildren();
	if (m_impl->window) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CExtensionBottomPanelTool::Refresh() { if (m_impl) m_impl->Refresh(); }
void CExtensionBottomPanelTool::SetActiveTab(ExtensionBottomPanelTab tab)
{
	if (m_impl) m_impl->ApplyActiveTab(tab);
}

bool CExtensionBottomPanelTool::RequestTabSelection(ExtensionBottomPanelTab tab) noexcept
{
	if (!m_impl || m_impl->closed || !IsSupportedTab(tab)) return false;
	if (m_impl->active == tab) return true;
	if (m_impl->tabSelection) {
		try {
			if (!m_impl->tabSelection(tab)) return false;
		}
		catch (...) {
			return false;
		}
		// The callback owns the committed state. The next model snapshot will call
		// SetActiveTab and update the native controls atomically with the rest of the
		// Workbench projection.
		return true;
	}
	m_impl->ApplyActiveTab(tab);
	return true;
}

void CExtensionBottomPanelTool::ShowProblems() { SetActiveTab(ExtensionBottomPanelTab::Problems); }
void CExtensionBottomPanelTool::ShowOutput() { SetActiveTab(ExtensionBottomPanelTab::Output); }

ExtensionBottomPanelTab CExtensionBottomPanelTool::ActiveTab() const noexcept
{
	return m_impl ? m_impl->active : ExtensionBottomPanelTab::Terminal;
}

bool CExtensionBottomPanelTool::RequestOutputChannelSelection(const std::string& channelId) noexcept
{
	return m_impl && !m_impl->closed && m_impl->RequestOutputSelection(channelId);
}

std::optional<std::string> CExtensionBottomPanelTool::SelectedOutputChannelId() const
{
	return m_impl ? m_impl->selectedOutputChannelId : std::nullopt;
}

LRESULT CALLBACK CExtensionBottomPanelTool::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	auto* self = reinterpret_cast<CExtensionBottomPanelTool*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<CExtensionBottomPanelTool*>(create->lpCreateParams);
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
		impl.terminalButton = ::CreateWindowExW(0, L"BUTTON", L"TERMINAL", tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kTerminalButton), instance, nullptr);
		impl.problemsButton = ::CreateWindowExW(0, L"BUTTON", L"PROBLEMS", tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsButton), instance, nullptr);
		impl.outputButton = ::CreateWindowExW(0, L"BUTTON", L"OUTPUT", tabStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputButton), instance, nullptr);
		impl.portsButton = ::CreateWindowExW(0, L"BUTTON", L"PORTS", tabStyle | WS_DISABLED,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPortsButton), instance, nullptr);
		impl.debugConsoleButton = ::CreateWindowExW(0, L"BUTTON", L"DEBUG CONSOLE", tabStyle | WS_DISABLED,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kDebugConsoleButton), instance, nullptr);
		const DWORD actionStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
		impl.maximizeButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPanelMaximizeButton), instance, nullptr);
		impl.closeButton = ::CreateWindowExW(0, L"BUTTON", L"", actionStyle,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPanelCloseButton), instance, nullptr);
		impl.problemsList = ::CreateWindowExW(0, WC_LISTVIEWW, L"",
			WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsList), instance, nullptr);
		ListView_SetExtendedListViewStyle(impl.problemsList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		const std::pair<const wchar_t*, int> columns[] = {
			{ L"Severity", 90 }, { L"Message", 360 }, { L"File", 320 }, { L"Source", 120 },
		};
		for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
			LVCOLUMNW column{};
			column.mask = LVCF_TEXT | LVCF_WIDTH;
			column.pszText = const_cast<wchar_t*>(columns[index].first);
			column.cx = columns[index].second;
			ListView_InsertColumn(impl.problemsList, index, &column);
		}
		impl.outputSelector = ::CreateWindowExW(0, L"COMBOBOX", L"",
			WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputSelector), instance, nullptr);
		impl.outputText = ::CreateWindowExW(0, L"EDIT", L"",
			WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputText), instance, nullptr);
		impl.UpdateActionVisibility();
		impl.ApplyControlPalette();
		impl.ApplyActiveTab(ExtensionBottomPanelTab::Terminal);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case kTerminalButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Terminal); return 0;
		case kProblemsButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Problems); return 0;
		case kOutputButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Output); return 0;
		case kPortsButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Ports); return 0;
		case kDebugConsoleButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::DebugConsole); return 0;
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
			if (IsPanelTabId(item.CtlID) || IsPanelActionId(item.CtlID)) {
				impl.DrawOwnerButton(item);
				return TRUE;
			}
			if (item.CtlID == kOutputSelector) {
				impl.DrawOutputItem(item);
				return TRUE;
			}
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
		const HDC dc = ::BeginPaint(window, &paint);
		RECT client{};
		::GetClientRect(window, &client);
		const HBRUSH brush = ::CreateSolidBrush(impl.palette.bottomPanel.ToColorRef());
		::FillRect(dc, &client, brush);
		::DeleteObject(brush);
		const int tabBottom = Scale(34, impl.dpi);
		if (impl.terminalHeaderBounds.right > impl.terminalHeaderBounds.left) {
			impl.terminal->PaintPanelHeader(dc, impl.terminalHeaderBounds, impl.dpi);
		}
		RECT separator{ client.left, std::max<LONG>(client.top, static_cast<LONG>(tabBottom - 1)),
			client.right, static_cast<LONG>(tabBottom) };
		const HBRUSH separatorBrush = ::CreateSolidBrush(impl.palette.border.ToColorRef());
		if (separatorBrush) {
			::FillRect(dc, &separator, separatorBrush);
			::DeleteObject(separatorBrush);
		}
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_NCDESTROY:
		impl.DestroyBrushes();
		impl.window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::extension
