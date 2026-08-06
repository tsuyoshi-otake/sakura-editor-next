/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/TextMateUtf8.h"

namespace textmate {

namespace {

[[nodiscard]] bool IsHighSurrogate(wchar_t c) noexcept { return c >= 0xD800 && c <= 0xDBFF; }
[[nodiscard]] bool IsLowSurrogate(wchar_t c) noexcept { return c >= 0xDC00 && c <= 0xDFFF; }

} // namespace

Utf8LineBuffer EncodeLineForSearch(std::wstring_view line)
{
	Utf8LineBuffer result;
	result.bytes.reserve(line.size() * 3 / 2 + 4);
	result.byteToUtf16Offset.reserve(line.size() * 2 + 1);

	std::size_t utf16Index = 0;
	wchar_t codepointBuffer[2];
	while (utf16Index < line.size()) {
		const wchar_t first = line[utf16Index];
		int units = 1;
		if (IsHighSurrogate(first) && utf16Index + 1 < line.size() && IsLowSurrogate(line[utf16Index + 1])) {
			codepointBuffer[0] = first;
			codepointBuffer[1] = line[utf16Index + 1];
			units = 2;
		} else if (IsHighSurrogate(first) || IsLowSurrogate(first)) {
			// Lone surrogate: encode U+FFFD instead so the byte buffer stays
			// well-formed UTF-8, consuming exactly the one offending code unit
			// so offsets stay aligned with the caller's original `line`.
			codepointBuffer[0] = static_cast<wchar_t>(0xFFFD);
			units = 1;
		} else {
			codepointBuffer[0] = first;
			units = 1;
		}

		char narrowBuffer[8] = {};
		const int narrowLength = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, codepointBuffer, units == 2 ? 2 : 1, narrowBuffer, static_cast<int>(sizeof(narrowBuffer)), nullptr, nullptr);
		if (narrowLength <= 0) {
			// WideCharToMultiByte can only fail here on an unpaired surrogate
			// that slipped past the checks above (defensive only); fall back
			// to the UTF-8 encoding of U+FFFD directly rather than dropping
			// the character, which would desynchronize the offset table.
			result.bytes.push_back(static_cast<char>(0xEF));
			result.bytes.push_back(static_cast<char>(0xBF));
			result.bytes.push_back(static_cast<char>(0xBD));
		} else {
			result.bytes.append(narrowBuffer, static_cast<std::size_t>(narrowLength));
		}

		const std::size_t byteCountForThisCodepoint = result.bytes.size() - result.byteToUtf16Offset.size();
		// byteToUtf16Offset.size() before this append equals the byte offset
		// where this codepoint's encoding started; push one entry per newly
		// appended byte, all mapping back to this codepoint's own UTF-16
		// start offset (see the struct comment for why only boundary entries
		// are ever actually read).
		for (std::size_t i = 0; i < byteCountForThisCodepoint; ++i) {
			result.byteToUtf16Offset.push_back(utf16Index);
		}

		utf16Index += static_cast<std::size_t>(units);
	}
	result.byteToUtf16Offset.push_back(utf16Index);

	return result;
}

std::wstring DecodeUtf8ToWide(std::string_view utf8)
{
	if (utf8.empty()) return std::wstring();

	const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	if (wideLength <= 0) return std::wstring();

	std::wstring result(static_cast<std::size_t>(wideLength), L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), wideLength);
	return result;
}

std::string EncodeUtf8(std::wstring_view wide)
{
	if (wide.empty()) return std::string();

	const int narrowLength = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	if (narrowLength <= 0) return std::string();

	std::string result(static_cast<std::size_t>(narrowLength), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), narrowLength, nullptr, nullptr);
	return result;
}

} // namespace textmate
