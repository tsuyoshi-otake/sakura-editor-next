/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "MarkdownCodeHighlighter.h"
#include "MarkdownParser.h"
#include "MarkdownInlineStyleRuns.h"
#include "MarkdownPreviewAsyncState.h"
#include "MarkdownPreviewScrollMap.h"
#include "MarkdownPreviewSurfaceAdapter.h"
#include "MarkdownPreviewWorkerRetirement.h"
#include "MermaidDiagram.h"
#include "workbench/rendering/FrameNativeSurfacePayloadAdapter.h"
#include "workbench/controls/COverlayScrollbar.h"
#include "workbench/rendering/CGdiBackBuffer.h"

namespace theme {
struct ThemePalette;
}

namespace markdown {

class IMarkdownRemoteImageFetcher;

//! A cached, GDI-only Markdown preview child window.
//!
//! Parsing and line wrapping happen before WM_PAINT.  Painting only consumes the
//! cached visual lines intersecting the native scroll position.
class CMarkdownPreviewWnd final {
public:
	explicit CMarkdownPreviewWnd(
		std::shared_ptr<IMarkdownRemoteImageFetcher> remoteImageFetcher = {},
		workbench::rendering::FrameSurfaceId surfaceId = kMarkdownPreviewSurfaceId) noexcept
		: m_remoteImageFetcher(std::move(remoteImageFetcher))
		, m_frameSurface(surfaceId)
		, m_nativeSurface(surfaceId)
	{
	}
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
	//! Moves the preview. Transient layouts preserve the cached line layout and
	//! defer the expensive width-dependent reflow until the committed call.
	void Layout(const RECT& bounds, unsigned int dpi, bool transient = false);
	void Show(bool visible) noexcept;
	//! Changes the logical host without exposing an HWND identity to the caller.
	[[nodiscard]] workbench::rendering::FrameSurfaceAdapterResult SetFrameHost(
		std::string_view hostId) noexcept;
	using NativeSurfaceSink = workbench::rendering::FrameNativeSurfacePayloadSink;
	using NativeSurfaceTarget = workbench::rendering::FrameNativeSurfacePayloadTarget;
	using NativeSurfaceFrame = workbench::rendering::FrameNativeSurfaceFrame;
	using NativeSurfaceResult = workbench::rendering::FrameNativeSurfacePayloadResult;
	//! Installs non-blocking register/update/close/submit hooks for this
	//! physical preview surface. Runtime/GPU work remains outside the UI paint.
	void SetNativeSurfaceSink(NativeSurfaceSink sink) noexcept;
	[[nodiscard]] bool SetNativeSurfaceTarget(const NativeSurfaceTarget& target) noexcept;
	[[nodiscard]] bool SetNativeSurfaceVisible(bool visible) noexcept;
	[[nodiscard]] const std::optional<NativeSurfaceTarget>& NativeSurfaceTargetSnapshot() const noexcept
	{
		return m_nativeSurfaceTarget;
	}
	void ClearNativeSurfaceTarget() noexcept;
	void SyncNativeSurfaceSize(std::uint32_t width, std::uint32_t height) noexcept;
	//! Publishes the latest captured payload after the enclosing GDI boundary.
	[[nodiscard]] NativeSurfaceResult PublishNativeSurface() noexcept;
	//! Advances the device domain and fences all work from the previous device.
	[[nodiscard]] workbench::rendering::FrameSurfaceAdapterResult NotifyFrameDeviceEpoch(
		std::uint64_t deviceEpoch) noexcept;
	//! Commits this preview after the enclosing GDI frame has actually flushed.
	//! No paint, wait, or flush is performed by this method.
	[[nodiscard]] std::optional<workbench::rendering::FrameSurfaceAdapterSnapshot>
		CommitGdiFrame() noexcept;
	[[nodiscard]] workbench::rendering::FrameSurfaceAdapterSnapshot FrameSurfaceSnapshot() const noexcept
	{
		return m_frameSurface.Snapshot();
	}
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
		//! Row shading behind a table row, sized by `left`/`width`.
		TableFill,
		//! One horizontal grid rule, sized by `left`/`width`.
		TableBorderH,
		//! One vertical grid rule at `left`, as tall as the line.
		TableBorderV,
		//! A laid-out Mermaid flowchart, sized by `width`/`height`.
		Diagram,
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
		std::size_t diagramIndex = std::numeric_limits<std::size_t>::max();
		int width = 0;
		std::size_t sourceLine = 0;
		std::vector<CodeHighlightToken> codeTokens;
	};

	struct CachedImage {
		std::wstring path;
		std::wstring allowedRoot;
		std::shared_ptr<void> bitmap;
		int width = 0;
		int height = 0;

		[[nodiscard]] HBITMAP Handle() const noexcept
		{
			return reinterpret_cast<HBITMAP>(bitmap.get());
		}

		static std::shared_ptr<void> Adopt(HBITMAP handle)
		{
			return std::shared_ptr<void>(handle, [](void* value) noexcept {
				if (value != nullptr) ::DeleteObject(reinterpret_cast<HGDIOBJ>(value));
			});
		}
	};

	struct WrappedTextBuildState {
		// Stable document blocks are observed by view so beginning a 2 MiB
		// paragraph cannot monopolize the UI thread merely by copying it. Temporary
		// synthetic blocks opt into ownedText before the first quantum runs.
		std::wstring ownedText;
		std::wstring_view text;
		std::wstring marker;
		std::wstring_view body;
		bool segmented = false;
		std::vector<InlineStyleRun> normalizedRuns;
		const std::vector<InlineStyleRun>* preparedRuns = nullptr;
		FontKind font = FontKind::Body;
		LineKind kind = LineKind::Text;
		int left = 0;
		int availableWidth = 1;
		int continuationLeft = -1;
		int continuationWidth = 0;
		std::size_t start = 0;
		std::size_t nextForcedBreak = std::wstring::npos;
		std::size_t sourceLine = 0;
		std::size_t codeSourceOffset = 0;
	};

	//! UI-thread-owned immutable-layout transaction.  The currently published
	//! vectors remain paintable while this staging generation advances through
	//! short message-queue slices.
	struct LayoutBuildState {
		std::uint64_t generation = 0;
		int clientWidth = 0;
		int top = 0;
		std::size_t nextBlock = 0;
		std::optional<PreviewScrollAnchor> scrollAnchor;
		std::vector<RenderLine> lines;
		std::vector<CachedImage> images;
		std::vector<mermaid::Diagram> diagrams;
		std::size_t decodedImagePixels = 0;
		std::optional<WrappedTextBuildState> wrappedText;
		std::size_t blockTextOffset = 0;
		std::size_t blockSourceLineOffset = 0;
		unsigned int blockStage = 0;
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
		std::vector<std::vector<InlineStyleRun>> inlineStyleRuns;
		std::vector<CachedImage> decodedImages;
		std::size_t decodedImagePixels = 0;
		bool truncated = false;
		bool failed = false;
	};

	//! Worker-owned state. It outlives the native window when cancellation is
	//! slow, so the UI-side preview object never has to wait for parsing to stop.
	struct WorkerState {
		std::mutex mutex;
		std::condition_variable condition;
		MarkdownPreviewAsyncState asyncState;
		std::optional<PreviewWorkItem> pendingWork;
		std::optional<PreviewWorkCompletion> completedWork;
		bool completionWakePosted = false;
		HWND completionTarget = nullptr;
		std::uint64_t lifetimeEpoch = 1;
		std::shared_ptr<IMarkdownRemoteImageFetcher> remoteImageFetcher;
	};

	static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
	LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

	void RebuildFonts();
	void DeleteFonts() noexcept;
	void RebuildLayout();
	void ContinueLayoutBuild();
	void CommitLayoutBuild();
	void CancelLayoutBuild() noexcept;
	void ScheduleLayoutBuildContinuation() noexcept;
	void UpdateScrollBar();
	void UpdateOverlayScrollbar();
	void ScrollTo(int position, bool notifySource = false);
	void ScrollBy(int delta, bool notifySource = true);
	void NotifySourceLineForScroll();
	void Paint(HDC dc, const RECT& paintRect);
	void DrawLine(HDC dc, const RenderLine& line, int top) const;
	void AppendWrappedText(HDC dc, const Block& block, FontKind font, LineKind kind,
		int left, int availableWidth, int* top, int continuationLeft = -1,
		int continuationWidth = 0, const CodeHighlightResult* codeHighlight = nullptr,
		std::size_t codeSourceOffset = 0);
	void BeginWrappedText(LayoutBuildState& build, const Block& block, FontKind font,
		LineKind kind, int left, int availableWidth, int continuationLeft = -1,
		int continuationWidth = 0, std::size_t codeSourceOffset = 0,
		bool retainText = false,
		const std::vector<InlineStyleRun>* preparedRuns = nullptr);
	void BeginWrappedTextView(LayoutBuildState& build, std::wstring_view text,
		std::size_t sourceLine, FontKind font, LineKind kind, int left,
		int availableWidth, std::size_t codeSourceOffset = 0);
	[[nodiscard]] bool ContinueWrappedText(HDC dc, LayoutBuildState& build,
		const CodeHighlightResult* codeHighlight,
		std::chrono::steady_clock::time_point deadline,
		std::size_t* remainingLineBudget);
	/*!
		@brief Lays a GFM table out as a real grid

		Cells are measured, columns are given widths, cell text wraps inside its
		own column, and the grid rules and row shading are emitted as their own
		render lines. The column-width policy follows comfy-table's dynamic
		arrangement: every column that fits its content keeps its natural width,
		and only the columns too wide for the remaining space share what is left.
	*/
	void AppendTable(HDC dc, const Block& block, int left, int availableWidth, int* top);
	/*!
		@brief Lays out a Mermaid block natively

		@retval false	The block is outside the supported flowchart subset, so
						the caller must keep its notice-plus-literal-source path
	*/
	[[nodiscard]] bool AppendMermaidDiagram(HDC dc, const Block& block, int left,
		int availableWidth, int* top);
	void DrawDiagram(HDC dc, const mermaid::Diagram& diagram, int left, int top) const;
	//! Measured width of one already-laid-out line, honouring its style runs.
	[[nodiscard]] int MeasureRenderLine(HDC dc, const RenderLine& line) const;
	void RebuildPaintResources();
	void DeletePaintResources() noexcept;
	void DeleteImages() noexcept;
	static void DeleteCachedImages(std::vector<CachedImage>& images) noexcept;
	static void WorkerMain(std::shared_ptr<WorkerState> state, std::stop_token stopToken) noexcept;
	void StopWorker() noexcept;
	void CommitCompletedWork(WPARAM completionState, LPARAM completionEpoch);
	[[nodiscard]] std::size_t GetOrLoadImage(const ResourceReference& resource);

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
	//! Grid rules for GFM tables, in the theme's border colour.
	HBRUSH m_borderBrush = nullptr;
	HPEN m_rulePen = nullptr;
	//! Diagram strokes, one per Mermaid link style.
	HPEN m_diagramPen = nullptr;
	HPEN m_diagramDottedPen = nullptr;
	HPEN m_diagramThickPen = nullptr;
	HBRUSH m_diagramArrowBrush = nullptr;
	std::vector<RenderLine> m_lines;
	std::vector<CachedImage> m_images;
	//! Worker-prepared image generation used to seed a new immutable layout.
	std::vector<CachedImage> m_preparedImages;
	std::size_t m_preparedImagePixels = 0;
	bool m_layoutImagesPrepared = false;
	//! Placed geometry for every natively rendered Mermaid block in this layout.
	std::vector<mermaid::Diagram> m_diagrams;
	std::size_t m_decodedImagePixels = 0;
	std::optional<LayoutBuildState> m_layoutBuild;
	std::uint64_t m_layoutGeneration = 0;
	bool m_layoutContinuationPosted = false;
	int m_contentHeight = 0;
	//! Client width used to build m_lines. Height changes never invalidate wrapping.
	int m_layoutWidth = -1;
	int m_scrollY = 0;
	int m_maxScroll = 0;
	bool m_transientLayout = false;
	bool m_suppressSizeLayout = false;
	bool m_layoutDirty = false;
	//! Old image handles remain valid during a transient document swap and are released at commit.
	bool m_imagesDirty = false;
	bool m_fontResourcesDirty = false;
	std::optional<std::size_t> m_revealSourceLine;
	std::optional<std::size_t> m_lastNotifiedSourceLine;
	std::function<void(std::size_t)> m_sourceLineCallback;
	std::optional<PreviewWorkCompletion> m_deferredCompletion;
	std::shared_ptr<IMarkdownRemoteImageFetcher> m_remoteImageFetcher;
	std::shared_ptr<WorkerState> m_workerState;
	std::jthread m_worker;
	std::optional<MarkdownPreviewWorkerRetirement::Reservation> m_workerRetirement;
	std::vector<std::optional<CodeHighlightResult>> m_codeHighlights;
	//! Width-independent inline normalization prepared by the parser worker.
	std::vector<std::vector<InlineStyleRun>> m_inlineStyleRuns;
	//! Pure logical identity/epoch fence for this native Markdown viewport.
	MarkdownPreviewSurfaceAdapter m_frameSurface;
	workbench::rendering::FrameNativeSurfacePayloadAdapter m_nativeSurface;
	std::optional<NativeSurfaceTarget> m_nativeSurfaceTarget;
	//! The shared VS Code-style overlay bar presents this window's explicit pixel model.
	workbench::controls::COverlayScrollbar m_overlayScrollbar;
	workbench::controls::OverlayScrollbarColors m_overlayColors;
	mutable workbench::rendering::CGdiBackBuffer m_backBuffer;
};

} // namespace markdown
