/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "textmate/TextMateUtf8.h"

// Forward-declared rather than included here: `OnigRegexType` is defined by
// <Onigmo/onigmo.h>, which is intentionally kept out of this public header so
// that nothing outside `sakura_core/textmate` needs Onigmo on its include
// path. `OnigmoRegexEngine.cpp` includes the real header.
//
// `onigmo.h` defines this type as `typedef struct re_pattern_buffer { ... }
// OnigRegexType;` — the struct's tag is `re_pattern_buffer`, and
// `OnigRegexType` is only ever a typedef alias for it, never a struct tag of
// its own. Forward-declaring `struct OnigRegexType;` here would introduce a
// *new, distinct* struct tag named `OnigRegexType` that collides with the
// typedef of the same name once `onigmo.h` is actually included
// (`OnigmoRegexEngine.cpp` does), which MSVC rejects as C2371 ("redefinition;
// different basic types"). Forward-declare the real tag and alias it under
// the same name instead, so this declaration and `onigmo.h`'s later
// definition both agree that `OnigRegexType` denotes `re_pattern_buffer`.
struct re_pattern_buffer;
using OnigRegexType = re_pattern_buffer;

namespace textmate {

//! One capture group of a completed match, in UTF-16 code-unit offsets into
//! the `Utf8LineBuffer`'s original line. Group 0 is always the whole match.
struct OnigmoCaptureGroup final {
	bool participated = false;
	std::size_t utf16Begin = 0;
	std::size_t utf16End = 0;
};

struct OnigmoMatchResult final {
	//! `groups[0]` is the whole match; `groups.size()` equals the pattern's
	//! group count plus one, matching Onigmo's own `region->num_regs`.
	std::vector<OnigmoCaptureGroup> groups;

	[[nodiscard]] const OnigmoCaptureGroup& WholeMatch() const noexcept { return groups[0]; }
};

//! RAII wrapper around one compiled Onigmo pattern (`OnigRegex`/`regex_t*`).
//! Move-only: an `OnigRegex` handle is a bare owning pointer with no copy
//! semantics of its own.
//!
//! Compiled with `ONIG_OPTION_CAPTURE_GROUP` and `ONIG_SYNTAX_DEFAULT` over
//! `ONIG_ENCODING_UTF_8`, matching the option/syntax/encoding combination
//! vscode-oniguruma (VS Code's own Oniguruma binding) uses for TextMate
//! grammar patterns; Onigmo is a source-compatible fork of the Oniguruma
//! engine vscode-oniguruma binds, so grammar patterns written against
//! Oniguruma's regex dialect should compile and match the same way here.
class OnigmoPattern final {
public:
	OnigmoPattern(const OnigmoPattern&) = delete;
	OnigmoPattern& operator=(const OnigmoPattern&) = delete;
	OnigmoPattern(OnigmoPattern&& other) noexcept;
	OnigmoPattern& operator=(OnigmoPattern&& other) noexcept;
	~OnigmoPattern();

	//! Compiles `patternSource` (already UTF-8, since Onigmo pattern text and
	//! subject text must share one encoding). On failure, returns `nullptr`
	//! and, when `errorMessage` is non-null, fills it with Onigmo's own
	//! diagnostic text converted to UTF-16.
	[[nodiscard]] static std::unique_ptr<OnigmoPattern> Compile(std::string_view utf8PatternSource, std::wstring* errorMessage);

	//! Searches `line` starting at UTF-16 code-unit offset `utf16SearchStart`
	//! (translated internally to the matching UTF-8 byte offset). Returns
	//! `std::nullopt` when there is no match anywhere in `[utf16SearchStart,
	//! end)` — Onigmo already scans forward internally, so callers should not
	//! loop calling `Search` at successive offsets to find "the next match";
	//! one call finds the leftmost match at or after the start offset.
	[[nodiscard]] std::optional<OnigmoMatchResult> Search(const Utf8LineBuffer& line, std::size_t utf16SearchStart) const;

private:
	explicit OnigmoPattern(OnigRegexType* regex) noexcept : m_regex(regex) {}
	void Reset() noexcept;

	OnigRegexType* m_regex = nullptr;
};

} // namespace textmate
