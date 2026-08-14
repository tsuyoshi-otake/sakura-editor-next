/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/quickinput/CQuickInputDialog.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"SakuraQuickInput";
constexpr int kInputControl = 100;

int Scale(HWND window, int value) noexcept
{
	const UINT dpi = window ? ::GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
	return ::MulDiv(value, static_cast<int>(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

void SetControlFont(HWND control) noexcept
{
	if (control) ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

bool EnsureWindowClass(HINSTANCE instance) noexcept
{
	WNDCLASSEXW existing{};
	existing.cbSize = sizeof(existing);
	if (::GetClassInfoExW(instance, kWindowClass, &existing)) return true;
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = CQuickInputDialog::WindowProcedure;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
	windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	windowClass.lpszClassName = kWindowClass;
	return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

CQuickInputDialog::CQuickInputDialog(const SQuickInputRequest& request)
	: m_request(request)
{
}

SQuickInputCompletion CQuickInputDialog::DoModal(HWND parent) noexcept
{
	if (!parent || !::IsWindow(parent)) {
		m_completion.state = EQuickInputState::HostLost;
		return m_completion;
	}
	if (!Create(parent)) {
		m_completion.state = EQuickInputState::Overloaded;
		return m_completion;
	}
	const BOOL parentWasEnabled = ::IsWindowEnabled(parent);
	if (parentWasEnabled) ::EnableWindow(parent, FALSE);
	::ShowWindow(m_window, SW_SHOW);
	::UpdateWindow(m_window);
	MSG message{};
	bool sawQuit = false;
	while (m_window && ::IsWindow(m_window)) {
		const BOOL read = ::GetMessageW(&message, nullptr, 0, 0);
		if (read <= 0) {
			sawQuit = read == 0;
			break;
		}
		if (!::IsDialogMessageW(m_window, &message)) {
			::TranslateMessage(&message);
			::DispatchMessageW(&message);
		}
	}
	if (parentWasEnabled && ::IsWindow(parent)) {
		::EnableWindow(parent, TRUE);
		::SetActiveWindow(parent);
	}
	if (sawQuit) ::PostQuitMessage(static_cast<int>(message.wParam));
	return m_completion;
}

bool CQuickInputDialog::Create(HWND parent) noexcept
{
	m_parent = parent;
	const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(parent, GWLP_HINSTANCE));
	if (!EnsureWindowClass(instance)) return false;
	RECT parentRect{};
	::GetWindowRect(parent, &parentRect);
	const int width = Scale(parent, 560);
	const int height = Scale(parent, m_request.kind == EQuickInputKind::QuickPick ? 410 : 190);
	const int x = parentRect.left + std::max(0L, (parentRect.right - parentRect.left - width) / 2);
	const int y = parentRect.top + std::max(0L, (parentRect.bottom - parentRect.top - height) / 3);
	const wchar_t* title = m_request.title.empty() ? L"Sakura Editor NEXT" : m_request.title.c_str();
	m_window = ::CreateWindowExW(
		WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
		kWindowClass, title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
		x, y, width, height, parent, nullptr, instance, this);
	return m_window != nullptr;
}

LRESULT CALLBACK CQuickInputDialog::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	auto* self = reinterpret_cast<CQuickInputDialog*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<CQuickInputDialog*>(create->lpCreateParams);
		self->m_window = window;
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	return self ? self->HandleMessage(message, wParam, lParam) : ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CQuickInputDialog::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	switch (message) {
	case WM_CREATE: {
		const auto instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(m_window, GWLP_HINSTANCE));
		m_prompt = ::CreateWindowExW(0, L"STATIC", m_request.placeholder.c_str(), WS_CHILD | WS_VISIBLE,
			0, 0, 0, 0, m_window, nullptr, instance, nullptr);
		if (m_request.kind == EQuickInputKind::QuickPick) {
			DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY;
			if (m_request.canPickMany) style |= LBS_EXTENDEDSEL;
			m_input = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", style,
				0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(kInputControl), instance, nullptr);
			for (std::size_t index = 0; index < m_request.items.size(); ++index) {
				const auto& item = m_request.items[index];
				std::wstring text = item.label;
				if (!item.description.empty()) text += L"  —  " + item.description;
				::SendMessageW(m_input, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
				if (item.picked) {
					::SendMessageW(m_input, m_request.canPickMany ? LB_SETSEL : LB_SETCURSEL,
						m_request.canPickMany ? TRUE : static_cast<WPARAM>(index),
						m_request.canPickMany ? static_cast<LPARAM>(index) : 0);
				}
			}
			if (!m_request.items.empty() && ::SendMessageW(m_input, LB_GETCURSEL, 0, 0) == LB_ERR) {
				::SendMessageW(m_input, LB_SETCURSEL, 0, 0);
			}
		} else {
			DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
			if (m_request.password) style |= ES_PASSWORD;
			m_input = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_request.value.c_str(), style,
				0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(kInputControl), instance, nullptr);
			::SendMessageW(m_input, EM_SETLIMITTEXT, 1024 * 1024, 0);
			::SendMessageW(m_input, EM_SETSEL, 0, -1);
		}
		m_ok = ::CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(IDOK), instance, nullptr);
		m_cancel = ::CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(IDCANCEL), instance, nullptr);
		SetControlFont(m_prompt);
		SetControlFont(m_input);
		SetControlFont(m_ok);
		SetControlFont(m_cancel);
		RECT client{};
		::GetClientRect(m_window, &client);
		Layout(client.right, client.bottom);
		::SetFocus(m_input);
		return 0;
	}
	case WM_SIZE:
		Layout(LOWORD(lParam), HIWORD(lParam));
		return 0;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK ||
			(LOWORD(wParam) == kInputControl && HIWORD(wParam) == LBN_DBLCLK && !m_request.canPickMany)) {
			Accept();
			return 0;
		}
		if (LOWORD(wParam) == IDCANCEL) {
			Cancel();
			return 0;
		}
		break;
	case WM_CLOSE:
		Cancel();
		return 0;
	case WM_DESTROY:
		m_window = nullptr;
		return 0;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CQuickInputDialog::Layout(int width, int height) noexcept
{
	if (!m_window) return;
	const int margin = Scale(m_window, 14);
	const int promptHeight = Scale(m_window, 24);
	const int buttonWidth = Scale(m_window, 88);
	const int buttonHeight = Scale(m_window, 28);
	const int gap = Scale(m_window, 8);
	::MoveWindow(m_prompt, margin, margin, std::max(0, width - margin * 2), promptHeight, TRUE);
	const int inputTop = margin + promptHeight;
	const int inputBottom = height - margin - buttonHeight - gap;
	::MoveWindow(m_input, margin, inputTop, std::max(0, width - margin * 2),
		std::max(Scale(m_window, 26), inputBottom - inputTop), TRUE);
	::MoveWindow(m_cancel, width - margin - buttonWidth, height - margin - buttonHeight,
		buttonWidth, buttonHeight, TRUE);
	::MoveWindow(m_ok, width - margin * 2 - buttonWidth * 2, height - margin - buttonHeight,
		buttonWidth, buttonHeight, TRUE);
}

void CQuickInputDialog::Accept() noexcept
{
	if (!m_window || !m_input) return;
	m_completion.selectedIndices.clear();
	m_completion.value.reset();
	if (m_request.kind == EQuickInputKind::QuickPick) {
		if (m_request.canPickMany) {
			const auto count = static_cast<int>(::SendMessageW(m_input, LB_GETSELCOUNT, 0, 0));
			if (count > 0) {
				std::vector<int> selected(static_cast<std::size_t>(count));
				const auto copied = static_cast<int>(::SendMessageW(m_input, LB_GETSELITEMS,
					static_cast<WPARAM>(selected.size()), reinterpret_cast<LPARAM>(selected.data())));
				for (int index = 0; index < copied; ++index) {
					if (selected[index] >= 0) m_completion.selectedIndices.push_back(static_cast<std::size_t>(selected[index]));
				}
			}
		} else {
			const auto selected = static_cast<int>(::SendMessageW(m_input, LB_GETCURSEL, 0, 0));
			if (selected == LB_ERR) return;
			m_completion.selectedIndices.push_back(static_cast<std::size_t>(selected));
		}
	} else {
		const int length = ::GetWindowTextLengthW(m_input);
		std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
		const int copied = ::GetWindowTextW(m_input, value.data(), static_cast<int>(value.size()));
		value.resize(static_cast<std::size_t>(std::max(0, copied)));
		m_completion.value = std::move(value);
	}
	m_completion.state = EQuickInputState::Accepted;
	::DestroyWindow(m_window);
}

void CQuickInputDialog::Cancel() noexcept
{
	m_completion.state = EQuickInputState::Cancelled;
	if (m_window) ::DestroyWindow(m_window);
}
