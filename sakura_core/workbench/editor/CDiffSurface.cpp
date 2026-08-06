/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/CDiffSurface.h"
#include "workbench/icons/CCodiconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <utility>

namespace {
constexpr wchar_t kWindowClass[] = L"SakuraWorkbenchDiffSurface";

int ScaleValue(int dip, unsigned int dpi) noexcept
{
	return std::max(0, (dip * static_cast<int>(dpi == 0 ? 96 : dpi) + 48) / 96);
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

void FillSolid(HDC dc, const RECT& bounds, COLORREF color) noexcept
{
	if (const HBRUSH brush = ::CreateSolidBrush(color); brush != nullptr) {
		::FillRect(dc, &bounds, brush);
		::DeleteObject(brush);
	}
}
} // namespace

CDiffSurface::CDiffSurface()
	: CWnd(L"CDiffSurface")
{
}

CDiffSurface::~CDiffSurface()
{
	Destroy();
	ReleaseFont();
}

HWND CDiffSurface::Open(HINSTANCE hInstance, HWND hwndParent)
{
	if (hInstance == nullptr || hwndParent == nullptr || GetHwnd() != nullptr) return nullptr;
	if (RegisterWC(hInstance, nullptr, nullptr, ::LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass) == 0
		&& ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	const HWND window = Create(hwndParent, 0, kWindowClass, L"Diff",
		WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 0, nullptr);
	if (window == nullptr) return nullptr;
	EnsureFont();
	m_hwndClose = ::CreateWindowExW(0, WC_BUTTONW, L"", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
		0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButtonId)), hInstance, nullptr);
	if (m_hwndClose == nullptr) {
		Destroy();
		return nullptr;
	}
	::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
	LayoutChildren();
	return window;
}

void CDiffSurface::Destroy() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) {
		CWnd::DestroyWindow();
	} else {
		_SetHwnd(nullptr);
		m_hwndClose = nullptr;
	}
	ReleaseCodiconFont();
}

void CDiffSurface::Layout(const RECT& bounds, unsigned int dpi)
{
	if (GetHwnd() == nullptr || !::IsWindow(GetHwnd())) return;
	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	(void)dpi; // Child-window DPI is authoritative; WM_DPICHANGED refreshes it.
	::SetWindowPos(GetHwnd(), nullptr, bounds.left, bounds.top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
	LayoutChildren();
	UpdateScrollRange();
	::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CDiffSurface::Show() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_SHOWNA);
}

void CDiffSurface::Hide() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindow(GetHwnd())) ::ShowWindow(GetHwnd(), SW_HIDE);
}

void CDiffSurface::Focus() noexcept
{
	if (GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd())) ::SetFocus(GetHwnd());
}

bool CDiffSurface::IsVisible() const noexcept
{
	return GetHwnd() != nullptr && ::IsWindowVisible(GetHwnd()) != FALSE;
}

void CDiffSurface::SetPalette(const theme::ThemePalette& palette) noexcept
{
	m_palette = palette;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CDiffSurface::ShowDiff(SDiffSurfaceContent content)
{
	m_content = std::move(content);
	m_hasContent = true;
	m_scrollOffset = 0;
	// A selection names rows of the comparison that is on screen. Carrying one
	// across a new comparison would name rows of a file the user is no longer
	// looking at, which is exactly how a wrong region gets staged.
	ClearSelection();
	UpdateScrollRange();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CDiffSurface::ClearDiff()
{
	m_content = {};
	m_hasContent = false;
	m_scrollOffset = 0;
	ClearSelection();
	UpdateScrollRange();
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

bool CDiffSurface::HasSelection() const noexcept
{
	return m_selectionAnchorRow >= 0 && m_selectionFocusRow >= 0;
}

bool CDiffSurface::SelectedRowRange(int& firstRow, int& lastRow) const noexcept
{
	if (!HasSelection()) return false;
	const int rowCount = static_cast<int>(m_content.rows.size());
	if (rowCount <= 0) return false;
	int first = (std::min)(m_selectionAnchorRow, m_selectionFocusRow);
	int last = (std::max)(m_selectionAnchorRow, m_selectionFocusRow);
	first = (std::max)(0, first);
	last = (std::min)(rowCount - 1, last);
	if (first > last) return false;
	firstRow = first;
	lastRow = last;
	return true;
}

void CDiffSurface::ClearSelection() noexcept
{
	if (!HasSelection()) return;
	m_selectionAnchorRow = -1;
	m_selectionFocusRow = -1;
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CDiffSurface::SetOnCloseRequested(CloseRequestedCallback callback)
{
	m_onCloseRequested = std::move(callback);
}

unsigned int CDiffSurface::Dpi() const noexcept
{
	const UINT dpi = GetHwnd() != nullptr ? ::GetDpiForWindow(GetHwnd()) : 96;
	return dpi == 0 ? 96 : dpi;
}

int CDiffSurface::ScaleDip(int dip) const noexcept
{
	return ScaleValue(dip, Dpi());
}

void CDiffSurface::EnsureFont()
{
	if (m_font == nullptr || m_boldFont == nullptr) {
		NONCLIENTMETRICSW metrics{ sizeof(metrics) };
		if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
			if (m_font == nullptr) m_font = ::CreateFontIndirectW(&metrics.lfMessageFont);
			if (m_boldFont == nullptr) {
				LOGFONTW bold = metrics.lfMessageFont;
				bold.lfWeight = FW_SEMIBOLD;
				m_boldFont = ::CreateFontIndirectW(&bold);
			}
		}
	}
	if (m_codeFont.Get() == nullptr || m_codeFont.Dpi() != Dpi()) {
		(void)m_codeFont.Recreate(theme::ThemeFontKind::Editor, Dpi());
	}
	m_rowHeight = ScaleDip(18);
	if (GetHwnd() == nullptr || m_codeFont.Get() == nullptr) return;
	if (const HDC dc = ::GetDC(GetHwnd()); dc != nullptr) {
		const HGDIOBJ old = ::SelectObject(dc, m_codeFont.Get());
		TEXTMETRICW metrics{};
		if (::GetTextMetricsW(dc, &metrics)) {
			m_rowHeight = std::max(ScaleDip(12), static_cast<int>(metrics.tmHeight) + ScaleDip(2));
		}
		if (old != nullptr) ::SelectObject(dc, old);
		::ReleaseDC(GetHwnd(), dc);
	}
}

void CDiffSurface::ReleaseFont() noexcept
{
	if (m_font != nullptr) ::DeleteObject(m_font);
	if (m_boldFont != nullptr) ::DeleteObject(m_boldFont);
	m_font = nullptr;
	m_boldFont = nullptr;
	m_codeFont.Reset();
	m_rowHeight = 0;
}

HFONT CDiffSurface::AcquireCodiconFont(int height) noexcept
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

void CDiffSurface::ReleaseCodiconFont() noexcept
{
	if (m_codiconFont != nullptr) ::DeleteObject(m_codiconFont);
	m_codiconFont = nullptr;
	m_codiconFontHeight = 0;
}

void CDiffSurface::DrawCloseButton(const DRAWITEMSTRUCT& draw) noexcept
{
	const bool selected = (draw.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
	const COLORREF backgroundColor = selected ? m_palette.panel.ToColorRef() : m_palette.raised.ToColorRef();
	FillSolid(draw.hDC, draw.rcItem, backgroundColor);
	if ((draw.itemState & ODS_FOCUS) != 0) {
		if (const HBRUSH border = ::CreateSolidBrush(m_palette.border.ToColorRef()); border != nullptr) {
			::FrameRect(draw.hDC, &draw.rcItem, border);
			::DeleteObject(border);
		}
	}
	const int width = static_cast<int>(draw.rcItem.right - draw.rcItem.left);
	const int height = static_cast<int>(draw.rcItem.bottom - draw.rcItem.top);
	const int side = (std::min)(ScaleDip(18), (std::min)(width, height));
	const workbench::icons::IconRect iconBox{
		draw.rcItem.left + (width - side) / 2,
		draw.rcItem.top + (height - side) / 2,
		draw.rcItem.left + (width - side) / 2 + side,
		draw.rcItem.top + (height - side) / 2 + side };
	const auto glyph = workbench::icons::FindCodiconGlyph(L"close");
	const COLORREF color = m_palette.secondaryText.ToColorRef();
	if (!PaintFontGlyph(draw.hDC, iconBox, AcquireCodiconFont(side), glyph.value_or(L'\0'), color)) {
		workbench::icons::codicons::Draw(draw.hDC, iconBox,
			workbench::icons::codicons::Icon::Close, color);
	}
}

void CDiffSurface::LayoutChildren()
{
	if (GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const int padding = ScaleDip(10);
	const int closeSide = ScaleDip(28);
	if (m_hwndClose != nullptr) {
		::SetWindowPos(m_hwndClose, nullptr,
			std::max(0L, client.right - padding - closeSide), (ScaleDip(44) - closeSide) / 2,
			closeSide, closeSide, SWP_NOZORDER | SWP_NOACTIVATE);
	}
}

void CDiffSurface::PaintText(HDC dc, const wchar_t* text, RECT bounds, COLORREF color, UINT format, bool bold)
{
	::SetTextColor(dc, color);
	::SetBkMode(dc, TRANSPARENT);
	const HGDIOBJ old = ::SelectObject(dc, bold ? m_boldFont : m_font);
	::DrawTextW(dc, text, -1, &bounds, format);
	if (old != nullptr) ::SelectObject(dc, old);
}

int CDiffSurface::RowHeight() const noexcept
{
	return m_rowHeight > 0 ? m_rowHeight : ScaleDip(18);
}

int CDiffSurface::ContentTop() const noexcept
{
	return ScaleDip(44) + ScaleDip(24);
}

int CDiffSurface::GutterWidth(HDC dc) const
{
	const std::size_t lineCount = (std::max)(m_content.originalLines.size(), m_content.modifiedLines.size());
	int digits = 1;
	for (std::size_t remaining = lineCount; remaining >= 10; remaining /= 10) ++digits;
	digits = (std::max)(digits, 2);
	SIZE size{};
	if (!::GetTextExtentPoint32W(dc, L"0", 1, &size)) size.cx = ScaleDip(7);
	return digits * (std::max<int>)(1, static_cast<int>(size.cx)) + ScaleDip(16);
}

void CDiffSurface::UpdateScrollRange()
{
	const int rowHeight = RowHeight();
	m_contentHeight = static_cast<int>(m_content.rows.size()) * rowHeight;
	RECT client{};
	if (GetHwnd() != nullptr) ::GetClientRect(GetHwnd(), &client);
	const int viewportHeight = (std::max)(1, static_cast<int>(client.bottom) - ContentTop());
	m_maxScrollOffset = (std::max)(0, m_contentHeight - viewportHeight);
	if (m_scrollOffset > m_maxScrollOffset) m_scrollOffset = m_maxScrollOffset;
	if (GetHwnd() == nullptr) return;
	SCROLLINFO scrollInfo{ sizeof(scrollInfo), SIF_RANGE | SIF_PAGE | SIF_POS, 0, m_contentHeight,
		static_cast<UINT>(viewportHeight), m_scrollOffset, 0 };
	::SetScrollInfo(GetHwnd(), SB_VERT, &scrollInfo, TRUE);
}

void CDiffSurface::ScrollTo(int offset) noexcept
{
	const int clamped = (std::max)(0, (std::min)(offset, m_maxScrollOffset));
	if (clamped == m_scrollOffset) return;
	m_scrollOffset = clamped;
	if (GetHwnd() != nullptr) {
		SCROLLINFO scrollInfo{ sizeof(scrollInfo), SIF_POS, 0, 0, 0, m_scrollOffset, 0 };
		::SetScrollInfo(GetHwnd(), SB_VERT, &scrollInfo, TRUE);
		::InvalidateRect(GetHwnd(), nullptr, FALSE);
	}
}

int CDiffSurface::RowAtPoint(int y) const noexcept
{
	const int rowHeight = RowHeight();
	if (rowHeight <= 0 || !m_hasContent) return -1;
	const int rowCount = static_cast<int>(m_content.rows.size());
	if (rowCount <= 0) return -1;
	const int contentTop = ContentTop();
	if (y < contentTop) return -1;
	const int row = (y - contentTop + m_scrollOffset) / rowHeight;
	if (row < 0 || row >= rowCount) return -1;
	return row;
}

bool CDiffSurface::IsRowSelected(int row) const noexcept
{
	int first = 0;
	int last = 0;
	if (!SelectedRowRange(first, last)) return false;
	return row >= first && row <= last;
}

void CDiffSurface::MoveSelectionTo(int row, bool extend)
{
	const int rowCount = static_cast<int>(m_content.rows.size());
	if (rowCount <= 0) return;
	const int clamped = (std::max)(0, (std::min)(row, rowCount - 1));
	if (!extend || m_selectionAnchorRow < 0) m_selectionAnchorRow = clamped;
	m_selectionFocusRow = clamped;
	EnsureRowVisible(clamped);
	if (GetHwnd() != nullptr) ::InvalidateRect(GetHwnd(), nullptr, FALSE);
}

void CDiffSurface::EnsureRowVisible(int row)
{
	const int rowHeight = RowHeight();
	if (rowHeight <= 0 || GetHwnd() == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	const int viewportHeight = (std::max)(1, static_cast<int>(client.bottom) - ContentTop());
	const int top = row * rowHeight;
	if (top < m_scrollOffset) {
		ScrollTo(top);
		return;
	}
	if (top + rowHeight > m_scrollOffset + viewportHeight) {
		ScrollTo(top + rowHeight - viewportHeight);
	}
}

void CDiffSurface::PaintSide(HDC dc, const RECT& column, int headerBottom, bool original)
{
	const int rowHeight = RowHeight();
	if (rowHeight <= 0 || column.right <= column.left) return;
	const int saved = ::SaveDC(dc);
	if (saved == 0) return;
	::IntersectClipRect(dc, column.left, headerBottom, column.right, column.bottom);
	const HGDIOBJ oldFont = ::SelectObject(dc, m_codeFont.Get() != nullptr ? m_codeFont.Get() : m_font);
	::SetBkMode(dc, TRANSPARENT);

	const int gutter = GutterWidth(dc);
	const RECT gutterRect{ column.left, headerBottom, column.left + gutter, column.bottom };
	FillSolid(dc, gutterRect, m_palette.editorGutterBackground.ToColorRef());

	const std::vector<std::wstring>& lines = original ? m_content.originalLines : m_content.modifiedLines;
	const int viewportHeight = (std::max)(1, static_cast<int>(column.bottom) - headerBottom);
	const int rowCount = static_cast<int>(m_content.rows.size());
	const int firstRow = (std::max)(0, m_scrollOffset / rowHeight);
	const int lastRow = (std::min)(rowCount, (m_scrollOffset + viewportHeight) / rowHeight + 1);
	const COLORREF wash = original
		? m_palette.diffRemovedLineBackground.ToColorRef()
		: m_palette.diffInsertedLineBackground.ToColorRef();

	for (int index = firstRow; index < lastRow; ++index) {
		const SDiffSurfaceRow& row = m_content.rows[static_cast<std::size_t>(index)];
		const int top = headerBottom + index * rowHeight - m_scrollOffset;
		const RECT rowRect{ column.left, top, column.right, top + rowHeight };
		const int number = original ? row.originalLineNumber : row.modifiedLineNumber;
		// GDI has no alpha, so the selection cannot be composited over the change
		// wash the way VS Code layers `editor.selectionBackground` over its diff
		// tint. The selection wins, because "these are the lines I am about to
		// stage" is the fact a user is checking here.
		const bool selected = IsRowSelected(index);
		if (number <= 0) {
			// VS Code paints a CSS diagonal stripe where one side has no line at all.
			// GDI has no gradient/stripe primitive, so a hatch brush is the native
			// stand-in; this divergence is recorded in workbench/scm/CLAUDE.md.
			FillSolid(dc, rowRect, selected ? m_palette.accent.ToColorRef() : m_palette.canvas.ToColorRef());
			// The background mode is already TRANSPARENT, so the hatch leaves the
			// fill below it visible and a selected empty row still reads as empty.
			if (const HBRUSH hatch = ::CreateHatchBrush(HS_FDIAGONAL, m_palette.diffDiagonalFill.ToColorRef());
				hatch != nullptr) {
				::FillRect(dc, &rowRect, hatch);
				::DeleteObject(hatch);
			}
			continue;
		}
		if (selected) {
			FillSolid(dc, rowRect, m_palette.accent.ToColorRef());
		} else if (row.changed) {
			// The wash covers the line-number margin too, exactly as the diff editor
			// tints the whole changed line upstream.
			FillSolid(dc, rowRect, wash);
		}
		const RECT numberRect{ column.left, top, column.left + gutter - ScaleDip(6), top + rowHeight };
		const std::wstring numberText = std::to_wstring(number);
		const COLORREF numberColor = selected
			? m_palette.highlightText.ToColorRef()
			: (row.changed ? m_palette.editorLineNumberActiveForeground : m_palette.editorLineNumberForeground).ToColorRef();
		::SetTextColor(dc, numberColor);
		RECT numberBounds = numberRect;
		::DrawTextW(dc, numberText.c_str(), -1, &numberBounds, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		const std::size_t lineIndex = static_cast<std::size_t>(number - 1);
		if (lineIndex >= lines.size()) continue;
		RECT textBounds{ column.left + gutter + ScaleDip(6), top, column.right - ScaleDip(4), top + rowHeight };
		::SetTextColor(dc, selected ? m_palette.highlightText.ToColorRef() : m_palette.primaryText.ToColorRef());
		::DrawTextW(dc, lines[lineIndex].c_str(), -1, &textBounds,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_EXPANDTABS | DT_END_ELLIPSIS);
	}

	if (oldFont != nullptr) ::SelectObject(dc, oldFont);
	::RestoreDC(dc, saved);
}

void CDiffSurface::Paint()
{
	PAINTSTRUCT ps{};
	const HDC dc = ::BeginPaint(GetHwnd(), &ps);
	if (dc == nullptr) return;
	RECT client{};
	::GetClientRect(GetHwnd(), &client);
	FillSolid(dc, client, m_palette.canvas.ToColorRef());

	const int padding = ScaleDip(10);
	const int headerHeight = ScaleDip(44);
	// The caption strip is whatever `ContentTop()` leaves below the header, so its
	// height is never computed a second time here.
	const int contentTop = ContentTop();
	const int closeSide = ScaleDip(28);

	const RECT header{ 0, 0, client.right, headerHeight };
	FillSolid(dc, header, m_palette.raised.ToColorRef());
	const std::wstring title = m_content.title.empty() ? std::wstring(L"Diff") : m_content.title;
	int titleRight = (std::max<int>)(padding, static_cast<int>(client.right) - padding * 2 - closeSide);
	if (m_content.truncated) {
		const int noteWidth = ScaleDip(200);
		RECT note{ (std::max)(padding, titleRight - noteWidth), 0, titleRight, headerHeight };
		PaintText(dc, L"Comparison truncated", note, m_palette.warning.ToColorRef(),
			DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		titleRight = (std::max)(padding, static_cast<int>(note.left) - ScaleDip(8));
	}
	PaintText(dc, title.c_str(), RECT{ padding, 0, titleRight, headerHeight },
		m_palette.primaryText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, true);

	const int mid = static_cast<int>(client.right) / 2;
	const RECT caption{ 0, headerHeight, client.right, contentTop };
	FillSolid(dc, caption, m_palette.panel.ToColorRef());
	PaintText(dc, m_content.originalLabel.c_str(), RECT{ padding, caption.top, (std::max)(padding, mid - padding), caption.bottom },
		m_palette.descriptionText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	PaintText(dc, m_content.modifiedLabel.c_str(), RECT{ mid + padding, caption.top, (std::max<int>)(mid + padding, static_cast<int>(client.right) - padding), caption.bottom },
		m_palette.descriptionText.ToColorRef(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

	if (!m_hasContent || m_content.rows.empty()) {
		const wchar_t* message = m_hasContent
			? L"The two sides are identical."
			: L"No comparison is open.";
		PaintText(dc, message, RECT{ padding, contentTop + ScaleDip(16), (std::max<int>)(padding, static_cast<int>(client.right) - padding), client.bottom },
			m_palette.disabledText.ToColorRef(), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
		::EndPaint(GetHwnd(), &ps);
		return;
	}

	PaintSide(dc, RECT{ 0, contentTop, mid, client.bottom }, contentTop, true);
	PaintSide(dc, RECT{ mid + 1, contentTop, client.right, client.bottom }, contentTop, false);
	FillSolid(dc, RECT{ mid, headerHeight, mid + 1, client.bottom }, m_palette.border.ToColorRef());

	::EndPaint(GetHwnd(), &ps);
}

void CDiffSurface::InvokeClose()
{
	if (!m_onCloseRequested) return;
	try {
		m_onCloseRequested();
	} catch (...) {
		// Callbacks are composition-owned; keep the surface alive.
	}
}

LRESULT CDiffSurface::DispatchEvent(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		Paint();
		return 0;
	case WM_DRAWITEM: {
		const DRAWITEMSTRUCT* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
		if (draw != nullptr && draw->hwndItem == m_hwndClose) {
			DrawCloseButton(*draw);
			return TRUE;
		}
		break;
	}
	case WM_SIZE:
		LayoutChildren();
		UpdateScrollRange();
		return 0;
	case WM_VSCROLL: {
		SCROLLINFO info{ sizeof(info), SIF_ALL, 0, 0, 0, 0, 0 };
		::GetScrollInfo(hwnd, SB_VERT, &info);
		switch (LOWORD(wp)) {
		case SB_LINEUP: ScrollTo(m_scrollOffset - RowHeight()); break;
		case SB_LINEDOWN: ScrollTo(m_scrollOffset + RowHeight()); break;
		case SB_PAGEUP: ScrollTo(m_scrollOffset - static_cast<int>(info.nPage)); break;
		case SB_PAGEDOWN: ScrollTo(m_scrollOffset + static_cast<int>(info.nPage)); break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: ScrollTo(info.nTrackPos); break;
		case SB_TOP: ScrollTo(0); break;
		case SB_BOTTOM: ScrollTo(m_maxScrollOffset); break;
		default: break;
		}
		return 0;
	}
	case WM_MOUSEWHEEL: {
		const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		ScrollTo(m_scrollOffset - notches * RowHeight() * 3);
		return 0;
	}
	case WM_LBUTTONDOWN: {
		::SetFocus(hwnd);
		const int row = RowAtPoint(GET_Y_LPARAM(lp));
		if (row < 0) {
			// A press below the last row clears the selection rather than keeping
			// one the user can no longer see the extent of.
			ClearSelection();
			return 0;
		}
		MoveSelectionTo(row, (wp & MK_SHIFT) != 0);
		m_selecting = true;
		::SetCapture(hwnd);
		return 0;
	}
	case WM_MOUSEMOVE:
		if (m_selecting) {
			// Dragging past either edge keeps extending, clamped to the rows that
			// exist, so a drag cannot select a row the comparison does not have.
			const int y = GET_Y_LPARAM(lp);
			const int rowHeight = RowHeight();
			const int rowCount = static_cast<int>(m_content.rows.size());
			if (rowCount > 0 && rowHeight > 0) {
				const int row = (std::max)(0, (std::min)(rowCount - 1,
					(y - ContentTop() + m_scrollOffset) / rowHeight));
				MoveSelectionTo(row, true);
			}
			return 0;
		}
		break;
	case WM_LBUTTONUP:
		if (m_selecting) {
			m_selecting = false;
			if (::GetCapture() == hwnd) ::ReleaseCapture();
			return 0;
		}
		break;
	case WM_CAPTURECHANGED:
		m_selecting = false;
		return 0;
	case WM_GETDLGCODE:
		// Arrow keys move the selection here, so they must not be consumed as
		// dialog navigation before this surface sees them.
		return DLGC_WANTARROWS | DLGC_WANTCHARS;
	case WM_KEYDOWN: {
		const bool extend = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const int rowCount = static_cast<int>(m_content.rows.size());
		const int focus = m_selectionFocusRow;
		switch (wp) {
		case VK_UP:
			if (rowCount > 0) { MoveSelectionTo(focus < 0 ? 0 : focus - 1, extend); return 0; }
			ScrollTo(m_scrollOffset - RowHeight());
			return 0;
		case VK_DOWN:
			if (rowCount > 0) { MoveSelectionTo(focus < 0 ? 0 : focus + 1, extend); return 0; }
			ScrollTo(m_scrollOffset + RowHeight());
			return 0;
		case VK_PRIOR:
			if (rowCount > 0) { MoveSelectionTo(focus < 0 ? 0 : focus - 10, extend); return 0; }
			ScrollTo(m_scrollOffset - (std::max)(RowHeight(), m_maxScrollOffset > 0 ? RowHeight() * 10 : 0));
			return 0;
		case VK_NEXT:
			if (rowCount > 0) { MoveSelectionTo(focus < 0 ? 0 : focus + 10, extend); return 0; }
			ScrollTo(m_scrollOffset + RowHeight() * 10);
			return 0;
		case VK_HOME:
			if (rowCount > 0) { MoveSelectionTo(0, extend); return 0; }
			ScrollTo(0);
			return 0;
		case VK_END:
			if (rowCount > 0) { MoveSelectionTo(rowCount - 1, extend); return 0; }
			ScrollTo(m_maxScrollOffset);
			return 0;
		case 'A':
			if (control && rowCount > 0) {
				m_selectionAnchorRow = 0;
				m_selectionFocusRow = rowCount - 1;
				::InvalidateRect(hwnd, nullptr, FALSE);
				return 0;
			}
			break;
		case VK_ESCAPE: InvokeClose(); return 0;
		default: break;
		}
		break;
	}
	case WM_DPICHANGED:
		ReleaseFont();
		ReleaseCodiconFont();
		EnsureFont();
		if (m_font != nullptr && m_hwndClose != nullptr) {
			::SendMessageW(m_hwndClose, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
		}
		LayoutChildren();
		UpdateScrollRange();
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_COMMAND:
		if (LOWORD(wp) == kCloseButtonId) {
			InvokeClose();
			return 0;
		}
		break;
	case WM_SETFOCUS:
		m_focused = true;
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_KILLFOCUS:
		m_focused = false;
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	case WM_NCDESTROY:
		m_hwndClose = nullptr;
		_SetHwnd(nullptr);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return ::DefWindowProcW(hwnd, msg, wp, lp);
	default:
		break;
	}
	return CWnd::DispatchEvent(hwnd, msg, wp, lp);
}
