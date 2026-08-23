/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "CMarkdownPreviewWnd.h"
#include "MarkdownRemoteImageFetcher.h"
#include "MarkdownPreviewWorkerRetirement.h"

#include <wincodec.h>
#define LUNASVG_BUILD_STATIC
#include <lunasvg.h>

#include "cxx/com_pointer.hpp"
#include "theme/CThemeService.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>


namespace markdown {

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
constexpr UINT kContinueLayoutBuildMessage = WM_APP + 2;
constexpr std::size_t kMaximumLayoutBlocksPerSlice = 2;
constexpr std::size_t kMaximumWrappedLinesPerSlice = 32;
constexpr auto kMaximumLayoutSliceDuration = std::chrono::milliseconds(2);

namespace {

class ScopedComApartment final {
public:
	ScopedComApartment() noexcept
		: m_result(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))
	{
	}

	~ScopedComApartment() noexcept
	{
		if (SUCCEEDED(m_result)) ::CoUninitialize();
	}

private:
	HRESULT m_result;
};

class CallbackCancellation final : public platform::request::IRequestCancellation {
public:
	explicit CallbackCancellation(std::function<bool()> callback)
		: m_callback(std::move(callback))
	{
	}

	bool IsCancellationRequested() const noexcept override
	{
		try {
			return !m_callback || m_callback();
		}
		catch (...) {
			return true;
		}
	}

private:
	std::function<bool()> m_callback;
};

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

[[nodiscard]] DecodedBitmap DecodeRasterFrame(
	IWICImagingFactory* factory, IWICBitmapFrameDecode* frame,
	std::size_t remainingPixels) noexcept
{
	DecodedBitmap result;
	if (factory == nullptr || frame == nullptr || remainingPixels == 0) return result;
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
#if defined(_MSC_VER)
		constexpr WICBitmapInterpolationMode kScalerMode = WICBitmapInterpolationModeHighQualityCubic;
#else
		constexpr WICBitmapInterpolationMode kScalerMode = WICBitmapInterpolationModeFant;
#endif
		if (FAILED(factory->CreateBitmapScaler(&scaler))
			|| FAILED(scaler->Initialize(frame, scaledWidth, scaledHeight, kScalerMode))) return result;
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
	return result;
}

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
		result = DecodeRasterFrame(factory, frame, remainingPixels);
	}
	catch (...) {
		return {};
	}
	return result;
}

[[nodiscard]] DecodedBitmap LoadEncodedRaster(
	const std::vector<std::uint8_t>& bytes, std::size_t remainingPixels) noexcept
{
	if (bytes.empty() || bytes.size() > kMaximumEncodedImageBytes
		|| bytes.size() > std::numeric_limits<DWORD>::max() || remainingPixels == 0) return {};
	try {
		cxx::com_pointer<IWICImagingFactory> factory;
		if (FAILED(factory.CreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER))) return {};
		cxx::com_pointer<IWICStream> stream;
		if (FAILED(factory->CreateStream(&stream))
			|| FAILED(stream->InitializeFromMemory(
				const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
				static_cast<DWORD>(bytes.size())))) return {};
		cxx::com_pointer<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromStream(stream, nullptr,
			WICDecodeMetadataCacheOnLoad, &decoder))) return {};
		cxx::com_pointer<IWICBitmapFrameDecode> frame;
		if (FAILED(decoder->GetFrame(0, &frame))) return {};
		return DecodeRasterFrame(factory, frame, remainingPixels);
	}
	catch (...) {
		return {};
	}
}

[[nodiscard]] bool ContainsAsciiInsensitive(std::string_view text, std::string_view needle) noexcept
{
	if (needle.empty() || needle.size() > text.size()) return false;
	return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
		[](char left, char right) {
			const auto fold = [](char value) {
				return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
			};
			return fold(left) == fold(right);
		}) != text.end();
}

struct SvgTextContext {
	double scaleX = 1.0;
	double scaleY = 1.0;
	double translateX = 0.0;
	double translateY = 0.0;
	double x = 0.0;
	double y = 0.0;
	double fontSize = 16.0;
	COLORREF fill = RGB(0, 0, 0);
	bool anchorMiddle = false;
	bool anchorEnd = false;
	bool hidden = false;
};

struct SvgTextRun {
	std::wstring text;
	double x = 0.0;
	double baseline = 0.0;
	double fontSize = 0.0;
	COLORREF fill = RGB(0, 0, 0);
	bool anchorMiddle = false;
	bool anchorEnd = false;
};

[[nodiscard]] char FoldAscii(char value) noexcept
{
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] std::optional<std::string_view> SvgAttribute(
	std::string_view tag, std::string_view name) noexcept
{
	for (std::size_t offset = 0; offset + name.size() <= tag.size();) {
		const auto found = std::search(tag.begin() + offset, tag.end(), name.begin(), name.end(),
			[](char left, char right) { return FoldAscii(left) == FoldAscii(right); });
		if (found == tag.end()) return std::nullopt;
		const auto position = static_cast<std::size_t>(found - tag.begin());
		const bool leftBoundary = position == 0
			|| tag[position - 1] == '<' || tag[position - 1] == ' '
			|| tag[position - 1] == '\t' || tag[position - 1] == '\r' || tag[position - 1] == '\n';
		auto cursor = position + name.size();
		while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) ++cursor;
		if (!leftBoundary || cursor >= tag.size() || tag[cursor] != '=') {
			offset = position + 1;
			continue;
		}
		++cursor;
		while (cursor < tag.size() && (tag[cursor] == ' ' || tag[cursor] == '\t')) ++cursor;
		if (cursor >= tag.size() || (tag[cursor] != '"' && tag[cursor] != '\'')) return std::nullopt;
		const auto quote = tag[cursor++];
		const auto end = tag.find(quote, cursor);
		if (end == std::string_view::npos) return std::nullopt;
		return tag.substr(cursor, end - cursor);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<double> ParseSvgNumber(std::string_view value) noexcept
{
	double result = 0.0;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	if (parsed.ec != std::errc{} || !std::isfinite(result)) return std::nullopt;
	return result;
}

[[nodiscard]] std::optional<COLORREF> ParseSvgColor(std::string_view value) noexcept
{
	if (value.size() != 4 && value.size() != 7) return std::nullopt;
	if (value.front() != '#') return std::nullopt;
	const auto digit = [](char character) -> int {
		if (character >= '0' && character <= '9') return character - '0';
		const auto folded = FoldAscii(character);
		return folded >= 'a' && folded <= 'f' ? folded - 'a' + 10 : -1;
	};
	int red = 0;
	int green = 0;
	int blue = 0;
	if (value.size() == 4) {
		red = digit(value[1]);
		green = digit(value[2]);
		blue = digit(value[3]);
		if (red < 0 || green < 0 || blue < 0) return std::nullopt;
		red *= 17;
		green *= 17;
		blue *= 17;
	} else {
		const int digits[] = { digit(value[1]), digit(value[2]), digit(value[3]),
			digit(value[4]), digit(value[5]), digit(value[6]) };
		if (std::ranges::any_of(digits, [](int value) { return value < 0; })) return std::nullopt;
		red = digits[0] * 16 + digits[1];
		green = digits[2] * 16 + digits[3];
		blue = digits[4] * 16 + digits[5];
	}
	return RGB(red, green, blue);
}

void ApplySvgTransform(SvgTextContext& context, std::string_view transform) noexcept
{
	std::size_t offset = 0;
	while (offset < transform.size()) {
		const auto open = transform.find('(', offset);
		if (open == std::string_view::npos) break;
		auto nameStart = open;
		while (nameStart > offset && transform[nameStart - 1] != ' '
			&& transform[nameStart - 1] != '\t' && transform[nameStart - 1] != ',') --nameStart;
		const auto close = transform.find(')', open + 1);
		if (close == std::string_view::npos) break;
		const auto name = transform.substr(nameStart, open - nameStart);
		auto arguments = transform.substr(open + 1, close - open - 1);
		const auto delimiter = arguments.find_first_of(", ");
		const auto first = ParseSvgNumber(arguments.substr(0, delimiter));
		std::optional<double> second;
		if (delimiter != std::string_view::npos) {
			auto secondStart = arguments.find_first_not_of(", \t", delimiter);
			if (secondStart != std::string_view::npos) second = ParseSvgNumber(arguments.substr(secondStart));
		}
		if (first && ContainsAsciiInsensitive(name, "translate")) {
			context.translateX += context.scaleX * *first;
			context.translateY += context.scaleY * second.value_or(0.0);
		} else if (first && ContainsAsciiInsensitive(name, "scale")) {
			context.scaleX *= *first;
			context.scaleY *= second.value_or(*first);
		}
		offset = close + 1;
	}
}

void ApplySvgTextAttributes(SvgTextContext& context, std::string_view tag) noexcept
{
	if (const auto value = SvgAttribute(tag, "x")) {
		if (const auto number = ParseSvgNumber(*value)) context.x = *number;
	}
	if (const auto value = SvgAttribute(tag, "y")) {
		if (const auto number = ParseSvgNumber(*value)) context.y = *number;
	}
	if (const auto value = SvgAttribute(tag, "font-size")) {
		if (const auto number = ParseSvgNumber(*value)) context.fontSize = *number;
	}
	if (const auto value = SvgAttribute(tag, "fill")) {
		if (const auto color = ParseSvgColor(*value)) context.fill = *color;
	}
	if (const auto value = SvgAttribute(tag, "text-anchor")) {
		context.anchorMiddle = ContainsAsciiInsensitive(*value, "middle");
		context.anchorEnd = ContainsAsciiInsensitive(*value, "end");
	}
	if (const auto value = SvgAttribute(tag, "aria-hidden")) {
		context.hidden = context.hidden || ContainsAsciiInsensitive(*value, "true");
	}
	if (const auto value = SvgAttribute(tag, "transform")) ApplySvgTransform(context, *value);
}

[[nodiscard]] std::wstring DecodeSvgText(std::string_view value)
{
	if (value.empty() || value.size() > 4096) return {};
	const auto required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0) return {};
	std::wstring decoded(static_cast<std::size_t>(required), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), decoded.data(), required) != required) return {};
	const std::pair<std::wstring_view, std::wstring_view> entities[] = {
		{ L"&amp;", L"&" }, { L"&lt;", L"<" }, { L"&gt;", L">" },
		{ L"&quot;", L"\"" }, { L"&apos;", L"'" }
	};
	for (const auto& [entity, replacement] : entities) {
		for (std::size_t position = 0; (position = decoded.find(entity, position)) != std::wstring::npos;) {
			decoded.replace(position, entity.size(), replacement);
			position += replacement.size();
		}
	}
	return decoded;
}

[[nodiscard]] std::vector<SvgTextRun> ExtractSvgTextRuns(
	std::string_view xml, double outputScaleX, double outputScaleY)
{
	struct Frame { std::string name; SvgTextContext context; };
	std::vector<Frame> stack;
	stack.push_back({ {}, {} });
	std::vector<SvgTextRun> runs;
	std::size_t totalCharacters = 0;
	std::size_t cursor = 0;
	while (cursor < xml.size() && runs.size() < 256 && totalCharacters < 4096) {
		const auto open = xml.find('<', cursor);
		if (open == std::string_view::npos) break;
		const auto close = xml.find('>', open + 1);
		if (close == std::string_view::npos) break;
		const auto tag = xml.substr(open, close - open + 1);
		if (tag.size() >= 3 && tag[1] == '/') {
			if (stack.size() > 1) stack.pop_back();
			cursor = close + 1;
			continue;
		}
		if (tag.size() < 3 || tag[1] == '!' || tag[1] == '?') {
			cursor = close + 1;
			continue;
		}
		auto nameEnd = tag.find_first_of(" \t\r\n/>", 1);
		if (nameEnd == std::string_view::npos) break;
		std::string name(tag.substr(1, nameEnd - 1));
		std::ranges::transform(name, name.begin(), FoldAscii);
		auto context = stack.back().context;
		ApplySvgTextAttributes(context, tag);
		const bool selfClosing = tag.size() >= 2 && tag[tag.size() - 2] == '/';
		if (!selfClosing) stack.push_back({ name, context });
		const auto textStart = close + 1;
		const auto nextOpen = xml.find('<', textStart);
		if (!selfClosing && nextOpen != std::string_view::npos
			&& (name == "text" || name == "tspan") && !context.hidden) {
			auto raw = xml.substr(textStart, nextOpen - textStart);
			const auto first = raw.find_first_not_of(" \t\r\n");
			const auto last = raw.find_last_not_of(" \t\r\n");
			if (first != std::string_view::npos && last != std::string_view::npos) {
				auto text = DecodeSvgText(raw.substr(first, last - first + 1));
				if (!text.empty() && text.size() <= 4096 - totalCharacters) {
					runs.push_back({ std::move(text),
						(context.translateX + context.scaleX * context.x) * outputScaleX,
						(context.translateY + context.scaleY * context.y) * outputScaleY,
						context.fontSize * std::abs(context.scaleY) * outputScaleY,
						context.fill, context.anchorMiddle, context.anchorEnd });
					totalCharacters += runs.back().text.size();
				}
			}
		}
		cursor = close + 1;
	}
	return runs;
}

void OverlaySvgText(BYTE* destination, int width, int height, std::size_t destinationStride,
	std::string_view xml, double sourceWidth, double sourceHeight) noexcept
{
	if (destination == nullptr || width <= 0 || height <= 0 || sourceWidth <= 0.0 || sourceHeight <= 0.0) return;
	try {
		const auto runs = ExtractSvgTextRuns(xml, width / sourceWidth, height / sourceHeight);
		if (runs.empty()) return;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* maskBits = nullptr;
		const auto maskBitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &maskBits, nullptr, 0);
		const auto maskDc = ::CreateCompatibleDC(nullptr);
		if (maskBitmap == nullptr || maskBits == nullptr || maskDc == nullptr) {
			if (maskDc != nullptr) ::DeleteDC(maskDc);
			if (maskBitmap != nullptr) ::DeleteObject(maskBitmap);
			return;
		}
		const auto oldBitmap = ::SelectObject(maskDc, maskBitmap);
		::SetBkMode(maskDc, TRANSPARENT);
		::SetTextColor(maskDc, RGB(255, 255, 255));
		const auto maskStride = static_cast<std::size_t>(width) * 4;
		for (const auto& run : runs) {
			const auto fontPixels = (std::max)(1, static_cast<int>(std::lround(run.fontSize)));
			const auto font = ::CreateFontW(-fontPixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
			if (font == nullptr) continue;
			std::memset(maskBits, 0, maskStride * static_cast<std::size_t>(height));
			const auto oldFont = ::SelectObject(maskDc, font);
			SIZE extent{};
			TEXTMETRIC metrics{};
			(void)::GetTextExtentPoint32W(maskDc, run.text.data(), static_cast<int>(run.text.size()), &extent);
			(void)::GetTextMetricsW(maskDc, &metrics);
			int left = static_cast<int>(std::lround(run.x));
			if (run.anchorMiddle) left -= extent.cx / 2;
			else if (run.anchorEnd) left -= extent.cx;
			const int top = static_cast<int>(std::lround(run.baseline)) - metrics.tmAscent;
			(void)::TextOutW(maskDc, left, top, run.text.data(), static_cast<int>(run.text.size()));
			::SelectObject(maskDc, oldFont);
			::DeleteObject(font);

			const BYTE red = GetRValue(run.fill);
			const BYTE green = GetGValue(run.fill);
			const BYTE blue = GetBValue(run.fill);
			for (int row = 0; row < height; ++row) {
				auto* target = destination + static_cast<std::size_t>(row) * destinationStride;
				const auto* mask = static_cast<const BYTE*>(maskBits) + static_cast<std::size_t>(row) * maskStride;
				for (int column = 0; column < width; ++column) {
					const auto index = static_cast<std::size_t>(column) * 4;
					const BYTE alpha = (std::max)({ mask[index], mask[index + 1], mask[index + 2] });
					if (alpha == 0) continue;
					const unsigned inverse = 255U - alpha;
					target[index] = static_cast<BYTE>((blue * alpha + target[index] * inverse + 127U) / 255U);
					target[index + 1] = static_cast<BYTE>((green * alpha + target[index + 1] * inverse + 127U) / 255U);
					target[index + 2] = static_cast<BYTE>((red * alpha + target[index + 2] * inverse + 127U) / 255U);
					target[index + 3] = static_cast<BYTE>((255U * alpha + target[index + 3] * inverse + 127U) / 255U);
				}
			}
		}
		::SelectObject(maskDc, oldBitmap);
		::DeleteDC(maskDc);
		::DeleteObject(maskBitmap);
	}
	catch (...) {
		return;
	}
}

[[nodiscard]] DecodedBitmap LoadSvgRaster(
	const std::vector<std::uint8_t>& bytes, std::size_t remainingPixels) noexcept
{
	constexpr std::size_t kMaximumSvgBytes = 2U * 1024U * 1024U;
	constexpr std::size_t kMaximumSvgElements = 20000;
	if (bytes.empty() || bytes.size() > kMaximumSvgBytes || remainingPixels == 0) return {};
	const std::string_view xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	// LunaSVG is an in-process static renderer: it does not execute scripts or
	// fetch URLs. Reject active/XML expansion, nested images, data payloads and
	// external references before parsing as an additional fail-closed boundary.
	if (!ContainsAsciiInsensitive(xml, "<svg")
		|| ContainsAsciiInsensitive(xml, "<!doctype")
		|| ContainsAsciiInsensitive(xml, "<!entity")
		|| ContainsAsciiInsensitive(xml, "<script")
		|| ContainsAsciiInsensitive(xml, "<foreignobject")
		|| ContainsAsciiInsensitive(xml, "<image")
		|| ContainsAsciiInsensitive(xml, "data:")
		|| ContainsAsciiInsensitive(xml, "href=\"http")
		|| ContainsAsciiInsensitive(xml, "href='http")
		|| ContainsAsciiInsensitive(xml, "href=\"//")
		|| ContainsAsciiInsensitive(xml, "href='//")
		|| ContainsAsciiInsensitive(xml, "url(http")
		|| ContainsAsciiInsensitive(xml, "url('//")
		|| ContainsAsciiInsensitive(xml, "url(\"//")
		|| static_cast<std::size_t>(std::ranges::count(xml, '<')) > kMaximumSvgElements) return {};

	try {
		auto document = lunasvg::Document::loadFromData(xml.data(), xml.size());
		if (!document) return {};
		const auto sourceWidth = document->width();
		const auto sourceHeight = document->height();
		if (!std::isfinite(sourceWidth) || !std::isfinite(sourceHeight)
			|| sourceWidth <= 0.0f || sourceHeight <= 0.0f
			|| sourceWidth * sourceHeight > static_cast<float>(kMaximumSourceImagePixels)) return {};
		auto width = static_cast<int>(std::ceil(sourceWidth));
		auto height = static_cast<int>(std::ceil(sourceHeight));
		if (width > static_cast<int>(kMaximumDecodedImageEdge)
			|| height > static_cast<int>(kMaximumDecodedImageEdge)) {
			const auto scale = static_cast<float>(kMaximumDecodedImageEdge)
				/ static_cast<float>((std::max)(width, height));
			width = (std::max)(1, static_cast<int>(std::floor(width * scale)));
			height = (std::max)(1, static_cast<int>(std::floor(height * scale)));
		}
		if (static_cast<std::size_t>(width) * height > remainingPixels) return {};
		auto rendered = document->renderToBitmap(
			static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 0x00000000);
		if (!rendered.valid() || rendered.data() == nullptr
			|| rendered.stride() < static_cast<std::uint32_t>(width) * 4U) return {};

		BITMAPINFO bitmapInfo{};
		bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmapInfo.bmiHeader.biWidth = width;
		bitmapInfo.bmiHeader.biHeight = -height;
		bitmapInfo.bmiHeader.biPlanes = 1;
		bitmapInfo.bmiHeader.biBitCount = 32;
		bitmapInfo.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		const auto bitmap = ::CreateDIBSection(nullptr, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (bitmap == nullptr || bits == nullptr) {
			if (bitmap != nullptr) ::DeleteObject(bitmap);
			return {};
		}
		const auto stride = static_cast<std::size_t>(width) * 4;
		for (int row = 0; row < height; ++row) {
			std::memcpy(static_cast<BYTE*>(bits) + static_cast<std::size_t>(row) * stride,
				rendered.data() + static_cast<std::size_t>(row) * rendered.stride(), stride);
		}
		OverlaySvgText(static_cast<BYTE*>(bits), width, height, stride, xml, sourceWidth, sourceHeight);
		return { bitmap, width, height };
	}
	catch (...) {
		return {};
	}
}

[[nodiscard]] DecodedBitmap LoadRemoteImage(
	const RemoteImageFetchResult& fetched, std::size_t remainingPixels) noexcept
{
	if (!fetched || !fetched.bytes) return {};
	if (fetched.mediaType == L"image/svg+xml") {
		return LoadSvgRaster(*fetched.bytes, remainingPixels);
	}
	return LoadEncodedRaster(*fetched.bytes, remainingPixels);
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
		WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
		0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), this);
	if (m_hWnd == nullptr) {
		return false;
	}
	// The preview owns one pixel-based scroll model. The overlay only presents
	// that model and never creates or hides a native non-client scrollbar.
	if (!m_overlayScrollbar.Create(parent, m_hWnd,
		[this](int position) { ScrollTo(position, true); },
		workbench::controls::OverlayScrollbarSource::ExplicitModel)) {
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return false;
	}
	if (!m_frameSurface.Open().Accepted()) {
		m_overlayScrollbar.Destroy();
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return false;
	}
	RebuildFonts();
	RebuildPaintResources();
	RebuildLayout();
	auto retirement = MarkdownPreviewWorkerRetirement::Instance().TryReserve();
	if (!retirement) {
		m_overlayScrollbar.Destroy();
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return false;
	}
	m_workerState = std::make_shared<WorkerState>();
	{
		std::lock_guard lock(m_workerState->mutex);
		m_workerState->completionTarget = m_hWnd;
		m_workerState->remoteImageFetcher = m_remoteImageFetcher;
	}
	try {
		const auto state = m_workerState;
		m_worker = std::jthread([state](std::stop_token stopToken) {
			WorkerMain(state, stopToken);
		});
		m_workerRetirement.emplace(std::move(*retirement));
	}
	catch (...) {
		m_workerState.reset();
		m_overlayScrollbar.Destroy();
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
		return false;
	}
	return true;
}

void CMarkdownPreviewWnd::Close() noexcept
{
	m_sourceLineCallback = {};
	(void)m_nativeSurface.Close();
	m_nativeSurface.SetSink({});
	m_nativeSurfaceTarget.reset();
	(void)m_frameSurface.Close();
	m_overlayScrollbar.Destroy();
	StopWorker();
	m_deferredCompletion.reset();
	if (m_hWnd != nullptr) {
		::DestroyWindow(m_hWnd);
		m_hWnd = nullptr;
	}
}

void CMarkdownPreviewWnd::StopWorker() noexcept
{
	const auto state = std::exchange(m_workerState, nullptr);
	if (state != nullptr) {
		std::lock_guard lock(state->mutex);
		// Detach the completion sink before closing the scheduler. A worker that
		// already finished parsing may still be between completion and PostMessage;
		// it must observe no HWND owned by this object after this point.
		state->completionTarget = nullptr;
		++state->lifetimeEpoch;
		state->asyncState.Close();
		state->pendingWork.reset();
		state->completedWork.reset();
		state->completionWakePosted = false;
	}
	if (m_worker.joinable()) {
		m_worker.request_stop();
		if (state != nullptr) state->condition.notify_all();
		if (m_workerRetirement) {
			(void)MarkdownPreviewWorkerRetirement::Instance().Retire(
				std::move(m_worker), std::move(*m_workerRetirement), state);
			m_workerRetirement.reset();
		}
	}
}

void CMarkdownPreviewWnd::SetDocument(Document document)
{
	if (m_frameSurface.IsOpen()
		&& !m_frameSurface.NotifyContent().Accepted()) {
		// A closed/exhausted surface cannot publish a replacement. Keep the
		// current document as the last-good projection instead of exposing a
		// partially applied native state.
		return;
	}
	m_deferredCompletion.reset();
	m_document = std::move(document);
	m_codeHighlights.clear();
	m_inlineStyleRuns.clear();
	m_preparedImages.clear();
	m_preparedImagePixels = 0;
	m_layoutImagesPrepared = false;
	m_renderFailed = false;
	if (m_transientLayout) {
		m_imagesDirty = true;
		m_layoutDirty = true;
		return;
	}
	m_imagesDirty = false;
	RebuildLayout();
}

bool CMarkdownPreviewWnd::QueueDocument(std::wstring source, ParseOptions options,
	bool truncated, PreviewRenderKey key)
{
	const auto state = m_workerState;
	if (state == nullptr) return false;
	{
		std::lock_guard lock(state->mutex);
		if (!m_worker.joinable()
			|| state->asyncState.Queue(key) == PreviewQueueAction::RejectedClosed) return false;
		// Replacing this optional is the bounded latest-pending-one contract.
		state->pendingWork = PreviewWorkItem{
			key, std::move(source), std::move(options), truncated };
	}
	state->condition.notify_one();
	return true;
}

void CMarkdownPreviewWnd::WorkerMain(
	std::shared_ptr<WorkerState> state, std::stop_token stopToken) noexcept
{
	const ScopedComApartment comApartment;
	while (!stopToken.stop_requested()) {
		PreviewWorkItem work;
		{
			std::unique_lock lock(state->mutex);
			state->condition.wait(lock, [state, stopToken] {
				return stopToken.stop_requested() || state->pendingWork.has_value();
			});
			if (stopToken.stop_requested()) return;
			const auto key = state->asyncState.TakeNext();
			if (!key || !state->pendingWork || state->pendingWork->key != *key) continue;
			work = std::move(*state->pendingWork);
			state->pendingWork.reset();
		}

		PreviewWorkCompletion completion;
		completion.key = work.key;
		completion.truncated = work.truncated;
		CallbackCancellation cancellation([state, stopToken, key = work.key] {
			if (stopToken.stop_requested()) return true;
			std::lock_guard lock(state->mutex);
			return !state->asyncState.IsCurrent(key);
		});
		try {
			completion.document = ParseMarkdown(work.source, work.options);
			completion.codeHighlights.resize(completion.document.blocks.size());
			completion.inlineStyleRuns.resize(completion.document.blocks.size());
			for (std::size_t index = 0; index < completion.document.blocks.size(); ++index) {
				if (stopToken.stop_requested()) break;
				{
					std::lock_guard lock(state->mutex);
					if (!state->asyncState.IsCurrent(work.key)) break;
				}
				const auto& block = completion.document.blocks[index];
				completion.inlineStyleRuns[index] =
					BuildInlineStyleRuns(block.text.size(), block.inlineSpans).runs;
				if (block.kind == BlockKind::CodeBlock) {
					completion.codeHighlights[index] = HighlightMarkdownCode(block.language, block.text);
				}
				if (block.kind != BlockKind::Image || block.images.empty()) continue;
				for (const auto& imageNode : block.images) {
				const auto& resource = imageNode.source;
				const bool local = resource.disposition == ResourceDisposition::ResolvedLocal;
				const bool remote = resource.disposition == ResourceDisposition::ResolvedHttps;
				if (!local && !remote) continue;
				const auto& identity = local ? resource.resolvedPath : resource.original;
				const auto duplicate = std::ranges::find_if(completion.decodedImages,
					[&identity, &resource](const CachedImage& image) {
						return _wcsicmp(image.path.c_str(), identity.c_str()) == 0
							&& _wcsicmp(image.allowedRoot.c_str(), resource.allowedRoot.c_str()) == 0;
					});
				if (duplicate != completion.decodedImages.end()
					|| completion.decodedImages.size() >= kMaximumCachedImages
					|| completion.decodedImagePixels >= kMaximumDecodedImagePixels) continue;
				DecodedBitmap decoded;
				if (local) {
					decoded = LoadVerifiedRaster(resource,
						kMaximumDecodedImagePixels - completion.decodedImagePixels);
				} else if (state->remoteImageFetcher) {
					const auto fetched = state->remoteImageFetcher->Fetch(resource.original, &cancellation);
					if (cancellation.IsCancellationRequested()) break;
					decoded = LoadRemoteImage(fetched,
						kMaximumDecodedImagePixels - completion.decodedImagePixels);
				}
				CachedImage cached;
				cached.path = identity;
				cached.allowedRoot = resource.allowedRoot;
				cached.bitmap = CachedImage::Adopt(decoded.bitmap);
				cached.width = decoded.width;
				cached.height = decoded.height;
				completion.decodedImages.push_back(std::move(cached));
				if (decoded.bitmap != nullptr) {
					completion.decodedImagePixels += static_cast<std::size_t>(decoded.width)
						* static_cast<std::size_t>(decoded.height);
				}
				}
			}
		}
		catch (...) {
			completion.failed = true;
			completion.document = {};
			completion.codeHighlights.clear();
			completion.inlineStyleRuns.clear();
		}

		HWND target = nullptr;
		std::uint64_t epoch = 0;
		bool wakeRequired = false;
		{
			std::lock_guard lock(state->mutex);
			const auto action = state->asyncState.Complete(work.key, !completion.failed);
			if (action == PreviewCompletionAction::DiscardClosed) return;
			if (action == PreviewCompletionAction::DiscardStale) continue;
			state->completedWork = std::move(completion);
			target = state->completionTarget;
			epoch = state->lifetimeEpoch;
			wakeRequired = !state->completionWakePosted;
			state->completionWakePosted = true;
		}
		if (!wakeRequired) continue;
		if (target == nullptr || !::PostMessageW(target, kCommitPreviewWorkMessage,
			reinterpret_cast<WPARAM>(state.get()), static_cast<LPARAM>(epoch))) {
			std::lock_guard lock(state->mutex);
			state->completionWakePosted = false;
			if (state->completedWork && state->completedWork->key == work.key
				&& state->lifetimeEpoch == epoch) {
				state->completedWork.reset();
				state->asyncState.MarkDeliveryFailed(work.key);
			}
		}
	}
}

void CMarkdownPreviewWnd::CommitCompletedWork(
	WPARAM completionState, LPARAM completionEpoch)
{
	const auto state = m_workerState;
	if (state == nullptr || completionState != reinterpret_cast<WPARAM>(state.get())) return;
	std::optional<PreviewWorkCompletion> completion;
	{
		std::lock_guard lock(state->mutex);
		state->completionWakePosted = false;
		if (static_cast<std::uint64_t>(completionEpoch) != state->lifetimeEpoch
			|| !state->completedWork || !state->asyncState.IsCurrent(state->completedWork->key)) {
			state->completedWork.reset();
			return;
		}
		completion = std::move(state->completedWork);
		state->completedWork.reset();
		state->asyncState.MarkDelivered(completion->key);
	}
	if (completion->failed) {
		// A failed parse is a surface-local failure. Keep the current document,
		// images, and code-highlight generation as the last-good projection.
		// The scheduler completion is terminal for this request; a later source
		// revision may still be queued normally.
		return;
	}
	if (m_transientLayout) {
		// Keep the currently painted line/image generation intact until the drag
		// commits. A newer completion replaces this one through the scheduler's
		// latest-result contract.
		m_deferredCompletion = std::move(completion);
		m_layoutDirty = true;
		return;
	}
	if (m_frameSurface.IsOpen()
		&& !m_frameSurface.NotifyContent().Accepted()) {
		// The frame fence rejected this completion (for example after close or
		// request-id exhaustion). Do not replace the last-good document.
		return;
	}
	m_document = std::move(completion->document);
	m_codeHighlights = std::move(completion->codeHighlights);
	m_inlineStyleRuns = std::move(completion->inlineStyleRuns);
	m_preparedImages = std::move(completion->decodedImages);
	m_preparedImagePixels = completion->decodedImagePixels;
	m_layoutImagesPrepared = true;
	m_sourceTruncated = completion->truncated;
	m_renderFailed = completion->failed;
	RebuildLayout();
}

void CMarkdownPreviewWnd::SetSourceTruncated(bool truncated)
{
	if (m_sourceTruncated == truncated) {
		return;
	}
	if (m_frameSurface.IsOpen()
		&& !m_frameSurface.NotifyContent().Accepted()) {
		return;
	}
	m_sourceTruncated = truncated;
	if (m_transientLayout) {
		m_layoutDirty = true;
		return;
	}
	RebuildLayout();
}

void CMarkdownPreviewWnd::SetPalette(const theme::ThemePalette& palette)
{
	if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyLayout();
	m_colors.background = palette.canvas.ToColorRef();
	m_colors.codeBackground = palette.raised.ToColorRef();
	m_colors.border = palette.border.ToColorRef();
	m_colors.primaryText = palette.primaryText.ToColorRef();
	m_colors.secondaryText = palette.secondaryText.ToColorRef();
	m_colors.link = palette.accent.ToColorRef();
	m_overlayColors = workbench::controls::ResolveOverlayScrollbarColors(palette, palette.canvas);
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
	if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyLayout();
	if (m_transientLayout) {
		m_fontResourcesDirty = true;
		m_layoutDirty = true;
		return;
	}
	RebuildFonts();
	RebuildLayout();
}

void CMarkdownPreviewWnd::Layout(const RECT& bounds, unsigned int dpi, bool transient)
{
	const auto effectiveDpi = dpi == 0 ? kDefaultDpi : dpi;
	const bool dpiChanged = m_dpi != effectiveDpi;
	if (dpiChanged) {
		m_dpi = effectiveDpi;
		m_fontResourcesDirty = true;
		m_layoutDirty = true;
	}
	m_transientLayout = transient;
	if (m_hWnd == nullptr) return;
	if (!transient && m_frameSurface.IsOpen()) {
		// A committed native layout is a new frame projection. The request is
		// only publishable later through CommitGdiFrame(), after the enclosing
		// frame's GDI flush.
		(void)m_frameSurface.NotifyLayout();
	}

	const int width = std::max(0L, bounds.right - bounds.left);
	const int height = std::max(0L, bounds.bottom - bounds.top);
	const bool widthChanged = width != m_layoutWidth;
	m_suppressSizeLayout = true;
	// Geometry is projected without copying or painting intermediate pixels. A
	// transient drag publishes the sibling bounds first; the enclosing frame
	// commits one no-erase invalidation after all children have been projected.
	::SetWindowPos(m_hWnd, nullptr, bounds.left, bounds.top, width, height,
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW);
	m_suppressSizeLayout = false;

	if (transient) {
		// The committed content range and scroll offset remain authoritative
		// throughout the drag. Only the sibling overlay geometry follows the
		// transient window rectangle.
		UpdateOverlayScrollbar();
		// Paint this child before the parent flushes the editor group. A queued
		// invalidation can otherwise survive one sample when a worker completion
		// arrives during a large-document drag, leaving old-width pixels on screen
		// while PrintWindow already renders the current geometry.
		::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE);
		return;
	}
	if (m_fontResourcesDirty) {
		RebuildFonts();
		RebuildPaintResources();
		m_fontResourcesDirty = false;
	}
	if (m_deferredCompletion) {
		if (m_frameSurface.IsOpen()
			&& !m_frameSurface.NotifyContent().Accepted()) {
			// Keep the deferred result and the currently painted document until a
			// fresh frame request can be accepted.
			return;
		}
		m_imagesDirty = false;
		m_document = std::move(m_deferredCompletion->document);
		m_codeHighlights = std::move(m_deferredCompletion->codeHighlights);
		m_inlineStyleRuns = std::move(m_deferredCompletion->inlineStyleRuns);
		m_preparedImages = std::move(m_deferredCompletion->decodedImages);
		m_preparedImagePixels = m_deferredCompletion->decodedImagePixels;
		m_layoutImagesPrepared = true;
		m_sourceTruncated = m_deferredCompletion->truncated;
		m_renderFailed = m_deferredCompletion->failed;
		m_deferredCompletion.reset();
		m_layoutDirty = true;
	}
	if (m_imagesDirty) {
		m_imagesDirty = false;
	}
	if (m_layoutDirty || dpiChanged || widthChanged) {
		m_layoutDirty = false;
		RebuildLayout();
	} else {
		UpdateScrollBar();
		::InvalidateRect(m_hWnd, nullptr, FALSE);
	}
}

void CMarkdownPreviewWnd::Show(bool visible) noexcept
{
	if (m_hWnd != nullptr) {
		(void)m_frameSurface.SetVisible(visible);
		(void)SetNativeSurfaceVisible(visible);
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
		CommitCompletedWork(wParam, lParam);
		return 0;

	case kContinueLayoutBuildMessage:
		m_layoutContinuationPosted = false;
		ContinueLayoutBuild();
		return 0;

	case WM_NCDESTROY: {
		CancelLayoutBuild();
		(void)m_nativeSurface.Close();
		m_nativeSurface.SetSink({});
		m_nativeSurfaceTarget.reset();
		(void)m_frameSurface.Close();
		m_overlayScrollbar.Destroy();
		m_backBuffer.Reset();
		StopWorker();
		const auto hwnd = m_hWnd;
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		m_hWnd = nullptr;
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	case WM_ERASEBKGND:
		return 1;

	case WM_SHOWWINDOW:
		(void)SetNativeSurfaceVisible(wParam != FALSE);
		return 0;

	case WM_SIZE:
		SyncNativeSurfaceSize(
			LOWORD(lParam) != 0 && HIWORD(lParam) != 0
				? static_cast<std::uint32_t>(LOWORD(lParam)) : 0,
			LOWORD(lParam) != 0 && HIWORD(lParam) != 0
				? static_cast<std::uint32_t>(HIWORD(lParam)) : 0);
		if (m_suppressSizeLayout || m_transientLayout) {
			UpdateOverlayScrollbar();
		} else if (static_cast<int>(LOWORD(lParam)) != m_layoutWidth) {
			if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyLayout();
			RebuildLayout();
		} else {
			// Wrapping is width-dependent only. A height-only resize updates the
			// viewport and clamps the pixel scroll model without touching m_lines.
			if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyLayout();
			UpdateScrollBar();
			::InvalidateRect(m_hWnd, nullptr, FALSE);
		}
		return 0;

	case WM_DPICHANGED:
		m_dpi = HIWORD(wParam) == 0 ? kDefaultDpi : HIWORD(wParam);
		if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyLayout();
		m_fontResourcesDirty = true;
		m_layoutDirty = true;
		if (!m_transientLayout) {
			RebuildFonts();
			RebuildPaintResources();
			m_fontResourcesDirty = false;
			m_layoutDirty = false;
			RebuildLayout();
		}
		return 0;

	case WM_PAINT: {
		PAINTSTRUCT paint{};
		const auto dc = ::BeginPaint(m_hWnd, &paint);
		Paint(dc, paint.rcPaint);
		::EndPaint(m_hWnd, &paint);
		// This child can repaint after the enclosing layout handler has returned.
		// Its own completed GDI batch is therefore the authoritative publication
		// boundary for an asynchronously prepared layout generation.
		(void)::GdiFlush();
		(void)m_frameSurface.CommitGdiFrame();
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
		case SB_THUMBTRACK:
			// No native thumb exists. HIWORD(wParam) is only 16 bits and cannot
			// represent the preview's pixel range, so explicit-model thumb input
			// is accepted solely through COverlayScrollbar's callback.
			return 0;
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
	DeleteCachedImages(m_images);
	m_decodedImagePixels = 0;
}

void CMarkdownPreviewWnd::DeleteCachedImages(std::vector<CachedImage>& images) noexcept
{
	images.clear();
}

std::size_t CMarkdownPreviewWnd::GetOrLoadImage(const ResourceReference& resource)
{
	const bool local = resource.disposition == ResourceDisposition::ResolvedLocal;
	const bool remote = resource.disposition == ResourceDisposition::ResolvedHttps;
	if (!local && !remote) return kInvalidImage;
	const auto& identity = local ? resource.resolvedPath : resource.original;
	for (std::size_t index = 0; index < m_images.size(); ++index) {
		const auto& image = m_images[index];
		if (_wcsicmp(image.path.c_str(), identity.c_str()) == 0
			&& _wcsicmp(image.allowedRoot.c_str(), resource.allowedRoot.c_str()) == 0) {
			return image.bitmap == nullptr ? kInvalidImage : index;
		}
	}
	// QueueDocument generations decode every admitted local image on their
	// worker. A missing entry is blocked/failed/over-budget and must not fall
	// back to synchronous WIC work on the UI layout slice.
	if (m_layoutImagesPrepared || remote) return kInvalidImage;
	if (m_images.size() >= kMaximumCachedImages || m_decodedImagePixels >= kMaximumDecodedImagePixels) {
		return kInvalidImage;
	}
	const auto decoded = LoadVerifiedRaster(resource, kMaximumDecodedImagePixels - m_decodedImagePixels);
	CachedImage cached;
	cached.path = resource.resolvedPath;
	cached.allowedRoot = resource.allowedRoot;
	cached.bitmap = CachedImage::Adopt(decoded.bitmap);
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
	CancelLayoutBuild();
	const auto scrollAnchor = m_revealSourceLine
		? std::optional<PreviewScrollAnchor>{}
		: CapturePreviewScrollAnchor(m_lines, m_scrollY);
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	LayoutBuildState build;
	build.generation = ++m_layoutGeneration;
	build.clientWidth = std::max(0L, client.right - client.left);
	// markdown.css: body { padding: 0 26px; padding-top: 1em; }
	build.top = ScaleDip(kPreviewFontSizePx);
	build.scrollAnchor = scrollAnchor;
	if (m_layoutImagesPrepared) {
		build.images = m_preparedImages;
		build.decodedImagePixels = m_preparedImagePixels;
	} else {
		// Direct Document callers can still reuse the current generation. Normal
		// source updates arrive with worker-decoded resources above.
		build.images = m_images;
		build.decodedImagePixels = m_decodedImagePixels;
	}
	m_layoutBuild.emplace(std::move(build));
	// Keep WM_SIZE and document commit bounded. The published vectors remain the
	// last-known-good generation until the coalesced continuation completes.
	ScheduleLayoutBuildContinuation();
}

void CMarkdownPreviewWnd::ContinueLayoutBuild()
{
	if (m_hWnd == nullptr || !m_layoutBuild) return;
	auto& build = *m_layoutBuild;
	const auto generation = build.generation;
	const auto sliceStarted = std::chrono::steady_clock::now();
	std::size_t processedBlocks = 0;
	const int clientWidth = build.clientWidth;
	auto& top = build.top;
	const int leftPadding = ScaleDip(26);
	const int rightPadding = ScaleDip(26);

	const auto dc = ::GetDC(m_hWnd);
	if (dc != nullptr) {
		// Existing paint data remains the last-known-good generation. Helpers use
		// the member vectors, so expose staging only while this UI slice executes.
		m_lines.swap(build.lines);
		m_images.swap(build.images);
		m_diagrams.swap(build.diagrams);
		std::swap(m_decodedImagePixels, build.decodedImagePixels);
		try {
		const auto lineGap = ScaleDip(kPreviewBlockGapPx);
		const auto sliceDeadline = sliceStarted + kMaximumLayoutSliceDuration;
		std::size_t remainingLineBudget = kMaximumWrappedLinesPerSlice;
		const auto appendLiteralBlock = [&](const Block& literalBlock,
			const CodeHighlightResult* codeHighlight) {
			for (;;) {
				if (!build.wrappedText) {
					if (build.blockTextOffset > literalBlock.text.size()) return true;
					const auto start = build.blockTextOffset;
					const auto end = literalBlock.text.find(L'\n', start);
					const auto length = end == std::wstring::npos
						? literalBlock.text.size() - start : end - start;
					const auto sourceLine = literalBlock.sourceLine + build.blockSourceLineOffset;
					build.blockTextOffset = end == std::wstring::npos
						? literalBlock.text.size() + 1 : end + 1;
					if (end != std::wstring::npos) ++build.blockSourceLineOffset;
					BeginWrappedTextView(build,
						std::wstring_view(literalBlock.text).substr(start, length), sourceLine,
						FontKind::Code, LineKind::Code, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding), start);
				}
				if (!ContinueWrappedText(dc, build, codeHighlight, sliceDeadline,
					&remainingLineBudget)) return false;
				if (build.blockTextOffset > literalBlock.text.size()) return true;
				if (remainingLineBudget == 0 || std::chrono::steady_clock::now() >= sliceDeadline) {
					return false;
				}
			}
		};
		while (build.nextBlock < m_document.blocks.size()
			&& processedBlocks < kMaximumLayoutBlocksPerSlice
			&& remainingLineBudget > 0) {
			const auto blockIndex = build.nextBlock;
			const auto& block = m_document.blocks[blockIndex];
			const auto* codeHighlight = blockIndex < m_codeHighlights.size()
				&& m_codeHighlights[blockIndex].has_value()
				? &*m_codeHighlights[blockIndex] : nullptr;
			const auto* inlineRuns = blockIndex < m_inlineStyleRuns.size()
				? &m_inlineStyleRuns[blockIndex] : nullptr;
			bool blockComplete = true;
			switch (block.kind) {
			case BlockKind::Heading: {
				const auto level = std::clamp(block.level, 1, 6);
				const auto font = static_cast<FontKind>(static_cast<int>(FontKind::Heading1) + level - 1);
				if (build.blockStage == 0) {
					// CSS margins collapse, so the heading's 24px top margin replaces the
					// previous block's gap rather than adding to it. h1 has margin-top 0.
					if (blockIndex != 0 && level != 1) {
						top += std::max(0, ScaleDip(kPreviewHeadingMarginTopPx) - lineGap);
					}
					BeginWrappedText(build, block, font, LineKind::Text, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding),
						-1, 0, 0, false, inlineRuns);
					build.blockStage = 1;
				}
				blockComplete = ContinueWrappedText(dc, build, nullptr, sliceDeadline,
					&remainingLineBudget);
				if (blockComplete) top += ScaleDip(kPreviewHeadingMarginBottomPx);
				break;
			}
			case BlockKind::Paragraph:
				if (build.blockStage == 0) {
					BeginWrappedText(build, block, FontKind::Body, LineKind::Text, leftPadding,
						std::max(1, clientWidth - leftPadding - rightPadding),
						-1, 0, 0, false, inlineRuns);
					build.blockStage = 1;
				}
				blockComplete = ContinueWrappedText(dc, build, nullptr, sliceDeadline,
					&remainingLineBudget);
				if (blockComplete) top += lineGap;
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
				if (build.blockStage == 0) {
					BeginWrappedText(build, renderedListItem, FontKind::Body, LineKind::Text, indent,
						fullWidth, continuationLeft,
						std::max(1, fullWidth - static_cast<int>(markerExtent.cx)), 0, true);
					build.blockStage = 1;
				}
				blockComplete = ContinueWrappedText(dc, build, nullptr, sliceDeadline,
					&remainingLineBudget);
				break;
			}

			case BlockKind::BlockQuote:
				if (build.blockStage == 0) {
					BeginWrappedText(build, block, FontKind::Body, LineKind::Quote,
						leftPadding + ScaleDip(12),
						std::max(1, clientWidth - leftPadding - rightPadding - ScaleDip(12)),
						-1, 0, 0, false, inlineRuns);
					build.blockStage = 1;
				}
				blockComplete = ContinueWrappedText(dc, build, nullptr, sliceDeadline,
					&remainingLineBudget);
				if (blockComplete) top += lineGap;
				break;

			case BlockKind::CodeBlock: {
				blockComplete = appendLiteralBlock(block, codeHighlight);
				if (blockComplete) top += lineGap;
				break;
			}

			case BlockKind::Image: {
				std::vector<std::size_t> imageIndices;
				imageIndices.reserve(block.images.size());
				for (const auto& imageNode : block.images) {
					imageIndices.push_back(GetOrLoadImage(imageNode.source));
				}
				const auto unavailable = std::ranges::find(imageIndices, kInvalidImage);
				if (!imageIndices.empty() && unavailable == imageIndices.end()) {
					const int availableWidth = std::max(1, clientWidth - leftPadding - rightPadding);
					const int imageGap = ScaleDip(4);
					int rowLeft = leftPadding;
					int rowTop = top;
					int rowHeight = 0;
					for (const auto imageIndex : imageIndices) {
						const auto& image = m_images[imageIndex];
						int displayWidth = std::min(image.width, availableWidth);
						int displayHeight = std::max(1, ::MulDiv(image.height, displayWidth, image.width));
						const int maximumHeight = std::max(1, ScaleDip(520));
						if (displayHeight > maximumHeight) {
							displayWidth = std::max(1, ::MulDiv(displayWidth, maximumHeight, displayHeight));
							displayHeight = maximumHeight;
						}
						if (rowLeft != leftPadding
							&& rowLeft + displayWidth > leftPadding + availableWidth) {
							rowTop += rowHeight + imageGap;
							rowLeft = leftPadding;
							rowHeight = 0;
						}
						RenderLine imageLine;
						imageLine.left = rowLeft;
						imageLine.top = rowTop;
						imageLine.height = displayHeight;
						imageLine.kind = LineKind::Image;
						imageLine.imageIndex = imageIndex;
						imageLine.width = displayWidth;
						imageLine.sourceLine = block.sourceLine;
						m_lines.push_back(std::move(imageLine));
						rowLeft += displayWidth + imageGap;
						rowHeight = std::max(rowHeight, displayHeight);
					}
					top = rowTop + rowHeight + lineGap;
				} else {
					Block fallback;
					fallback.sourceLine = block.sourceLine;
					const auto unavailableIndex = unavailable == imageIndices.end()
						? 0U : static_cast<std::size_t>(unavailable - imageIndices.begin());
					const auto* imageNode = unavailableIndex < block.images.size()
						? &block.images[unavailableIndex] : nullptr;
					const auto disposition = imageNode != nullptr
						? imageNode->source.disposition : ResourceDisposition::Invalid;
					fallback.text = disposition == ResourceDisposition::ExternalBlocked
						|| disposition == ResourceDisposition::UnsafeSchemeBlocked
						|| disposition == ResourceDisposition::OutsideAllowedRoots
						? L"Blocked image" : L"Image unavailable";
					if (imageNode != nullptr && !imageNode->altText.empty()) {
						fallback.text.append(L": ");
						fallback.text.append(imageNode->altText);
					}
					if (build.blockStage == 0) {
						BeginWrappedText(build, fallback, FontKind::Body, LineKind::Notice,
							leftPadding, std::max(1, clientWidth - leftPadding - rightPadding),
							-1, 0, 0, true);
						build.blockStage = 1;
					}
					blockComplete = ContinueWrappedText(dc, build, nullptr, sliceDeadline,
						&remainingLineBudget);
					if (blockComplete) top += lineGap;
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
					while (build.blockSourceLineOffset < block.frontMatterFields.size()) {
						if (!build.wrappedText) {
							const auto& field = block.frontMatterFields[build.blockSourceLineOffset++];
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
							BeginWrappedText(build, renderedField, FontKind::Body, LineKind::Table,
								leftPadding, std::max(1, clientWidth - leftPadding - rightPadding),
								-1, 0, 0, true);
						}
						if (!ContinueWrappedText(dc, build, nullptr, sliceDeadline,
							&remainingLineBudget)) {
							blockComplete = false;
							break;
						}
						if (remainingLineBudget == 0
							|| std::chrono::steady_clock::now() >= sliceDeadline) {
							blockComplete = build.blockSourceLineOffset == block.frontMatterFields.size();
							break;
						}
					}
					if (blockComplete) top += lineGap;
					break;
				case FrontMatterMode::CodeBlock: {
					blockComplete = appendLiteralBlock(block, nullptr);
					if (blockComplete) top += lineGap;
					break;
				}
				}
				break;

			case BlockKind::Math:
			case BlockKind::MermaidDiagram: {
				if (build.blockStage == 0) {
					if (block.kind == BlockKind::MermaidDiagram
						&& AppendMermaidDiagram(dc, block, leftPadding,
							std::max(1, clientWidth - leftPadding - rightPadding), &top)) {
						build.blockStage = 3;
					} else {
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
						BeginWrappedText(build, notice, FontKind::Body, LineKind::Notice,
							leftPadding, std::max(1, clientWidth - leftPadding - rightPadding),
							-1, 0, 0, true);
						build.blockStage = 1;
					}
				}
				if (build.blockStage == 1) {
					if (!ContinueWrappedText(dc, build, nullptr, sliceDeadline,
						&remainingLineBudget)) {
						blockComplete = false;
						break;
					}
					build.blockStage = 2;
				}
				if (build.blockStage == 2) blockComplete = appendLiteralBlock(block, nullptr);
				if (blockComplete) top += lineGap;
				break;
			}

			case BlockKind::HorizontalRule:
				m_lines.push_back({ {}, {}, leftPadding, top, std::max(ScaleDip(12), 1), FontKind::Body, LineKind::Rule });
				m_lines.back().sourceLine = block.sourceLine;
				top += std::max(ScaleDip(12), 1) + lineGap;
				break;
			}
			if (!blockComplete) break;
			++build.nextBlock;
			build.wrappedText.reset();
			build.blockTextOffset = 0;
			build.blockSourceLineOffset = 0;
			build.blockStage = 0;
			++processedBlocks;
			if (std::chrono::steady_clock::now() - sliceStarted >= kMaximumLayoutSliceDuration) {
				break;
			}
		}
		const bool blocksComplete = build.nextBlock == m_document.blocks.size();
		if (blocksComplete && m_renderFailed) {
			Block notice;
			notice.sourceLine = m_document.blocks.empty() ? 0 : m_document.blocks.back().sourceLine;
			notice.text = L"Markdown preview failed safely while parsing this revision.";
			AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
				std::max(1, clientWidth - leftPadding - rightPadding), &top);
			top += lineGap;
		}
		if (blocksComplete && m_sourceTruncated) {
			Block notice;
			notice.sourceLine = m_document.blocks.empty() ? 0 : m_document.blocks.back().sourceLine;
			notice.text = L"Preview truncated: showing the first 2 MiB or 200,000 lines.";
			AppendWrappedText(dc, notice, FontKind::Body, LineKind::Notice, leftPadding,
				std::max(1, clientWidth - leftPadding - rightPadding), &top);
			top += lineGap;
		}
		}
		catch (...) {
			m_lines.swap(build.lines);
			m_images.swap(build.images);
			m_diagrams.swap(build.diagrams);
			std::swap(m_decodedImagePixels, build.decodedImagePixels);
			::ReleaseDC(m_hWnd, dc);
			m_layoutDirty = true;
			CancelLayoutBuild();
			return;
		}
		m_lines.swap(build.lines);
		m_images.swap(build.images);
		m_diagrams.swap(build.diagrams);
		std::swap(m_decodedImagePixels, build.decodedImagePixels);
		::ReleaseDC(m_hWnd, dc);
	} else {
		m_layoutDirty = true;
		CancelLayoutBuild();
		return;
	}
	if (!m_layoutBuild || m_layoutBuild->generation != generation) return;
	if (m_layoutBuild->nextBlock != m_document.blocks.size()) {
		ScheduleLayoutBuildContinuation();
		return;
	}
	CommitLayoutBuild();
}

void CMarkdownPreviewWnd::CommitLayoutBuild()
{
	if (!m_layoutBuild) return;
	auto completed = std::move(*m_layoutBuild);
	m_layoutBuild.reset();
	DeleteImages();
	m_lines = std::move(completed.lines);
	m_images = std::move(completed.images);
	m_diagrams = std::move(completed.diagrams);
	m_decodedImagePixels = completed.decodedImagePixels;
	m_layoutWidth = completed.clientWidth;
	m_contentHeight = completed.top + ScaleDip(12);
	if (m_frameSurface.IsOpen()) (void)m_frameSurface.NotifyContent();
	UpdateScrollBar();
	if (m_revealSourceLine) {
		RevealSourceLine(*m_revealSourceLine);
	} else if (completed.scrollAnchor) {
		if (const auto restored = RestorePreviewScrollAnchor(m_lines, *completed.scrollAnchor)) {
			ScrollTo(*restored, false);
		}
	}
	::InvalidateRect(m_hWnd, nullptr, FALSE);
}

void CMarkdownPreviewWnd::CancelLayoutBuild() noexcept
{
	if (!m_layoutBuild) return;
	DeleteCachedImages(m_layoutBuild->images);
	m_layoutBuild.reset();
}

void CMarkdownPreviewWnd::ScheduleLayoutBuildContinuation() noexcept
{
	if (m_layoutContinuationPosted || m_hWnd == nullptr || !m_layoutBuild) return;
	if (::PostMessageW(m_hWnd, kContinueLayoutBuildMessage, 0, 0)) {
		m_layoutContinuationPosted = true;
		return;
	}
	// Keep the published generation intact. A later layout notification may
	// retry; failure to queue must never expose a partial staging generation.
	m_layoutDirty = true;
	CancelLayoutBuild();
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
	m_overlayScrollbar.SetScrollModel({ m_contentHeight, page, m_scrollY });
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
	if (notifySource) m_revealSourceLine.reset();
	const auto bounded = std::clamp(position, 0, m_maxScroll);
	if (bounded == m_scrollY) {
		return;
	}
	m_scrollY = bounded;
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	m_overlayScrollbar.SetScrollModel({ m_contentHeight,
		std::max(0, static_cast<int>(client.bottom - client.top)), m_scrollY });
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

void CMarkdownPreviewWnd::Paint(HDC dc, const RECT& paintRect)
{
	RECT client{};
	::GetClientRect(m_hWnd, &client);
	const int width = std::max(0L, client.right - client.left);
	const int height = std::max(0L, client.bottom - client.top);
	if (width == 0 || height == 0) {
		SyncNativeSurfaceSize(0, 0);
		return;
	}
	SyncNativeSurfaceSize(static_cast<std::uint32_t>(width),
		static_cast<std::uint32_t>(height));

	// Keep all line drawing offscreen.  The cached brushes/pens below avoid an
	// allocation for every wrapped line while the memory surface prevents the
	// scroll bar and glyph runs from visibly tearing during rapid updates.
	const bool buffered = m_backBuffer.Ensure(dc, width, height);
	const HDC targetDc = buffered ? m_backBuffer.Dc() : dc;
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
		(void)m_backBuffer.Present(dc, paintRect);
	}
	// Capture only after the retained buffer has been painted. Publication is
	// performed by the owner after its post-GDI boundary; this method only copies
	// the requested dirty rectangle into the bounded immutable mailbox.
	const auto sourceDc = buffered ? m_backBuffer.Dc() : dc;
	(void)m_nativeSurface.CapturePending(sourceDc, paintRect);
}

void CMarkdownPreviewWnd::SetNativeSurfaceSink(NativeSurfaceSink sink) noexcept
{
	m_nativeSurface.SetSink(std::move(sink));
	if (m_nativeSurfaceTarget && m_nativeSurface.HasTarget()
		&& !m_nativeSurface.IsRegistered()) {
		(void)m_nativeSurface.Register(*m_nativeSurfaceTarget);
	}
}

bool CMarkdownPreviewWnd::SetNativeSurfaceTarget(
	const NativeSurfaceTarget& target) noexcept
{
	if (target.surfaceId != m_frameSurface.StableSurfaceId()) return false;
	const auto result = m_nativeSurface.IsRegistered()
		? m_nativeSurface.Update(target) : m_nativeSurface.Register(target);
	if (result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Invalid
		&& result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Stale) {
		m_nativeSurfaceTarget = target;
	}
	return result.Accepted();
}

bool CMarkdownPreviewWnd::SetNativeSurfaceVisible(const bool visible) noexcept
{
	if (!m_nativeSurfaceTarget) return false;
	auto target = *m_nativeSurfaceTarget;
	target.visible = visible;
	const auto result = m_nativeSurface.IsRegistered()
		? m_nativeSurface.Update(target) : m_nativeSurface.Register(target);
	if (result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Invalid
		&& result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Stale) {
		m_nativeSurfaceTarget = target;
	}
	return result.Accepted();
}

void CMarkdownPreviewWnd::ClearNativeSurfaceTarget() noexcept
{
	(void)m_nativeSurface.Close();
	m_nativeSurfaceTarget.reset();
}

CMarkdownPreviewWnd::NativeSurfaceResult
CMarkdownPreviewWnd::PublishNativeSurface() noexcept
{
	return m_nativeSurface.PublishPending();
}

void CMarkdownPreviewWnd::SyncNativeSurfaceSize(
	const std::uint32_t width, const std::uint32_t height) noexcept
{
	if (!m_nativeSurfaceTarget) return;
	auto target = *m_nativeSurfaceTarget;
	if (target.width != width || target.height != height) {
		if (target.layoutEpoch == (std::numeric_limits<std::uint64_t>::max)()) return;
		if (target.layoutEpoch <= m_nativeSurfaceTarget->layoutEpoch) {
			target.layoutEpoch = m_nativeSurfaceTarget->layoutEpoch + 1;
		}
	}
	target.width = width;
	target.height = height;
	const auto result = m_nativeSurface.IsRegistered()
		? m_nativeSurface.Update(target) : m_nativeSurface.Register(target);
	if (result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Invalid
		&& result.status != workbench::rendering::EFrameNativeSurfacePayloadStatus::Stale) {
		m_nativeSurfaceTarget = target;
	}
}

workbench::rendering::FrameSurfaceAdapterResult CMarkdownPreviewWnd::SetFrameHost(
	std::string_view hostId) noexcept
{
	return m_frameSurface.SetHost(hostId);
}

workbench::rendering::FrameSurfaceAdapterResult CMarkdownPreviewWnd::NotifyFrameDeviceEpoch(
	std::uint64_t deviceEpoch) noexcept
{
	return m_frameSurface.NotifyDeviceEpoch(deviceEpoch);
}

std::optional<workbench::rendering::FrameSurfaceAdapterSnapshot>
CMarkdownPreviewWnd::CommitGdiFrame() noexcept
{
	return m_frameSurface.CommitGdiFrame();
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
				const auto oldBitmap = static_cast<HBITMAP>(::SelectObject(imageDc, image.Handle()));
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
	LayoutBuildState build;
	build.top = *top;
	BeginWrappedText(build, block, font, kind, left, availableWidth,
		continuationLeft, continuationWidth, codeSourceOffset);
	while (build.wrappedText) {
		auto budget = std::numeric_limits<std::size_t>::max();
		(void)ContinueWrappedText(dc, build, codeHighlight,
			std::chrono::steady_clock::time_point::max(), &budget);
	}
	*top = build.top;
}

void CMarkdownPreviewWnd::BeginWrappedText(LayoutBuildState& build, const Block& block,
	FontKind font, LineKind kind, int left, int availableWidth, int continuationLeft,
	int continuationWidth, std::size_t codeSourceOffset, bool retainText,
	const std::vector<InlineStyleRun>* preparedRuns)
{
	build.wrappedText.emplace();
	auto& state = *build.wrappedText;
	if (retainText) {
		state.ownedText = block.marker;
		state.ownedText.append(block.text);
		state.text = state.ownedText;
	} else if (!block.marker.empty()) {
		state.marker = block.marker;
		state.body = block.text;
		state.segmented = true;
	} else {
		state.text = block.text;
	}
	std::vector<InlineSpan> spans = block.inlineSpans;
	for (auto& span : spans) {
		span.start += block.marker.size();
	}
	const auto textSize = state.segmented
		? state.marker.size() + state.body.size() : state.text.size();
	if (preparedRuns != nullptr && block.marker.empty() && !retainText) {
		state.preparedRuns = preparedRuns;
	} else {
		state.normalizedRuns = BuildInlineStyleRuns(textSize, spans).runs;
	}
	state.font = font;
	state.kind = kind;
	state.left = left;
	state.availableWidth = std::max(1, availableWidth);
	state.continuationLeft = continuationLeft;
	state.continuationWidth = continuationWidth;
	state.nextForcedBreak = state.segmented ? state.marker.find(L'\n') : state.text.find(L'\n');
	if (state.segmented && state.nextForcedBreak == std::wstring::npos) {
		const auto bodyBreak = state.body.find(L'\n');
		if (bodyBreak != std::wstring::npos) state.nextForcedBreak = state.marker.size() + bodyBreak;
	}
	state.sourceLine = block.sourceLine;
	state.codeSourceOffset = codeSourceOffset;
}

void CMarkdownPreviewWnd::BeginWrappedTextView(LayoutBuildState& build, std::wstring_view text,
	std::size_t sourceLine, FontKind font, LineKind kind, int left, int availableWidth,
	std::size_t codeSourceOffset)
{
	build.wrappedText.emplace();
	auto& state = *build.wrappedText;
	state.text = text;
	state.normalizedRuns = BuildInlineStyleRuns(text.size(), {}).runs;
	state.font = font;
	state.kind = kind;
	state.left = left;
	state.availableWidth = std::max(1, availableWidth);
	state.nextForcedBreak = text.find(L'\n');
	state.sourceLine = sourceLine;
	state.codeSourceOffset = codeSourceOffset;
}

bool CMarkdownPreviewWnd::ContinueWrappedText(HDC dc, LayoutBuildState& build,
	const CodeHighlightResult* codeHighlight,
	std::chrono::steady_clock::time_point deadline, std::size_t* remainingLineBudget)
{
	if (!build.wrappedText) return true;
	if (remainingLineBudget == nullptr || *remainingLineBudget == 0) return false;
	auto& state = *build.wrappedText;
	const auto textSize = [&]() noexcept {
		return state.segmented ? state.marker.size() + state.body.size() : state.text.size();
	};
	const auto charAt = [&](std::size_t offset) noexcept -> wchar_t {
		if (!state.segmented) return state.text[offset];
		return offset < state.marker.size()
			? state.marker[offset] : state.body[offset - state.marker.size()];
	};
	const auto findBreak = [&](std::size_t offset) noexcept {
		if (!state.segmented) return state.text.find(L'\n', offset);
		if (offset < state.marker.size()) {
			const auto markerBreak = state.marker.find(L'\n', offset);
			if (markerBreak != std::wstring::npos) return markerBreak;
			offset = state.marker.size();
		}
		const auto bodyBreak = state.body.find(L'\n', offset - state.marker.size());
		return bodyBreak == std::wstring::npos
			? std::wstring::npos : state.marker.size() + bodyBreak;
	};
	const auto copyRange = [&](std::size_t offset, std::size_t length) {
		if (!state.segmented) return std::wstring(state.text.substr(offset, length));
		std::wstring result;
		result.reserve(length);
		if (offset < state.marker.size()) {
			const auto markerLength = std::min(length, state.marker.size() - offset);
			result.append(state.marker, offset, markerLength);
			offset += markerLength;
			length -= markerLength;
		}
		if (length != 0) result.append(state.body.substr(offset - state.marker.size(), length));
		return result;
	};
	const auto selectedFont = GetFont(state.font);
	const auto oldFont = ::SelectObject(dc, selectedFont);
	const auto lineHeight = GetLineHeight(dc, state.font);
	TEXTMETRICW textMetrics{};
	(void)::GetTextMetricsW(dc, &textMetrics);
	const auto averageCharacterWidth = std::max(1, static_cast<int>(textMetrics.tmAveCharWidth));
	int currentLeft = state.start == 0 || state.continuationLeft < 0
		? state.left : state.continuationLeft;
	int currentWidth = state.start == 0 || state.continuationLeft < 0
		? state.availableWidth : std::max(1, state.continuationWidth);
	do {
		const auto remaining = textSize() - state.start;
		const auto forcedLength = state.nextForcedBreak == std::wstring::npos
			? remaining : std::min(remaining, state.nextForcedBreak - state.start);
		std::size_t length = forcedLength;
		if (forcedLength > 0) {
			// Never measure the whole remaining block for every wrapped row. That
			// made one long paragraph O(N^2). Grow a bounded probe geometrically;
			// the sum of measured characters is now linear in the input length.
			const auto estimatedFit = static_cast<std::size_t>(currentWidth)
				/ static_cast<std::size_t>(averageCharacterWidth);
			std::size_t probeLength = std::min(forcedLength,
				std::max<std::size_t>(64, estimatedFit * 2 + 1));
			for (;;) {
				int fit = 0;
				SIZE measured{};
				std::wstring probeStorage;
				const wchar_t* probeData = nullptr;
				if (!state.segmented) {
					probeData = state.text.data() + state.start;
				} else if (state.start >= state.marker.size()) {
					probeData = state.body.data() + state.start - state.marker.size();
				} else if (state.start + probeLength <= state.marker.size()) {
					probeData = state.marker.data() + state.start;
				} else {
					probeStorage = copyRange(state.start, probeLength);
					probeData = probeStorage.data();
				}
				(void)::GetTextExtentExPointW(dc, probeData,
					static_cast<int>(probeLength), currentWidth, &fit, nullptr, &measured);
				fit = std::max(1, fit);
				if (static_cast<std::size_t>(fit) < probeLength || probeLength == forcedLength) {
					length = std::min(forcedLength, static_cast<std::size_t>(fit));
					break;
				}
				const auto nextProbe = probeLength > forcedLength / 2
					? forcedLength : probeLength * 2;
				if (nextProbe == probeLength) {
					length = probeLength;
					break;
				}
				probeLength = nextProbe;
			}
			if (length < forcedLength) {
				std::size_t breakAt = length;
				while (breakAt > 0 && !IsWrapSpace(charAt(state.start + breakAt - 1))) {
					--breakAt;
				}
				if (breakAt > 0) {
					length = breakAt;
				}
			}
		}
		// `forcedLength` stops measurement at the next GFM hard line break. Keep
		// its index across wrapped rows so a newline-free long block is scanned
		// once, not once per output row.
		while (length > 0 && IsWrapSpace(charAt(state.start + length - 1))) {
			--length;
		}
		RenderLine renderLine;
		renderLine.text = copyRange(state.start, length);
		const auto& styleRuns = state.preparedRuns != nullptr
			? *state.preparedRuns : state.normalizedRuns;
		renderLine.styleRuns = ClipInlineStyleRuns(styleRuns, state.start, length);
		renderLine.left = currentLeft;
		renderLine.top = build.top;
		renderLine.height = lineHeight;
		renderLine.font = state.font;
		renderLine.kind = state.kind;
		renderLine.sourceLine = state.sourceLine;
		if (codeHighlight != nullptr) {
			renderLine.codeTokens = ClipCodeHighlightTokens(
				codeHighlight->tokens, state.codeSourceOffset + state.start, length);
		}
		m_lines.push_back(std::move(renderLine));
		build.top += lineHeight;
		--*remainingLineBudget;
		if (remaining == 0) {
			::SelectObject(dc, oldFont);
			build.wrappedText.reset();
			return true;
		}
		state.start += std::max<std::size_t>(1, length);
		while (state.start < textSize()
			&& (IsWrapSpace(charAt(state.start)) || charAt(state.start) == L'\n')) {
			++state.start;
		}
		if (state.nextForcedBreak != std::wstring::npos && state.start > state.nextForcedBreak) {
			state.nextForcedBreak = findBreak(state.start);
		}
		if (state.continuationLeft >= 0) {
			currentLeft = state.continuationLeft;
			currentWidth = std::max(1, state.continuationWidth);
		}
		if (*remainingLineBudget == 0 || std::chrono::steady_clock::now() >= deadline) {
			::SelectObject(dc, oldFont);
			return false;
		}
	} while (state.start < textSize());
	::SelectObject(dc, oldFont);
	build.wrappedText.reset();
	return true;
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
