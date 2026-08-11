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

[[nodiscard]] HFONT CreatePreviewFont(const LOGFONT& base, int percentage, bool bold,
	bool italic, bool underline, bool strikethrough) noexcept
{
	LOGFONT font = base;
	if (font.lfHeight == 0) {
		font.lfHeight = -16;
	}
	const auto sign = font.lfHeight < 0 ? -1 : 1;
	const LONG absoluteHeight = std::max<LONG>(1, font.lfHeight * sign);
	font.lfHeight = sign * std::max<LONG>(1, ::MulDiv(absoluteHeight, percentage, 100));
	font.lfWeight = bold ? FW_BOLD : (font.lfWeight == 0 ? FW_NORMAL : font.lfWeight);
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

	constexpr int headingPercentages[] = { 170, 150, 135, 120, 110, 100 };
	for (std::size_t style = 0; style < m_bodyFonts.size(); ++style) {
		const auto styleValue = static_cast<unsigned int>(style);
		const bool strong = (styleValue & kStyleStrong) != 0;
		const bool emphasis = (styleValue & kStyleEmphasis) != 0;
		const bool link = (styleValue & kStyleLink) != 0;
		const bool strikethrough = (styleValue & kStyleStrikethrough) != 0;
		m_bodyFonts[style] = CreatePreviewFont(proseFont, 100, strong, emphasis, link, strikethrough);
		m_codeFonts[style] = CreatePreviewFont(codeFont, 100, strong, emphasis, link, strikethrough);
		if (m_codeFonts[style] == nullptr) {
			LOGFONT fallback = codeFont;
			(void)wcscpy_s(fallback.lfFaceName, LF_FACESIZE, L"Consolas");
			m_codeFonts[style] = CreatePreviewFont(fallback, 100, strong, emphasis, link, strikethrough);
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
	int top = ScaleDip(12);
	const int leftPadding = ScaleDip(14);
	const int rightPadding = ScaleDip(14);
	const int clientWidth = std::max(0L, client.right - client.left);

	const auto dc = ::GetDC(m_hWnd);
	if (dc != nullptr) {
		const auto lineGap = ScaleDip(6);
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
				for (const auto& row : block.tableRows) {
					Block renderedRow;
					renderedRow.kind = BlockKind::Paragraph;
					renderedRow.sourceLine = block.sourceLine;
					renderedRow.text.append(L"\x2502 ");
					for (const auto& cell : row.cells) {
						const auto cellStart = renderedRow.text.size();
						renderedRow.text.append(cell.text);
						for (const auto& span : cell.inlineSpans) {
							auto adjusted = span;
							adjusted.start += cellStart;
							renderedRow.inlineSpans.push_back(std::move(adjusted));
						}
						renderedRow.text.append(L" \x2502 ");
					}
					if (row.header && !renderedRow.text.empty()) {
						renderedRow.inlineSpans.push_back({ InlineKind::Strong, 0,
							renderedRow.text.size(), std::nullopt });
					}
					AppendWrappedText(dc, renderedRow, FontKind::Body,
						row.header ? LineKind::TableHeader : LineKind::Table, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding), &top);
				}
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
	::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
	::ShowScrollBar(m_hWnd, SB_VERT, m_maxScroll > 0 ? TRUE : FALSE);
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
	::SetScrollInfo(m_hWnd, SB_VERT, &info, TRUE);
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
	} else if (line.kind == LineKind::TableHeader) {
		RECT headerRect{ ScaleDip(8), top, std::max<LONG>(ScaleDip(8), client.right - ScaleDip(8)), top + line.height };
		::FillRect(dc, &headerRect, m_codeBackgroundBrush != nullptr ? m_codeBackgroundBrush
			: static_cast<HBRUSH>(::GetStockObject(LTGRAY_BRUSH)));
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
