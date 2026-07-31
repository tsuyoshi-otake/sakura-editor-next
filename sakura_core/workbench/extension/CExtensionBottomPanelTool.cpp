/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/extension/CExtensionBottomPanelTool.h"

#include <CommCtrl.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace workbench::extension {
namespace {

constexpr wchar_t kWindowClass[] = L"SakuraExtensionBottomPanel";
constexpr UINT_PTR kTerminalButton = 100;
constexpr UINT_PTR kProblemsButton = 101;
constexpr UINT_PTR kOutputButton = 102;
constexpr UINT_PTR kProblemsList = 103;
constexpr UINT_PTR kOutputSelector = 104;
constexpr UINT_PTR kOutputText = 105;
constexpr unsigned int kDefaultDpi = 96;

int Scale(int value, unsigned int dpi) noexcept
{
	return ::MulDiv(value, static_cast<int>(dpi ? dpi : kDefaultDpi), kDefaultDpi);
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
	HWND problemsList = nullptr;
	HWND outputSelector = nullptr;
	HWND outputText = nullptr;
	RECT bounds{};
	unsigned int dpi = kDefaultDpi;
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);
	theme::CThemeFont font;
	ExtensionBottomPanelTab active = ExtensionBottomPanelTab::Terminal;
	ProblemActivationCallback problemActivation;
	OutputChannelSelectionCallback outputChannelSelection;
	TabSelectionCallback tabSelection;
	win32::ProblemsPanelSnapshot problems;
	win32::OutputPanelSnapshot outputs;
	std::optional<std::string> selectedOutputChannelId;
	bool closed = false;

	void ApplyFont(HWND control) const
	{
		if (control && font.Get()) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font.Get()), TRUE);
	}

	void ApplyActiveTab(ExtensionBottomPanelTab tab)
	{
		if (closed) return;
		active = tab;
		const bool showTerminal = tab == ExtensionBottomPanelTab::Terminal;
		::ShowWindow(terminal->GetHwnd(), showTerminal ? SW_SHOW : SW_HIDE);
		::ShowWindow(problemsList, tab == ExtensionBottomPanelTab::Problems ? SW_SHOW : SW_HIDE);
		::ShowWindow(outputSelector, tab == ExtensionBottomPanelTab::Output ? SW_SHOW : SW_HIDE);
		::ShowWindow(outputText, tab == ExtensionBottomPanelTab::Output ? SW_SHOW : SW_HIDE);
		::SendMessageW(terminalButton, BM_SETSTATE, showTerminal, 0);
		::SendMessageW(problemsButton, BM_SETSTATE, tab == ExtensionBottomPanelTab::Problems, 0);
		::SendMessageW(outputButton, BM_SETSTATE, tab == ExtensionBottomPanelTab::Output, 0);
		if (!showTerminal) terminal->Deactivate();
		LayoutChildren();
	}

	void LayoutChildren()
	{
		if (!window) return;
		const int width = std::max(0L, bounds.right - bounds.left);
		const int height = std::max(0L, bounds.bottom - bounds.top);
		const int tabHeight = Scale(30, dpi);
		const int tabWidth = Scale(105, dpi);
		::MoveWindow(window, bounds.left, bounds.top, width, height, TRUE);
		::MoveWindow(terminalButton, 0, 0, tabWidth, tabHeight, TRUE);
		::MoveWindow(problemsButton, tabWidth, 0, tabWidth, tabHeight, TRUE);
		::MoveWindow(outputButton, tabWidth * 2, 0, tabWidth, tabHeight, TRUE);
		RECT content{ 0, tabHeight, width, height };
		terminal->Layout(content, dpi);
		::MoveWindow(problemsList, 0, tabHeight, width, std::max(0, height - tabHeight), TRUE);
		const int selectorHeight = Scale(28, dpi);
		::MoveWindow(outputSelector, 0, tabHeight, width, selectorHeight, TRUE);
		::MoveWindow(outputText, 0, tabHeight + selectorHeight, width,
			std::max(0, height - tabHeight - selectorHeight), TRUE);
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
		std::wstring label = L"Problems (" + std::to_wstring(problems.entries.size()) + L")";
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
		SelectOutput(static_cast<int>(std::distance(outputs.channels.begin(), selected)));
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
		m_impl->ApplyFont(m_impl->problemsList);
		m_impl->ApplyFont(m_impl->outputSelector);
		m_impl->ApplyFont(m_impl->outputText);
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
	m_impl->terminal->Close();
	if (m_impl->window) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
}

terminal::CTerminalTool* CExtensionBottomPanelTool::Terminal() noexcept
{
	return m_impl ? m_impl->terminal.get() : nullptr;
}

void CExtensionBottomPanelTool::SetPalette(const theme::ThemePalette& palette)
{
	if (!m_impl) return;
	m_impl->palette = palette;
	m_impl->terminal->SetPalette(palette);
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

void CExtensionBottomPanelTool::Refresh() { if (m_impl) m_impl->Refresh(); }
void CExtensionBottomPanelTool::SetActiveTab(ExtensionBottomPanelTab tab)
{
	if (m_impl) m_impl->ApplyActiveTab(tab);
}

bool CExtensionBottomPanelTool::RequestTabSelection(ExtensionBottomPanelTab tab) noexcept
{
	if (!m_impl || m_impl->closed) return false;
	if (m_impl->active == tab) return true;
	if (m_impl->tabSelection) {
		try {
			if (!m_impl->tabSelection(tab)) return false;
		}
		catch (...) {
			return false;
		}
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
	switch (message) {
	case WM_CREATE: {
		const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(window, GWLP_HINSTANCE));
		impl.terminalButton = ::CreateWindowExW(0, L"BUTTON", L"Terminal", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kTerminalButton), instance, nullptr);
		impl.problemsButton = ::CreateWindowExW(0, L"BUTTON", L"Problems (0)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kProblemsButton), instance, nullptr);
		impl.outputButton = ::CreateWindowExW(0, L"BUTTON", L"Output", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputButton), instance, nullptr);
		impl.problemsList = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
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
			WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputSelector), instance, nullptr);
		impl.outputText = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
			0, 0, 0, 0, window, reinterpret_cast<HMENU>(kOutputText), instance, nullptr);
		impl.ApplyActiveTab(ExtensionBottomPanelTab::Terminal);
		return 0;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case kTerminalButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Terminal); return 0;
		case kProblemsButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Problems); return 0;
		case kOutputButton: (void)self->RequestTabSelection(ExtensionBottomPanelTab::Output); return 0;
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
		const HBRUSH brush = ::CreateSolidBrush(impl.palette.panel.ToColorRef());
		::FillRect(dc, &client, brush);
		::DeleteObject(brush);
		::EndPaint(window, &paint);
		return 0;
	}
	case WM_NCDESTROY:
		impl.window = nullptr;
		::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace workbench::extension
