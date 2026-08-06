/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"
#include "textmate/OnigmoRegexEngine.h"

#include <algorithm>
#include <mutex>

// Onigmo is compiled straight into this binary, never linked as a DLL. On MSVC
// `onigmo.h` otherwise defaults `ONIG_EXTERN` to `__declspec(dllimport) extern`,
// so every call here would go through an import thunk to a symbol that is in
// fact local, which the linker reports as LNK4217. The Onigmo `.c` files already
// get this define from the build, but the define has to be true at *this*
// include site too, and putting it here rather than in a project file keeps
// MSBuild and CMake from drifting apart on it.
#ifndef ONIG_EXTERN
#define ONIG_EXTERN extern
#endif
#include "Onigmo/onigmo.h"

namespace textmate {

namespace {

//! `onig_initialize` is process-global, idempotent-by-construction state (it
//! registers the encoding table Onigmo searches use); Onigmo has no
//! reference-counted "current user" concept, so this project follows the
//! same policy Sakura NEXT already applies to other long-lived native
//! libraries it never explicitly tears down: initialize once, lazily, on
//! first use, and never call `onig_end`. Calling `onig_end` would be unsafe
//! here regardless, because `OnigmoPattern` instances can legitimately
//! outlive most other static state (a cached compiled grammar can be held by
//! a workbench-lifetime object), and Onigmo has no way to "reinitialize"
//! after `onig_end` short of a fresh `onig_initialize` call racing any
//! pattern still alive.
bool EnsureOnigmoInitialized() noexcept
{
	static const bool initialized = [] {
		OnigEncoding encodings[] = {ONIG_ENCODING_UTF_8};
		return onig_initialize(encodings, 1) == ONIG_NORMAL;
	}();
	return initialized;
}

[[nodiscard]] std::size_t ByteOffsetToUtf16(const Utf8LineBuffer& line, std::size_t byteOffset) noexcept
{
	if (byteOffset >= line.byteToUtf16Offset.size()) {
		return line.byteToUtf16Offset.empty() ? 0 : line.byteToUtf16Offset.back();
	}
	return line.byteToUtf16Offset[byteOffset];
}

//! Inverse of the above: the *first* byte offset whose entry equals
//! `utf16Offset`. `byteToUtf16Offset` is monotonically non-decreasing by
//! construction (see `TextMateUtf8.cpp`), and the tokenizer only ever asks
//! for offsets that are themselves boundary values produced by this same
//! table (0, a line's UTF-16 length, or a previous match's own boundary), so
//! the searched-for value is always present exactly.
[[nodiscard]] std::size_t Utf16OffsetToByteOffset(const Utf8LineBuffer& line, std::size_t utf16Offset) noexcept
{
	const auto it = std::lower_bound(line.byteToUtf16Offset.begin(), line.byteToUtf16Offset.end(), utf16Offset);
	if (it == line.byteToUtf16Offset.end()) return line.bytes.size();
	return static_cast<std::size_t>(std::distance(line.byteToUtf16Offset.begin(), it));
}

} // namespace

OnigmoPattern::OnigmoPattern(OnigmoPattern&& other) noexcept : m_regex(other.m_regex)
{
	other.m_regex = nullptr;
}

OnigmoPattern& OnigmoPattern::operator=(OnigmoPattern&& other) noexcept
{
	if (this != &other) {
		Reset();
		m_regex = other.m_regex;
		other.m_regex = nullptr;
	}
	return *this;
}

OnigmoPattern::~OnigmoPattern()
{
	Reset();
}

void OnigmoPattern::Reset() noexcept
{
	if (m_regex) {
		onig_free(m_regex);
		m_regex = nullptr;
	}
}

std::unique_ptr<OnigmoPattern> OnigmoPattern::Compile(std::string_view utf8PatternSource, std::wstring* errorMessage)
{
	if (!EnsureOnigmoInitialized()) {
		if (errorMessage) *errorMessage = L"textmate: Onigmo library failed to initialize";
		return nullptr;
	}

	OnigRegex regex = nullptr;
	OnigErrorInfo errorInfo{};
	const auto* patternBegin = reinterpret_cast<const OnigUChar*>(utf8PatternSource.data());
	const auto* patternEnd = patternBegin + utf8PatternSource.size();

	// ONIG_OPTION_CAPTURE_GROUP + ONIG_SYNTAX_DEFAULT over ONIG_ENCODING_UTF_8
	// is the same option/syntax/encoding combination vscode-oniguruma uses to
	// compile TextMate grammar patterns; see the class comment in
	// OnigmoRegexEngine.h.
	const int status = onig_new(&regex, patternBegin, patternEnd, ONIG_OPTION_CAPTURE_GROUP, ONIG_ENCODING_UTF_8, ONIG_SYNTAX_DEFAULT, &errorInfo);
	if (status != ONIG_NORMAL) {
		if (errorMessage) {
			char buffer[ONIG_MAX_ERROR_MESSAGE_LEN] = {};
			onig_error_code_to_str(reinterpret_cast<OnigUChar*>(buffer), status, &errorInfo);
			*errorMessage = DecodeUtf8ToWide(buffer);
		}
		return nullptr;
	}

	return std::unique_ptr<OnigmoPattern>(new OnigmoPattern(regex));
}

std::optional<OnigmoMatchResult> OnigmoPattern::Search(const Utf8LineBuffer& line, std::size_t utf16SearchStart) const
{
	if (!m_regex) return std::nullopt;

	const std::size_t byteStart = Utf16OffsetToByteOffset(line, utf16SearchStart);
	const auto* stringBegin = reinterpret_cast<const OnigUChar*>(line.bytes.data());
	const auto* stringEnd = stringBegin + line.bytes.size();
	const auto* searchStart = stringBegin + byteStart;

	OnigRegion* region = onig_region_new();
	if (!region) return std::nullopt;

	// vscode-textmate/vscode-oniguruma always search the full remaining
	// buffer as the match *range* (never a narrower window than
	// [searchStart, end)); `begin`/`end`/`while`/`match` all rely on this so
	// a pattern can match anywhere at or after the cursor, not only exactly
	// at it.
	const OnigPosition matchStart = onig_search(m_regex, stringBegin, stringEnd, searchStart, stringEnd, region, ONIG_OPTION_NONE);
	if (matchStart < 0) {
		onig_region_free(region, 1);
		return std::nullopt;
	}

	OnigmoMatchResult result;
	result.groups.reserve(static_cast<std::size_t>(region->num_regs));
	for (int i = 0; i < region->num_regs; ++i) {
		OnigmoCaptureGroup group;
		if (region->beg[i] != ONIG_REGION_NOTPOS && region->end[i] != ONIG_REGION_NOTPOS) {
			group.participated = true;
			group.utf16Begin = ByteOffsetToUtf16(line, static_cast<std::size_t>(region->beg[i]));
			group.utf16End = ByteOffsetToUtf16(line, static_cast<std::size_t>(region->end[i]));
		}
		result.groups.push_back(group);
	}

	onig_region_free(region, 1);
	return result;
}

} // namespace textmate
