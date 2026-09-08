/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "markdown/CMarkdownPreviewWnd.h"
#include <windowsx.h>
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace markdown {
namespace {
unsigned int FontStyle(const InlineStyleRun& run) noexcept
{
	return (run.Has(InlineStyleFlag::Strong) ? 1u : 0u)
		| (run.Has(InlineStyleFlag::Emphasis) ? 2u : 0u)
		| (run.Has(InlineStyleFlag::Link) ? 4u : 0u)
		| (run.Has(InlineStyleFlag::Strikethrough) ? 8u : 0u);
}
bool LowSurrogate(wchar_t value) noexcept { return value >= 0xdc00 && value <= 0xdfff; }
bool HighSurrogate(wchar_t value) noexcept { return value >= 0xd800 && value <= 0xdbff; }
std::size_t ScalarBoundary(std::wstring_view text, std::size_t position) noexcept
{
	position = std::min(position, text.size());
	if (position && position < text.size() && LowSurrogate(text[position]) && HighSurrogate(text[position - 1])) --position;
	return position;
}
}

void CMarkdownPreviewWnd::EndTextSelection() noexcept
{
	m_selecting = false;
	if (m_hWnd && ::GetCapture() == m_hWnd) ::ReleaseCapture();
}
void CMarkdownPreviewWnd::ResetTextSelection() noexcept
{
	EndTextSelection();
	m_selectionAnchor = m_selectionCaret = 0;
	m_selectionAvailable = m_selectionContentCurrent = false;
	if (m_hWnd) ::InvalidateRect(m_hWnd, nullptr, FALSE);
}
void CMarkdownPreviewWnd::SelectAllText() noexcept
{
	if (!m_selectionAvailable || !m_hWnd) return;
	EndTextSelection(); m_selectionAnchor = 0; m_selectionCaret = m_selectionText.size();
	::InvalidateRect(m_hWnd, nullptr, FALSE);
}
std::wstring CMarkdownPreviewWnd::SelectedText() const
{
	if (!m_selectionAvailable || !m_hWnd) return {};
	const auto [first, last] = std::minmax(m_selectionAnchor, m_selectionCaret);
	return m_selectionText.substr(first, last - first);
}
bool CMarkdownPreviewWnd::CopySelection()
{
	const auto text = SelectedText();
	if (text.empty()) return false;
	if (m_copySink) { const auto sink = m_copySink; return sink(text); }
	const auto memory = ::GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
	if (!memory) return false;
	auto* target = static_cast<wchar_t*>(::GlobalLock(memory));
	if (!target) { ::GlobalFree(memory); return false; }
	std::copy(text.begin(), text.end(), target); target[text.size()] = 0;
	::GlobalUnlock(memory);
	if (!::OpenClipboard(m_hWnd)) { ::GlobalFree(memory); return false; }
	const bool copied = ::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, memory);
	::CloseClipboard(); if (!copied) ::GlobalFree(memory);
	return copied;
}
void CMarkdownPreviewWnd::SetFindCallback(std::function<void(PreviewFindAction)> callback)
{
	m_findCallback = std::move(callback);
}
void CMarkdownPreviewWnd::SetCopySink(std::function<bool(std::wstring_view)> sink)
{
	m_copySink = std::move(sink);
}
void CMarkdownPreviewWnd::SetCopyCommand(std::function<void()> callback)
{
	m_copyCommand = std::move(callback);
}
PreviewFindResult CMarkdownPreviewWnd::FindText(std::wstring_view query, bool previous, bool matchCase)
{
	if (!m_selectionAvailable || !m_hWnd) return PreviewFindResult::Unavailable;
	if (query.empty() || query.size() > 1024 || query.find(L'\0') != std::wstring_view::npos
		|| m_selectionText.size() > INT_MAX) return PreviewFindResult::Invalid;
	for (std::size_t index = 0; index < query.size(); ++index) {
		if (LowSurrogate(query[index])) return PreviewFindResult::Invalid;
		if (HighSurrogate(query[index]) && (++index == query.size() || !LowSurrogate(query[index])))
			return PreviewFindResult::Invalid;
	}
	const auto [first, last] = std::minmax(m_selectionAnchor, m_selectionCaret);
	const auto search = [&](std::size_t start, std::size_t end) {
		return ::FindStringOrdinal(previous ? FIND_FROMEND : FIND_FROMSTART,
			m_selectionText.data() + start, static_cast<int>(end - start), query.data(),
			static_cast<int>(query.size()), !matchCase);
	};
	auto start = previous ? 0 : last;
	auto found = search(start, previous ? first : m_selectionText.size());
	bool wrapped = false;
	if (found < 0) {
		start = previous ? first : 0;
		found = search(start, previous ? m_selectionText.size() : last); wrapped = true;
	}
	if (found < 0) return ::GetLastError() == ERROR_SUCCESS ? PreviewFindResult::NotFound : PreviewFindResult::Invalid;
	EndTextSelection();
	m_selectionAnchor = start + static_cast<std::size_t>(found);
	m_selectionCaret = m_selectionAnchor + query.size();
	// Text offsets follow reading order, while table paint lines are ordered by
	// geometry. A user-requested find does one bounded pass to reveal its match.
	for (const auto& line : m_lines) {
		if (line.textOffset != std::wstring::npos && line.textOffset <= m_selectionAnchor
			&& m_selectionAnchor < line.textOffset + line.text.size()) {
			ScrollTo(std::max(0, line.top - ScaleDip(20)), false); break;
		}
	}
	::InvalidateRect(m_hWnd, nullptr, FALSE);
	return wrapped ? PreviewFindResult::Wrapped : PreviewFindResult::Found;
}

int CMarkdownPreviewWnd::TextPositionX(HDC dc, const RenderLine& line, std::size_t position) const
{
	position = std::min(position, line.text.size());
	int x = line.left;
	const auto measure = [&](std::size_t start, std::size_t length, unsigned int style, bool code) {
		if (start >= position) return;
		const auto oldFont = ::SelectObject(dc, GetFont(line.font, style, code));
		SIZE size{}; ::GetTextExtentPoint32W(dc, line.text.data() + start,
			static_cast<int>(std::min(length, position - start)), &size);
		::SelectObject(dc, oldFont); x += size.cx;
	};
	if (line.styleRuns.empty() || !line.codeTokens.empty()) measure(0, position, 0, false);
	else for (const auto& run : line.styleRuns) measure(run.start, run.length, FontStyle(run), run.Has(InlineStyleFlag::Code));
	return x;
}

void CMarkdownPreviewWnd::DrawSelection(HDC dc, const RenderLine& line, int top) const
{
	if (!m_selectionAvailable || line.textOffset == std::wstring::npos || line.text.empty()) return;
	const auto [first, last] = std::minmax(m_selectionAnchor, m_selectionCaret);
	const auto start = std::max(first, line.textOffset), end = std::min(last, line.textOffset + line.text.size());
	if (start >= end) return;
	RECT selected{ TextPositionX(dc, line, start - line.textOffset), top,
		TextPositionX(dc, line, end - line.textOffset), top + line.height };
	const auto saved = ::SaveDC(dc);
	if (!saved) return;
	::IntersectClipRect(dc, selected.left, selected.top, selected.right, selected.bottom);
	::SetDCBrushColor(dc, m_colors.selectionBackground);
	::FillRect(dc, &selected, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
	::SetTextColor(dc, m_colors.selectionText); ::SetBkMode(dc, TRANSPARENT);
	int x = line.left;
	const auto draw = [&](std::size_t startOffset, std::size_t length, unsigned int style, bool code) {
		const auto oldFont = ::SelectObject(dc, GetFont(line.font, style, code));
		const auto* text = line.text.data() + startOffset;
		::TextOutW(dc, x, top, text, static_cast<int>(length));
		SIZE size{}; ::GetTextExtentPoint32W(dc, text, static_cast<int>(length), &size); x += size.cx;
		::SelectObject(dc, oldFont);
	};
	if (line.styleRuns.empty() || !line.codeTokens.empty()) draw(0, line.text.size(), 0, false);
	else for (const auto& run : line.styleRuns) draw(run.start, run.length, FontStyle(run), run.Has(InlineStyleFlag::Code));
	::RestoreDC(dc, saved);
}

std::size_t CMarkdownPreviewWnd::HitText(POINT point) const
{
	if (m_lines.empty() || !m_selectionAvailable) return 0;
	point.y += m_scrollY;
	auto first = std::lower_bound(m_lines.begin(), m_lines.end(), point.y,
		[](const RenderLine& line, int y) { return line.top + line.height < y; });
	if (first != m_lines.begin()) --first;
	const auto dc = ::GetDC(m_hWnd);
	if (!dc) return m_selectionCaret;
	const RenderLine* closest{}; long long distance = LLONG_MAX;
	for (auto it = first; it != m_lines.end(); ++it) {
		if (closest && it->top > std::max<long>(point.y, closest->top + closest->height)) break;
		if (it->textOffset == std::wstring::npos || it->text.empty()) continue;
		const auto right = TextPositionX(dc, *it, it->text.size());
		const auto dx = std::max({ 0L, it->left - point.x, point.x - right });
		const auto dy = std::max({ 0L, it->top - point.y, point.y - (it->top + it->height) });
		const auto current = static_cast<long long>(dy) * dy + static_cast<long long>(dx) * dx;
		if (current < distance) { closest = &*it; distance = current; }
	}
	std::size_t result = point.y < m_lines.front().top ? 0 : m_selectionText.size();
	if (closest) {
		std::size_t low = 0, high = closest->text.size();
		while (low < high) {
			const auto middle = low + (high - low) / 2;
			if (TextPositionX(dc, *closest, middle) < point.x) low = middle + 1; else high = middle;
		}
		if (low && point.x - TextPositionX(dc, *closest, low - 1) < TextPositionX(dc, *closest, low) - point.x) --low;
		result = closest->textOffset + ScalarBoundary(closest->text, low);
	}
	::ReleaseDC(m_hWnd, dc);
	return ScalarBoundary(m_selectionText, result);
}

bool CMarkdownPreviewWnd::HandleSelectionMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED || message == WM_KILLFOCUS
		|| (message == WM_SHOWWINDOW && wParam == FALSE)) { EndTextSelection(); return false; }
	const auto copy = [&] {
		if (const auto callback = m_copyCommand) callback(); else (void)CopySelection();
	};
	if (message == WM_COPY) { copy(); return true; }
	if (message == WM_LBUTTONDOWN && m_selectionAvailable) {
		::SetFocus(m_hWnd);
		const auto position = HitText({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		if ((wParam & MK_SHIFT) == 0) m_selectionAnchor = position;
		m_selectionCaret = position; m_selecting = true; ::SetCapture(m_hWnd);
		::InvalidateRect(m_hWnd, nullptr, FALSE); return true;
	}
	if ((message == WM_MOUSEMOVE || message == WM_LBUTTONUP) && m_selecting) {
		RECT client{}; ::GetClientRect(m_hWnd, &client);
		POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (point.y < 0 || point.y > client.bottom) ScrollBy(point.y < 0 ? -ScaleDip(36) : ScaleDip(36), false);
		point.y = std::clamp(point.y, 0L, client.bottom);
		m_selectionCaret = HitText(point);
		if (message == WM_LBUTTONUP) EndTextSelection();
		::InvalidateRect(m_hWnd, nullptr, FALSE); return true;
	}
	if (message == WM_KEYDOWN) {
		const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
		if (control && wParam == 'A') { SelectAllText(); return true; }
		if (control && wParam == 'C') { copy(); return true; }
		if (((control && wParam == 'F') || wParam == VK_F3) && m_findCallback) {
			if (const auto callback = m_findCallback) callback(wParam != VK_F3 ? PreviewFindAction::Show
				: shift ? PreviewFindAction::Previous : PreviewFindAction::Next);
			return true;
		}

	}
	return false;
}

} // namespace markdown
