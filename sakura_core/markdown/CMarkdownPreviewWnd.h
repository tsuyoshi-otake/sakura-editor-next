/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>

#include "MarkdownParser.h"

namespace theme {
struct ThemePalette;
}

namespace markdown {

//! A cached, GDI-only Markdown preview child window.
//!
//! Parsing and line wrapping happen before WM_PAINT.  Painting only consumes the
//! cached visual lines intersecting the native scroll position.
class CMarkdownPreviewWnd final {
public:
	CMarkdownPreviewWnd() = default;
	~CMarkdownPreviewWnd();

	CMarkdownPreviewWnd(const CMarkdownPreviewWnd&) = delete;
	CMarkdownPreviewWnd& operator=(const CMarkdownPreviewWnd&) = delete;

	[[nodiscard]] bool Create(HWND parent);
	void Close() noexcept;
	void SetDocument(Document document);
	void SetSourceTruncated(bool truncated);
	void SetPalette(const theme::ThemePalette& palette);
	void SetEditorFont(const LOGFONT& font, unsigned int dpi);
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show(bool visible) const noexcept;

	[[nodiscard]] HWND GetHwnd() const noexcept { return m_hWnd; }
	[[nodiscard]] bool IsCreated() const noexcept { return m_hWnd != nullptr; }

private:
	enum class FontKind {
		Body,
		Heading1,
		Heading2,
		Heading3,
		Heading4,
		Heading5,
		Heading6,
		Code,
	};

	enum class LineKind {
		Text,
		Quote,
		Code,
		Notice,
		Rule,
	};

	struct Colors {
		COLORREF background = RGB(255, 255, 255);
		COLORREF codeBackground = RGB(245, 245, 245);
		COLORREF border = RGB(190, 190, 190);
		COLORREF primaryText = RGB(32, 32, 32);
		COLORREF secondaryText = RGB(96, 96, 96);
		COLORREF link = RGB(0, 102, 204);
	};

	struct RenderLine {
		std::wstring text;
		std::vector<InlineSpan> inlineSpans;
		int left = 0;
		int top = 0;
		int height = 0;
		FontKind font = FontKind::Body;
		LineKind kind = LineKind::Text;
	};

	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

	void RebuildFonts();
	void DeleteFonts() noexcept;
	void RebuildLayout();
	void UpdateScrollBar();
	void ScrollTo(int position);
	void ScrollBy(int delta);
	void Paint(HDC dc, const RECT& paintRect) const;
	void DrawLine(HDC dc, const RenderLine& line, int top) const;
	void AppendWrappedText(HDC dc, const Block& block, FontKind font, LineKind kind,
		int left, int availableWidth, int* top, int continuationLeft = -1,
		int continuationWidth = 0);
	void RebuildPaintResources();
	void DeletePaintResources() noexcept;

	[[nodiscard]] HFONT GetFont(FontKind kind, bool link = false) const noexcept;
	[[nodiscard]] int GetLineHeight(HDC dc, FontKind kind) const;
	[[nodiscard]] int ScaleDip(int dip) const noexcept;

	HWND m_hWnd = nullptr;
	Document m_document;
	LOGFONT m_editorFont{};
	unsigned int m_editorFontDpi = 96;
	unsigned int m_dpi = 96;
	bool m_hasEditorFont = false;
	bool m_sourceTruncated = false;
	Colors m_colors;
	HFONT m_bodyFont = nullptr;
	HFONT m_bodyLinkFont = nullptr;
	std::array<HFONT, 6> m_headingFonts{};
	std::array<HFONT, 6> m_headingLinkFonts{};
	HFONT m_codeFont = nullptr;
	HBRUSH m_backgroundBrush = nullptr;
	HBRUSH m_codeBackgroundBrush = nullptr;
	HBRUSH m_quoteBrush = nullptr;
	HBRUSH m_noticeBrush = nullptr;
	HPEN m_rulePen = nullptr;
	std::vector<RenderLine> m_lines;
	int m_contentHeight = 0;
	int m_scrollY = 0;
	int m_maxScroll = 0;
};

} // namespace markdown
