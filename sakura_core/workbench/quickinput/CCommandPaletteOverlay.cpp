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
#include "workbench/icons/LabelRunPainter.h"
#include "CSelectLang.h"
#include "sakura_rc.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace workbench::quickinput {
namespace {

constexpr wchar_t kWindowClassName[] = L"SakuraEditor.Next.CommandPaletteOverlay";

constexpr COLORREF kFallbackPanel = RGB(37, 37, 38);
//! A Quick Input query is a single-line editor.  Keep the chrome row close to
//! the workbench's other compact inputs instead of allocating two text lines.
constexpr int kInputRowHeightDip = 26;
constexpr int kHeaderTopPaddingDip = 6;
constexpr int kHeaderHorizontalPaddingDip = 6;
constexpr int kHeaderBottomPaddingDip = 4;
constexpr int kListScrollablePaddingDip = 6;
constexpr int kListEntryPaddingDip = 6;
constexpr int kQuickPickCompactRowHeightDip = 22;
constexpr int kQuickPickDetailRowHeightDip = 44;
constexpr int kSeparatorRowHeightDip = 30;
//! VS Code's `cornerRadius-xLarge` token is 12px in the 1.134.0 size ramp.
constexpr int kWidgetCornerRadiusDip = 12;
//! The native child cannot paint outside its parent, so the shadow is kept as
//! a themed inner edge while the rounded region clips the four outer corners.
constexpr int kWidgetShadowWidthDip = 2;
constexpr int kInputCornerRadiusDip = 6;
constexpr int kRowCornerRadiusDip = 3;

[[nodiscard]] std::wstring LocalizedString(UINT resourceId, const wchar_t* fallback)
{
	const auto localized = CSelectLang::LoadStringW(resourceId);
	if (!localized.empty()) return std::wstring(localized);
	return fallback != nullptr ? std::wstring(fallback) : std::wstring();
}

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

//! Queue a repaint without asking USER to erase the class background first.
//! The overlay and all of its children paint their complete surfaces, so an
//! erase pass only exposes a blank intermediate frame while the layout moves.
void QueueNoEraseInvalidate(HWND window) noexcept
{
	if (window != nullptr) ::InvalidateRect(window, nullptr, FALSE);
}

//! Complete the first visible frame before returning to the caller. A queued
//! invalidation is insufficient here because the editor, preview, and workbench
//! siblings can repaint after the command handler and temporarily cover a newly
//! shown child. Visibility therefore includes a fully painted overlay subtree.
void PaintOverlayNow(HWND window) noexcept
{
	if (window == nullptr) return;
	(void)::RedrawWindow(window, nullptr, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

//! A child overlay moving, shrinking, or hiding exposes sibling surfaces in
//! its old rectangle. SWP_NOREDRAW/SWP_NOCOPYBITS deliberately avoid copying
//! stale pixels, so the parent owns one synchronous repaint of that rectangle.
void PaintParentRegionNow(HWND parent, const RECT& bounds) noexcept
{
	if (parent == nullptr || ::IsRectEmpty(&bounds) != FALSE) return;
	RECT dirty = bounds;
	(void)::RedrawWindow(parent, &dirty, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_ALLCHILDREN);
}

[[nodiscard]] COLORREF BlendTowardBlack(COLORREF color, int amount) noexcept
{
	const int clamped = (std::clamp)(amount, 0, 255);
	const int red = GetRValue(color);
	const int green = GetGValue(color);
	const int blue = GetBValue(color);
	return RGB(
		red * (255 - clamped) / 255,
		green * (255 - clamped) / 255,
		blue * (255 - clamped) / 255);
}

void FillRoundedRect(HDC dc, const RECT& bounds, int radius, COLORREF color) noexcept
{
	if (dc == nullptr || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	const HBRUSH brush = ::CreateSolidBrush(color);
	const HPEN pen = ::CreatePen(PS_SOLID, 1, color);
	if (brush != nullptr && pen != nullptr) {
		const HGDIOBJ oldBrush = ::SelectObject(dc, brush);
		const HGDIOBJ oldPen = ::SelectObject(dc, pen);
		(void)::RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
			radius, radius);
		if (oldPen != nullptr && oldPen != HGDI_ERROR) ::SelectObject(dc, oldPen);
		if (oldBrush != nullptr && oldBrush != HGDI_ERROR) ::SelectObject(dc, oldBrush);
	}
	if (pen != nullptr) ::DeleteObject(pen);
	if (brush != nullptr) ::DeleteObject(brush);
}

void FrameRoundedRect(HDC dc, const RECT& bounds, int radius, COLORREF color) noexcept
{
	if (dc == nullptr || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
	const HPEN pen = ::CreatePen(PS_SOLID, 1, color);
	if (pen == nullptr) return;
	const HGDIOBJ oldPen = ::SelectObject(dc, pen);
	const HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
	(void)::RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
		radius, radius);
	if (oldBrush != nullptr && oldBrush != HGDI_ERROR) ::SelectObject(dc, oldBrush);
	if (oldPen != nullptr && oldPen != HGDI_ERROR) ::SelectObject(dc, oldPen);
	::DeleteObject(pen);
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
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
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
	if (m_list != nullptr && ::IsWindow(m_list)) {
		(void)::RemoveWindowSubclass(m_list, &CCommandPaletteOverlay::ListSubclassProc,
			static_cast<UINT_PTR>(kListControl));
	}
	if (m_input != nullptr && ::IsWindow(m_input)) {
		(void)::RemoveWindowSubclass(m_input, &CCommandPaletteOverlay::InputSubclassProc,
			static_cast<UINT_PTR>(kInputControl));
	}
	m_overlayScrollbar.Destroy();
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
	m_inputFrame = {};
	m_inputLineHeight = 0;
	m_inputLineHeightDpi = 0;
	m_inputMode = false;
	m_suppressInputChange = false;
	m_inputPrompt.clear();
	m_inputPlaceholder.clear();
	m_rowPixelOffsets.clear();
	m_rowPixelOffsetsDpi = 0;
	m_wheelDeltaRemainder = 0;
	m_lastSelectableIndex = -1;
	m_repairingSelection = false;
	m_terminalCallbackInProgress = false;
	m_items.clear();
	m_lastNotifiedSelectionId.clear();
	m_selectionNotificationsEnabled = true;
	m_stringsCallback = {};
	m_searchCallback = {};
	m_selectionCallback = {};
	m_acceptCallback = {};
	m_cancelCallback = {};
	ResetBrushes();
	m_font.Reset();
	ReleaseCodiconFont();
}

bool CCommandPaletteOverlay::Show(
	std::vector<CommandPaletteItem> items,
	std::wstring_view initiallySelectedId)
{
	if (m_window == nullptr || !::IsWindow(m_window) || items.empty()) return false;

	if (!IsVisible()) {
		m_previousFocus = ::GetFocus();
		if (IsPaletteTarget(m_window, m_previousFocus)) m_previousFocus = m_parent;
	}
	m_inputMode = false;
	m_suppressInputChange = true;
	m_inputPrompt.clear();
	m_inputPlaceholder.clear();
	m_selectionNotificationsEnabled = false;
	m_lastNotifiedSelectionId.clear();
	m_items = std::move(items);
	if (m_input != nullptr) {
		::SetWindowTextW(m_input, L">");
		::SendMessageW(m_input, EM_SETSEL, 1, 1);
	}
	if (m_prompt != nullptr) {
		::ShowWindow(m_prompt, SW_HIDE);
	}
	if (m_close != nullptr) {
		::ShowWindow(m_close, SW_HIDE);
	}
	m_suppressInputChange = false;
	RefreshStrings();
	if (m_list != nullptr) ::ShowWindow(m_list, SW_SHOW);
	if (m_empty != nullptr) ::ShowWindow(m_empty, SW_HIDE);
	PopulateList(initiallySelectedId);
	Layout();
	::SetWindowPos(m_window, HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
		| SWP_NOREDRAW | SWP_NOCOPYBITS);
	::ShowWindow(m_window, SW_SHOWNOACTIVATE);
	::SetFocus(m_input);
	PaintOverlayNow(m_window);
	m_selectionNotificationsEnabled = true;
	NotifySelectionChanged();
	return true;
}

bool CCommandPaletteOverlay::ShowInput(
	std::wstring_view prompt, std::wstring_view placeholder, std::wstring_view value)
{
	if (m_window == nullptr || !::IsWindow(m_window)) return false;

	if (!IsVisible()) {
		m_previousFocus = ::GetFocus();
		if (IsPaletteTarget(m_window, m_previousFocus)) m_previousFocus = m_parent;
	}
	m_inputMode = true;
	m_suppressInputChange = true;
	m_inputPrompt.assign(prompt);
	m_inputPlaceholder.assign(placeholder);
	m_items.clear();
	m_lastSelectableIndex = -1;
	m_lastNotifiedSelectionId.clear();
	m_selectionNotificationsEnabled = false;
	if (m_prompt != nullptr) {
		::SetWindowTextW(m_prompt, m_inputPrompt.empty() ? L">" : m_inputPrompt.c_str());
	}
	if (m_input != nullptr) {
		::SetWindowTextW(m_input, std::wstring(value).c_str());
		::SendMessageW(m_input, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
	}
	m_suppressInputChange = false;
	if (m_prompt != nullptr) ::ShowWindow(m_prompt, SW_SHOW);
	if (m_close != nullptr) ::ShowWindow(m_close, SW_SHOW);
	if (m_list != nullptr) ::ShowWindow(m_list, SW_HIDE);
	if (m_empty != nullptr) ::ShowWindow(m_empty, SW_HIDE);
	RefreshStrings();
	Layout();
	::SetWindowPos(m_window, HWND_TOP, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
		| SWP_NOREDRAW | SWP_NOCOPYBITS);
	::ShowWindow(m_window, SW_SHOWNOACTIVATE);
	::SetFocus(m_input);
	PaintOverlayNow(m_window);
	return true;
}

void CCommandPaletteOverlay::Hide() noexcept
{
	if (m_window == nullptr || !::IsWindow(m_window)) return;
	RECT previousBounds{};
	const bool repaintParent = IsVisible()
		&& ::GetWindowRect(m_window, &previousBounds) != FALSE;
	if (repaintParent) {
		(void)::MapWindowPoints(nullptr, m_parent,
			reinterpret_cast<POINT*>(&previousBounds), 2);
	}
	::ShowWindow(m_window, SW_HIDE);
	if (repaintParent) PaintParentRegionNow(m_parent, previousBounds);
	RestoreFocus();
}

bool CCommandPaletteOverlay::IsVisible() const noexcept
{
	return m_window != nullptr && ::IsWindowVisible(m_window) != FALSE;
}

bool CCommandPaletteOverlay::PreTranslateMessage(MSG& message) noexcept
{
	if (!IsVisible()) return false;
	if (!IsPaletteTarget(m_window, message.hwnd)) {
		if (IsQuickInputDismissMouseMessage(message.message)) Cancel();
		return false;
	}
	if (message.message == WM_MOUSEWHEEL && !m_inputMode && m_list != nullptr) {
		// Quick Input deliberately keeps keyboard focus in the query EDIT. Windows
		// therefore delivers wheel input to the EDIT even while the pointer is over
		// the results. Route that input to the scrolling surface and consume the
		// original message so one wheel notch is applied exactly once.
		(void)::SendMessageW(m_list, message.message, message.wParam, message.lParam);
		return true;
	}

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

	// VS Code caps Quick Input at 62% of the host width / 600 CSS px and gives
	// its list 40% of the host height as a maximum. The list itself remains
	// content-sized, which is what keeps a two-item theme picker compact.  The
	// pure helper keeps the same arithmetic available to narrow/DPI tests.
	EnsureRowPixelOffsets();
	const int contentHeight = m_items.empty()
		? Scale(kQuickPickDetailRowHeightDip)
		: (m_rowPixelOffsets.empty() ? 0 : m_rowPixelOffsets.back());
	const auto geometry = ComputeQuickInputLayout(parentWidth, parentHeight,
		static_cast<int>(::GetDpiForWindow(m_window)), contentHeight, m_inputMode);
	const int x = geometry.x;
	const int y = geometry.y;
	const int width = geometry.width;
	const int height = geometry.height;
	RECT previousBounds{};
	const bool repaintParent = IsVisible()
		&& ::GetWindowRect(m_window, &previousBounds) != FALSE;
	if (repaintParent) {
		(void)::MapWindowPoints(nullptr, m_parent,
			reinterpret_cast<POINT*>(&previousBounds), 2);
	}
	::SetWindowPos(m_window, HWND_TOP, x, y, width, height,
		SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
	Layout(width, height);
	if (repaintParent) {
		PaintParentRegionNow(m_parent, previousBounds);
		PaintOverlayNow(m_window);
	}
}

void CCommandPaletteOverlay::RefreshStrings() noexcept
{
	if (m_window == nullptr || !::IsWindow(m_window)) return;
	QuickInputStrings strings {
		LocalizedString(STR_WORKBENCH_COMMAND_PALETTE_SEARCH_PLACEHOLDER, L"Type to search commands"),
		LocalizedString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS, L"No matching commands"),
	};
	if (m_stringsCallback) {
		try {
			const auto localized = m_stringsCallback();
			if (!localized.placeholder.empty()) strings.placeholder = localized.placeholder;
			if (!localized.noResults.empty()) strings.noResults = localized.noResults;
		}
		catch (...) {
			// Keep the command-palette defaults when a caller cannot resolve text.
		}
	}
	if (m_empty != nullptr) {
		::SetWindowTextW(m_empty, strings.noResults.c_str());
	}
	if (m_input != nullptr) {
		const std::wstring_view placeholder = !m_inputPlaceholder.empty()
			? std::wstring_view(m_inputPlaceholder) : std::wstring_view(strings.placeholder);
		::SendMessageW(m_input, EM_SETCUEBANNER, FALSE,
			reinterpret_cast<LPARAM>(placeholder.data()));
	}
	if (m_inputMode && m_prompt != nullptr) {
		::SetWindowTextW(m_prompt, m_inputPrompt.empty() ? L">" : m_inputPrompt.c_str());
	}
	::InvalidateRect(m_window, nullptr, FALSE);
}

void CCommandPaletteOverlay::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	RebuildBrushes();
	if (m_window != nullptr) {
		QueueNoEraseInvalidate(m_window);
		QueueNoEraseInvalidate(m_prompt);
		QueueNoEraseInvalidate(m_input);
		QueueNoEraseInvalidate(m_list);
		QueueNoEraseInvalidate(m_close);
		QueueNoEraseInvalidate(m_empty);
		UpdateOverlayScrollbar();
		if (IsVisible()) PaintOverlayNow(m_window);
	}
}

void CCommandPaletteOverlay::SetStringsCallback(StringsCallback callback)
{
	m_stringsCallback = std::move(callback);
	RefreshStrings();
}

void CCommandPaletteOverlay::SetSearchCallback(SearchCallback callback)
{
	m_searchCallback = std::move(callback);
}

void CCommandPaletteOverlay::SetSelectionCallback(SelectionCallback callback)
{
	m_selectionCallback = std::move(callback);
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

LRESULT CALLBACK CCommandPaletteOverlay::InputSubclassProc(
	HWND window,
	UINT message,
	WPARAM wParam,
	LPARAM lParam,
	UINT_PTR id,
	DWORD_PTR data) noexcept
{
	auto* const self = reinterpret_cast<CCommandPaletteOverlay*>(data);
	if (message == WM_NCDESTROY) {
		(void)::RemoveWindowSubclass(window, &CCommandPaletteOverlay::InputSubclassProc, id);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	// The clamp has to run after USER has already moved the caret.
	// PreTranslateMessage sees a key before the EDIT does, and a caret placed with
	// the mouse produces no EN_CHANGE at all, so neither can enforce the marker.
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	bool caretMayHaveMoved = false;
	switch (message) {
	case WM_KEYDOWN:
	case WM_CHAR:
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
	case WM_LBUTTONDBLCLK:
	case WM_SETFOCUS:
		caretMayHaveMoved = true;
		break;
	case WM_MOUSEMOVE:
		// Only a drag selection can move the caret; plain hovering must not pay
		// for two cross-control messages on every mouse move.
		caretMayHaveMoved = (wParam & MK_LBUTTON) != 0;
		break;
	default:
		break;
	}
	if (caretMayHaveMoved && self != nullptr) {
		self->PinCommandPaletteCaret();
	}
	return result;
}

LRESULT CALLBACK CCommandPaletteOverlay::ListSubclassProc(
	HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id,
	DWORD_PTR data) noexcept
{
	auto* const self = reinterpret_cast<CCommandPaletteOverlay*>(data);
	if (message == WM_NCDESTROY) {
		(void)::RemoveWindowSubclass(window, &CCommandPaletteOverlay::ListSubclassProc, id);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if (self != nullptr && self->m_list == window && message == WM_MOUSEWHEEL) {
		// Hiding the native bar also makes LISTBOX drop wheel messages. Keep the
		// list as the scroll authority, but advance its top index explicitly.
		self->ScrollListByWheel(wParam);
		return 0;
	}
	if (self != nullptr && self->m_list == window && message == WM_ERASEBKGND
		&& self->m_panelBrush != nullptr) {
		RECT client{};
		::GetClientRect(window, &client);
		::FillRect(reinterpret_cast<HDC>(wParam), &client, self->m_panelBrush);
		return 1;
	}
	if (self != nullptr && self->m_list == window && message == WM_PAINT
		&& self->m_panelBrush != nullptr) {
		// The parent deliberately uses no-erase invalidation during geometry and
		// theme commits.  Paint the list's complete backing surface before the
		// owner-drawn rows so an uncovered tail can never reveal the terminal or
		// another sibling behind the palette.
		RECT client{};
		::GetClientRect(window, &client);
		const HDC dc = ::GetDCEx(window, nullptr, DCX_INTERSECTUPDATE | DCX_CACHE);
		if (dc != nullptr) {
			::FillRect(dc, &client, self->m_panelBrush);
			::ReleaseDC(window, dc);
		}
	}
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if (self != nullptr && self->m_list == window && message == WM_LBUTTONUP
		&& !self->m_inputMode && self->IsVisible()) {
		// VS Code Quick Picks accept a selectable item on the first mouse click.
		// The native LISTBOX has already committed its selection above, so use the
		// same terminal path as Enter without requiring a second click.
		const LRESULT hit = ::SendMessageW(window, LB_ITEMFROMPOINT, 0, lParam);
		const int index = static_cast<int>(LOWORD(hit));
		const bool outside = HIWORD(hit) != 0;
		if (!outside && index >= 0 && static_cast<std::size_t>(index) < self->m_items.size()
			&& self->m_items[static_cast<std::size_t>(index)].enabled
			&& !self->m_items[static_cast<std::size_t>(index)].separator) {
			(void)::SendMessageW(window, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
			self->EnsureSelectableSelection();
			self->NotifySelectionChanged();
			self->Accept();
		}
	}
	if (self != nullptr && self->m_list == window
		&& (message == WM_VSCROLL || message == WM_KEYDOWN || message == WM_SIZE)) {
		if (message == WM_VSCROLL) {
			// A native LISTBOX scrolls by moving existing pixels and invalidating only
			// the newly exposed strip. Owner-drawn variable-height rows cannot rely on
			// that optimization after their top index changes, so repaint the bounded
			// visible viewport synchronously and leave no stale update region behind.
			(void)::RedrawWindow(window, nullptr, nullptr,
				RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
		}
		self->UpdateOverlayScrollbar();
	}
	return result;
}

LRESULT CCommandPaletteOverlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept
{
	switch (message) {
	case WM_CREATE: {
		const HINSTANCE instance = ::GetModuleHandleW(nullptr);
		m_prompt = ::CreateWindowExW(
			0, L"STATIC", L">",
			WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
			0, 0, 0, 0, m_window, nullptr, instance, nullptr);
		m_input = ::CreateWindowExW(
			0, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputControl)), instance, nullptr);
		m_list = ::CreateWindowExW(
			0, L"LISTBOX", L"",
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWVARIABLE
				| LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
			0, 0, 0, 0, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListControl)), instance, nullptr);
		const auto emptyText = LocalizedString(STR_WORKBENCH_COMMAND_PALETTE_NO_RESULTS, L"No matching commands");
		m_empty = ::CreateWindowExW(
			0, L"STATIC", emptyText.c_str(),
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
		(void)::SetWindowSubclass(m_list, &CCommandPaletteOverlay::ListSubclassProc,
			static_cast<UINT_PTR>(kListControl), reinterpret_cast<DWORD_PTR>(this));
		(void)::SetWindowSubclass(m_input, &CCommandPaletteOverlay::InputSubclassProc,
			static_cast<UINT_PTR>(kInputControl), reinterpret_cast<DWORD_PTR>(this));
		(void)m_overlayScrollbar.Create(m_window, m_list, [this](int pixelOffset) {
			ScrollListToPixelOffset(pixelOffset);
		}, controls::OverlayScrollbarSource::ExplicitModel);
		m_overlayScrollbar.SetHideNativeBar(true);
		::SendMessageW(m_input, EM_SETLIMITTEXT, 4096, 0);
		const int inputPadding = Scale(kListEntryPaddingDip);
		::SendMessageW(m_input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
			MAKELONG(inputPadding, inputPadding));
		const HFONT font = ControlFont(m_font.Get());
		SetControlFont(m_prompt, font);
		SetControlFont(m_input, font);
		SetControlFont(m_list, font);
		SetControlFont(m_empty, font);
		SetControlFont(m_close, font);
		RefreshStrings();
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
			const auto index = static_cast<std::size_t>(measure->itemID);
			int height = kQuickPickCompactRowHeightDip;
			if (index < m_items.size()) {
				if (m_items[index].separator) {
					height = kSeparatorRowHeightDip;
				} else if (!m_items[index].detail.empty()) {
					height = kQuickPickDetailRowHeightDip;
				}
			}
			measure->itemHeight = static_cast<UINT>(Scale(height));
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
		::SetBkColor(dc, m_palette.inputBackground.ToColorRef());
		::SetTextColor(dc, m_palette.primaryText.ToColorRef());
		return reinterpret_cast<LRESULT>(m_inputBrush != nullptr
			? m_inputBrush : ::GetSysColorBrush(COLOR_WINDOW));
	}
	case WM_CTLCOLORLISTBOX: {
		const HDC dc = reinterpret_cast<HDC>(wParam);
		::SetBkMode(dc, OPAQUE);
		::SetBkColor(dc, m_palette.quickInputBackground.ToColorRef());
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
		if (control == kListControl && notification == LBN_SELCHANGE) {
			EnsureSelectableSelection();
			NotifySelectionChanged();
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
		m_overlayScrollbar.Destroy();
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
	// VS Code's Quick Input header is exactly one input line: six DIP above a
	// 26-DIP input and four DIP below.  The list then owns its independent
	// seven-DIP bottom padding. Keep horizontal spacing independent so a
	// vertical tweak cannot silently restore the old dialog-like header.
	const int horizontalMargin = Scale(kHeaderHorizontalPaddingDip);
	const int topInset = Scale(kHeaderTopPaddingDip);
	const int bottomInset = Scale(kHeaderBottomPaddingDip);
	const int listScrollablePadding = Scale(kListScrollablePaddingDip);
	const int rowHeight = Scale(kInputRowHeightDip);
	int promptWidth = Scale(26);
	const int closeWidth = m_inputMode ? Scale(30) : 0;
	const int gap = m_inputMode ? Scale(4) : 0;
	const int listTop = topInset + rowHeight + bottomInset;
	const int listBottom = (std::max)(listTop, height);
	const int listWidth = (std::max)(0, width - listScrollablePadding * 2);
	if (!m_inputMode) {
		promptWidth = 0;
	}
	if (m_inputMode && m_prompt != nullptr) {
		const std::wstring prompt = ReadWindowText(m_prompt);
		if (!prompt.empty()) {
			HDC dc = ::GetDC(m_window);
			if (dc != nullptr) {
				const HFONT font = ControlFont(m_font.Get());
				const HGDIOBJ previous = font != nullptr ? ::SelectObject(dc, font) : nullptr;
				SIZE extent{};
				if (::GetTextExtentPoint32W(dc, prompt.c_str(), static_cast<int>(prompt.size()), &extent)) {
					const int topWidth = (std::max)(0,
						width - horizontalMargin * 2 - closeWidth - gap * 2);
					const int minimumInputWidth = Scale(120);
					const int maximumPromptWidth = (std::max)(promptWidth, topWidth - minimumInputWidth);
					promptWidth = (std::min)(maximumPromptWidth,
						(std::max)(promptWidth, static_cast<int>(extent.cx) + gap));
				}
				if (previous != nullptr && previous != HGDI_ERROR) ::SelectObject(dc, previous);
				::ReleaseDC(m_window, dc);
			}
		}
	}
	const int inputWidth = m_inputMode
		? (std::max)(0,
			width - horizontalMargin * 2 - promptWidth - closeWidth - gap * 2)
		: listWidth;
	if (m_input != nullptr) {
		const int inputPadding = Scale(kListEntryPaddingDip);
		::SendMessageW(m_input, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
			MAKELONG(inputPadding, inputPadding));
	}
	const int inputX = m_inputMode
		? horizontalMargin + promptWidth + gap : horizontalMargin;
	const int effectiveDpi = static_cast<int>(::GetDpiForWindow(m_window));
	MeasureInputLineHeight();
	const auto inputGeometry = ComputeQuickInputRowGeometry(
		inputX, topInset, inputWidth,
		effectiveDpi > 0 ? effectiveDpi : USER_DEFAULT_SCREEN_DPI,
		m_inputLineHeight);
	m_inputFrame = inputGeometry.frame;
	const int editorWidth = inputGeometry.editor.right - inputGeometry.editor.left;
	const int editorHeight = inputGeometry.editor.bottom - inputGeometry.editor.top;

	const UINT commonFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
	struct Placement {
		HWND window;
		int x;
		int y;
		int width;
		int height;
	};
	std::array<Placement, 5> placements{};
	if (m_inputMode) {
		placements = {{
			{ m_prompt, horizontalMargin, topInset, promptWidth, rowHeight },
			{ m_input, inputGeometry.editor.left, inputGeometry.editor.top,
				editorWidth, editorHeight },
			{ m_close, (std::max)(horizontalMargin,
				width - horizontalMargin - closeWidth), topInset, closeWidth, rowHeight },
			{ m_list, horizontalMargin, listTop, listWidth, 0 },
			{ m_empty, horizontalMargin, listTop, listWidth, 0 },
		}};
	} else {
		placements = {{
			{ m_prompt, 0, 0, 0, 0 },
			{ m_input, inputGeometry.editor.left, inputGeometry.editor.top,
				editorWidth, editorHeight },
			{ m_close, 0, 0, 0, 0 },
			{ m_list, listScrollablePadding, listTop, listWidth,
				(std::max)(0, listBottom - listTop) },
			{ m_empty, listScrollablePadding, listTop, listWidth,
				(std::max)(0, listBottom - listTop) },
		}};
	}
	if (width > 0 && height > 0) {
		const int radius = (std::min)(Scale(kWidgetCornerRadiusDip),
			(std::min)(width / 2, height / 2));
		HRGN region = ::CreateRoundRectRgn(0, 0, width + 1, height + 1,
			radius * 2, radius * 2);
		if (region != nullptr) {
			if (::SetWindowRgn(m_window, region, TRUE) == 0) {
				::DeleteObject(region);
			}
		}
	}

	HDWP transaction = ::BeginDeferWindowPos(static_cast<int>(std::size(placements)));
	bool positioned = transaction != nullptr;
	if (positioned) {
		for (const auto& placement : placements) {
			if (placement.window == nullptr) continue;
			transaction = ::DeferWindowPos(transaction, placement.window, nullptr,
				placement.x, placement.y, placement.width, placement.height, commonFlags);
			if (transaction == nullptr) {
				positioned = false;
				break;
			}
		}
		if (positioned) positioned = ::EndDeferWindowPos(transaction) != FALSE;
	}
	if (!positioned) {
		// A failed HDWP must not fall back to one-child-at-a-time painting.
		for (const auto& placement : placements) {
			if (placement.window != nullptr) {
				::SetWindowPos(placement.window, nullptr, placement.x, placement.y,
					placement.width, placement.height, commonFlags);
			}
		}
	}
	UpdateOverlayScrollbar();
	QueueNoEraseInvalidate(m_window);
	for (const auto& placement : placements) QueueNoEraseInvalidate(placement.window);
}

void CCommandPaletteOverlay::PopulateList(std::wstring_view preferredSelectionId) noexcept
{
	if (m_list == nullptr) return;
	// The owner-drawn LISTBOX must never observe a partially rebuilt native item
	// array paired with the already-replaced m_items model.  Search uses the same
	// redraw transaction for its result list.
	::SendMessageW(m_list, WM_SETREDRAW, FALSE, 0);
	::SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
	for (const auto& item : m_items) {
		::SendMessageW(m_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
	}
	RebuildRowPixelOffsets();
	m_wheelDeltaRemainder = 0;
	m_lastSelectableIndex = -1;
	int selected = -1;
	if (!preferredSelectionId.empty()) {
		for (std::size_t index = 0; index < m_items.size(); ++index) {
			if (m_items[index].enabled && !m_items[index].separator
				&& m_items[index].id == preferredSelectionId) {
				selected = static_cast<int>(index);
				break;
			}
		}
	}
	for (std::size_t index = 0; index < m_items.size(); ++index) {
		if (selected < 0 && m_items[index].enabled && !m_items[index].separator) {
			selected = static_cast<int>(index);
			break;
		}
	}
	m_lastSelectableIndex = selected;
	if (selected >= 0) ::SendMessageW(m_list, LB_SETCURSEL, selected, 0);
	const bool hasSelectable = selected >= 0;
	if (m_empty != nullptr) ::ShowWindow(m_empty, hasSelectable ? SW_HIDE : SW_SHOW);
	UpdateOverlayScrollbar();
	::SendMessageW(m_list, WM_SETREDRAW, TRUE, 0);
	QueueNoEraseInvalidate(m_list);
}

void CCommandPaletteOverlay::UpdateSearch() noexcept
{
	if (m_input == nullptr || m_inputMode || m_suppressInputChange) return;
	const auto previousSelectionId = SelectedItemId();
	NormalizeCommandPaletteInput();
	const std::wstring value = ReadWindowText(m_input);
	const std::wstring query(StripCommandPaletteProviderPrefix(value));
	if (!m_searchCallback) {
		PopulateList(previousSelectionId);
		Layout();
		if (IsVisible()) PaintOverlayNow(m_window);
		NotifySelectionChanged();
		return;
	}
	try {
		m_items = m_searchCallback(query);
	}
	catch (...) {
		m_items.clear();
	}
	PopulateList(previousSelectionId);
	Layout();
	if (IsVisible()) PaintOverlayNow(m_window);
	NotifySelectionChanged();
}

void CCommandPaletteOverlay::NormalizeCommandPaletteInput() noexcept
{
	if (m_input == nullptr || m_inputMode || m_suppressInputChange) return;
	const std::wstring value = ReadWindowText(m_input);
	if (!value.empty() && value.front() == L'>') return;

	DWORD selectionStart = 0;
	DWORD selectionEnd = 0;
	(void)::SendMessageW(m_input, EM_GETSEL,
		reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
	std::wstring normalized;
	normalized.reserve(value.size() + 1);
	normalized.push_back(L'>');
	normalized.append(value);
	m_suppressInputChange = true;
	::SetWindowTextW(m_input, normalized.c_str());
	m_suppressInputChange = false;
	const auto clampSelection = [size = normalized.size()](DWORD position) {
		return static_cast<WPARAM>((std::min)(static_cast<std::size_t>(position + 1), size));
	};
	(void)::SendMessageW(m_input, EM_SETSEL,
		clampSelection(selectionStart), clampSelection(selectionEnd));
}

void CCommandPaletteOverlay::PinCommandPaletteCaret() noexcept
{
	// The Command Palette is locked to the `>` provider, so the marker is chrome
	// rather than editable text. A caret placed in front of it lets the next
	// keystroke insert ahead of the marker; NormalizeCommandPaletteInput then sees
	// text that does not start with `>`, prepends a second one, and the stray
	// marker stays in the query forever. ShowInput's generic prompt contract has
	// no marker, so the clamp applies to Command Palette mode only.
	if (m_input == nullptr || m_inputMode) return;
	constexpr DWORD kMarkerLength = 1;
	DWORD selectionStart = 0;
	DWORD selectionEnd = 0;
	(void)::SendMessageW(m_input, EM_GETSEL,
		reinterpret_cast<WPARAM>(&selectionStart), reinterpret_cast<LPARAM>(&selectionEnd));
	if (selectionStart >= kMarkerLength && selectionEnd >= kMarkerLength) return;
	(void)::SendMessageW(m_input, EM_SETSEL,
		static_cast<WPARAM>((std::max)(selectionStart, kMarkerLength)),
		static_cast<LPARAM>((std::max)(selectionEnd, kMarkerLength)));
}

void CCommandPaletteOverlay::MeasureInputLineHeight() noexcept
{
	if (m_window == nullptr) return;
	const UINT dpi = ::GetDpiForWindow(m_window);
	const UINT effectiveDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
	if (m_inputLineHeight > 0 && m_inputLineHeightDpi == effectiveDpi) return;
	const int height = controls::MeasureTextLineHeight(m_window, ControlFont(m_font.Get()));
	if (height <= 0) return;
	m_inputLineHeight = height;
	m_inputLineHeightDpi = effectiveDpi;
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
		if (m_items[static_cast<std::size_t>(current)].enabled
			&& !m_items[static_cast<std::size_t>(current)].separator) {
			::SendMessageW(m_list, LB_SETCURSEL, current, 0);
			m_lastSelectableIndex = current;
			::InvalidateRect(m_list, nullptr, FALSE);
			UpdateOverlayScrollbar();
			NotifySelectionChanged();
			return;
		}
	}
}

void CCommandPaletteOverlay::NotifySelectionChanged() noexcept
{
	if (!m_selectionNotificationsEnabled || !m_selectionCallback) return;
	auto selected = SelectedItemId();
	if (selected.empty() || selected == m_lastNotifiedSelectionId) return;
	m_lastNotifiedSelectionId = selected;
	try {
		m_selectionCallback(std::move(selected));
	}
	catch (...) {
		// Selection previews are advisory. Keep the Quick Pick usable if a
		// presentation owner cannot render one item.
	}
}

std::wstring CCommandPaletteOverlay::SelectedItemId() const
{
	if (m_list == nullptr) return {};
	const int selected = static_cast<int>(::SendMessageW(m_list, LB_GETCURSEL, 0, 0));
	if (selected < 0 || static_cast<std::size_t>(selected) >= m_items.size()
		|| !m_items[static_cast<std::size_t>(selected)].enabled
		|| m_items[static_cast<std::size_t>(selected)].separator) {
		return {};
	}
	return m_items[static_cast<std::size_t>(selected)].id;
}

void CCommandPaletteOverlay::EnsureSelectableSelection() noexcept
{
	if (m_list == nullptr || m_repairingSelection || m_items.empty()) return;
	const int selected = static_cast<int>(::SendMessageW(m_list, LB_GETCURSEL, 0, 0));
	if (selected >= 0 && static_cast<std::size_t>(selected) < m_items.size()
		&& m_items[static_cast<std::size_t>(selected)].enabled
		&& !m_items[static_cast<std::size_t>(selected)].separator) {
		m_lastSelectableIndex = selected;
		return;
	}

	int replacement = m_lastSelectableIndex;
	if (replacement < 0 || static_cast<std::size_t>(replacement) >= m_items.size()
		|| !m_items[static_cast<std::size_t>(replacement)].enabled
		|| m_items[static_cast<std::size_t>(replacement)].separator) {
		replacement = -1;
		for (std::size_t index = 0; index < m_items.size(); ++index) {
			if (m_items[index].enabled && !m_items[index].separator) {
				replacement = static_cast<int>(index);
				break;
			}
		}
	}
	if (replacement < 0) return;
	m_repairingSelection = true;
	(void)::SendMessageW(m_list, LB_SETCURSEL, replacement, 0);
	m_repairingSelection = false;
	m_lastSelectableIndex = replacement;
}

void CCommandPaletteOverlay::Accept() noexcept
{
	if (!IsVisible() || m_terminalCallbackInProgress) return;
	if (m_inputMode) {
		const std::wstring value = ReadWindowText(m_input);
		const auto callback = m_acceptCallback;
		m_terminalCallbackInProgress = true;
		Hide();
		try {
			if (callback) callback(value);
		}
		catch (...) {
			// A terminal owner callback must never terminate the editor's UI loop.
		}
		m_terminalCallbackInProgress = false;
		return;
	}
	if (m_list == nullptr) return;
	const int selected = static_cast<int>(::SendMessageW(m_list, LB_GETCURSEL, 0, 0));
	if (selected < 0 || static_cast<std::size_t>(selected) >= m_items.size()
		|| !m_items[static_cast<std::size_t>(selected)].enabled
		|| m_items[static_cast<std::size_t>(selected)].separator) {
		return;
	}
	std::wstring commandId = m_items[static_cast<std::size_t>(selected)].id;
	const auto callback = m_acceptCallback;
	m_terminalCallbackInProgress = true;
	Hide();
	try {
		if (callback) callback(std::move(commandId));
	}
	catch (...) {
		// A terminal owner callback must never terminate the editor's UI loop.
	}
	m_terminalCallbackInProgress = false;
}

void CCommandPaletteOverlay::Cancel() noexcept
{
	if (!IsVisible() || m_terminalCallbackInProgress) return;
	const auto callback = m_cancelCallback;
	m_terminalCallbackInProgress = true;
	Hide();
	try {
		if (callback) callback();
	}
	catch (...) {
		// See Accept: cancellation is a terminal UI event, not an exception path.
	}
	m_terminalCallbackInProgress = false;
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

void CCommandPaletteOverlay::Paint(HDC dc, const RECT&) noexcept
{
	const COLORREF panelColor = m_palette.quickInputBackground.ToColorRef();
	RECT frame{};
	::GetClientRect(m_window, &frame);
	const COLORREF surface = panelColor == 0 ? kFallbackPanel : panelColor;
	const int frameHalfWidth = static_cast<int>((frame.right - frame.left) / 2);
	const int frameHalfHeight = static_cast<int>((frame.bottom - frame.top) / 2);
	const int frameRadius = (std::min)(Scale(kWidgetCornerRadiusDip),
		(std::min)(frameHalfWidth, frameHalfHeight));
	const int shadowWidth = (std::min)(Scale(kWidgetShadowWidthDip),
		(std::min)(frameHalfWidth, frameHalfHeight));
	if (shadowWidth > 0) {
		// `shadow-xl` is represented by a compact inner feather because a child
		// HWND cannot paint outside its parent's client area.  The palette surface
		// and border remain opaque, while the two-pixel edge preserves the elevation
		// cue on both light and dark themes.
		const COLORREF shadow = BlendTowardBlack(surface, 40);
		FillRoundedRect(dc, frame, frameRadius * 2, shadow);
	}
	RECT surfaceFrame = frame;
	::InflateRect(&surfaceFrame, -shadowWidth, -shadowWidth);
	const int surfaceRadius = (std::max)(0, frameRadius - shadowWidth);
	FillRoundedRect(dc, surfaceFrame, surfaceRadius * 2, surface);
	FrameRoundedRect(dc, frame, frameRadius * 2, m_palette.border.ToColorRef());
	if (m_input != nullptr && ::IsWindowVisible(m_input) != FALSE) {
		const RECT inputFrame = m_inputFrame;
		if (inputFrame.right > inputFrame.left && inputFrame.bottom > inputFrame.top) {
			const int inputRadius = Scale(kInputCornerRadiusDip);
			FillRoundedRect(dc, inputFrame, inputRadius * 2,
				m_palette.inputBackground.ToColorRef());
			// Command Palette keeps the provider marker inside the focused input
			// control.  VS Code exposes that focus with the blue focusBorder; the
			// generic ShowInput contract retains its neutral input.border frame.
			const COLORREF inputBorder = m_inputMode
				? m_palette.inputBorder.ToColorRef() : m_palette.accent.ToColorRef();
			FrameRoundedRect(dc, inputFrame, inputRadius * 2,
				inputBorder);
		}
	}
}

void CCommandPaletteOverlay::DrawItem(const DRAWITEMSTRUCT& draw) noexcept
{
	RECT bounds = draw.rcItem;
	const bool valid = draw.itemID < m_items.size();
	const bool separator = valid && m_items[draw.itemID].separator;
	const bool selected = !separator && (draw.itemState & ODS_SELECTED) != 0;
	const bool enabled = valid && m_items[draw.itemID].enabled && !separator;
	// Selection is a quiet list-surface elevation. The accent is reserved for
	// focus/action affordances; using it as a full row fill makes the light
	// Sakura palette look like a banner instead of a Quick Pick selection.
	const COLORREF background = selected
		? m_palette.listActiveSelectionBackground.ToColorRef()
		: m_palette.quickInputBackground.ToColorRef();
	// LISTBOX gives owner-draw rows a square band.  Clear that band first, then
	// paint the actual row with VS Code's 3-DIP radius so the list's six-DIP
	// scrollable side padding remains visible at both corners.
	FillWithColor(draw.hDC, bounds, m_palette.quickInputBackground.ToColorRef());
	if (selected) {
		const int rowHalfWidth = static_cast<int>((bounds.right - bounds.left) / 2);
		const int rowHalfHeight = static_cast<int>((bounds.bottom - bounds.top) / 2);
		const int radius = (std::min)(Scale(kRowCornerRadiusDip),
			(std::min)(rowHalfWidth, rowHalfHeight));
		FillRoundedRect(draw.hDC, bounds, radius * 2, background);
	}
	if (!valid) return;

	const auto& item = m_items[draw.itemID];
	const int padding = Scale(kListEntryPaddingDip);
	const int lineHeight = Scale(17);
	const int iconSide = Scale(16);
	const int lineGap = Scale(8);
	const bool hasDetail = !item.detail.empty();
	if (separator) {
		RECT heading = bounds;
		heading.left += padding;
		heading.right -= padding;
		heading.top += Scale(3);
		heading.bottom = heading.top + lineHeight;
		::SetBkMode(draw.hDC, TRANSPARENT);
		::SetTextColor(draw.hDC, m_palette.secondaryText.ToColorRef());
		const HFONT oldFont = reinterpret_cast<HFONT>(::SelectObject(
			draw.hDC, ControlFont(m_font.Get())));
		::DrawTextW(draw.hDC, item.label.c_str(), -1, &heading,
			DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
		if (oldFont != nullptr) ::SelectObject(draw.hDC, oldFont);
		return;
	}
	RECT content = bounds;
	content.left += padding;
	content.right -= padding;
	const int contentHeight = content.bottom - content.top;
	RECT label = content;
	label.top += hasDetail ? Scale(3) : (std::max)(0, (contentHeight - lineHeight) / 2);
	label.bottom = label.top + lineHeight;
	RECT description = label;
	RECT detail = content;
	detail.top = label.top + lineHeight;
	detail.bottom = detail.top + lineHeight;
	const COLORREF labelColor = selected
		? m_palette.listActiveSelectionForeground.ToColorRef()
		: (enabled ? m_palette.primaryText.ToColorRef() : m_palette.disabledText.ToColorRef());
	const COLORREF detailColor = selected
		? m_palette.listActiveSelectionForeground.ToColorRef()
		: (enabled ? m_palette.descriptionText.ToColorRef() : m_palette.disabledText.ToColorRef());
	::SetBkMode(draw.hDC, TRANSPARENT);
	::SetTextColor(draw.hDC, labelColor);
	const HFONT oldFont = reinterpret_cast<HFONT>(::SelectObject(draw.hDC, ControlFont(m_font.Get())));
	const auto& faceName = workbench::icons::CCodiconFont::Instance().FaceName();
	const auto labelRuns = workbench::icons::ParseLabelWithIcons(item.label, faceName);
	const int availableWidth = (std::max)(0,
		static_cast<int>(content.right - content.left));
	if (!item.description.empty() && availableWidth > 0) {
		SIZE descriptionExtent{};
		(void)::GetTextExtentPoint32W(draw.hDC, item.description.c_str(),
			static_cast<int>(item.description.size()), &descriptionExtent);
		const int descriptionWidth = (std::min)(static_cast<int>(descriptionExtent.cx),
			(std::max)(0, availableWidth / 2));
		if (descriptionWidth > 0 && descriptionWidth + lineGap < availableWidth) {
			label.right = content.right - descriptionWidth - lineGap;
			description.left = label.right + lineGap;
			description.right = content.right;
		}
	}
	const workbench::icons::SLabelRunFontProvider fonts{
		.acquire = [this](std::wstring_view, int height) { return AcquireCodiconFont(height); },
		.release = {},
	};
	workbench::icons::DrawLabelRuns(draw.hDC, labelRuns, label, iconSide, labelColor, fonts);
	::SetTextColor(draw.hDC, detailColor);
	if (!item.description.empty()) {
		::DrawTextW(draw.hDC, item.description.c_str(), -1, &description,
			DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
	}
	if (hasDetail) {
		::DrawTextW(draw.hDC, item.detail.c_str(), -1, &detail,
			DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
	}
	if (oldFont != nullptr) ::SelectObject(draw.hDC, oldFont);
}

void CCommandPaletteOverlay::DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept
{
	const bool selected = (draw.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
	FillWithColor(draw.hDC, draw.rcItem,
		selected ? m_palette.listHoverBackground.ToColorRef()
			: m_palette.quickInputBackground.ToColorRef());
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

void CCommandPaletteOverlay::ScrollListByWheel(WPARAM wParam) noexcept
{
	if (m_list == nullptr) return;
	const int count = static_cast<int>(::SendMessageW(m_list, LB_GETCOUNT, 0, 0));
	if (count <= 0) return;

	m_wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
	const int notches = m_wheelDeltaRemainder / WHEEL_DELTA;
	if (notches == 0) return;
	m_wheelDeltaRemainder -= notches * WHEEL_DELTA;

	UINT linesPerNotch = 3;
	if (::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0) == FALSE) {
		linesPerNotch = 3;
	}
	int rowsPerNotch = static_cast<int>(linesPerNotch);
	if (linesPerNotch == WHEEL_PAGESCROLL) {
		EnsureRowPixelOffsets();
		RECT client{};
		::GetClientRect(m_list, &client);
		const int viewport = (std::max)(1, static_cast<int>(client.bottom - client.top));
		const int top = (std::max)(0,
			static_cast<int>(::SendMessageW(m_list, LB_GETTOPINDEX, 0, 0)));
		rowsPerNotch = 0;
		int pixels = 0;
		for (int index = top; index < count && pixels < viewport; ++index) {
			const auto row = static_cast<std::size_t>(index);
			if (row + 1 >= m_rowPixelOffsets.size()) break;
			pixels += m_rowPixelOffsets[row + 1] - m_rowPixelOffsets[row];
			++rowsPerNotch;
		}
		rowsPerNotch = (std::max)(1, rowsPerNotch);
	}
	if (rowsPerNotch <= 0) return;

	const int top = static_cast<int>(::SendMessageW(m_list, LB_GETTOPINDEX, 0, 0));
	const long long rowDelta = -static_cast<long long>(notches) * rowsPerNotch;
	const int next = static_cast<int>((std::clamp)(
		static_cast<long long>(top) + rowDelta, 0LL, static_cast<long long>(count - 1)));
	if (next == top) return;
	(void)::SendMessageW(m_list, LB_SETTOPINDEX, static_cast<WPARAM>(next), 0);
	(void)::RedrawWindow(m_list, nullptr, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	UpdateOverlayScrollbar();
}

void CCommandPaletteOverlay::ScrollListToPixelOffset(int pixelOffset) noexcept
{
	if (m_list == nullptr || m_items.empty()) return;
	EnsureRowPixelOffsets();
	if (m_rowPixelOffsets.size() < 2) return;
	const int maximum = m_rowPixelOffsets.back();
	const int requested = (std::clamp)(pixelOffset, 0, maximum);
	const auto end = m_rowPixelOffsets.end() - 1;
	auto upper = (std::lower_bound)(m_rowPixelOffsets.begin(), end, requested);
	std::size_t row = 0;
	if (upper == end) {
		row = m_items.size() - 1;
	} else if (upper == m_rowPixelOffsets.begin()) {
		row = 0;
	} else {
		const auto lower = upper - 1;
		const auto nearest = requested - *lower <= *upper - requested ? lower : upper;
		row = static_cast<std::size_t>(nearest - m_rowPixelOffsets.begin());
	}
	(void)::SendMessageW(m_list, LB_SETTOPINDEX, static_cast<WPARAM>(row), 0);
	(void)::RedrawWindow(m_list, nullptr, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	// ScrollToPosition refreshes the overlay after this callback. Publish the
	// actual LISTBOX-clamped top row before that refresh reads the pixel model.
	m_overlayScrollbar.SetScrollModel(ListScrollModel());
}

void CCommandPaletteOverlay::RebuildRowPixelOffsets() noexcept
{
	m_rowPixelOffsets.clear();
	m_rowPixelOffsets.reserve(m_items.size() + 1);
	m_rowPixelOffsets.push_back(0);
	for (const auto& item : m_items) {
		int height = kQuickPickCompactRowHeightDip;
		if (item.separator) height = kSeparatorRowHeightDip;
		else if (!item.detail.empty()) height = kQuickPickDetailRowHeightDip;
		m_rowPixelOffsets.push_back(m_rowPixelOffsets.back() + Scale(height));
	}
	const UINT dpi = m_window != nullptr ? ::GetDpiForWindow(m_window) : USER_DEFAULT_SCREEN_DPI;
	m_rowPixelOffsetsDpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
}

void CCommandPaletteOverlay::EnsureRowPixelOffsets() noexcept
{
	const UINT dpi = m_window != nullptr ? ::GetDpiForWindow(m_window) : USER_DEFAULT_SCREEN_DPI;
	const UINT effectiveDpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
	if (m_rowPixelOffsets.size() != m_items.size() + 1
		|| m_rowPixelOffsetsDpi != effectiveDpi) {
		RebuildRowPixelOffsets();
	}
}

controls::OverlayScrollbarModel CCommandPaletteOverlay::ListScrollModel() noexcept
{
	EnsureRowPixelOffsets();
	RECT client{};
	if (m_list != nullptr) ::GetClientRect(m_list, &client);
	const int viewport = (std::max)(0, static_cast<int>(client.bottom - client.top));
	const int count = static_cast<int>(m_items.size());
	const int top = m_list == nullptr || count <= 0 ? 0 : (std::clamp)(
		static_cast<int>(::SendMessageW(m_list, LB_GETTOPINDEX, 0, 0)), 0, count - 1);
	const int offset = static_cast<std::size_t>(top) < m_rowPixelOffsets.size()
		? m_rowPixelOffsets[static_cast<std::size_t>(top)] : 0;
	return { .contentExtent = m_rowPixelOffsets.empty() ? 0 : m_rowPixelOffsets.back(),
		.viewportExtent = viewport, .offset = offset };
}

void CCommandPaletteOverlay::UpdateOverlayScrollbar() noexcept
{
	if (m_overlayScrollbar.Get() == nullptr || m_list == nullptr) return;
	const UINT dpi = m_window != nullptr ? ::GetDpiForWindow(m_window) : USER_DEFAULT_SCREEN_DPI;
	m_overlayScrollbar.SetDpi(dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi);
	m_overlayScrollbar.SetColors(workbench::controls::ResolveOverlayScrollbarColors(
		m_palette, m_palette.quickInputBackground));
	m_overlayScrollbar.SetScrollModel(ListScrollModel());
	m_overlayScrollbar.Update();
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
	m_panelBrush = ::CreateSolidBrush(m_palette.quickInputBackground.ToColorRef());
	m_inputBrush = ::CreateSolidBrush(m_palette.inputBackground.ToColorRef());
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
