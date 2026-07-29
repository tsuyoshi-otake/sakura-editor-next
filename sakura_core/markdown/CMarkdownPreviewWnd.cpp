/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "CMarkdownPreviewWnd.h"

#include "theme/CThemeService.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace markdown {
namespace {

constexpr wchar_t kPreviewWindowClass[] = L"SakuraMarkdownPreview";
constexpr int kDefaultDpi = 96;

[[nodiscard]] HFONT CreatePreviewFont(const LOGFONT& base, int percentage, bool bold, bool underline) noexcept
{
	LOGFONT font = base;
	if (font.lfHeight == 0) {
		font.lfHeight = -16;
	}
	const auto sign = font.lfHeight < 0 ? -1 : 1;
	const LONG absoluteHeight = std::max<LONG>(1, font.lfHeight * sign);
	font.lfHeight = sign * std::max<LONG>(1, ::MulDiv(absoluteHeight, percentage, 100));
	font.lfWeight = bold ? FW_BOLD : (font.lfWeight == 0 ? FW_NORMAL : font.lfWeight);
	font.lfUnderline = underline ? TRUE : FALSE;
	return ::CreateFontIndirectW(&font);
}

[[nodiscard]] std::vector<InlineSpan> ClipInlineSpans(const std::vector<InlineSpan>& spans,
	std::size_t start, std::size_t length)
{
	std::vector<InlineSpan> result;
	const auto end = start + length;
	for (const auto& span : spans) {
		const auto spanEnd = span.start + span.length;
		const auto overlapStart = std::max(start, span.start);
		const auto overlapEnd = std::min(end, spanEnd);
		if (overlapStart < overlapEnd) {
			result.push_back({ span.kind, overlapStart - start, overlapEnd - overlapStart });
		}
	}
	return result;
}

[[nodiscard]] bool IsWrapSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t';
}

} // namespace

CMarkdownPreviewWnd::~CMarkdownPreviewWnd()
{
	Close();
	DeleteFonts();
	DeletePaintResources();
}

bool CMarkdownPreviewWnd::Create(HWND parent)
{
	if (m_hWnd != nullptr) {
		return true;
	}
	if (parent == nullptr) {
		return false;
	}

	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSW windowClass{};
		windowClass.style = CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc = &CMarkdownPreviewWnd::WindowProc;
		windowClass.hInstance = ::GetModuleHandleW(nullptr);
		windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
		windowClass.lpszClassName = kPreviewWindowClass;
		if (::RegisterClassW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			return false;
		}
		classRegistered = true;
	}

	m_hWnd = ::CreateWindowExW(0, kPreviewWindowClass, L"",
		WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VSCROLL | WS_TABSTOP,
		0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), this);
	if (m_hWnd == nullptr) {
		return false;
	}
	RebuildFonts();
	RebuildPaintResources();
	RebuildLayout();
	return true;
}

void CMarkdownPreviewWnd::Close() noexcept
{
	if (m_hWnd != nullptr) {
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
}

void CMarkdownPreviewWnd::SetDocument(Document document)
{
	m_document = std::move(document);
	RebuildLayout();
}

void CMarkdownPreviewWnd::SetSourceTruncated(bool truncated)
{
	if (m_sourceTruncated == truncated) {
		return;
	}
	m_sourceTruncated = truncated;
	RebuildLayout();
}

void CMarkdownPreviewWnd::SetPalette(const theme::ThemePalette& palette)
{
	m_colors.background = palette.canvas.ToColorRef();
	m_colors.codeBackground = palette.raised.ToColorRef();
	m_colors.border = palette.border.ToColorRef();
	m_colors.primaryText = palette.primaryText.ToColorRef();
	m_colors.secondaryText = palette.secondaryText.ToColorRef();
	m_colors.link = palette.accent.ToColorRef();
	RebuildPaintResources();
	if (m_hWnd != nullptr) {
		::InvalidateRect(m_hWnd, nullptr, FALSE);
	}
}

void CMarkdownPreviewWnd::SetEditorFont(const LOGFONT& font, unsigned int dpi)
{
	if (m_hasEditorFont && m_editorFontDpi == dpi && std::memcmp(&m_editorFont, &font, sizeof(LOGFONT)) == 0) {
		return;
	}
	m_editorFont = font;
	m_editorFontDpi = dpi == 0 ? kDefaultDpi : dpi;
	m_hasEditorFont = true;
	RebuildFonts();
	RebuildLayout();
}

void CMarkdownPreviewWnd::Layout(const RECT& bounds, unsigned int dpi)
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	if (m_dpi != effectiveDpi) {
		m_dpi = effectiveDpi;
		RebuildFonts();
		RebuildPaintResources();
	}
	if (m_hWnd != nullptr) {
		::MoveWindow(m_hWnd, bounds.left, bounds.top,
			std::max(0L, bounds.right - bounds.left), std::max(0L, bounds.bottom - bounds.top), TRUE);
	}
}

void CMarkdownPreviewWnd::Show(bool visible) const noexcept
{
	if (m_hWnd != nullptr) {
		::ShowWindow(m_hWnd, visible ? SW_SHOWNA : SW_HIDE);
	}
}

LRESULT CALLBACK CMarkdownPreviewWnd::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	CMarkdownPreviewWnd* preview = nullptr;
	if (message == WM_NCCREATE) {
		const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		preview = static_cast<CMarkdownPreviewWnd*>(create->lpCreateParams);
		preview->m_hWnd = hwnd;
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(preview));
	} else {
		preview = reinterpret_cast<CMarkdownPreviewWnd*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	}
	if (preview != nullptr) {
		return preview->HandleMessage(message, wParam, lParam);
	}
	return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CMarkdownPreviewWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message) {
	case WM_NCDESTROY: {
		const auto hwnd = m_hWnd;
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		m_hWnd = nullptr;
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_SIZE:
		RebuildLayout();
		return 0;

	case WM_DPICHANGED:
		m_dpi = HIWORD(wParam) == 0 ? kDefaultDpi : HIWORD(wParam);
		RebuildFonts();
		RebuildPaintResources();
		RebuildLayout();
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const auto dc = ::BeginPaint(m_hWnd, &paint);
		Paint(dc, paint.rcPaint);
		::EndPaint(m_hWnd, &paint);
		return 0;
	}

	case WM_VSCROLL: {
		int position = m_scrollY;
		switch (LOWORD(wParam)) {
		case SB_LINEUP: position -= ScaleDip(36); break;
		case SB_LINEDOWN: position += ScaleDip(36); break;
		case SB_PAGEUP: position -= ScaleDip(180); break;
		case SB_PAGEDOWN: position += ScaleDip(180); break;
		case SB_TOP: position = 0; break;
		case SB_BOTTOM: position = m_maxScroll; break;
		case SB_THUMBPOSITION:
		case SB_THUMBTRACK: {
			SCROLLINFO info{ sizeof(info), SIF_TRACKPOS };
			if (::GetScrollInfo(m_hWnd, SB_VERT, &info)) {
				position = info.nTrackPos;
			}
			break;
		}
		default:
			break;
		}
		ScrollTo(position);
		return 0;
	}

	case WM_MOUSEWHEEL:
		ScrollBy(-::MulDiv(GET_WHEEL_DELTA_WPARAM(wParam), ScaleDip(54), WHEEL_DELTA));
		return 0;

	case WM_LBUTTONDOWN:
		::SetFocus(m_hWnd);
		return 0;

	case WM_GETDLGCODE:
		return DLGC_WANTARROWS | DLGC_WANTCHARS;

	case WM_KEYDOWN:
		switch (wParam) {
		case VK_UP: ScrollBy(-ScaleDip(36)); return 0;
		case VK_DOWN: ScrollBy(ScaleDip(36)); return 0;
		case VK_HOME: ScrollTo(0); return 0;
		case VK_END: ScrollTo(m_maxScroll); return 0;
		case VK_PRIOR: ScrollBy(-ScaleDip(180)); return 0;
		case VK_NEXT: ScrollBy(ScaleDip(180)); return 0;
		case VK_SPACE: ScrollBy((::GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -ScaleDip(180) : ScaleDip(180)); return 0;
		default: break;
		}
		break;

	default:
		break;
	}
	return ::DefWindowProcW(m_hWnd, message, wParam, lParam);
}

void CMarkdownPreviewWnd::RebuildFonts()
{
	DeleteFonts();
	LOGFONT font = m_editorFont;
	if (!m_hasEditorFont) {
		font.lfHeight = -16;
		font.lfWeight = FW_NORMAL;
	}
	const auto sourceDpi = m_editorFontDpi == 0 ? kDefaultDpi : m_editorFontDpi;
	if (font.lfHeight != 0 && sourceDpi != m_dpi) {
		font.lfHeight = ::MulDiv(font.lfHeight, static_cast<int>(m_dpi), static_cast<int>(sourceDpi));
	}
	// Preview prose deliberately follows Windows' proportional UI typography;
	// source code remains monospace even if the editor's document font differs.
	LOGFONT proseFont = font;
	proseFont.lfWeight = FW_NORMAL;
	proseFont.lfQuality = CLEARTYPE_QUALITY;
	(void)wcscpy_s(proseFont.lfFaceName, LF_FACESIZE, L"Segoe UI Variable");
	LOGFONT codeFont = font;
	codeFont.lfWeight = FW_NORMAL;
	codeFont.lfQuality = CLEARTYPE_QUALITY;
	(void)wcscpy_s(codeFont.lfFaceName, LF_FACESIZE, L"Cascadia Mono");

	m_bodyFont = CreatePreviewFont(proseFont, 100, false, false);
	m_bodyLinkFont = CreatePreviewFont(proseFont, 100, false, true);
	m_codeFont = CreatePreviewFont(codeFont, 100, false, false);
	if (m_codeFont == nullptr) {
		(void)wcscpy_s(codeFont.lfFaceName, LF_FACESIZE, L"Consolas");
		m_codeFont = CreatePreviewFont(codeFont, 100, false, false);
	}
	constexpr int headingPercentages[] = { 170, 150, 135, 120, 110, 100 };
	for (std::size_t index = 0; index < m_headingFonts.size(); ++index) {
		m_headingFonts[index] = CreatePreviewFont(proseFont, headingPercentages[index], true, false);
		m_headingLinkFonts[index] = CreatePreviewFont(proseFont, headingPercentages[index], true, true);
	}
}

void CMarkdownPreviewWnd::DeleteFonts() noexcept
{
	auto deleteFont = [](HFONT& font) {
		if (font != nullptr) {
			::DeleteObject(font);
			font = nullptr;
		}
	};
	deleteFont(m_bodyFont);
	deleteFont(m_bodyLinkFont);
	deleteFont(m_codeFont);
	for (auto& font : m_headingFonts) {
		deleteFont(font);
	}
	for (auto& font : m_headingLinkFonts) {
		deleteFont(font);
	}
}

void CMarkdownPreviewWnd::RebuildPaintResources()
{
	DeletePaintResources();
	m_backgroundBrush = ::CreateSolidBrush(m_colors.background);
	m_codeBackgroundBrush = ::CreateSolidBrush(m_colors.codeBackground);
	m_quoteBrush = ::CreateSolidBrush(m_colors.border);
	m_noticeBrush = ::CreateSolidBrush(m_colors.codeBackground);
	m_rulePen = ::CreatePen(PS_SOLID, std::max(1, ScaleDip(1)), m_colors.border);
}

void CMarkdownPreviewWnd::DeletePaintResources() noexcept
{
	auto deleteObject = [](auto& object) {
		if (object != nullptr) {
			::DeleteObject(object);
			object = nullptr;
		}
	};
	deleteObject(m_backgroundBrush);
	deleteObject(m_codeBackgroundBrush);
	deleteObject(m_quoteBrush);
	deleteObject(m_noticeBrush);
	deleteObject(m_rulePen);
}

void CMarkdownPreviewWnd::RebuildLayout()
{
	if (m_hWnd == nullptr) {
		return;
	}
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	m_lines.clear();
	int top = ScaleDip(12);
	const int leftPadding = ScaleDip(14);
	const int rightPadding = ScaleDip(14);
	const int clientWidth = std::max(0L, client.right - client.left);

	const auto dc = ::GetDC(m_hWnd);
	if (dc != nullptr) {
		const auto lineGap = ScaleDip(6);
		for (const auto& block : m_document.blocks) {
			switch (block.kind) {
			case BlockKind::Heading: {
				const auto level = std::clamp(block.level, 1, 6);
				const auto font = static_cast<FontKind>(static_cast<int>(FontKind::Heading1) + level - 1);
				AppendWrappedText(dc, block, font, LineKind::Text, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				top += lineGap;
				break;
			}
			case BlockKind::Paragraph:
				AppendWrappedText(dc, block, FontKind::Body, LineKind::Text, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				top += lineGap;
				break;

			case BlockKind::BulletListItem:
			case BlockKind::OrderedListItem: {
				const auto indent = leftPadding + ScaleDip(18) * std::max(0, block.level);
				const auto oldFont = static_cast<HFONT>(::SelectObject(dc, GetFont(FontKind::Body)));
				SIZE markerExtent{};
				(void)::GetTextExtentPoint32W(dc, block.marker.data(), static_cast<int>(block.marker.size()), &markerExtent);
				::SelectObject(dc, oldFont);
				const auto fullWidth = std::max(1, clientWidth - indent - rightPadding);
				const auto continuationLeft = indent + markerExtent.cx;
				AppendWrappedText(dc, block, FontKind::Body, LineKind::Text, indent,
					fullWidth, &top, continuationLeft,
					std::max(1, fullWidth - static_cast<int>(markerExtent.cx)));
				break;
			}

			case BlockKind::BlockQuote:
				AppendWrappedText(dc, block, FontKind::Body, LineKind::Quote, leftPadding + ScaleDip(12),
					std::max(1, clientWidth - leftPadding - rightPadding - ScaleDip(12)), &top);
				top += lineGap;
				break;

			case BlockKind::CodeBlock: {
				std::size_t start = 0;
				do {
					const auto end = block.text.find(L'\n', start);
					Block codeLine;
					codeLine.kind = BlockKind::CodeBlock;
					codeLine.text = block.text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
					AppendWrappedText(dc, codeLine, FontKind::Code, LineKind::Code, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding), &top);
					if (end == std::wstring::npos) {
						break;
					}
					start = end + 1;
				} while (start <= block.text.size());
				top += lineGap;
				break;
			}

			case BlockKind::HorizontalRule:
				m_lines.push_back({ {}, {}, leftPadding, top, std::max(ScaleDip(12), 1), FontKind::Body, LineKind::Rule });
				top += std::max(ScaleDip(12), 1) + lineGap;
				break;
			}
		}
		if (m_sourceTruncated) {
			Block notice;
			notice.text = L"Preview truncated: showing the first 2 MiB or 200,000 lines.";
			AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
				std::max(1, clientWidth - leftPadding - rightPadding), &top);
			top += lineGap;
		}
		::ReleaseDC(m_hWnd, dc);
	}
	m_contentHeight = top + ScaleDip(12);
	UpdateScrollBar();
	::InvalidateRect(m_hWnd, nullptr, FALSE);
}

void CMarkdownPreviewWnd::UpdateScrollBar()
{
	if (m_hWnd == nullptr) {
		return;
	}
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	const int page = std::max(0, static_cast<int>(client.bottom - client.top));
	m_maxScroll = std::max(0, m_contentHeight - page);
	m_scrollY = std::clamp(m_scrollY, 0, m_maxScroll);
	SCROLLINFO info{};
	info.cbSize = sizeof(info);
	info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	info.nMin = 0;
	info.nMax = std::max(0, m_contentHeight - 1);
	info.nPage = static_cast<UINT>(page);
	info.nPos = m_scrollY;
	::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
	::ShowScrollBar(m_hWnd, SB_VERT, m_maxScroll > 0 ? TRUE : FALSE);
}

void CMarkdownPreviewWnd::ScrollTo(int position)
{
	const auto bounded = std::clamp(position, 0, m_maxScroll);
	if (bounded == m_scrollY) {
		return;
	}
	m_scrollY = bounded;
	SCROLLINFO info{};
	info.cbSize = sizeof(info);
	info.fMask = SIF_POS;
	info.nPos = m_scrollY;
	::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
	::InvalidateRect(m_hWnd, nullptr, FALSE);
}

void CMarkdownPreviewWnd::ScrollBy(int delta)
{
	ScrollTo(m_scrollY + delta);
}

void CMarkdownPreviewWnd::Paint(HDC dc, const RECT& paintRect) const
{
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	if (width == 0 || height == 0) {
		return;
	}

	// Keep all line drawing offscreen.  The cached brushes/pens below avoid an
	// allocation for every wrapped line while the memory surface prevents the
	// scroll bar and glyph runs from visibly tearing during rapid updates.
	const auto bufferDc = ::CreateCompatibleDC(dc);
	const auto bitmap = bufferDc == nullptr ? nullptr : ::CreateCompatibleBitmap(dc, width, height);
	HDC targetDc = dc;
	HBITMAP oldBitmap = nullptr;
	const bool buffered = bufferDc != nullptr && bitmap != nullptr;
	if (bufferDc == nullptr || bitmap == nullptr) {
		if (bitmap != nullptr) ::DeleteObject(bitmap);
		if (bufferDc != nullptr) ::DeleteDC(bufferDc);
	} else {
		targetDc = bufferDc;
		oldBitmap = static_cast<HBITMAP>(::SelectObject(bufferDc, bitmap));
	}
	::FillRect(targetDc, &client, m_backgroundBrush != nullptr ? m_backgroundBrush
		: static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));

	const auto first = std::lower_bound(m_lines.begin(), m_lines.end(), m_scrollY,
		[](const RenderLine& line, int scrollY) { return line.top + line.height <= scrollY; });
	for (auto iterator = first; iterator != m_lines.end(); ++iterator) {
		const auto top = iterator->top - m_scrollY;
		if (top >= client.bottom) {
			break;
		}
		if (top + iterator->height <= paintRect.top) {
			continue;
		}
		DrawLine(targetDc, *iterator, top);
	}
	if (buffered) {
		::BitBlt(dc, paintRect.left, paintRect.top, paintRect.right - paintRect.left,
			paintRect.bottom - paintRect.top, bufferDc, paintRect.left, paintRect.top, SRCCOPY);
		::SelectObject(bufferDc, oldBitmap);
		::DeleteObject(bitmap);
		::DeleteDC(bufferDc);
	}
}

void CMarkdownPreviewWnd::DrawLine(HDC dc, const RenderLine& line, int top) const
{
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	if (line.kind == LineKind::Rule) {
		const auto oldPen = static_cast<HPEN>(::SelectObject(dc, m_rulePen != nullptr ? m_rulePen
			: static_cast<HPEN>(::GetStockObject(BLACK_PEN))));
		::MoveToEx(dc, line.left, top + line.height / 2, nullptr);
		::LineTo(dc, std::max<LONG>(line.left, client.right - ScaleDip(14)), top + line.height / 2);
		::SelectObject(dc, oldPen);
		return;
	}
	if (line.kind == LineKind::Code) {
		RECT codeRect{ ScaleDip(6), top, std::max<LONG>(ScaleDip(6), client.right - ScaleDip(6)), top + line.height };
		::FillRect(dc, &codeRect, m_codeBackgroundBrush != nullptr ? m_codeBackgroundBrush
			: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH)));
	} else if (line.kind == LineKind::Quote) {
		RECT quoteBar{ ScaleDip(10), top, ScaleDip(13), top + line.height };
		::FillRect(dc, &quoteBar, m_quoteBrush != nullptr ? m_quoteBrush
			: static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)));
	} else if (line.kind == LineKind::Notice) {
		RECT noticeRect{ ScaleDip(8), top, std::max<LONG>(ScaleDip(8), client.right - ScaleDip(8)), top + line.height };
		::FillRect(dc, &noticeRect, m_noticeBrush != nullptr ? m_noticeBrush
			: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH)));
	}

	::SetBkMode(dc, TRANSPARENT);
	int left = line.left;
	std::size_t cursor = 0;
	auto drawSegment = [&](std::wstring_view text, bool link) {
		if (text.empty()) {
			return;
		}
		const auto font = GetFont(line.font, link);
		const auto oldFont = ::SelectObject(dc, font);
		::SetTextColor(dc, link ? m_colors.link
		: (line.kind == LineKind::Quote || line.kind == LineKind::Notice
			? m_colors.secondaryText : m_colors.primaryText));
		::TextOutW(dc, left, top, text.data(), static_cast<int>(text.size()));
		SIZE extent{};
		::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
		left += extent.cx;
		::SelectObject(dc, oldFont);
	};

	for (const auto& span : line.inlineSpans) {
		const auto spanStart = std::min(span.start, line.text.size());
		const auto spanEnd = std::min(line.text.size(), span.start + span.length);
		if (spanStart > cursor) {
			drawSegment(std::wstring_view(line.text).substr(cursor, spanStart - cursor), false);
		}
		if (spanEnd > spanStart) {
			drawSegment(std::wstring_view(line.text).substr(spanStart, spanEnd - spanStart), true);
		}
		cursor = std::max(cursor, spanEnd);
	}
	if (cursor < line.text.size()) {
		drawSegment(std::wstring_view(line.text).substr(cursor), false);
	}
}

void CMarkdownPreviewWnd::AppendWrappedText(HDC dc, const Block& block, FontKind font, LineKind kind,
	int left, int availableWidth, int* top, int continuationLeft, int continuationWidth)
{
	std::wstring text = block.marker;
	text.append(block.text);
	std::vector<InlineSpan> spans = block.inlineSpans;
	for (auto& span : spans) {
		span.start += block.marker.size();
	}
	const auto selectedFont = GetFont(font);
	const auto oldFont = ::SelectObject(dc, selectedFont);
	const auto lineHeight = GetLineHeight(dc, font);
	int currentLeft = left;
	int currentWidth = std::max(1, availableWidth);
	std::size_t start = 0;
	do {
		const auto remaining = text.size() - start;
		std::size_t length = remaining;
		if (remaining > 0) {
			int fit = 0;
			SIZE measured{};
			(void)::GetTextExtentExPointW(dc, text.data() + start, static_cast<int>(remaining), currentWidth,
				&fit, nullptr, &measured);
			fit = std::max(1, fit);
			length = static_cast<std::size_t>(fit);
			if (length < remaining) {
				std::size_t breakAt = length;
				while (breakAt > 0 && !IsWrapSpace(text[start + breakAt - 1])) {
					--breakAt;
				}
				if (breakAt > 0) {
					length = breakAt;
				}
			}
		}
		while (length > 0 && IsWrapSpace(text[start + length - 1])) {
			--length;
		}
		m_lines.push_back({ text.substr(start, length), ClipInlineSpans(spans, start, length),
			currentLeft, *top, lineHeight, font, kind });
		*top += lineHeight;
		if (remaining == 0) {
			break;
		}
		start += std::max<std::size_t>(1, length);
		while (start < text.size() && IsWrapSpace(text[start])) {
			++start;
		}
		if (continuationLeft >= 0) {
			currentLeft = continuationLeft;
			currentWidth = std::max(1, continuationWidth);
		}
	} while (start < text.size());
	::SelectObject(dc, oldFont);
}

HFONT CMarkdownPreviewWnd::GetFont(FontKind kind, bool link) const noexcept
{
	if (kind == FontKind::Body) {
		return link && m_bodyLinkFont != nullptr ? m_bodyLinkFont
			: (m_bodyFont != nullptr ? m_bodyFont : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT)));
	}
	if (kind == FontKind::Code) {
		return m_codeFont != nullptr ? m_codeFont : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
	}
	const auto index = static_cast<std::size_t>(static_cast<int>(kind) - static_cast<int>(FontKind::Heading1));
	const auto& font = link ? m_headingLinkFonts[index] : m_headingFonts[index];
	return font != nullptr ? font : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

int CMarkdownPreviewWnd::GetLineHeight(HDC dc, FontKind kind) const
{
	const auto oldFont = ::SelectObject(dc, GetFont(kind));
	TEXTMETRICW metrics{};
	::GetTextMetricsW(dc, &metrics);
	::SelectObject(dc, oldFont);
	return std::max<LONG>(1, metrics.tmHeight + ScaleDip(3));
}

int CMarkdownPreviewWnd::ScaleDip(int dip) const noexcept
{
	const auto dpi = m_dpi == 0 ? kDefaultDpi : m_dpi;
	return ::MulDiv(dip, static_cast<int>(dpi), kDefaultDpi);
}

} // namespace markdown
