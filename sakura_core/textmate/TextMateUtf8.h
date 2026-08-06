/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace textmate {

//! One line of document text, re-encoded to UTF-8 for Onigmo (which only
//! understands byte-oriented encodings) alongside a byte-offset -> UTF-16
//! code-unit-offset table.
//!
//! `byteToUtf16Offset[b]` is only meaningful when `b` is a UTF-8 codepoint
//! boundary (0, the byte immediately after any encoded codepoint, or
//! `bytes.size()`); Onigmo never reports a match/capture boundary anywhere
//! else, because it treats the buffer as well-formed UTF-8, so interior
//! bytes of a multi-byte sequence are never queried and their table entries
//! are unspecified filler. `byteToUtf16Offset.size() == bytes.size() + 1`,
//! with the final entry mapping one-past-the-end of the byte buffer to the
//! UTF-16 length of the (sanitized) line.
struct Utf8LineBuffer final {
	std::string bytes;
	std::vector<std::size_t> byteToUtf16Offset;
};

//! Encodes `line` to UTF-8 and builds `byteToUtf16Offset`. A lone (unpaired)
//! UTF-16 surrogate is replaced with U+FFFD one code unit at a time, so the
//! result always has exactly `line.size()` UTF-16 code units of input
//! consumed — i.e. `byteToUtf16Offset` values index directly into the
//! *original* `line`, not into some re-lengthed copy of it. This mirrors how
//! a real document line (already validated on load) is expected to behave;
//! lone surrogates should not occur in practice, but a defensive fallback
//! that keeps the offset mapping exact is cheaper than special-casing every
//! caller for a malformed line.
[[nodiscard]] Utf8LineBuffer EncodeLineForSearch(std::wstring_view line);

//! General-purpose UTF-8 -> UTF-16 decode, for content that is not a
//! document line being prepared for search (Onigmo diagnostic text, and the
//! plist grammar loader's XML character data). Malformed input decodes with
//! U+FFFD substitution rather than failing, since a diagnostic string or a
//! best-effort grammar load should never itself throw.
[[nodiscard]] std::wstring DecodeUtf8ToWide(std::string_view utf8);

//! General-purpose UTF-16 -> UTF-8 encode for content that is not a document
//! line being prepared for search — chiefly regex *pattern* source text
//! (`match`/`begin`/`end`/`while` strings), which Onigmo also requires in
//! UTF-8 since pattern and subject must share one encoding, but which never
//! needs an offset-mapping table because nothing reports positions back into
//! pattern source.
[[nodiscard]] std::string EncodeUtf8(std::wstring_view wide);

} // namespace textmate
