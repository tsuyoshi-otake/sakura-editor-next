/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <windows.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "MarkdownCodeHighlighter.h"
#include "MarkdownParser.h"
#include "MarkdownInlineStyleRuns.h"
#include "MarkdownPreviewAsyncState.h"
#include "MarkdownPreviewScrollMap.h"

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
	//! Queues one immutable UI-thread snapshot. Parsing and highlighting run only
	//! on the persistent worker; a custom window message commits the latest key.
	[[nodiscard]] bool QueueDocument(std::wstring source, ParseOptions options,
		bool truncated, PreviewRenderKey key);
	void SetSourceTruncated(bool truncated);
	void SetPalette(const theme::ThemePalette& palette);
	void SetEditorFont(const LOGFONT& font, unsigned int dpi);
	void Layout(const RECT& bounds, unsigned int dpi);
	void Show(bool visible) const noexcept;
	void RevealSourceLine(std::size_t sourceLine);
	void SetSourceLineCallback(std::function<void(std::size_t)> callback);

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
		Table,
		TableHeader,
		Image,
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
		std::vector<InlineStyleRun> styleRuns;
		int left = 0;
		int top = 0;
		int height = 0;
		FontKind font = FontKind::Body;
		LineKind kind = LineKind::Text;
		std::size_t imageIndex = std::numeric_limits<std::size_t>::max();
		int width = 0;
		std::size_t sourceLine = 0;
		std::vector<CodeHighlightToken> codeTokens;
	};

	struct CachedImage {
		std::wstring path;
		std::wstring allowedRoot;
		HBITMAP bitmap = nullptr;
		int width = 0;
		int height = 0;
	};

	struct PreviewWorkItem {
		PreviewRenderKey key;
		std::wstring source;
		ParseOptions options;
		bool truncated = false;
	};

	struct PreviewWorkCompletion {
		PreviewRenderKey key;
		Document document;
		std::vector<std::optional<CodeHighlightResult>> codeHighlights;
		bool truncated = false;
		bool failed = false;
	};

	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

	void RebuildFonts();
	void DeleteFonts() noexcept;
	void RebuildLayout();
	void UpdateScrollBar();
	void ScrollTo(int position, bool notifySource = false);
	void ScrollBy(int delta, bool notifySource = true);
	void NotifySourceLineForScroll();
	void Paint(HDC dc, const RECT& paintRect) const;
	void DrawLine(HDC dc, const RenderLine& line, int top) const;
	void AppendWrappedText(HDC dc, const Block& block, FontKind font, LineKind kind,
		int left, int availableWidth, int* top, int continuationLeft = -1,
		int continuationWidth = 0, const CodeHighlightResult* codeHighlight = nullptr,
		std::size_t codeSourceOffset = 0);
	void RebuildPaintResources();
	void DeletePaintResources() noexcept;
	void DeleteImages() noexcept;
	void WorkerMain(std::stop_token stopToken) noexcept;
	void StopWorker() noexcept;
	void CommitCompletedWork();
	[[nodiscard]] std::size_t GetOrLoadImage(const ResourceReference& resource) noexcept;

	[[nodiscard]] HFONT GetFont(FontKind kind, unsigned int style = 0, bool inlineCode = false) const noexcept;
	[[nodiscard]] int GetLineHeight(HDC dc, FontKind kind) const;
	[[nodiscard]] int ScaleDip(int dip) const noexcept;

	HWND m_hWnd = nullptr;
	Document m_document;
	LOGFONT m_editorFont{};
	unsigned int m_editorFontDpi = 96;
	unsigned int m_dpi = 96;
	bool m_hasEditorFont = false;
	bool m_sourceTruncated = false;
	bool m_renderFailed = false;
	Colors m_colors;
	std::array<HFONT, 16> m_bodyFonts{};
	std::array<std::array<HFONT, 16>, 6> m_headingFonts{};
	std::array<HFONT, 16> m_codeFonts{};
	HBRUSH m_backgroundBrush = nullptr;
	HBRUSH m_codeBackgroundBrush = nullptr;
	HBRUSH m_quoteBrush = nullptr;
	HBRUSH m_noticeBrush = nullptr;
	HPEN m_rulePen = nullptr;
	std::vector<RenderLine> m_lines;
	std::vector<CachedImage> m_images;
	std::size_t m_decodedImagePixels = 0;
	int m_contentHeight = 0;
	int m_scrollY = 0;
	int m_maxScroll = 0;
	std::optional<std::size_t> m_revealSourceLine;
	std::optional<std::size_t> m_lastNotifiedSourceLine;
	std::function<void(std::size_t)> m_sourceLineCallback;
	std::mutex m_workerMutex;
	std::condition_variable m_workerCondition;
	MarkdownPreviewAsyncState m_asyncState;
	std::optional<PreviewWorkItem> m_pendingWork;
	std::optional<PreviewWorkCompletion> m_completedWork;
	std::jthread m_worker;
	std::vector<std::optional<CodeHighlightResult>> m_codeHighlights;
};

} // namespace markdown
