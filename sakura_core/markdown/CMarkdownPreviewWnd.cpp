/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "CMarkdownPreviewWnd.h"

#include <wincodec.h>

#include "cxx/com_pointer.hpp"
#include "theme/CThemeService.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <utility>


namespace markdown {
namespace {

constexpr wchar_t kPreviewWindowClass[] = L"SakuraMarkdownPreview";
constexpr int kDefaultDpi = 96;
constexpr unsigned int kStyleStrong = 1U;
constexpr unsigned int kStyleEmphasis = 2U;
constexpr unsigned int kStyleLink = 4U;
constexpr unsigned int kStyleStrikethrough = 8U;
constexpr std::size_t kInvalidImage = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaximumCachedImages = 16;
constexpr std::uint64_t kMaximumEncodedImageBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumSourceImagePixels = 32ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumDecodedImagePixels = 16ULL * 1024ULL * 1024ULL;
constexpr UINT kMaximumDecodedImageEdge = 1600;
constexpr UINT kCommitPreviewWorkMessage = WM_APP + 1;

/*!
	@name VS Code preview typography

	Taken from `extensions/markdown-language-features/media/markdown.css` at the
	commit pinned in `upstream-parity-manifest.json`, and from the defaults of
	`markdown.preview.fontSize` (14) and `markdown.preview.lineHeight` (1.6).
	The values are CSS pixels, which are device-independent pixels, so they scale
	through ScaleDip() exactly as the rest of the layout does.

	The prose face is the Windows end of upstream's font stack: the stack starts
	with the Apple faces, then "Segoe WPC" and "Segoe UI". "Segoe UI Variable" is
	a different, Windows 11-only face that upstream never selects.
*/
///@{
constexpr wchar_t kPreviewProseFace[] = L"Segoe UI";
constexpr wchar_t kPreviewCodeFallbackFace[] = L"Consolas";
constexpr int kPreviewFontSizePx = 14;
constexpr int kPreviewLineHeightPx = 22;      //!< 14px * 1.6, matching the CSS fallback
constexpr int kPreviewCodeLineHeightPx = 19;  //!< 1.357em of 14px
constexpr int kPreviewHeadingLineHeightPermille = 1250;
constexpr int kPreviewHeadingWeight = FW_SEMIBOLD;  //!< CSS font-weight: 600
//! h1..h6 font sizes as `em` per-mille.
constexpr int kPreviewHeadingPermille[] = { 2000, 1500, 1250, 1000, 875, 850 };
//! Block separation: the browser default `p { margin-bottom: 1em }` upstream keeps.
constexpr int kPreviewBlockGapPx = 14;
//! markdown.css: h1..h6 { margin-top: 24px; margin-bottom: 16px }
/*!
	@name GFM table geometry

	`markdown.css` gives `td`/`th` `padding: 6px 13px` and a `1px solid` border on
	every cell, and shades every second body row. The values are CSS pixels, so
	they scale through ScaleDip() like the rest of the layout.
*/
///@{
constexpr int kPreviewTableCellPadXPx = 13;
constexpr int kPreviewTableCellPadYPx = 6;
constexpr int kPreviewTableBorderPx = 1;
//! A column is never squeezed below this, so a narrow pane still shows structure.
constexpr int kPreviewTableMinimumColumnPx = 32;
///@}
constexpr int kPreviewHeadingMarginTopPx = 24;
constexpr int kPreviewHeadingMarginBottomPx = 16;
///@}

/*!
	@brief Creates one preview face at `permille` of the base size

	VS Code's markdown.css sizes headings in `em` (2, 1.5, 1.25, 1, 0.875, 0.85).
	Per-mille rather than per-cent keeps 0.875em exact instead of rounding it to
	88%, which at 14px is a visible half-pixel of leading across a document.
*/
[[nodiscard]] HFONT CreatePreviewFont(const LOGFONT& base, int permille, bool bold,
	bool italic, bool underline, bool strikethrough) noexcept
{
	LOGFONT font = base;
	if (font.lfHeight == 0) {
		font.lfHeight = -16;
	}
	const auto sign = font.lfHeight < 0 ? -1 : 1;
	const LONG absoluteHeight = std::max<LONG>(1, font.lfHeight * sign);
	font.lfHeight = sign * std::max<LONG>(1, ::MulDiv(absoluteHeight, permille, 1000));
	font.lfWeight = bold ? kPreviewHeadingWeight : (font.lfWeight == 0 ? FW_NORMAL : font.lfWeight);
	font.lfItalic = italic ? TRUE : FALSE;
	font.lfUnderline = underline ? TRUE : FALSE;
	font.lfStrikeOut = strikethrough ? TRUE : FALSE;
	return ::CreateFontIndirectW(&font);
}

[[nodiscard]] bool IsWrapSpace(wchar_t value) noexcept
{
	return value == L' ' || value == L'\t';
}

[[nodiscard]] std::vector<CodeHighlightToken> ClipCodeHighlightTokens(
	const std::vector<CodeHighlightToken>& tokens, std::size_t lineStart, std::size_t lineLength)
{
	std::vector<CodeHighlightToken> clipped;
	if (tokens.empty() || lineLength == 0) return clipped;
	const auto maximumLength = std::numeric_limits<std::size_t>::max() - lineStart;
	const auto lineEnd = lineStart + std::min(lineLength, maximumLength);
	const auto first = std::lower_bound(tokens.begin(), tokens.end(), lineStart,
		[](const CodeHighlightToken& token, std::size_t start) {
			return token.start + std::min(token.length,
				std::numeric_limits<std::size_t>::max() - token.start) <= start;
		});
	for (auto iterator = first; iterator != tokens.end() && iterator->start < lineEnd; ++iterator) {
		const auto tokenEnd = iterator->start + std::min(iterator->length,
			std::numeric_limits<std::size_t>::max() - iterator->start);
		const auto start = std::max(lineStart, iterator->start);
		const auto end = std::min(lineEnd, tokenEnd);
		if (start < end) clipped.push_back({ iterator->kind, start - lineStart, end - start });
	}
	return clipped;
}

class ScopedHandle final {
public:
	explicit ScopedHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : m_value(value) {}
	~ScopedHandle()
	{
		if (m_value != INVALID_HANDLE_VALUE && m_value != nullptr) ::CloseHandle(m_value);
	}
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	[[nodiscard]] HANDLE Get() const noexcept { return m_value; }
	[[nodiscard]] bool IsValid() const noexcept { return m_value != INVALID_HANDLE_VALUE && m_value != nullptr; }

private:
	HANDLE m_value;
};

[[nodiscard]] std::wstring GetFinalDosPath(HANDLE handle)
{
	const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
	const auto required = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
	if (required == 0) return {};
	std::wstring path(required, L'\0');
	const auto written = ::GetFinalPathNameByHandleW(handle, path.data(), required, flags);
	if (written == 0 || written >= required) return {};
	path.resize(written);
	if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
	else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
	std::replace(path.begin(), path.end(), L'/', L'\\');
	while (path.size() > 3 && path.back() == L'\\') path.pop_back();
	return path;
}

[[nodiscard]] bool IsFinalPathInside(std::wstring_view root, std::wstring_view candidate) noexcept
{
	if (root.empty() || candidate.size() < root.size()) return false;
	if (::CompareStringOrdinal(candidate.data(), static_cast<int>(root.size()), root.data(),
		static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) return false;
	return candidate.size() == root.size() || root.back() == L'\\' || candidate[root.size()] == L'\\';
}

struct DecodedBitmap {
	HBITMAP bitmap = nullptr;
	int width = 0;
	int height = 0;
};

//! Decodes only after the opened file and allowed-root handles prove the final
//! target remains inside the parser-approved root. The decoder consumes the
//! verified handle, avoiding a path re-open between validation and decode.
[[nodiscard]] DecodedBitmap LoadVerifiedRaster(const ResourceReference& resource,
	std::size_t remainingPixels) noexcept
{
	DecodedBitmap result;
	if (resource.disposition != ResourceDisposition::ResolvedLocal
		|| resource.resolvedPath.empty() || resource.allowedRoot.empty() || remainingPixels == 0) return result;
	try {
		ScopedHandle root(::CreateFileW(resource.allowedRoot.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		ScopedHandle file(::CreateFileW(resource.resolvedPath.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
		if (!root.IsValid() || !file.IsValid()) return result;
		LARGE_INTEGER encodedSize{};
		BY_HANDLE_FILE_INFORMATION fileInfo{};
		if (!::GetFileSizeEx(file.Get(), &encodedSize) || encodedSize.QuadPart <= 0
			|| static_cast<std::uint64_t>(encodedSize.QuadPart) > kMaximumEncodedImageBytes
			|| !::GetFileInformationByHandle(file.Get(), &fileInfo)
			|| (fileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return result;
		const auto finalRoot = GetFinalDosPath(root.Get());
		const auto finalFile = GetFinalDosPath(file.Get());
		if (!IsFinalPathInside(finalRoot, finalFile)) return result;

		cxx::com_pointer<IWICImagingFactory> factory;
		if (FAILED(factory.CreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER))) return result;
		cxx::com_pointer<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(file.Get()), nullptr,
			WICDecodeMetadataCacheOnLoad, &decoder))) return result;
		cxx::com_pointer<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return result;
		UINT width = 0;
		UINT height = 0;
		if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0
			|| static_cast<std::uint64_t>(width) * height > kMaximumSourceImagePixels) return result;

		UINT scaledWidth = width;
		UINT scaledHeight = height;
		if (scaledWidth > kMaximumDecodedImageEdge || scaledHeight > kMaximumDecodedImageEdge) {
			if (scaledWidth >= scaledHeight) {
				scaledHeight = (std::max)(1U, static_cast<UINT>(
					(static_cast<std::uint64_t>(scaledHeight) * kMaximumDecodedImageEdge) / scaledWidth));
				scaledWidth = kMaximumDecodedImageEdge;
			} else {
				scaledWidth = (std::max)(1U, static_cast<UINT>(
					(static_cast<std::uint64_t>(scaledWidth) * kMaximumDecodedImageEdge) / scaledHeight));
				scaledHeight = kMaximumDecodedImageEdge;
			}
		}
		const auto decodedPixels = static_cast<std::size_t>(scaledWidth) * scaledHeight;
		if (decodedPixels > remainingPixels) return result;

		cxx::com_pointer<IWICBitmapScaler> scaler;
		IWICBitmapSource* source = frame;
		if (scaledWidth != width || scaledHeight != height) {
			if (FAILED(factory->CreateBitmapScaler(&scaler))
				|| FAILED(scaler->Initialize(frame, scaledWidth, scaledHeight,
					WICBitmapInterpolationModeHighQualityCubic))) return result;
			source = scaler;
		}
		cxx::com_pointer<IWICFormatConverter> converter;
		if (FAILED(factory->CreateFormatConverter(&converter))
			|| FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA,
				WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) return result;

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(scaledWidth);
		bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(scaledHeight);
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		const auto bitmap = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) ::DeleteObject(bitmap);
			return result;
		}
		const UINT stride = scaledWidth * 4;
		const auto byteCount = static_cast<std::size_t>(stride) * scaledHeight;
		if (byteCount > std::numeric_limits<UINT>::max()
			|| FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(byteCount), static_cast<BYTE*>(bits)))) {
			::DeleteObject(bitmap);
			return result;
		}
		result.bitmap = bitmap;
		result.width = static_cast<int>(scaledWidth);
		result.height = static_cast<int>(scaledHeight);
	}
	catch (...) {
		return {};
	}
	return result;
}

} // namespace

CMarkdownPreviewWnd::~CMarkdownPreviewWnd()
{
	Close();
	DeleteFonts();
	DeletePaintResources();
	DeleteImages();
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
	// The window keeps its SB_VERT scroll model; the overlay hides the platform
	// bar and paints the same VS Code bar the Explorer tree uses.
	(void)m_overlayScrollbar.Create(parent, m_hWnd,
		[this](int position) { ScrollTo(position, true); },
		workbench::controls::OverlayScrollbarSource::TargetWindowBar);
	RebuildFonts();
	RebuildPaintResources();
	RebuildLayout();
	try {
		m_worker = std::jthread([this](std::stop_token stopToken) { WorkerMain(stopToken); });
	}
	catch (...) {
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return false;
	}
	return true;
}

void CMarkdownPreviewWnd::Close() noexcept
{
	m_sourceLineCallback = {};
	m_overlayScrollbar.Detach();
	StopWorker();
	if (m_hWnd != nullptr) {
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
}

void CMarkdownPreviewWnd::StopWorker() noexcept
{
	{
		std::lock_guard lock(m_workerMutex);
		m_asyncState.Close();
		m_pendingWork.reset();
		m_completedWork.reset();
	}
	if (m_worker.joinable()) {
		m_worker.request_stop();
		m_workerCondition.notify_all();
		m_worker.join();
	}
}

void CMarkdownPreviewWnd::SetDocument(Document document)
{
	DeleteImages();
	m_document = std::move(document);
	m_codeHighlights.clear();
	m_renderFailed = false;
	RebuildLayout();
}

bool CMarkdownPreviewWnd::QueueDocument(std::wstring source, ParseOptions options,
	bool truncated, PreviewRenderKey key)
{
	{
		std::lock_guard lock(m_workerMutex);
		if (!m_worker.joinable()
			|| m_asyncState.Queue(key) == PreviewQueueAction::RejectedClosed) return false;
		// Replacing this optional is the bounded latest-pending-one contract.
		m_pendingWork = PreviewWorkItem{
			key, std::move(source), std::move(options), truncated };
	}
	m_workerCondition.notify_one();
	return true;
}

void CMarkdownPreviewWnd::WorkerMain(std::stop_token stopToken) noexcept
{
	while (!stopToken.stop_requested()) {
		PreviewWorkItem work;
		{
			std::unique_lock lock(m_workerMutex);
			m_workerCondition.wait(lock, [this, stopToken] {
				return stopToken.stop_requested() || m_pendingWork.has_value();
			});
			if (stopToken.stop_requested()) return;
			const auto key = m_asyncState.TakeNext();
			if (!key || !m_pendingWork || m_pendingWork->key != *key) continue;
			work = std::move(*m_pendingWork);
			m_pendingWork.reset();
		}

		PreviewWorkCompletion completion;
		completion.key = work.key;
		completion.truncated = work.truncated;
		try {
			completion.document = ParseMarkdown(work.source, work.options);
			completion.codeHighlights.resize(completion.document.blocks.size());
			for (std::size_t index = 0; index < completion.document.blocks.size(); ++index) {
				const auto& block = completion.document.blocks[index];
				if (block.kind == BlockKind::CodeBlock) {
					completion.codeHighlights[index] = HighlightMarkdownCode(block.language, block.text);
				}
			}
		}
		catch (...) {
			completion.failed = true;
			completion.document = {};
			completion.codeHighlights.clear();
		}

		HWND target = nullptr;
		{
			std::lock_guard lock(m_workerMutex);
			const auto action = m_asyncState.Complete(work.key, !completion.failed);
			if (action == PreviewCompletionAction::DiscardClosed) return;
			if (action == PreviewCompletionAction::DiscardStale) continue;
			m_completedWork = std::move(completion);
			target = m_hWnd;
		}
		if (target == nullptr || !::PostMessageW(target, kCommitPreviewWorkMessage, 0, 0)) {
			std::lock_guard lock(m_workerMutex);
			if (m_completedWork && m_completedWork->key == work.key) {
				m_completedWork.reset();
				m_asyncState.MarkDeliveryFailed(work.key);
			}
		}
	}
}

void CMarkdownPreviewWnd::CommitCompletedWork()
{
	std::optional<PreviewWorkCompletion> completion;
	{
		std::lock_guard lock(m_workerMutex);
		if (!m_completedWork || !m_asyncState.IsCurrent(m_completedWork->key)) {
			m_completedWork.reset();
			return;
		}
		completion = std::move(m_completedWork);
		m_completedWork.reset();
		m_asyncState.MarkDelivered(completion->key);
	}
	DeleteImages();
	m_document = std::move(completion->document);
	m_codeHighlights = std::move(completion->codeHighlights);
	m_sourceTruncated = completion->truncated;
	m_renderFailed = completion->failed;
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
	m_overlayColors.background = m_colors.background;
	m_overlayColors.trackHover = palette.raised.ToColorRef();
	m_overlayColors.thumb = palette.border.ToColorRef();
	m_overlayColors.thumbHover = palette.secondaryText.ToColorRef();
	UpdateOverlayScrollbar();
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
		UpdateScrollBar();
	}
}

void CMarkdownPreviewWnd::Show(bool visible) noexcept
{
	if (m_hWnd != nullptr) {
		::ShowWindow(m_hWnd, visible ? SW_SHOWNA : SW_HIDE);
		// The overlay is a sibling window, so it does not inherit the hide.
		UpdateOverlayScrollbar();
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
	case kCommitPreviewWorkMessage:
		CommitCompletedWork();
		return 0;

	case WM_NCDESTROY: {
		StopWorker();
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
		ScrollTo(position, true);
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
		case VK_HOME: ScrollTo(0, true); return 0;
		case VK_END: ScrollTo(m_maxScroll, true); return 0;
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
	// The preview's own size comes from markdown.preview.fontSize, not from the
	// editor's document font. Upstream applies the editor font family to code
	// spans only (--vscode-editor-font-family), so that is all m_editorFont is
	// consulted for here.
	LOGFONT font{};
	font.lfHeight = -ScaleDip(kPreviewFontSizePx);
	font.lfWeight = FW_NORMAL;
	font.lfCharSet = DEFAULT_CHARSET;
	// VS Code renders through DirectWrite with subpixel antialiasing. CLEARTYPE
	// is GDI's equivalent, and must be set on every face the preview creates:
	// a face left at DEFAULT_QUALITY falls back to greyscale antialiasing and
	// reads as a visibly different weight beside the editor.
	font.lfQuality = CLEARTYPE_QUALITY;

	LOGFONT proseFont = font;
	(void)wcscpy_s(proseFont.lfFaceName, LF_FACESIZE, kPreviewProseFace);
	LOGFONT codeFont = font;
	codeFont.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
	if (m_hasEditorFont && m_editorFont.lfFaceName[0] != L'\0') {
		(void)wcscpy_s(codeFont.lfFaceName, LF_FACESIZE, m_editorFont.lfFaceName);
	} else {
		(void)wcscpy_s(codeFont.lfFaceName, LF_FACESIZE, kPreviewCodeFallbackFace);
	}

	const auto* const headingPercentages = kPreviewHeadingPermille;
	for (std::size_t style = 0; style < m_bodyFonts.size(); ++style) {
		const auto styleValue = static_cast<unsigned int>(style);
		const bool strong = (styleValue & kStyleStrong) != 0;
		const bool emphasis = (styleValue & kStyleEmphasis) != 0;
		const bool link = (styleValue & kStyleLink) != 0;
		const bool strikethrough = (styleValue & kStyleStrikethrough) != 0;
		m_bodyFonts[style] = CreatePreviewFont(proseFont, 1000, strong, emphasis, link, strikethrough);
		m_codeFonts[style] = CreatePreviewFont(codeFont, 1000, strong, emphasis, link, strikethrough);
		if (m_codeFonts[style] == nullptr) {
			LOGFONT fallback = codeFont;
			(void)wcscpy_s(fallback.lfFaceName, LF_FACESIZE, kPreviewCodeFallbackFace);
			m_codeFonts[style] = CreatePreviewFont(fallback, 1000, strong, emphasis, link, strikethrough);
		}
		for (std::size_t heading = 0; heading < m_headingFonts.size(); ++heading) {
			m_headingFonts[heading][style] = CreatePreviewFont(proseFont, headingPercentages[heading],
				true, emphasis, link, strikethrough);
		}
	}
}

void CMarkdownPreviewWnd::RevealSourceLine(std::size_t sourceLine)
{
	m_revealSourceLine = sourceLine;
	if (const auto top = PreviewTopForSourceLine(m_lines, sourceLine)) ScrollTo(*top, false);
}

void CMarkdownPreviewWnd::SetSourceLineCallback(std::function<void(std::size_t)> callback)
{
	m_sourceLineCallback = std::move(callback);
	m_lastNotifiedSourceLine.reset();
}

void CMarkdownPreviewWnd::DeleteFonts() noexcept
{
	auto deleteFont = [](HFONT& font) {
		if (font != nullptr) {
			::DeleteObject(font);
			font = nullptr;
		}
	};
	for (auto& font : m_headingFonts) {
		for (auto& variant : font) deleteFont(variant);
	}
	for (auto& font : m_bodyFonts) deleteFont(font);
	for (auto& font : m_codeFonts) deleteFont(font);
}

void CMarkdownPreviewWnd::RebuildPaintResources()
{
	DeletePaintResources();
	m_backgroundBrush = ::CreateSolidBrush(m_colors.background);
	m_codeBackgroundBrush = ::CreateSolidBrush(m_colors.codeBackground);
	m_quoteBrush = ::CreateSolidBrush(m_colors.border);
	m_noticeBrush = ::CreateSolidBrush(m_colors.codeBackground);
	m_borderBrush = ::CreateSolidBrush(m_colors.border);
	m_rulePen = ::CreatePen(PS_SOLID, std::max(1, ScaleDip(1)), m_colors.border);
	m_diagramPen = ::CreatePen(PS_SOLID, std::max(1, ScaleDip(1)), m_colors.secondaryText);
	m_diagramDottedPen = ::CreatePen(PS_DOT, 1, m_colors.secondaryText);
	m_diagramThickPen = ::CreatePen(PS_SOLID, std::max(2, ScaleDip(2)), m_colors.secondaryText);
	m_diagramArrowBrush = ::CreateSolidBrush(m_colors.secondaryText);
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
	deleteObject(m_borderBrush);
	deleteObject(m_rulePen);
	deleteObject(m_diagramPen);
	deleteObject(m_diagramDottedPen);
	deleteObject(m_diagramThickPen);
	deleteObject(m_diagramArrowBrush);
}

void CMarkdownPreviewWnd::DeleteImages() noexcept
{
	for (auto& image : m_images) {
		if (image.bitmap != nullptr) {
			::DeleteObject(image.bitmap);
			image.bitmap = nullptr;
		}
	}
	m_images.clear();
	m_decodedImagePixels = 0;
}

std::size_t CMarkdownPreviewWnd::GetOrLoadImage(const ResourceReference& resource) noexcept
{
	if (resource.disposition != ResourceDisposition::ResolvedLocal) return kInvalidImage;
	for (std::size_t index = 0; index < m_images.size(); ++index) {
		const auto& image = m_images[index];
		if (_wcsicmp(image.path.c_str(), resource.resolvedPath.c_str()) == 0
			&& _wcsicmp(image.allowedRoot.c_str(), resource.allowedRoot.c_str()) == 0) {
			return image.bitmap == nullptr ? kInvalidImage : index;
		}
	}
	if (m_images.size() >= kMaximumCachedImages || m_decodedImagePixels >= kMaximumDecodedImagePixels) {
		return kInvalidImage;
	}
	const auto decoded = LoadVerifiedRaster(resource, kMaximumDecodedImagePixels - m_decodedImagePixels);
	CachedImage cached;
	cached.path = resource.resolvedPath;
	cached.allowedRoot = resource.allowedRoot;
	cached.bitmap = decoded.bitmap;
	cached.width = decoded.width;
	cached.height = decoded.height;
	m_images.push_back(std::move(cached));
	if (decoded.bitmap == nullptr) return kInvalidImage;
	m_decodedImagePixels += static_cast<std::size_t>(decoded.width) * static_cast<std::size_t>(decoded.height);
	return m_images.size() - 1;
}

void CMarkdownPreviewWnd::RebuildLayout()
{
	if (m_hWnd == nullptr) {
		return;
	}
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	m_lines.clear();
	m_diagrams.clear();
	// markdown.css: body { padding: 0 26px; padding-top: 1em; }
	int top = ScaleDip(kPreviewFontSizePx);
	const int leftPadding = ScaleDip(26);
	const int rightPadding = ScaleDip(26);
	const int clientWidth = std::max(0L, client.right - client.left);

	const auto dc = ::GetDC(m_hWnd);
	if (dc != nullptr) {
		const auto lineGap = ScaleDip(kPreviewBlockGapPx);
		const auto appendLiteralBlock = [&](const Block& literalBlock,
			const CodeHighlightResult* codeHighlight) {
			std::size_t start = 0;
			std::size_t sourceLineOffset = 0;
			do {
				const auto end = literalBlock.text.find(L'\n', start);
				Block literalLine;
				literalLine.kind = literalBlock.kind;
				literalLine.text = literalBlock.text.substr(start,
					end == std::wstring::npos ? std::wstring::npos : end - start);
				literalLine.sourceLine = literalBlock.sourceLine + sourceLineOffset;
				AppendWrappedText(dc, literalLine, FontKind::Code, LineKind::Code, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top,
					-1, 0, codeHighlight, start);
				if (end == std::wstring::npos) break;
				start = end + 1;
				++sourceLineOffset;
			} while (start <= literalBlock.text.size());
		};
		for (std::size_t blockIndex = 0; blockIndex < m_document.blocks.size(); ++blockIndex) {
			const auto& block = m_document.blocks[blockIndex];
			const auto* codeHighlight = blockIndex < m_codeHighlights.size()
				&& m_codeHighlights[blockIndex].has_value()
				? &*m_codeHighlights[blockIndex] : nullptr;
			switch (block.kind) {
			case BlockKind::Heading: {
				const auto level = std::clamp(block.level, 1, 6);
				const auto font = static_cast<FontKind>(static_cast<int>(FontKind::Heading1) + level - 1);
				// CSS margins collapse, so the heading's 24px top margin replaces the
				// previous block's gap rather than adding to it. h1 has margin-top 0.
				if (blockIndex != 0 && level != 1) {
					top += std::max(0, ScaleDip(kPreviewHeadingMarginTopPx) - lineGap);
				}
				AppendWrappedText(dc, block, font, LineKind::Text, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				top += ScaleDip(kPreviewHeadingMarginBottomPx);
				break;
			}
			case BlockKind::Paragraph:
				AppendWrappedText(dc, block, FontKind::Body, LineKind::Text, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				top += lineGap;
				break;

			case BlockKind::BulletListItem:
			case BlockKind::OrderedListItem: {
				Block renderedListItem = block;
				switch (block.taskListState) {
				case TaskListState::NotTask:
					break;
				case TaskListState::Unchecked:
					renderedListItem.marker.append(L"\x2610 ");
					break;
				case TaskListState::Checked:
					renderedListItem.marker.append(L"\x2611 ");
					break;
				}
				const auto indent = leftPadding + ScaleDip(18) * std::max(0, block.level);
				const auto oldFont = static_cast<HFONT>(::SelectObject(dc, GetFont(FontKind::Body)));
				SIZE markerExtent{};
				(void)::GetTextExtentPoint32W(dc, renderedListItem.marker.data(),
					static_cast<int>(renderedListItem.marker.size()), &markerExtent);
				::SelectObject(dc, oldFont);
				const auto fullWidth = std::max(1, clientWidth - indent - rightPadding);
				const auto continuationLeft = indent + markerExtent.cx;
				AppendWrappedText(dc, renderedListItem, FontKind::Body, LineKind::Text, indent,
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
				appendLiteralBlock(block, codeHighlight);
				top += lineGap;
				break;
			}

			case BlockKind::Image: {
				const auto imageIndex = block.image.has_value() ? GetOrLoadImage(block.image->source) : kInvalidImage;
				if (imageIndex != kInvalidImage) {
					const auto& image = m_images[imageIndex];
					const int availableWidth = std::max(1, clientWidth - leftPadding - rightPadding);
					int displayWidth = std::min(image.width, availableWidth);
					int displayHeight = std::max(1, ::MulDiv(image.height, displayWidth, image.width));
					const int maximumHeight = std::max(1, ScaleDip(520));
					if (displayHeight > maximumHeight) {
						displayWidth = std::max(1, ::MulDiv(displayWidth, maximumHeight, displayHeight));
						displayHeight = maximumHeight;
					}
					RenderLine imageLine;
					imageLine.left = leftPadding;
					imageLine.top = top;
					imageLine.height = displayHeight;
					imageLine.kind = LineKind::Image;
					imageLine.imageIndex = imageIndex;
					imageLine.width = displayWidth;
					imageLine.sourceLine = block.sourceLine;
					m_lines.push_back(std::move(imageLine));
					top += displayHeight + lineGap;
				} else {
					Block fallback;
					fallback.sourceLine = block.sourceLine;
					const auto disposition = block.image.has_value()
						? block.image->source.disposition : ResourceDisposition::Invalid;
					fallback.text = disposition == ResourceDisposition::ExternalBlocked
						|| disposition == ResourceDisposition::UnsafeSchemeBlocked
						|| disposition == ResourceDisposition::OutsideAllowedRoots
						? L"Blocked image" : L"Image unavailable";
					if (block.image.has_value() && !block.image->altText.empty()) {
						fallback.text.append(L": ");
						fallback.text.append(block.image->altText);
					}
					AppendWrappedText(dc, fallback, FontKind::Body, LineKind::Notice, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding), &top);
					top += lineGap;
				}
				break;
			}

			case BlockKind::Table: {
				AppendTable(dc, block, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				top += lineGap;
				break;
			}

			case BlockKind::FrontMatter:
				switch (block.frontMatterMode) {
				case FrontMatterMode::Hide:
					break;
				case FrontMatterMode::Table:
					for (const auto& field : block.frontMatterFields) {
						Block renderedField;
						renderedField.kind = BlockKind::Paragraph;
						renderedField.sourceLine = block.sourceLine;
						renderedField.text = field.name;
						renderedField.text.append(L": ");
						renderedField.text.append(field.value);
						if (!field.name.empty()) {
							renderedField.inlineSpans.push_back(
								{ InlineKind::Strong, 0, field.name.size(), std::nullopt });
						}
						AppendWrappedText(dc, renderedField, FontKind::Body, LineKind::Table,
							leftPadding, std::max(1, clientWidth - leftPadding - rightPadding), &top);
					}
					top += lineGap;
					break;
				case FrontMatterMode::CodeBlock: {
					appendLiteralBlock(block, nullptr);
					top += lineGap;
					break;
				}
				}
				break;

			case BlockKind::Math:
			case BlockKind::MermaidDiagram: {
				if (block.kind == BlockKind::MermaidDiagram
					&& AppendMermaidDiagram(dc, block, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding), &top)) {
					top += lineGap;
					break;
				}
				Block notice;
				notice.kind = BlockKind::Paragraph;
				notice.sourceLine = block.sourceLine;
				const auto feature = block.kind == BlockKind::Math ? L"Math" : L"Mermaid";
				switch (block.fallbackKind) {
				case NativeFallbackKind::None:
				case NativeFallbackKind::LiteralSource:
					notice.text = feature + std::wstring(L" rendering is unavailable; showing literal source.");
					break;
				case NativeFallbackKind::UnsupportedSyntax:
					notice.text = feature + std::wstring(L" syntax is unsupported; showing literal source.");
					break;
				case NativeFallbackKind::LimitExceeded:
					notice.text = feature + std::wstring(L" exceeded native limits; showing bounded literal source.");
					break;
				}
				AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
					std::max(1, clientWidth - leftPadding - rightPadding), &top);
				appendLiteralBlock(block, nullptr);
				top += lineGap;
				break;
			}

			case BlockKind::HorizontalRule:
				m_lines.push_back({ {}, {}, leftPadding, top, std::max(ScaleDip(12), 1), FontKind::Body, LineKind::Rule });
				m_lines.back().sourceLine = block.sourceLine;
				top += std::max(ScaleDip(12), 1) + lineGap;
				break;
			}
		}
		if (m_renderFailed) {
			Block notice;
			notice.sourceLine = m_document.blocks.empty() ? 0 : m_document.blocks.back().sourceLine;
			notice.text = L"Markdown preview failed safely while parsing this revision.";
			AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
				std::max(1, clientWidth - leftPadding - rightPadding), &top);
			top += lineGap;
		}
		if (m_sourceTruncated) {
			Block notice;
			notice.sourceLine = m_document.blocks.empty() ? 0 : m_document.blocks.back().sourceLine;
			notice.text = L"Preview truncated: showing the first 2 MiB or 200,000 lines.";
			AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
				std::max(1, clientWidth - leftPadding - rightPadding), &top);
			top += lineGap;
		}
		::ReleaseDC(m_hWnd, dc);
	}
	m_contentHeight = top + ScaleDip(12);
	UpdateScrollBar();
	if (m_revealSourceLine) RevealSourceLine(*m_revealSourceLine);
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
	::SetScrollInfo(m_hWnd, SB_VERT, &info, FALSE);
	UpdateOverlayScrollbar();
}

void CMarkdownPreviewWnd::UpdateOverlayScrollbar()
{
	m_overlayScrollbar.SetDpi(m_dpi);
	m_overlayScrollbar.SetColors(m_overlayColors);
	m_overlayScrollbar.Update();
}

void CMarkdownPreviewWnd::ScrollTo(int position, bool notifySource)
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
	::SetScrollInfo(m_hWnd, SB_VERT, &info, FALSE);
	UpdateOverlayScrollbar();
	::InvalidateRect(m_hWnd, nullptr, FALSE);
	if (notifySource) NotifySourceLineForScroll();
}

void CMarkdownPreviewWnd::ScrollBy(int delta, bool notifySource)
{
	ScrollTo(m_scrollY + delta, notifySource);
}

void CMarkdownPreviewWnd::NotifySourceLineForScroll()
{
	if (!m_sourceLineCallback || m_lines.empty()) return;
	const auto sourceLine = SourceLineForPreviewScroll(m_lines, m_scrollY);
	if (!sourceLine || m_lastNotifiedSourceLine == sourceLine) return;
	m_lastNotifiedSourceLine = sourceLine;
	m_sourceLineCallback(*sourceLine);
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

bool CMarkdownPreviewWnd::AppendMermaidDiagram(HDC dc, const Block& block, int left,
	int availableWidth, int* top)
{
	const auto font = GetFont(FontKind::Body, 0, false);
	const auto oldFont = ::SelectObject(dc, font);
	TEXTMETRICW metrics{};
	::GetTextMetricsW(dc, &metrics);
	mermaid::LayoutMetrics layout;
	layout.lineHeight = std::max<int>(metrics.tmHeight, ScaleDip(16));
	layout.nodePaddingX = ScaleDip(12);
	layout.nodePaddingY = ScaleDip(7);
	layout.rankSeparation = ScaleDip(40);
	layout.nodeSeparation = ScaleDip(24);
	layout.minimumNodeWidth = ScaleDip(44);
	layout.edgeLabelPadding = ScaleDip(4);
	mermaid::Diagram diagram;
	const auto outcome = mermaid::BuildFlowchart(block.text, layout, mermaid::BuildLimits{},
		[dc](std::wstring_view label) {
			SIZE size{};
			::GetTextExtentPoint32W(dc, label.data(), static_cast<int>(label.size()), &size);
			return static_cast<int>(size.cx);
		}, &diagram);
	::SelectObject(dc, oldFont);
	if (outcome != mermaid::BuildOutcome::Supported) {
		return false;
	}

	// A diagram wider than the pane would be silently cut off, and this preview
	// has no horizontal scrolling, so centre what fits and let the caller keep
	// the literal source when it does not.
	if (diagram.width > availableWidth) {
		return false;
	}
	const auto margin = ScaleDip(8);
	m_diagrams.push_back(std::move(diagram));
	RenderLine line;
	line.kind = LineKind::Diagram;
	line.font = FontKind::Body;
	line.left = left + std::max(0, (availableWidth - m_diagrams.back().width) / 2);
	line.top = *top + margin;
	line.width = m_diagrams.back().width;
	line.height = m_diagrams.back().height;
	line.sourceLine = block.sourceLine;
	line.diagramIndex = m_diagrams.size() - 1;
	m_lines.push_back(std::move(line));
	*top += m_diagrams.back().height + margin * 2;
	return true;
}

void CMarkdownPreviewWnd::DrawDiagram(HDC dc, const mermaid::Diagram& diagram, int left, int top) const
{
	const auto fallbackPen = static_cast<HPEN>(::GetStockObject(BLACK_PEN));
	const auto borderPen = m_rulePen != nullptr ? m_rulePen : fallbackPen;
	const auto fillBrush = m_codeBackgroundBrush != nullptr ? m_codeBackgroundBrush
		: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH));
	// The head has to be the stroke's colour, not the border's, or an arrow
	// reads as a separate outlined shape sitting at the end of the line.
	const auto edgeBrush = m_diagramArrowBrush != nullptr ? m_diagramArrowBrush
		: static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH));

	// Edges first, so a node's fill covers the stub that ends underneath it.
	for (const auto& edge : diagram.edges) {
		if (edge.points.size() < 2) continue;
		HPEN pen = borderPen;
		switch (edge.style) {
		case mermaid::EdgeStyle::Solid: pen = m_diagramPen != nullptr ? m_diagramPen : fallbackPen; break;
		case mermaid::EdgeStyle::Dotted: pen = m_diagramDottedPen != nullptr ? m_diagramDottedPen : fallbackPen; break;
		case mermaid::EdgeStyle::Thick: pen = m_diagramThickPen != nullptr ? m_diagramThickPen : fallbackPen; break;
		}
		const auto oldPen = static_cast<HPEN>(::SelectObject(dc, pen));
		::MoveToEx(dc, left + edge.points.front().x, top + edge.points.front().y, nullptr);
		for (std::size_t index = 1; index < edge.points.size(); ++index) {
			::LineTo(dc, left + edge.points[index].x, top + edge.points[index].y);
		}
		if (edge.arrow) {
			const auto& tip = edge.points.back();
			const auto& previous = edge.points[edge.points.size() - 2];
			const double dx = static_cast<double>(tip.x - previous.x);
			const double dy = static_cast<double>(tip.y - previous.y);
			const auto length = std::max(1.0, std::sqrt(dx * dx + dy * dy));
			const auto ux = dx / length;
			const auto uy = dy / length;
			const auto size = static_cast<double>(std::max(6, ScaleDip(7)));
			const auto baseX = tip.x - ux * size;
			const auto baseY = tip.y - uy * size;
			const auto spread = size * 0.45;
			POINT head[3] = {
				{ left + tip.x, top + tip.y },
				{ left + static_cast<int>(baseX - uy * spread), top + static_cast<int>(baseY + ux * spread) },
				{ left + static_cast<int>(baseX + uy * spread), top + static_cast<int>(baseY - ux * spread) },
			};
			const auto oldBrush = static_cast<HBRUSH>(::SelectObject(dc, edgeBrush));
			(void)::Polygon(dc, head, 3);
			::SelectObject(dc, oldBrush);
		}
		::SelectObject(dc, oldPen);
	}

	const auto font = GetFont(FontKind::Body, 0, false);
	const auto oldFont = ::SelectObject(dc, font);
	::SetBkMode(dc, TRANSPARENT);

	// Edge labels sit on top of their line, on the page background, so the
	// stroke does not read through the text.
	for (const auto& edge : diagram.edges) {
		if (edge.label.empty()) continue;
		SIZE size{};
		::GetTextExtentPoint32W(dc, edge.label.c_str(), static_cast<int>(edge.label.size()), &size);
		const auto pad = std::max(2, ScaleDip(3));
		RECT labelRect{
			left + edge.labelCenter.x - size.cx / 2 - pad,
			top + edge.labelCenter.y - size.cy / 2,
			left + edge.labelCenter.x - size.cx / 2 + size.cx + pad,
			top + edge.labelCenter.y - size.cy / 2 + size.cy,
		};
		::FillRect(dc, &labelRect, m_backgroundBrush != nullptr ? m_backgroundBrush
			: static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
		::SetTextColor(dc, m_colors.secondaryText);
		::TextOutW(dc, labelRect.left + pad, labelRect.top,
			edge.label.c_str(), static_cast<int>(edge.label.size()));
	}

	for (const auto& node : diagram.nodes) {
		const auto x = left + node.x;
		const auto y = top + node.y;
		const auto right = x + node.width;
		const auto bottom = y + node.height;
		const auto oldPen = static_cast<HPEN>(::SelectObject(dc, borderPen));
		const auto oldBrush = static_cast<HBRUSH>(::SelectObject(dc, fillBrush));
		switch (node.shape) {
		case mermaid::NodeShape::Rectangle:
			(void)::Rectangle(dc, x, y, right, bottom);
			break;
		case mermaid::NodeShape::Rounded:
			(void)::RoundRect(dc, x, y, right, bottom, ScaleDip(8), ScaleDip(8));
			break;
		case mermaid::NodeShape::Stadium:
			(void)::RoundRect(dc, x, y, right, bottom, node.height, node.height);
			break;
		case mermaid::NodeShape::Circle:
			(void)::Ellipse(dc, x, y, right, bottom);
			break;
		case mermaid::NodeShape::Rhombus: {
			POINT points[4] = { { x + node.width / 2, y }, { right, y + node.height / 2 },
				{ x + node.width / 2, bottom }, { x, y + node.height / 2 } };
			(void)::Polygon(dc, points, 4);
			break;
		}
		case mermaid::NodeShape::Hexagon: {
			const auto notch = std::min(node.width / 4, node.height / 2);
			POINT points[6] = { { x + notch, y }, { right - notch, y }, { right, y + node.height / 2 },
				{ right - notch, bottom }, { x + notch, bottom }, { x, y + node.height / 2 } };
			(void)::Polygon(dc, points, 6);
			break;
		}
		}
		::SelectObject(dc, oldBrush);
		::SelectObject(dc, oldPen);

		SIZE size{};
		::GetTextExtentPoint32W(dc, node.label.c_str(), static_cast<int>(node.label.size()), &size);
		::SetTextColor(dc, m_colors.primaryText);
		::TextOutW(dc, x + (node.width - size.cx) / 2, y + (node.height - size.cy) / 2,
			node.label.c_str(), static_cast<int>(node.label.size()));
	}
	::SelectObject(dc, oldFont);
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
	if (line.kind == LineKind::Image) {
		if (line.imageIndex < m_images.size() && m_images[line.imageIndex].bitmap != nullptr) {
			const auto& image = m_images[line.imageIndex];
			const auto imageDc = ::CreateCompatibleDC(dc);
			if (imageDc != nullptr) {
				const auto oldBitmap = static_cast<HBITMAP>(::SelectObject(imageDc, image.bitmap));
				BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
				(void)::AlphaBlend(dc, line.left, top, line.width, line.height,
					imageDc, 0, 0, image.width, image.height, blend);
				::SelectObject(imageDc, oldBitmap);
				::DeleteDC(imageDc);
			}
		}
		return;
	}
	if (line.kind == LineKind::Diagram) {
		if (line.diagramIndex < m_diagrams.size()) {
			DrawDiagram(dc, m_diagrams[line.diagramIndex], line.left, top);
		}
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
	} else if (line.kind == LineKind::TableFill) {
		// Sized by the table, not by the pane: a GFM table is only as wide as its
		// columns, so shading the full client width would be wrong.
		RECT fillRect{ line.left, top, line.left + line.width, top + line.height };
		::FillRect(dc, &fillRect, m_codeBackgroundBrush != nullptr ? m_codeBackgroundBrush
			: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH)));
		return;
	} else if (line.kind == LineKind::TableBorderH || line.kind == LineKind::TableBorderV) {
		RECT ruleRect = line.kind == LineKind::TableBorderH
			? RECT{ line.left, top, line.left + line.width, top + std::max(1, line.height) }
			: RECT{ line.left, top, line.left + std::max(1, line.width), top + line.height };
		::FillRect(dc, &ruleRect, m_borderBrush != nullptr ? m_borderBrush
			: static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)));
		return;
	}

	::SetBkMode(dc, TRANSPARENT);
	int left = line.left;
	auto drawSegment = [&](std::wstring_view text, unsigned int style, bool inlineCode,
		bool inlineImage, std::optional<COLORREF> explicitColor = std::nullopt) {
		if (text.empty()) {
			return;
		}
		const auto font = GetFont(line.font, style, inlineCode);
		const auto oldFont = ::SelectObject(dc, font);
		const bool link = (style & kStyleLink) != 0;
		::SetTextColor(dc, explicitColor.value_or(link ? m_colors.link
		: (line.kind == LineKind::Quote || line.kind == LineKind::Notice || inlineImage
			? m_colors.secondaryText : m_colors.primaryText)));
		SIZE extent{};
		(void)::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
		if (inlineCode) {
			RECT codeRect{ left - ScaleDip(1), top, left + extent.cx + ScaleDip(1), top + line.height };
			::FillRect(dc, &codeRect, m_codeBackgroundBrush != nullptr ? m_codeBackgroundBrush
				: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH)));
		}
		(void)::TextOutW(dc, left, top, text.data(), static_cast<int>(text.size()));
		left += extent.cx;
		::SelectObject(dc, oldFont);
	};
	const auto codeTokenColor = [this](CodeTokenKind kind) noexcept {
		switch (kind) {
		case CodeTokenKind::Comment:
		case CodeTokenKind::Punctuation:
			return m_colors.secondaryText;
		case CodeTokenKind::Keyword:
		case CodeTokenKind::Type:
		case CodeTokenKind::Operator:
		case CodeTokenKind::Preprocessor:
		case CodeTokenKind::Tag:
		case CodeTokenKind::Attribute:
		case CodeTokenKind::Heading:
		case CodeTokenKind::Link:
			return m_colors.link;
		case CodeTokenKind::Literal:
		case CodeTokenKind::Number:
		case CodeTokenKind::String:
		case CodeTokenKind::Variable:
		case CodeTokenKind::Emphasis:
		case CodeTokenKind::Code:
			return m_colors.primaryText;
		}
		return m_colors.primaryText;
	};

	if (!line.codeTokens.empty()) {
		std::size_t position = 0;
		for (const auto& token : line.codeTokens) {
			if (token.start > position) {
				drawSegment(std::wstring_view(line.text).substr(position, token.start - position),
					0, false, false);
			}
			drawSegment(std::wstring_view(line.text).substr(token.start, token.length),
				0, false, false, codeTokenColor(token.kind));
			position = token.start + token.length;
		}
		if (position < line.text.size()) {
			drawSegment(std::wstring_view(line.text).substr(position), 0, false, false);
		}
		return;
	}

	// Style boundaries were normalized once during layout. Paint is a single
	// branch-reduced sweep over non-overlapping runs, never a segment×span scan.
	for (const auto& run : line.styleRuns) {
		unsigned int style = 0;
		if (run.Has(InlineStyleFlag::Strong)) style |= kStyleStrong;
		if (run.Has(InlineStyleFlag::Emphasis)) style |= kStyleEmphasis;
		if (run.Has(InlineStyleFlag::Link)) style |= kStyleLink;
		if (run.Has(InlineStyleFlag::Strikethrough)) style |= kStyleStrikethrough;
		drawSegment(std::wstring_view(line.text).substr(run.start, run.length), style,
			run.Has(InlineStyleFlag::Code), run.Has(InlineStyleFlag::Image));
	}
}

int CMarkdownPreviewWnd::MeasureRenderLine(HDC dc, const RenderLine& line) const
{
	int width = 0;
	const auto measureRun = [&](std::wstring_view text, unsigned int style, bool inlineCode) {
		if (text.empty()) return;
		const auto oldFont = ::SelectObject(dc, GetFont(line.font, style, inlineCode));
		SIZE extent{};
		(void)::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
		::SelectObject(dc, oldFont);
		width += extent.cx;
	};
	if (line.styleRuns.empty()) {
		measureRun(line.text, 0, false);
		return width;
	}
	for (const auto& run : line.styleRuns) {
		unsigned int style = 0;
		if (run.Has(InlineStyleFlag::Strong)) style |= kStyleStrong;
		if (run.Has(InlineStyleFlag::Emphasis)) style |= kStyleEmphasis;
		if (run.Has(InlineStyleFlag::Link)) style |= kStyleLink;
		if (run.Has(InlineStyleFlag::Strikethrough)) style |= kStyleStrikethrough;
		measureRun(std::wstring_view(line.text).substr(run.start, run.length), style,
			run.Has(InlineStyleFlag::Code));
	}
	return width;
}

void CMarkdownPreviewWnd::AppendTable(HDC dc, const Block& block, int left, int availableWidth, int* top)
{
	if (block.tableRows.empty()) return;

	std::size_t columnCount = 0;
	for (const auto& row : block.tableRows) {
		columnCount = std::max(columnCount, row.cells.size());
	}
	if (columnCount == 0) return;

	const auto cellPadX = ScaleDip(kPreviewTableCellPadXPx);
	const auto cellPadY = ScaleDip(kPreviewTableCellPadYPx);
	const auto border = std::max(1, ScaleDip(kPreviewTableBorderPx));
	const auto minimumContent = std::max(1, ScaleDip(kPreviewTableMinimumColumnPx) - 2 * cellPadX);

	// One temporary block per cell keeps the existing wrapping and inline-style
	// machinery: a cell is just a very narrow paragraph.
	const auto makeCellBlock = [&block](const TableCell& cell, bool header) {
		Block cellBlock;
		cellBlock.kind = BlockKind::Paragraph;
		cellBlock.sourceLine = block.sourceLine;
		cellBlock.text = cell.text;
		cellBlock.inlineSpans = cell.inlineSpans;
		if (header && !cellBlock.text.empty()) {
			// GFM header cells are `th`, which markdown.css sets to weight 600.
			cellBlock.inlineSpans.push_back(
				{ InlineKind::Strong, 0, cellBlock.text.size(), std::nullopt });
		}
		return cellBlock;
	};

	// Natural content width per column, measured with the face each cell will use.
	std::vector<int> content(columnCount, 0);
	for (const auto& row : block.tableRows) {
		for (std::size_t column = 0; column < row.cells.size(); ++column) {
			RenderLine probe;
			probe.font = FontKind::Body;
			const auto cellBlock = makeCellBlock(row.cells[column], row.header);
			probe.text = cellBlock.text;
			probe.styleRuns = BuildInlineStyleRuns(probe.text.size(), cellBlock.inlineSpans).runs;
			content[column] = std::max(content[column], MeasureRenderLine(dc, probe));
		}
	}

	// comfy-table's dynamic arrangement: columns that already fit keep their
	// natural width, and only the ones still too wide share what is left. The
	// narrowest column is settled first, so a single wide column cannot starve
	// the others.
	const auto chromeWidth = static_cast<int>(columnCount + 1) * border
		+ static_cast<int>(columnCount) * 2 * cellPadX;
	int budget = std::max(static_cast<int>(columnCount) * minimumContent, availableWidth - chromeWidth);
	std::vector<std::size_t> order(columnCount);
	for (std::size_t column = 0; column < columnCount; ++column) order[column] = column;
	std::stable_sort(order.begin(), order.end(),
		[&content](std::size_t a, std::size_t b) { return content[a] < content[b]; });
	std::vector<int> columnWidth(columnCount, 0);
	auto unsettled = static_cast<int>(columnCount);
	for (const auto column : order) {
		const auto share = std::max(minimumContent, budget / std::max(1, unsettled));
		columnWidth[column] = std::max(minimumContent, std::min(content[column], share));
		budget -= columnWidth[column];
		--unsettled;
	}

	std::vector<int> columnLeft(columnCount + 1, 0);
	columnLeft[0] = left;
	for (std::size_t column = 0; column < columnCount; ++column) {
		columnLeft[column + 1] = columnLeft[column] + border + cellPadX
			+ columnWidth[column] + cellPadX;
	}
	const auto tableRight = columnLeft[columnCount] + border;
	const auto tableWidth = tableRight - left;

	const auto appendHorizontalRule = [&](int ruleTop) {
		RenderLine rule;
		rule.kind = LineKind::TableBorderH;
		rule.left = left;
		rule.width = tableWidth;
		rule.top = ruleTop;
		rule.height = border;
		rule.sourceLine = block.sourceLine;
		m_lines.push_back(std::move(rule));
	};

	appendHorizontalRule(*top);
	*top += border;

	const auto lineHeight = GetLineHeight(dc, FontKind::Body);
	std::size_t bodyRowIndex = 0;
	for (const auto& row : block.tableRows) {
		const auto rowTop = *top;
		const auto rowLineKind = row.header ? LineKind::TableHeader : LineKind::Table;
		const auto rowStart = m_lines.size();

		// The shading is emitted first and in line-height slices so that the row
		// stays sorted by bottom edge, which is what the paint loop binary-searches
		// on, and so that it never paints over the text that follows it.
		const bool shaded = !row.header && (bodyRowIndex % 2) == 1;
		if (!row.header) ++bodyRowIndex;

		int rowBottom = rowTop + cellPadY;
		std::vector<std::pair<std::size_t, std::size_t>> cellRanges;
		cellRanges.reserve(columnCount);
		const auto shadeStart = m_lines.size();
		for (std::size_t column = 0; column < columnCount; ++column) {
			const auto cellStart = m_lines.size();
			int cellTop = rowTop + cellPadY;
			if (column < row.cells.size()) {
				const auto cellBlock = makeCellBlock(row.cells[column], row.header);
				if (!cellBlock.text.empty()) {
					AppendWrappedText(dc, cellBlock, FontKind::Body, rowLineKind,
						columnLeft[column] + border + cellPadX, columnWidth[column], &cellTop);
				}
			}
			cellRanges.emplace_back(cellStart, m_lines.size());
			rowBottom = std::max(rowBottom, cellTop);
		}
		const auto rowHeight = std::max(lineHeight + 2 * cellPadY,
			rowBottom - rowTop + cellPadY);

		// Alignment is applied after wrapping, because it needs the measured
		// width of each produced line rather than the column's width.
		for (std::size_t column = 0; column < columnCount; ++column) {
			const auto alignment = column < block.tableAlignments.size()
				? block.tableAlignments[column] : TableAlignment::Default;
			if (alignment == TableAlignment::Default || alignment == TableAlignment::Left) continue;
			for (auto index = cellRanges[column].first; index < cellRanges[column].second; ++index) {
				const auto measured = MeasureRenderLine(dc, m_lines[index]);
				const auto slack = std::max(0, columnWidth[column] - measured);
				m_lines[index].left += alignment == TableAlignment::Center ? slack / 2 : slack;
			}
		}

		if (row.header || shaded) {
			std::vector<RenderLine> shading;
			for (int offset = 0; offset < rowHeight; offset += lineHeight) {
				RenderLine fill;
				fill.kind = LineKind::TableFill;
				fill.left = left;
				fill.width = tableWidth;
				fill.top = rowTop + offset;
				fill.height = std::min(lineHeight, rowHeight - offset);
				fill.sourceLine = block.sourceLine;
				shading.push_back(std::move(fill));
			}
			m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(shadeStart),
				shading.begin(), shading.end());
		}

		for (std::size_t column = 0; column <= columnCount; ++column) {
			RenderLine rule;
			rule.kind = LineKind::TableBorderV;
			rule.left = columnLeft[column];
			rule.width = border;
			rule.top = rowTop;
			rule.height = rowHeight;
			rule.sourceLine = block.sourceLine;
			m_lines.push_back(std::move(rule));
		}

		// Cells were laid out independently, so the row's lines are interleaved by
		// column. The paint loop requires the whole list sorted by bottom edge.
		std::stable_sort(m_lines.begin() + static_cast<std::ptrdiff_t>(rowStart), m_lines.end(),
			[](const RenderLine& a, const RenderLine& b) {
				return a.top + a.height < b.top + b.height;
			});

		*top = rowTop + rowHeight;
		appendHorizontalRule(*top);
		*top += border;
	}
}

void CMarkdownPreviewWnd::AppendWrappedText(HDC dc, const Block& block, FontKind font, LineKind kind,
	int left, int availableWidth, int* top, int continuationLeft, int continuationWidth,
	const CodeHighlightResult* codeHighlight, std::size_t codeSourceOffset)
{
	std::wstring text = block.marker;
	text.append(block.text);
	std::vector<InlineSpan> spans = block.inlineSpans;
	for (auto& span : spans) {
		span.start += block.marker.size();
	}
	const auto normalizedRuns = BuildInlineStyleRuns(text.size(), spans).runs;
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
		// A newline inside a block's text is a GFM hard line break, which ends the
		// line box wherever it falls rather than wrapping at the measured fit.
		const auto forcedBreak = text.find(L'\n', start);
		if (forcedBreak != std::wstring::npos && forcedBreak < start + length) {
			length = forcedBreak - start;
		}
		while (length > 0 && IsWrapSpace(text[start + length - 1])) {
			--length;
		}
		RenderLine renderLine;
		renderLine.text = text.substr(start, length);
		renderLine.styleRuns = ClipInlineStyleRuns(normalizedRuns, start, length);
		renderLine.left = currentLeft;
		renderLine.top = *top;
		renderLine.height = lineHeight;
		renderLine.font = font;
		renderLine.kind = kind;
		renderLine.sourceLine = block.sourceLine;
		if (codeHighlight != nullptr) {
			renderLine.codeTokens = ClipCodeHighlightTokens(
				codeHighlight->tokens, codeSourceOffset + start, length);
		}
		m_lines.push_back(std::move(renderLine));
		*top += lineHeight;
		if (remaining == 0) {
			break;
		}
		start += std::max<std::size_t>(1, length);
		while (start < text.size() && (IsWrapSpace(text[start]) || text[start] == L'\n')) {
			++start;
		}
		if (continuationLeft >= 0) {
			currentLeft = continuationLeft;
			currentWidth = std::max(1, continuationWidth);
		}
	} while (start < text.size());
	::SelectObject(dc, oldFont);
}

HFONT CMarkdownPreviewWnd::GetFont(FontKind kind, unsigned int style, bool inlineCode) const noexcept
{
	const auto styleIndex = static_cast<std::size_t>(
		style & (kStyleStrong | kStyleEmphasis | kStyleLink | kStyleStrikethrough));
	if (kind == FontKind::Code || inlineCode) {
		const auto font = m_codeFonts[styleIndex];
		return font != nullptr ? font : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
	}
	if (kind == FontKind::Body) {
		const auto font = m_bodyFonts[styleIndex];
		return font != nullptr ? font : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
	}
	const auto index = static_cast<std::size_t>(static_cast<int>(kind) - static_cast<int>(FontKind::Heading1));
	const auto font = m_headingFonts[index][styleIndex];
	return font != nullptr ? font : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
}

/*!
	@brief The line box height for one font kind

	VS Code sets line-height in CSS rather than deriving it from the face: 22px
	for prose, 1.357em for code, and 1.25em for headings. The measured text
	height is still the floor, so a face with unusually tall metrics cannot be
	clipped by a smaller CSS box.
*/
int CMarkdownPreviewWnd::GetLineHeight(HDC dc, FontKind kind) const
{
	const auto oldFont = ::SelectObject(dc, GetFont(kind));
	TEXTMETRICW metrics{};
	::GetTextMetricsW(dc, &metrics);
	::SelectObject(dc, oldFont);

	int cssHeight = ScaleDip(kPreviewLineHeightPx);
	if (kind == FontKind::Code) {
		cssHeight = ScaleDip(kPreviewCodeLineHeightPx);
	} else if (kind != FontKind::Body) {
		const auto heading = static_cast<std::size_t>(
			static_cast<int>(kind) - static_cast<int>(FontKind::Heading1));
		const auto headingSize = ::MulDiv(ScaleDip(kPreviewFontSizePx),
			kPreviewHeadingPermille[heading], 1000);
		cssHeight = ::MulDiv(headingSize, kPreviewHeadingLineHeightPermille, 1000);
	}
	return std::max<LONG>(1, std::max<LONG>(metrics.tmHeight, cssHeight));
}

int CMarkdownPreviewWnd::ScaleDip(int dip) const noexcept
{
	const auto dpi = m_dpi == 0 ? kDefaultDpi : m_dpi;
	return ::MulDiv(dip, static_cast<int>(dpi), kDefaultDpi);
}

} // namespace markdown
