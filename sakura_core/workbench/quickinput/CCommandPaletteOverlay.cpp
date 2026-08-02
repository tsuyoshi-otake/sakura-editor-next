/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/quickinput/CCommandPaletteOverlay.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <string>
#include <utility>

namespace workbench::quickinput {
namespace {

constexpr wchar_t kWindowClassName[] = L"SakuraEditor.Next.CommandPaletteOverlay";

constexpr COLORREF kFallbackPanel = RGB(37, 37, 38);

void SetControlFont(HWND control, HFONT font) noexcept
{
	if (control != nullptr && font != nullptr) {
		::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}
}

void FillWithColor(HDC dc, const RECT& bounds, COLORREF color) noexcept
{
	const HBRUSH brush = ::CreateSolidBrush(color);
	if (brush != nullptr) {
		::FillRect(dc, &bounds, brush);
		::DeleteObject(brush);
	}
}

[[nodiscard]] bool PaintFontGlyph(
	HDC dc,
	const workbench::icons::IconRect& box,
	HFONT font,
	wchar_t glyph,
	COLORREF color
) noexcept
{
	if (dc == nullptr || font == nullptr || glyph == L'\0' || box.Width() <= 0 || box.Height() <= 0) {
		return false;
	}
	const int saved = ::SaveDC(dc);
	if (saved == 0) return false;
	const HGDIOBJ oldFont = ::SelectObject(dc, font);
	if (oldFont == nullptr || oldFont == HGDI_ERROR) {
		::RestoreDC(dc, saved);
		return false;
	}
	::SetBkMode(dc, TRANSPARENT);
	::SetTextColor(dc, color);
	RECT glyphRect{ box.left, box.top, box.right, box.bottom };
	const wchar_t text[] = { glyph, L'\0' };
	const int drawn = ::DrawTextW(dc, text, 1, &glyphRect,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP | DT_NOPREFIX);
	::RestoreDC(dc, saved);
	return drawn != 0;
}

} // namespace

CCommandPaletteOverlay::~CCommandPaletteOverlay() noexcept
{
	Destroy();
}

ATOM CCommandPaletteOverlay::RegisterWindowClass() noexcept
{
	static const ATOM atom = []() noexcept {
		WNDCLASSEXW windowClass{};
		windowClass.cbSize = sizeof(windowClass);
		windowClass.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc = &CCommandPaletteOverlay::WindowProc;
		windowClass.hInstance = ::GetModuleHandleW(nullptr);
		windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
		windowClass.lpszClassName = kWindowClassName;
		const ATOM registered = ::RegisterClassExW(&windowClass);
		if (registered != 0) return registered;
		if (::GetLastError() == ERROR_CLASS_ALREADY_EXISTS) return static_cast<ATOM>(1);
		return static_cast<ATOM>(0);
	}();
	return atom;
}

bool CCommandPaletteOverlay::Create(HWND parent) noexcept
{
	if (m_window != nullptr) return m_parent == parent;
	if (parent == nullptr || ::IsWindow(parent) == FALSE || RegisterWindowClass() == 0) return false;

	m_parent = parent;
	(void)m_font.RecreateForWindow(theme::ThemeFontKind::Chrome, parent);
	RebuildBrushes();
	m_window = ::CreateWindowExW(
		WS_EX_CONTROLPARENT,
		kWindowClassName,
		L"",
		WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP,
		0, 0, 0, 0,
		parent,
		nullptr,
		::GetModuleHandleW(nullptr),
		this);
	if (m_window == nullptr) {
		ResetBrushes();
		m_parent = nullptr;
		return false;
	}
	return true;
}

void CCommandPaletteOverlay::Destroy() noexcept
{
	if (m_window != nullptr && ::IsWindow(m_window)) {
		::DestroyWindow(m_window);
	}
	m_window = nullptr;
	m_parent = nullptr;
	m_prompt = nullptr;
	m_input = nullptr;
	m_list = nullptr;
	m_close = nullptr;
	m_empty = nullptr;
	m_previousFocus = nullptr;
	m_items.clear();
	ResetBrushes();
	m_font.Reset();
	ReleaseCodiconFont();
}

bool CCommandPaletteOverlay::Show(std::vector<CommandPaletteItem> items)
{
	if (m_window == nullptr || !::IsWindow(m_window) || items.empty()) return false;

	if (!IsVisible()) {
		m_previousFocus = ::GetFocus();
		if (IsPaletteTarget(m_window, m_previousFocus)) m_previousFocus = m_parent;
		if (m_input != nullptr) ::SetWindowTextW(m_input, L"");
	}
	m_items = std::move(items);
	PopulateList();
	Layout();
	::SetWindowPos(m_window, HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	::ShowWindow(m_window, SW_SHOWNOACTIVATE);
	::SetFocus(m_input);
	::InvalidateRect(m_window, nullptr, FALSE);
	return true;
}

void CCommandPaletteOverlay::Hide() noexcept
{
	if (m_window == nullptr || !::IsWindow(m_window)) return;
	::ShowWindow(m_window, SW_HIDE);
	RestoreFocus();
}

bool CCommandPaletteOverlay::IsVisible() const noexcept
{
	return m_window != nullptr && ::IsWindowVisible(m_window) != FALSE;
}

bool CCommandPaletteOverlay::PreTranslateMessage(MSG& message) noexcept
{
	if (!IsVisible() || !IsPaletteTarget(m_window, message.hwnd)) return false;

	if (message.message == WM_KEYDOWN) {
		const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
		if (message.wParam == L'P' && control && shift && !alt) return true;
		if (message.wParam == VK_ESCAPE) {
			Cancel();
			return true;
		}
		if (message.wParam == VK_RETURN) {
			Accept();
			return true;
		}
		if (!control && !shift && !alt) {
			if (message.wParam == VK_UP) {
				MoveSelection(-1);
				return true;
			}
			if (message.wParam == VK_DOWN) {
				MoveSelection(1);
				return true;
			}
		}
	}

	return ::IsDialogMessageW(m_window, &message) != FALSE;
}

void CCommandPaletteOverlay::Layout() noexcept
{
	if (m_window == nullptr || m_parent == nullptr || !::IsWindow(m_parent)) return;
	RECT parentClient{};
	if (::GetClientRect(m_parent, &parentClient) == FALSE) return;
	const int parentWidth = parentClient.right - parentClient.left;
	const int parentHeight = parentClient.bottom - parentClient.top;
	if (parentWidth <= 0 || parentHeight <= 0) return;

	const int maximumWidth = Scale(680);
	const int maximumHeight = Scale(480);
	const int minimumWidth = Scale(320);
	const int minimumHeight = Scale(220);
	const int margin = Scale(16);
	const int width = (std::min)(maximumWidth, (std::max)(minimumWidth, parentWidth - margin * 2));
	const int height = (std::min)(maximumHeight, (std::max)(minimumHeight, parentHeight - margin * 2));
	const int x = (std::max)(0, (parentWidth - width) / 2);
	const int y = (std::max)(Scale(10), (parentHeight - height) / 6);
	::SetWindowPos(m_window, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
	Layout(width, height);
}

void CCommandPaletteOverlay::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	RebuildBrushes();
	if (m_window != nullptr) {
		::InvalidateRect(m_window, nullptr, FALSE);
		if (m_input != nullptr) ::InvalidateRect(m_input, nullptr, TRUE);
		if (m_list != nullptr) ::InvalidateRect(m_list, nullptr, TRUE);
	}
}

void CCommandPaletteOverlay::SetSearchCallback(SearchCallback callback)
{
	m_searchCallback = std::move(callback);
}

void CCommandPaletteOverlay::SetAcceptCallback(AcceptCallback callback)
{
	m_acceptCallback = std::move(callback);
}

void CCommandPaletteOverlay::SetCancelCallback(CancelCallback callback)
{
	m_cancelCallback = std::move(callback);
}

LRESULT CALLBACK CCommandPaletteOverlay::WindowProc(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	auto* self = reinterpret_cast<CCommandPaletteOverlay*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_NCCREATE) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		self = static_cast<CCommandPaletteOverlay*>(create->lpCreateParams);
		self->m_window = window;
		::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (self != nullptr) return self->HandleMessage(message, wParam, lParam);
	return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CCommandPaletteOverlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	switch (message) {
	case WM_CREATE: {
		const HINSTANCE instance = ::GetModuleHandleW(nullptr);
		m_prompt = ::CreateWindowExW(
			0, L"STATIC", L">",
			WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
			0, 0, 0, 0, m_window, nullptr, instance, nullptr);
		m_input = ::CreateWindowExW(
			0, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputControl)), instance, nullptr);
		m_list = ::CreateWindowExW(
			0, L"LISTBOX", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED
				| LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListControl)), instance, nullptr);
		m_empty = ::CreateWindowExW(
			0, L"STATIC", L"No commands found",
			WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEmptyControl)), instance, nullptr);
		m_close = ::CreateWindowExW(
			0, L"BUTTON", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseControl)), instance, nullptr);
		if (m_prompt == nullptr || m_input == nullptr || m_list == nullptr
			|| m_empty == nullptr || m_close == nullptr) {
			return -1;
		}
		::SendMessageW(m_input, EM_SETLIMITTEXT, 4096, 0);
		::SendMessageW(m_input, EM_SETCUEBANNER, FALSE,
			reinterpret_cast<LPARAM>(L"Type to search commands"));
		const HFONT font = ControlFont(m_font.Get());
		SetControlFont(m_prompt, font);
		SetControlFont(m_input, font);
		SetControlFont(m_list, font);
		SetControlFont(m_empty, font);
		SetControlFont(m_close, font);
		RECT client{};
		::GetClientRect(m_window, &client);
		Layout(client.right, client.bottom);
		return 0;
	}
	case WM_SIZE:
		Layout(LOWORD(lParam), HIWORD(lParam));
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(m_window, &paint);
		if (dc != nullptr) Paint(dc, paint.rcPaint);
		::EndPaint(m_window, &paint);
		return 0;
	}
	case WM_MEASUREITEM: {
		auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
		if (measure != nullptr && measure->CtlID == kListControl) {
			measure->itemHeight = static_cast<UINT>(Scale(44));
			return TRUE;
		}
		break;
	}
	case WM_DRAWITEM: {
		const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
		if (draw == nullptr) break;
		if (draw->CtlID == kListControl) {
			DrawItem(*draw);
			return TRUE;
		}
		if (draw->CtlID == kCloseControl) {
			DrawCloseButton(*draw);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLORSTATIC: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetBkMode(dc, TRANSPARENT);
		::SetTextColor(dc, m_palette.secondaryText.ToColorRef());
		return reinterpret_cast<LRESULT>(m_panelBrush != nullptr
			? m_panelBrush : ::GetSysColorBrush(COLOR_WINDOW));
	}
	case WM_CTLCOLOREDIT: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetBkMode(dc, OPAQUE);
		::SetBkColor(dc, m_palette.raised.ToColorRef());
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		return reinterpret_cast<LRESULT>(m_inputBrush != nullptr
			? m_inputBrush : ::GetSysColorBrush(COLOR_WINDOW));
	}
	case WM_CTLCOLORLISTBOX: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetBkMode(dc, OPAQUE);
		::SetBkColor(dc, m_palette.panel.ToColorRef());
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		return reinterpret_cast<LRESULT>(m_panelBrush != nullptr
			? m_panelBrush : ::GetSysColorBrush(COLOR_WINDOW));
	}
	case WM_COMMAND: {
		const int control = LOWORD(wParam);
		const int notification = HIWORD(wParam);
		if (control == kInputControl && notification == EN_CHANGE) {
			UpdateSearch();
			return 0;
		}
		if (control == kListControl && notification == LBN_DBLCLK) {
			Accept();
			return 0;
		}
		if (control == kCloseControl) {
			Cancel();
			return 0;
		}
		break;
	}
	case WM_LBUTTONDOWN:
		::SetFocus(m_input);
		return 0;
	case WM_CLOSE:
		Cancel();
		return 0;
	case WM_DESTROY:
		m_prompt = nullptr;
		m_input = nullptr;
		m_list = nullptr;
		m_close = nullptr;
		m_empty = nullptr;
		return 0;
	case WM_NCDESTROY:
		::SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
		m_window = nullptr;
		return 0;
	}
	return ::DefWindowProcW(m_window, message, wParam, lParam);
}

void CCommandPaletteOverlay::Layout(int width, int height) noexcept
{
	if (m_window == nullptr) return;
	const int margin = Scale(12);
	const int rowHeight = Scale(38);
	const int promptWidth = Scale(26);
	const int closeWidth = Scale(30);
	const int gap = Scale(6);
	const int listTop = margin + rowHeight + gap;
	const int listBottom = (std::max)(listTop + Scale(60), height - margin);
	const int listWidth = (std::max)(0, width - margin * 2);

	::MoveWindow(m_prompt, margin, margin, promptWidth, rowHeight, TRUE);
	::MoveWindow(m_input, margin + promptWidth + gap, margin,
		(std::max)(0, width - margin * 2 - promptWidth - closeWidth - gap * 2), rowHeight, TRUE);
	::MoveWindow(m_close, (std::max)(margin, width - margin - closeWidth), margin,
		closeWidth, rowHeight, TRUE);
	::MoveWindow(m_list, margin, listTop, listWidth, (std::max)(0, listBottom - listTop), TRUE);
	::MoveWindow(m_empty, margin, listTop, listWidth, (std::max)(0, listBottom - listTop), TRUE);
}

void CCommandPaletteOverlay::PopulateList() noexcept
{
	if (m_list == nullptr) return;
	::SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
	for (const auto& item : m_items) {
		::SendMessageW(m_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
	}
	int selected = -1;
	for (std::size_t index = 0; index < m_items.size(); ++index) {
		if (m_items[index].enabled) {
			selected = static_cast<int>(index);
			break;
		}
	}
	if (selected >= 0) ::SendMessageW(m_list, LB_SETCURSEL, selected, 0);
	if (m_empty != nullptr) ::ShowWindow(m_empty, m_items.empty() ? SW_SHOW : SW_HIDE);
	::InvalidateRect(m_list, nullptr, TRUE);
}

void CCommandPaletteOverlay::UpdateSearch() noexcept
{
	if (m_input == nullptr) return;
	const std::wstring query = ReadWindowText(m_input);
	if (!m_searchCallback) {
		PopulateList();
		return;
	}
	try {
		m_items = m_searchCallback(query);
	}
	catch (...) {
		m_items.clear();
	}
	PopulateList();
}

void CCommandPaletteOverlay::MoveSelection(int direction) noexcept
{
	if (m_list == nullptr || m_items.empty() || direction == 0) return;
	const int count = static_cast<int>(m_items.size());
	int current = static_cast<int>(::SendMessageW(m_list, LB_GETCURSEL, 0, 0));
	if (current < 0 || current >= count) current = direction > 0 ? -1 : count;
	for (int step = 0; step < count; ++step) {
		current += direction;
		if (current < 0) current = count - 1;
		if (current >= count) current = 0;
		if (m_items[static_cast<std::size_t>(current)].enabled) {
			::SendMessageW(m_list, LB_SETCURSEL, current, 0);
			::InvalidateRect(m_list, nullptr, FALSE);
			return;
		}
	}
}

void CCommandPaletteOverlay::Accept() noexcept
{
	if (!IsVisible() || m_list == nullptr) return;
	const int selected = static_cast<int>(::SendMessageW(m_list, LB_GETCURSEL, 0, 0));
	if (selected < 0 || static_cast<std::size_t>(selected) >= m_items.size()
		|| !m_items[static_cast<std::size_t>(selected)].enabled) {
		return;
	}
	std::wstring commandId = m_items[static_cast<std::size_t>(selected)].id;
	Hide();
	if (m_acceptCallback) m_acceptCallback(std::move(commandId));
}

void CCommandPaletteOverlay::Cancel() noexcept
{
	if (!IsVisible()) return;
	Hide();
	if (m_cancelCallback) m_cancelCallback();
}

void CCommandPaletteOverlay::RestoreFocus() noexcept
{
	const HWND restore = m_previousFocus;
	m_previousFocus = nullptr;
	if (restore != nullptr && ::IsWindow(restore) && !IsPaletteTarget(m_window, restore)) {
		::SetFocus(restore);
	} else if (m_parent != nullptr && ::IsWindow(m_parent)) {
		::SetFocus(m_parent);
	}
}

void CCommandPaletteOverlay::Paint(HDC dc, const RECT& bounds) noexcept
{
	const COLORREF panelColor = m_palette.panel.ToColorRef();
	if (m_panelBrush != nullptr) {
		::FillRect(dc, &bounds, m_panelBrush);
	} else {
		FillWithColor(dc, bounds, panelColor == 0 ? kFallbackPanel : panelColor);
	}
	const HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef());
	if (border != nullptr) {
		RECT frame{};
		::GetClientRect(m_window, &frame);
		::FrameRect(dc, &frame, border);
		::DeleteObject(border);
	}
}

void CCommandPaletteOverlay::DrawItem(const DRAWITEMSTRUCT& draw) noexcept
{
	RECT bounds = draw.rcItem;
	const bool valid = draw.itemID < m_items.size();
	const bool selected = (draw.itemState & ODS_SELECTED) != 0;
	const bool enabled = valid && m_items[draw.itemID].enabled;
	const COLORREF background = selected ? m_palette.accent.ToColorRef() : m_palette.panel.ToColorRef();
	FillWithColor(draw.hDC, bounds, background);
	if (!valid) return;

	const auto& item = m_items[draw.itemID];
	const int padding = Scale(8);
	const int lineHeight = Scale(17);
	RECT label = bounds;
	label.left += padding;
	label.right -= padding;
	label.top += Scale(3);
	label.bottom = label.top + lineHeight;
	RECT detail = label;
	detail.top += lineHeight;
	detail.bottom = detail.top + lineHeight;
	const COLORREF labelColor = selected
		? m_palette.highlightText.ToColorRef()
		: (enabled ? m_palette.primaryText.ToColorRef() : m_palette.disabledText.ToColorRef());
	const COLORREF detailColor = selected
		? m_palette.highlightText.ToColorRef()
		: (enabled ? m_palette.descriptionText.ToColorRef() : m_palette.disabledText.ToColorRef());
	::SetBkMode(draw.hDC, TRANSPARENT);
	::SetTextColor(draw.hDC, labelColor);
	const HFONT oldFont = reinterpret_cast<HFONT>(::SelectObject(draw.hDC, ControlFont(m_font.Get())));
	::DrawTextW(draw.hDC, item.label.c_str(), -1, &label,
		DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
	::SetTextColor(draw.hDC, detailColor);
	if (!item.detail.empty()) {
		::DrawTextW(draw.hDC, item.detail.c_str(), -1, &detail,
			DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
	}
	if (oldFont != nullptr) ::SelectObject(draw.hDC, oldFont);
}

void CCommandPaletteOverlay::DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept
{
	const bool selected = (draw.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
	FillWithColor(draw.hDC, draw.rcItem,
		selected ? m_palette.raised.ToColorRef() : m_palette.panel.ToColorRef());
	const int width = static_cast<int>(draw.rcItem.right - draw.rcItem.left);
	const int height = static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
	const int side = (std::min)(Scale(18), (std::min)(width, height));
	const workbench::icons::IconRect iconBox{
		draw.rcItem.left + (draw.rcItem.right - draw.rcItem.left - side) / 2,
		draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - side) / 2,
		draw.rcItem.left + (draw.rcItem.right - draw.rcItem.left - side) / 2 + side,
		draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - side) / 2 + side };
	const auto glyph = workbench::icons::FindCodiconGlyph(L"close");
	const COLORREF color = m_palette.secondaryText.ToColorRef();
	if (!PaintFontGlyph(draw.hDC, iconBox, AcquireCodiconFont(side), glyph.value_or(L'\0'), color)) {
		workbench::icons::codicons::Draw(draw.hDC, iconBox,
			workbench::icons::codicons::Icon::Close, color);
	}
}

HFONT CCommandPaletteOverlay::AcquireCodiconFont(int height) noexcept
{
	if (height <= 0) return nullptr;
	const auto faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	if (faceName.empty() || faceName.size() >= LF_FACESIZE) {
		ReleaseCodiconFont();
		return nullptr;
	}
	if (m_codiconFont != nullptr && m_codiconFontHeight == height) return m_codiconFont;

	ReleaseCodiconFont();
	LOGFONTW logFont{};
	logFont.lfHeight = -height;
	logFont.lfWeight = FW_NORMAL;
	logFont.lfCharSet = DEFAULT_CHARSET;
	logFont.lfOutPrecision = OUT_TT_PRECIS;
	logFont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	logFont.lfQuality = CLEARTYPE_QUALITY;
	logFont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	std::copy(faceName.begin(), faceName.end(), logFont.lfFaceName);
	logFont.lfFaceName[faceName.size()] = L'\0';
	m_codiconFont = ::CreateFontIndirectW(&logFont);
	if (m_codiconFont != nullptr) m_codiconFontHeight = height;
	return m_codiconFont;
}

void CCommandPaletteOverlay::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

void CCommandPaletteOverlay::RebuildBrushes() noexcept
{
	ResetBrushes();
	m_panelBrush = ::CreateSolidBrush(m_palette.panel.ToColorRef());
	m_inputBrush = ::CreateSolidBrush(m_palette.raised.ToColorRef());
}

void CCommandPaletteOverlay::ResetBrushes() noexcept
{
	if (m_panelBrush != nullptr) {
		::DeleteObject(m_panelBrush);
		m_panelBrush = nullptr;
	}
	if (m_inputBrush != nullptr) {
		::DeleteObject(m_inputBrush);
		m_inputBrush = nullptr;
	}
}

int CCommandPaletteOverlay::Scale(int value) const noexcept
{
	const UINT dpi = m_window != nullptr ? ::GetDpiForWindow(m_window) : USER_DEFAULT_SCREEN_DPI;
	return ::MulDiv(value, static_cast<int>(dpi != 0 ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

bool CCommandPaletteOverlay::IsPaletteTarget(HWND palette, HWND target) noexcept
{
	return palette != nullptr && target != nullptr
		&& (target == palette || ::IsChild(palette, target) != FALSE);
}

std::wstring CCommandPaletteOverlay::ReadWindowText(HWND window)
{
	if (window == nullptr) return {};
	const int length = ::GetWindowTextLengthW(window);
	if (length <= 0) return {};
	std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
	const int copied = ::GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
	text.resize(static_cast<std::size_t>((std::max)(0, copied)));
	return text;
}

HFONT CCommandPaletteOverlay::ControlFont(HFONT fallback) noexcept
{
	return fallback != nullptr ? fallback : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

} // namespace workbench::quickinput
