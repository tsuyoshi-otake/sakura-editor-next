/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/CTerminalWnd.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/window/TerminalColorResolver.h"
#include "terminal/window/TerminalInput.h"
#include "terminal/window/TerminalRenderMapping.h"
#include "theme/CThemeService.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <windowsx.h>
#include <imm.h>

namespace terminal {
namespace {

constexpr wchar_t kTerminalWindowClass[] = L"SakuraNativeTerminalWindow";
constexpr unsigned int kDefaultDpi = 96;
constexpr UINT_PTR kInputRetryTimer = 0x5345;
constexpr UINT kInputRetryMilliseconds = 30;
constexpr std::size_t kPendingInteractiveInputLimit = CTerminalSession::kInputLimitBytes;

bool IsPointSelected( TerminalSelectionPoint point, TerminalSelectionPoint anchor, TerminalSelectionPoint active ) noexcept
{
	const auto less = [](const TerminalSelectionPoint& left, const TerminalSelectionPoint& right) {
		return left.row < right.row || (left.row == right.row && left.column < right.column);
	};
	if( less(active, anchor) ) std::swap(anchor, active);
	return !less(point, anchor) && less(point, active);
}

bool EnsureTerminalClass( HINSTANCE instance )
{
	WNDCLASSEXW windowClass{};
	windowClass.cbSize = sizeof(windowClass);
	windowClass.style = CS_DBLCLKS;
	windowClass.lpfnWndProc = CTerminalWnd::WindowProc;
	windowClass.hInstance = instance;
	windowClass.hCursor = ::LoadCursor(nullptr, IDC_IBEAM);
	windowClass.lpszClassName = kTerminalWindowClass;
	if( ::RegisterClassExW(&windowClass) != 0 ) return true;
	return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

TerminalKeyEvent KeyEventFromMessage( const MSG& message ) noexcept
{
	TerminalKeyEvent event;
	event.virtualKey = static_cast<std::uint32_t>(message.wParam);
	event.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	event.control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
	event.scanCode = static_cast<std::uint16_t>((message.lParam >> 16) & 0xff);
	event.repeatCount = static_cast<std::uint16_t>(message.lParam & 0xffff);
	event.keyDown = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
	event.enhanced = (message.lParam & (1LL << 24)) != 0;
	event.capsLock = (::GetKeyState(VK_CAPITAL) & 1) != 0;
	event.numLock = (::GetKeyState(VK_NUMLOCK) & 1) != 0;
	event.rightAlt = (::GetKeyState(VK_RMENU) & 0x8000) != 0;
	event.rightControl = (::GetKeyState(VK_RCONTROL) & 0x8000) != 0;
	return event;
}

std::string EncodeAltPrintable( const MSG& message )
{
	BYTE keyboardState[256]{};
	if( !::GetKeyboardState(keyboardState) ) return {};
	wchar_t text[8]{};
	const auto scanCode = static_cast<UINT>((message.lParam >> 16) & 0xff);
	const auto count = ::ToUnicodeEx(static_cast<UINT>(message.wParam), scanCode, keyboardState, text,
		static_cast<int>(std::size(text)), 0, ::GetKeyboardLayout(0));
	if( count <= 0 ) return {};
	auto result = EncodeTerminalText(std::wstring_view(text, static_cast<std::size_t>(count)));
	result.insert(result.begin(), '\x1b');
	return result;
}

} // namespace

struct CTerminalWnd::Impl {
	HWND window{};
	HINSTANCE instance{};
	TerminalModel* model{};
	SakuraTerminalInputAdapter* inputAdapter{};
	InputSink inputSink;
	ResizeSink resizeSink;
	HFONT font{};
	HFONT boldFont{};
	HDC backBufferDc{};
	HBITMAP backBufferBitmap{};
	HGDIOBJ backBufferOriginalBitmap{};
	SIZE backBufferSize{};
	unsigned int dpi{ kDefaultDpi };
	int cellWidth{ 8 };
	int cellHeight{ 16 };
	std::size_t visibleRows{ 1 };
	std::size_t scrollOffset{};
	TerminalSize terminalSize{ 1, 1 };
	bool selecting{};
	bool selectionMoved{};
	bool caretShown{};
	TerminalSelectionPoint selectionAnchor{};
	TerminalSelectionPoint selectionActive{};
	unsigned int pressedMouseButton{ 3 };
	wchar_t pendingHighSurrogate{};
	std::vector<std::uint8_t> pendingInteractiveInput;
	bool inputBackpressured{};
	bool inputRejected{};
	bool closed{};
	theme::ThemePalette palette = theme::CThemeService::PaletteFor(theme::ThemeMode::Dark);

	bool AppendPendingInteractiveInput( std::span<const std::uint8_t> bytes )
	{
		if( bytes.size() > kPendingInteractiveInputLimit - pendingInteractiveInput.size() ) {
			inputRejected = true;
			inputBackpressured = false;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		pendingInteractiveInput.insert(pendingInteractiveInput.end(), bytes.begin(), bytes.end());
		inputBackpressured = true;
		if( window ) {
			::SetTimer(window, kInputRetryTimer, kInputRetryMilliseconds, nullptr);
			::InvalidateRect(window, nullptr, FALSE);
		}
		return true;
	}

	bool Send( std::string_view bytes )
	{
		if( bytes.empty() ) return true;
		const auto input = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
		if( !inputSink ) {
			inputRejected = true;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		if( !pendingInteractiveInput.empty() ) return AppendPendingInteractiveInput(input);
		switch( inputSink(input) ) {
		case TerminalQueueInputResult::Accepted:
			inputBackpressured = false;
			inputRejected = false;
			return true;
		case TerminalQueueInputResult::QueueFull:
			return AppendPendingInteractiveInput(input);
		case TerminalQueueInputResult::NotRunning:
			inputRejected = true;
			inputBackpressured = false;
			if( window ) ::InvalidateRect(window, nullptr, FALSE);
			return false;
		}
		return false;
	}

	void RetryPendingInteractiveInput()
	{
		if( pendingInteractiveInput.empty() || !inputSink ) {
			if( window ) ::KillTimer(window, kInputRetryTimer);
			return;
		}
		switch( inputSink(pendingInteractiveInput) ) {
		case TerminalQueueInputResult::Accepted:
			pendingInteractiveInput.clear();
			inputBackpressured = false;
			inputRejected = false;
			if( window ) {
				::KillTimer(window, kInputRetryTimer);
				::InvalidateRect(window, nullptr, FALSE);
			}
			break;
		case TerminalQueueInputResult::QueueFull:
			// Keep the bounded buffer intact and retry on the same low-rate timer.
			inputBackpressured = true;
			break;
		case TerminalQueueInputResult::NotRunning:
			// Ownership ends with this viewport once the session has stopped.  The
			// visible warning records the rejected input; retaining it could leak a
			// large paste across a tab restart into a different process.
			pendingInteractiveInput.clear();
			inputBackpressured = false;
			inputRejected = true;
			if( window ) {
				::KillTimer(window, kInputRetryTimer);
				::InvalidateRect(window, nullptr, FALSE);
			}
			break;
		}
	}

	TerminalViewport Viewport() const noexcept
	{
		return model ? CalculateTerminalViewport(*model, visibleRows, scrollOffset) : TerminalViewport{};
	}

	bool EnsureBackBuffer( HDC referenceDc )
	{
		RECT client{};
		if( referenceDc == nullptr || window == nullptr || !::GetClientRect(window, &client) ) return false;
		const LONG width = std::max<LONG>(0, client.right - client.left);
		const LONG height = std::max<LONG>(0, client.bottom - client.top);
		if( width == 0 || height == 0 ) return false;
		if( backBufferDc && backBufferBitmap && backBufferSize.cx == width && backBufferSize.cy == height ) return true;
		if( backBufferDc == nullptr ) {
			backBufferDc = ::CreateCompatibleDC(referenceDc);
			if( backBufferDc == nullptr ) return false;
		}
		const HBITMAP replacement = ::CreateCompatibleBitmap(referenceDc, width, height);
		if( replacement == nullptr ) return false;
		const auto previous = ::SelectObject(backBufferDc, replacement);
		if( previous == nullptr || previous == HGDI_ERROR ) {
			::DeleteObject(replacement);
			return false;
		}
		if( backBufferBitmap ) ::DeleteObject(backBufferBitmap);
		else backBufferOriginalBitmap = previous;
		backBufferBitmap = replacement;
		backBufferSize = { width, height };
		return true;
	}

	void ReleaseBackBuffer() noexcept
	{
		if( backBufferDc && backBufferBitmap ) {
			if( backBufferOriginalBitmap ) ::SelectObject(backBufferDc, backBufferOriginalBitmap);
			::DeleteObject(backBufferBitmap);
		}
		if( backBufferDc ) ::DeleteDC(backBufferDc);
		backBufferDc = nullptr;
		backBufferBitmap = nullptr;
		backBufferOriginalBitmap = nullptr;
		backBufferSize = {};
	}

	void RecreateFont()
	{
		if( font ) {
			::DeleteObject(font);
			font = nullptr;
		}
		if( boldFont ) {
			::DeleteObject(boldFont);
			boldFont = nullptr;
		}
		const HDC dc = ::GetDC(window);
		const auto fontSpec = theme::CThemeService::FontSpec(theme::ThemeFontKind::Terminal);
		const wchar_t* face = theme::CThemeService::ResolveFontFamily(theme::ThemeFontKind::Terminal);
		font = ::CreateFontW(-::MulDiv(fontSpec.pointSize, static_cast<int>(dpi), 72), 0, 0, 0, fontSpec.weight,
			FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			FIXED_PITCH | FF_MODERN, face);
		boldFont = ::CreateFontW(-::MulDiv(fontSpec.pointSize, static_cast<int>(dpi), 72), 0, 0, 0, FW_BOLD,
			FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			FIXED_PITCH | FF_MODERN, face);
		if( dc ) {
			const auto previous = font ? ::SelectObject(dc, font) : nullptr;
			TEXTMETRICW metrics{};
			if( ::GetTextMetricsW(dc, &metrics) ) {
				cellWidth = std::max(1L, metrics.tmAveCharWidth);
				cellHeight = std::max(1L, metrics.tmHeight + metrics.tmExternalLeading);
			}
			if( previous ) ::SelectObject(dc, previous);
			::ReleaseDC(window, dc);
		}
		RecreateCaret();
	}

	void RecreateCaret()
	{
		if( window == nullptr || ::GetFocus() != window ) return;
		caretShown = false;
		::DestroyCaret();
		if( ::CreateCaret(window, nullptr, std::max(1, cellWidth / 8), cellHeight) ) {
			UpdateCaret();
		}
	}

	void UpdateCaret()
	{
		if( window == nullptr || ::GetFocus() != window ) return;
		if( !model || !model->Modes().cursorVisible || scrollOffset != 0 ) {
			if( caretShown ) {
				::HideCaret(window);
				caretShown = false;
			}
			return;
		}
		const auto row = std::min(model->CursorRow(), visibleRows == 0 ? 0 : visibleRows - 1);
		::SetCaretPos(static_cast<int>(model->CursorColumn()) * cellWidth, static_cast<int>(row) * cellHeight);
		if( !caretShown ) {
			::ShowCaret(window);
			caretShown = true;
		}
	}

	void NotifySize()
	{
		if( window == nullptr ) return;
		RECT client{};
		::GetClientRect(window, &client);
		const int clientWidth = static_cast<int>(client.right - client.left);
		const int clientHeight = static_cast<int>(client.bottom - client.top);
		const auto columns = static_cast<std::uint16_t>(std::clamp(clientWidth / std::max(1, cellWidth), 1, 65535));
		const auto rows = static_cast<std::uint16_t>(std::clamp(clientHeight / std::max(1, cellHeight), 1, 65535));
		visibleRows = rows;
		const TerminalSize next{ columns, rows };
		if( next.columns != terminalSize.columns || next.rows != terminalSize.rows ) {
			terminalSize = next;
			if( resizeSink ) resizeSink(next);
		}
		UpdateScrollbar();
		UpdateCaret();
	}

	void UpdateScrollbar()
	{
		if( window == nullptr ) return;
		SCROLLINFO info{};
		info.cbSize = sizeof(info);
		info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
		const auto viewport = Viewport();
		info.nMin = 0;
		info.nMax = viewport.totalRows == 0 ? 0 : static_cast<int>(std::min<std::size_t>(viewport.totalRows - 1, INT_MAX));
		info.nPage = static_cast<UINT>(std::min<std::size_t>(viewport.visibleRows, UINT_MAX));
		info.nPos = static_cast<int>(std::min<std::size_t>(viewport.topRow, INT_MAX));
		::SetScrollInfo(window, SB_VERT, &info, TRUE);
	}

	void SetScrollTop( std::size_t top )
	{
		const auto viewport = Viewport();
		const auto bottomTop = viewport.totalRows - viewport.visibleRows;
		top = std::min(top, bottomTop);
		scrollOffset = bottomTop - top;
		UpdateScrollbar();
		UpdateCaret();
		::InvalidateRect(window, nullptr, FALSE);
	}

	void ScrollLines( int lines )
	{
		const auto viewport = Viewport();
		const auto current = static_cast<long long>(viewport.topRow);
		const auto target = std::clamp<long long>(current + lines, 0, static_cast<long long>(viewport.totalRows - viewport.visibleRows));
		SetScrollTop(static_cast<std::size_t>(target));
	}

	TerminalSelectionPoint PointToCell( int x, int y ) const noexcept
	{
		return TerminalCellFromPoint(Viewport(), x, y, cellWidth, cellHeight, model ? model->Columns() : 0);
	}

	bool HasSelection() const noexcept
	{
		return model != nullptr && selectionAnchor != selectionActive;
	}

	void ClearSelection()
	{
		selectionAnchor = selectionActive;
		::InvalidateRect(window, nullptr, FALSE);
	}

	void SendMouse( TerminalMouseAction action, int x, int y, unsigned int button, WPARAM keys )
	{
		if( !model ) return;
		const auto cell = PointToCell(x, y);
		const auto viewport = Viewport();
		TerminalMouseEvent event;
		event.action = action;
		event.button = button;
		event.column = cell.column;
		event.row = cell.row >= viewport.topRow ? cell.row - viewport.topRow : 0;
		event.shift = (keys & MK_SHIFT) != 0;
		event.control = (keys & MK_CONTROL) != 0;
		event.alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
		if( inputAdapter ) {
			if( const auto encoded = inputAdapter->EncodeMouse(event) ) Send(*encoded);
		} else {
			Send(EncodeTerminalMouse(event, model->Modes()));
		}
	}

	bool MouseReporting() const noexcept
	{
		if( !model ) return false;
		const auto& modes = model->Modes();
		return modes.mouseButtonTracking || modes.mouseDragTracking || modes.mouseAnyEventTracking;
	}

	void Paint()
	{
		PAINTSTRUCT paint{};
		const HDC dc = ::BeginPaint(window, &paint);
		if( dc == nullptr ) return;
		const int width = paint.rcPaint.right - paint.rcPaint.left;
		const int height = paint.rcPaint.bottom - paint.rcPaint.top;
		if( width <= 0 || height <= 0 ) {
			::EndPaint(window, &paint);
			return;
		}
		const bool buffered = EnsureBackBuffer(dc);
		const HDC memory = buffered ? backBufferDc : dc;
		const COLORREF defaultBackground = palette.canvas.ToColorRef();
		const COLORREF defaultForeground = palette.primaryText.ToColorRef();
		const auto dcBrush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
		const auto previousBrush = ::SelectObject(memory, dcBrush);
		::SetDCBrushColor(memory, defaultBackground);
		::FillRect(memory, &paint.rcPaint, dcBrush);
		const auto dcPen = static_cast<HPEN>(::GetStockObject(DC_PEN));
		const auto previousPen = ::SelectObject(memory, dcPen);
		const auto previousFont = font ? ::SelectObject(memory, font) : nullptr;
		HFONT selectedFont = font;
		::SetBkMode(memory, OPAQUE);

		if( model ) {
			const auto viewport = Viewport();
			// Reuse one pair of contiguous scratch buffers for every visible row.
			// TUI repaint cost stays O(visible cells) without two heap allocations
			// per row per frame.
			std::wstring batchText;
			std::vector<int> batchAdvances;
			batchText.reserve(model->Columns());
			batchAdvances.reserve(model->Columns());
			const auto paintTop = std::max<LONG>(0, paint.rcPaint.top);
			const auto paintBottom = std::max<LONG>(0, paint.rcPaint.bottom);
			const auto firstVisible = std::min<std::size_t>(viewport.visibleRows, static_cast<std::size_t>(paintTop / cellHeight));
			const auto lastVisible = std::min<std::size_t>(viewport.visibleRows, static_cast<std::size_t>((paintBottom + cellHeight - 1) / cellHeight));
			for( auto visualRow = firstVisible; visualRow < lastVisible; ++visualRow ) {
				const auto globalRow = viewport.topRow + visualRow;
				const auto* row = GetTerminalRow(*model, globalRow);
				if( !row ) continue;
				batchText.clear();
				batchAdvances.clear();
				std::size_t batchStart{};
				std::size_t batchEnd{};
				TerminalAttributes batchAttributes{};
				bool batchSelected{};
				bool hasBatch{};
				const auto flushBatch = [&] {
					if( !hasBatch || batchText.empty() ) return;
					const HFONT desiredFont = batchAttributes.bold && boldFont ? boldFont : font;
					if( desiredFont && desiredFont != selectedFont ) {
						::SelectObject(memory, desiredFont);
						selectedFont = desiredFont;
					}
					auto background = ResolveTerminalColor(batchAttributes.background, palette, defaultBackground,
						TerminalColorRole::Background);
					auto foreground = batchAttributes.inverse
						? ResolveTerminalColor(batchAttributes.foreground, palette, defaultForeground, TerminalColorRole::Background)
						: ResolveTerminalForeground(batchAttributes.foreground, palette, defaultForeground, background);
					if( batchAttributes.inverse ) std::swap(foreground, background);
					if( batchSelected ) background = palette.accent.ToColorRef();
					RECT runRect{
						static_cast<LONG>(batchStart * cellWidth), static_cast<LONG>(visualRow * cellHeight),
						static_cast<LONG>(batchEnd * cellWidth), static_cast<LONG>((visualRow + 1) * cellHeight),
					};
					::SetTextColor(memory, foreground);
					::SetBkColor(memory, background);
					::ExtTextOutW(memory, runRect.left, runRect.top, ETO_OPAQUE | ETO_CLIPPED, &runRect,
						batchText.data(), static_cast<UINT>(batchText.size()), batchAdvances.data());
					if( batchAttributes.underline ) {
						::SetDCPenColor(memory, foreground);
						::MoveToEx(memory, runRect.left, runRect.bottom - 2, nullptr);
						::LineTo(memory, runRect.right, runRect.bottom - 2);
					}
					batchText.clear();
					batchAdvances.clear();
					hasBatch = false;
				};

				for( std::size_t column = 0; column < row->cells.size(); ) {
					const auto& cell = row->cells[column];
					if( cell.continuation ) { ++column; continue; }
					const auto attributes = row->AttributesAt(column);
					const bool selected = HasSelection()
						&& IsPointSelected({ globalRow, column }, selectionAnchor, selectionActive);
					const auto columnsWide = std::max<std::size_t>(1, cell.width);
					const auto text = cell.Text();
					// Complex graphemes need GDI's own shaping and cannot safely share a
					// per-cell advance array. They remain a rare single-call fallback.
					if( text.size() > 1 ) {
						flushBatch();
						batchStart = column;
						batchEnd = std::min(row->cells.size(), column + columnsWide);
						batchAttributes = attributes;
						batchSelected = selected;
						batchText.assign(text);
						batchAdvances.assign(text.size(), 0);
						batchAdvances.back() = static_cast<int>(columnsWide * cellWidth);
						hasBatch = true;
						flushBatch();
						column += columnsWide;
						continue;
					}
					if( hasBatch && (attributes != batchAttributes || selected != batchSelected) ) flushBatch();
					if( !hasBatch ) {
						batchStart = column;
						batchAttributes = attributes;
						batchSelected = selected;
						hasBatch = true;
					}
					batchEnd = std::min(row->cells.size(), column + columnsWide);
					batchText.push_back(text.empty() ? L' ' : text.front());
					batchAdvances.push_back(static_cast<int>(columnsWide * cellWidth));
					column += columnsWide;
				}
				flushBatch();
			}
		}
		if( previousFont ) ::SelectObject(memory, previousFont);
		if( inputBackpressured || inputRejected ) {
			RECT warning{ 0, std::max<LONG>(0, height - cellHeight), width, height };
			::SetBkColor(memory, inputRejected ? RGB(128, 40, 40) : palette.raised.ToColorRef());
			::SetTextColor(memory, palette.primaryText.ToColorRef());
			const wchar_t* message = inputRejected ? L"Terminal input was rejected; restart the session to continue."
				: L"Terminal input is waiting for the process to catch up.";
			::ExtTextOutW(memory, warning.left + 4, warning.top, ETO_OPAQUE | ETO_CLIPPED, &warning, message,
				static_cast<UINT>(wcslen(message)), nullptr);
		}
		if( previousPen ) ::SelectObject(memory, previousPen);
		if( previousBrush ) ::SelectObject(memory, previousBrush);
		if( buffered ) ::BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, width, height, memory,
			paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
		::EndPaint(window, &paint);
	}

	void HandleChar( wchar_t value, bool alt )
	{
		std::wstring text;
		if( value >= 0xd800 && value <= 0xdbff ) {
			if( pendingHighSurrogate ) text.push_back(pendingHighSurrogate);
			pendingHighSurrogate = value;
			if( !text.empty() ) Send(EncodeTerminalText(text));
			return;
		}
		if( pendingHighSurrogate ) {
			text.push_back(pendingHighSurrogate);
			pendingHighSurrogate = 0;
		}
		text.push_back(value);
		auto bytes = EncodeTerminalText(text);
		if( alt ) bytes.insert(bytes.begin(), '\x1b');
		Send(bytes);
	}

	LRESULT HandleMessage( UINT message, WPARAM wParam, LPARAM lParam )
	{
		switch( message ) {
		case WM_ERASEBKGND:
			return 1;
		case WM_PAINT:
			Paint();
			return 0;
		case WM_SIZE:
			NotifySize();
			// A terminal is first created at 0x0 while its panel is materialized.
			// Resizing it into view must schedule a paint even when it has not
			// received focus yet; otherwise the first prompt remains hidden until
			// a click happens to invalidate the window.
			::InvalidateRect(window, nullptr, FALSE);
			return 0;
		case WM_SHOWWINDOW:
			if( wParam != FALSE ) ::InvalidateRect(window, nullptr, FALSE);
			return ::DefWindowProcW(window, message, wParam, lParam);
		case WM_TIMER:
			if( wParam == kInputRetryTimer ) {
				RetryPendingInteractiveInput();
				return 0;
			}
			return ::DefWindowProcW(window, message, wParam, lParam);
		case WM_SETFOCUS:
			RecreateCaret();
			if( inputAdapter ) {
				if( const auto encoded = inputAdapter->EncodeFocus(true) ) Send(*encoded);
			}
			return 0;
		case WM_KILLFOCUS:
			if( inputAdapter ) {
				if( const auto encoded = inputAdapter->EncodeFocus(false) ) Send(*encoded);
			}
			if( caretShown ) ::HideCaret(window);
			caretShown = false;
			::DestroyCaret();
			return 0;
		case WM_CHAR:
			HandleChar(static_cast<wchar_t>(wParam), false);
			return 0;
		case WM_SYSCHAR:
			HandleChar(static_cast<wchar_t>(wParam), true);
			return 0;
		case WM_IME_STARTCOMPOSITION: {
			const HIMC context = ::ImmGetContext(window);
			if( context ) {
				COMPOSITIONFORM form{};
				form.dwStyle = CFS_POINT;
				form.ptCurrentPos = { static_cast<LONG>(model ? model->CursorColumn() * cellWidth : 0), static_cast<LONG>(model ? (model->CursorRow() + 1) * cellHeight : cellHeight) };
				::ImmSetCompositionWindow(context, &form);
				::ImmReleaseContext(window, context);
			}
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		case WM_LBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_RBUTTONDOWN: {
			::SetFocus(window);
			const auto button = message == WM_LBUTTONDOWN ? 0u : message == WM_MBUTTONDOWN ? 1u : 2u;
			pressedMouseButton = button;
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) {
				SendMouse(TerminalMouseAction::Press, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), button, wParam);
			} else if( message == WM_LBUTTONDOWN ) {
				selecting = true;
				selectionMoved = false;
				selectionAnchor = selectionActive = PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				::SetCapture(window);
				::InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		}
		case WM_MOUSEMOVE:
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) {
				SendMouse(TerminalMouseAction::Move, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), pressedMouseButton, wParam);
			} else if( selecting ) {
				const auto next = PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				selectionMoved = selectionMoved || next != selectionAnchor;
				selectionActive = next;
				if( selectionActive.column < (model ? model->Columns() : 0) ) ++selectionActive.column;
				::InvalidateRect(window, nullptr, FALSE);
			}
			return 0;
		case WM_LBUTTONUP:
		case WM_MBUTTONUP:
		case WM_RBUTTONUP: {
			const auto button = message == WM_LBUTTONUP ? 0u : message == WM_MBUTTONUP ? 1u : 2u;
			if( MouseReporting() && (wParam & MK_SHIFT) == 0 ) SendMouse(TerminalMouseAction::Release, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), button, wParam);
			if( selecting ) {
				const auto next = PointToCell(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
				selectionMoved = selectionMoved || next != selectionAnchor;
				selectionActive = next;
				if( selectionMoved && selectionActive.column < (model ? model->Columns() : 0) ) ++selectionActive.column;
				selecting = false;
				::ReleaseCapture();
				::InvalidateRect(window, nullptr, FALSE);
			}
			pressedMouseButton = 3;
			return 0;
		}
		case WM_MOUSEWHEEL: {
			const auto delta = GET_WHEEL_DELTA_WPARAM(wParam);
			POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			::ScreenToClient(window, &point);
			if( MouseReporting() ) SendMouse(delta > 0 ? TerminalMouseAction::WheelUp : TerminalMouseAction::WheelDown, point.x, point.y, 0, GET_KEYSTATE_WPARAM(wParam));
			else ScrollLines(delta > 0 ? -3 : 3);
			return 0;
		}
		case WM_VSCROLL: {
			SCROLLINFO info{};
			info.cbSize = sizeof(info);
			info.fMask = SIF_ALL;
			::GetScrollInfo(window, SB_VERT, &info);
			int position = info.nPos;
			switch( LOWORD(wParam) ) {
			case SB_LINEUP: --position; break;
			case SB_LINEDOWN: ++position; break;
			case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
			case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
			case SB_THUMBPOSITION:
			case SB_THUMBTRACK: position = info.nTrackPos; break;
			case SB_TOP: position = info.nMin; break;
			case SB_BOTTOM: position = info.nMax; break;
			default: return 0;
			}
			SetScrollTop(static_cast<std::size_t>(std::max(info.nMin, position)));
			return 0;
		}
		case WM_PASTE:
			PasteFromClipboard();
			return 0;
		case WM_DPICHANGED:
			dpi = HIWORD(wParam) == 0 ? kDefaultDpi : HIWORD(wParam);
			RecreateFont();
			NotifySize();
			::InvalidateRect(window, nullptr, FALSE);
			return 0;
		default:
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
	}

	bool CopySelectionToClipboard()
	{
		if( !HasSelection() ) return false;
		const auto text = ExtractTerminalSelection(*model, selectionAnchor, selectionActive);
		if( !::OpenClipboard(window) ) return false;
		bool copied = false;
		if( ::EmptyClipboard() ) {
			const auto bytes = (text.size() + 1) * sizeof(wchar_t);
			const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
			if( memory ) {
				if( void* destination = ::GlobalLock(memory) ) {
					std::memcpy(destination, text.c_str(), bytes);
					::GlobalUnlock(memory);
					if( ::SetClipboardData(CF_UNICODETEXT, memory) ) copied = true;
				}
				if( !copied ) ::GlobalFree(memory);
			}
		}
		::CloseClipboard();
		return copied;
	}

	bool PasteFromClipboard()
	{
		if( !model || !::OpenClipboard(window) ) return false;
		bool pasted = false;
		const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
		if( data ) {
			const auto* text = static_cast<const wchar_t*>(::GlobalLock(data));
			if( text ) {
				const auto capacity = ::GlobalSize(data) / sizeof(wchar_t);
				const auto length = wcsnlen_s(text, capacity);
				pasted = Send(EncodeTerminalPaste(std::wstring_view(text, length), model->Modes().bracketedPaste));
				::GlobalUnlock(data);
			}
		}
		::CloseClipboard();
		return pasted;
	}
};

CTerminalWnd::CTerminalWnd()
	: m_impl(std::make_unique<Impl>())
{
}

CTerminalWnd::~CTerminalWnd()
{
	Close();
}

bool CTerminalWnd::Create( HWND parent, HINSTANCE instance )
{
	if( m_impl->closed || m_impl->window || parent == nullptr || instance == nullptr || !EnsureTerminalClass(instance) ) return false;
	m_impl->instance = instance;
	m_impl->window = ::CreateWindowExW(0, kTerminalWindowClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL,
		0, 0, 0, 0, parent, nullptr, instance, m_impl.get());
	if( !m_impl->window ) return false;
	m_impl->RecreateFont();
	m_impl->NotifySize();
	return true;
}

void CTerminalWnd::Layout( const RECT& bounds, unsigned int dpi )
{
	if( m_impl->closed || !m_impl->window ) return;
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	if( m_impl->dpi != effectiveDpi ) {
		m_impl->dpi = effectiveDpi;
		m_impl->RecreateFont();
	}
	::SetWindowPos(m_impl->window, nullptr, bounds.left, bounds.top, std::max(0L, bounds.right - bounds.left),
		std::max(0L, bounds.bottom - bounds.top), SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
	m_impl->NotifySize();
	::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CTerminalWnd::SetModel( TerminalModel* model )
{
	m_impl->model = model;
	m_impl->scrollOffset = 0;
	m_impl->selectionAnchor = {};
	m_impl->selectionActive = {};
	m_impl->UpdateScrollbar();
	m_impl->UpdateCaret();
	InvalidateAll();
}

void CTerminalWnd::SetInputAdapter( SakuraTerminalInputAdapter* inputAdapter )
{
	m_impl->inputAdapter = inputAdapter;
}

void CTerminalWnd::SetInputSink( InputSink sink )
{
	m_impl->inputSink = std::move(sink);
	if( !m_impl->pendingInteractiveInput.empty() ) m_impl->RetryPendingInteractiveInput();
}

void CTerminalWnd::SetResizeSink( ResizeSink sink )
{
	m_impl->resizeSink = std::move(sink);
	if( m_impl->resizeSink ) m_impl->resizeSink(m_impl->terminalSize);
}

void CTerminalWnd::SetPalette( const theme::ThemePalette& palette )
{
	m_impl->palette = palette;
	if( m_impl->window ) ::InvalidateRect(m_impl->window, nullptr, FALSE);
}

void CTerminalWnd::InvalidateDirtyRows( const std::vector<std::size_t>& dirtyScreenRows )
{
	if( !m_impl->window || !m_impl->model ) return;
	m_impl->UpdateScrollbar();
	const auto viewport = m_impl->Viewport();
	const auto visible = MapDirtyRowsToViewport(*m_impl->model, viewport, dirtyScreenRows);
	RECT client{};
	::GetClientRect(m_impl->window, &client);
	for( std::size_t index = 0; index < visible.size(); ) {
		const auto first = visible[index];
		auto last = first;
		while( ++index < visible.size() && visible[index] == last + 1 ) last = visible[index];
		RECT rectangle{ 0, static_cast<LONG>(first * m_impl->cellHeight), client.right,
			static_cast<LONG>((last + 1) * m_impl->cellHeight) };
		::InvalidateRect(m_impl->window, &rectangle, FALSE);
	}
	m_impl->UpdateCaret();
}

void CTerminalWnd::InvalidateAll()
{
	if( m_impl->window ) {
		m_impl->UpdateScrollbar();
		m_impl->UpdateCaret();
		::InvalidateRect(m_impl->window, nullptr, FALSE);
	}
}

bool CTerminalWnd::PreTranslateMessage( MSG& message )
{
	if( m_impl->closed || !m_impl->window || message.hwnd != m_impl->window ||
		(message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN &&
		 message.message != WM_KEYUP && message.message != WM_SYSKEYUP) ) return false;
	const auto event = KeyEventFromMessage(message);
	if( event.alt && (event.virtualKey == VK_F4 || event.virtualKey == VK_SPACE) ) return false;
	if( event.keyDown ) {
		switch( ResolveTerminalShortcut(event, m_impl->HasSelection()) ) {
		case TerminalShortcutAction::Copy:
			static_cast<void>(CopySelectionToClipboard());
			return true;
		case TerminalShortcutAction::Paste:
			static_cast<void>(PasteFromClipboard());
			return true;
		case TerminalShortcutAction::SendInterrupt:
			m_impl->Send(std::string_view("\x03", 1));
			return true;
		case TerminalShortcutAction::None:
			break;
		}
	}
	if( m_impl->inputAdapter ) {
		// Printable keys are completed by TranslateMessage/WM_CHAR. The VT input
		// adapter can return an engaged but empty value for their WM_KEYDOWN because
		// UnicodeChar is not available yet. Consuming it would suppress WM_CHAR.
		if( const auto encoded = m_impl->inputAdapter->EncodeKey(event); encoded && !encoded->empty() ) {
			m_impl->Send(*encoded);
			return true;
		}
	}
	if( event.keyDown ) {
		const auto bytes = EncodeTerminalKey(event);
		if( !bytes.empty() ) {
			m_impl->Send(bytes);
			return true;
		}
	}
	if( event.keyDown && message.message == WM_SYSKEYDOWN && event.alt ) {
		const auto printable = EncodeAltPrintable(message);
		if( !printable.empty() ) {
			m_impl->Send(printable);
			return true;
		}
	}
	return false;
}

void CTerminalWnd::Focus()
{
	if( m_impl->window ) ::SetFocus(m_impl->window);
}

void CTerminalWnd::Close() noexcept
{
	if( !m_impl || m_impl->closed ) return;
	m_impl->closed = true;
	if( m_impl->window ) ::KillTimer(m_impl->window, kInputRetryTimer);
	if( m_impl->window ) ::DestroyWindow(m_impl->window);
	m_impl->window = nullptr;
	m_impl->ReleaseBackBuffer();
	if( m_impl->font ) ::DeleteObject(m_impl->font);
	m_impl->font = nullptr;
	if( m_impl->boldFont ) ::DeleteObject(m_impl->boldFont);
	m_impl->boldFont = nullptr;
	m_impl->model = nullptr;
	m_impl->inputAdapter = nullptr;
	m_impl->inputSink = {};
	m_impl->resizeSink = {};
}

HWND CTerminalWnd::GetHwnd() const noexcept
{
	return m_impl->window;
}

TerminalSize CTerminalWnd::GetTerminalSize() const noexcept
{
	return m_impl->terminalSize;
}

bool CTerminalWnd::HasSelection() const noexcept
{
	return m_impl->HasSelection();
}

bool CTerminalWnd::CopySelectionToClipboard()
{
	return m_impl->CopySelectionToClipboard();
}

bool CTerminalWnd::PasteFromClipboard()
{
	return m_impl->PasteFromClipboard();
}

LRESULT CALLBACK CTerminalWnd::WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	if( message == WM_NCCREATE ) {
		const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		auto* impl = static_cast<Impl*>(create->lpCreateParams);
		if( impl ) {
			impl->window = window;
			::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
		}
	}
	auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
	if( impl ) {
		if( message == WM_NCDESTROY ) {
			impl->window = nullptr;
			::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
			return ::DefWindowProcW(window, message, wParam, lParam);
		}
		return impl->HandleMessage(message, wParam, lParam);
	}
	return ::DefWindowProcW(window, message, wParam, lParam);
}

} // namespace terminal
