/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "RustFixtureTsv.h"

// The UTF-8 / CESU-8 whole-buffer codec corpus (Issue #443): rust/native/fixtures/utf8-codec-corpus.tsv.
// This header owns what a row means: its columns, the hex notation of bytes and UTF-16 units, and the
// modes a row applies to. RustFixtureTsv.h owns where the file is and how a line is read.
namespace utf8_codec_corpus {

static_assert(sizeof(wchar_t) == 2, "the corpus stores UTF-16 code units");

// The two readings of the same bytes: CUtf8's UTF-8 mode and its CESU-8 mode.
enum class Mode { Utf8, Cesu8 };

inline constexpr std::array kModes{ Mode::Utf8, Mode::Cesu8 };

constexpr bool IsCesu8(Mode mode)
{
	return mode == Mode::Cesu8;
}

constexpr const char* Name(Mode mode)
{
	return IsCesu8(mode) ? "cesu8" : "utf8";
}

// Which modes a row applies to: Both when UTF-8 and CESU-8 give the same expectation.
enum class Variants { Both, Utf8, Cesu8 };

constexpr bool Applies(Variants variants, Mode mode)
{
	return variants == Variants::Both || (variants == Variants::Cesu8) == IsCesu8(mode);
}

inline std::string Describe(const std::string& id, Mode mode)
{
	return id + '/' + Name(mode);
}

// "41 C3 A9" for bytes, "0041 DC80" for UTF-16 units and "-" for none: the notation of the corpus file,
// also used to report a mismatch in a form that can be pasted back into a row.
inline std::string FormatBytes(std::string_view bytes)
{
	if (bytes.empty()) {
		return "-";
	}
	constexpr std::string_view digits = "0123456789ABCDEF";
	std::string text;
	for (const char byte : bytes) {
		if (!text.empty()) {
			text += ' ';
		}
		const auto value = static_cast<unsigned char>(byte);
		text += digits[value >> 4];
		text += digits[value & 0xF];
	}
	return text;
}

inline std::string FormatUnits(std::wstring_view units)
{
	if (units.empty()) {
		return "-";
	}
	constexpr std::string_view digits = "0123456789ABCDEF";
	std::string text;
	for (const wchar_t unit : units) {
		if (!text.empty()) {
			text += ' ';
		}
		const auto value = static_cast<std::uint16_t>(unit);
		for (int shift = 12; shift >= 0; shift -= 4) {
			text += digits[(value >> shift) & 0xF];
		}
	}
	return text;
}

// Immutable: LoadCorpus builds every case in one expression and the tests only read it.
struct DecodeCase {
	const std::string id;
	const std::string cls;
	const Variants variants;
	// The bytes handed to the decoder.
	const std::string input;
	// The UTF-16 units the decoder must produce. Decoding never reports loss.
	const std::wstring expected;
};

struct EncodeCase {
	const std::string id;
	const std::string cls;
	const Variants variants;
	// The UTF-16 units handed to the encoder.
	const std::wstring input;
	// The bytes the encoder must produce.
	const std::string expected;
	// The production result is RESULT_LOSESOME: an unpaired surrogate outside U+DC00..U+DCFF became '?'.
	const bool lossy;
};

struct Corpus {
	const std::vector<DecodeCase> decode;
	const std::vector<EncodeCase> encode;
};

namespace detail {

inline Variants ParseVariants(std::string_view text)
{
	if (text == "both") {
		return Variants::Both;
	}
	if (text == "utf8") {
		return Variants::Utf8;
	}
	if (text == "cesu8") {
		return Variants::Cesu8;
	}
	throw std::runtime_error("unknown variants: " + std::string(text));
}

inline bool ParseLossy(std::string_view text)
{
	if (text == "0") {
		return false;
	}
	if (text == "1") {
		return true;
	}
	throw std::runtime_error("lossy must be 0 or 1");
}

inline std::string ParseBytes(std::string_view text)
{
	std::string bytes;
	if (text == "-") {
		return bytes;
	}
	for (const auto token : rust_fixture_tsv::Split(text, ' ')) {
		if (token.size() != 2) {
			throw std::runtime_error("a byte must be two hex digits: " + std::string(token));
		}
		bytes.push_back(static_cast<char>(rust_fixture_tsv::ParseNumber<unsigned>(token, 16, "invalid hex byte")));
	}
	return bytes;
}

inline std::wstring ParseUnits(std::string_view text)
{
	std::wstring units;
	if (text == "-") {
		return units;
	}
	for (const auto token : rust_fixture_tsv::Split(text, ' ')) {
		if (token.size() != 4) {
			throw std::runtime_error("a UTF-16 unit must be four hex digits: " + std::string(token));
		}
		units.push_back(static_cast<wchar_t>(rust_fixture_tsv::ParseNumber<unsigned>(token, 16, "invalid hex unit")));
	}
	return units;
}

} // namespace detail

inline Corpus LoadCorpus()
{
	constexpr std::string_view header = "id\tclass\tdirection\tvariants\tinput\toutput\tlossy";
	std::vector<DecodeCase> decode;
	std::vector<EncodeCase> encode;
	std::set<std::string> ids;
	rust_fixture_tsv::ForEachRow(rust_fixture_tsv::Locate("utf8-codec-corpus.tsv"), header, 7,
		[&](const auto& fields) {
			const auto id = fields[0];
			if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos) {
				throw std::runtime_error("invalid id");
			}
			if (!ids.emplace(id).second) {
				throw std::runtime_error("duplicate id: " + std::string(id));
			}
			if (fields[1].empty()) {
				throw std::runtime_error("empty class");
			}
			const auto variants = detail::ParseVariants(fields[3]);
			const bool lossy = detail::ParseLossy(fields[6]);
			if (fields[2] == "decode") {
				if (lossy) {
					throw std::runtime_error("a decode row cannot be lossy");
				}
				decode.push_back({
					std::string(id), std::string(fields[1]), variants,
					detail::ParseBytes(fields[4]), detail::ParseUnits(fields[5]),
				});
			} else if (fields[2] == "encode") {
				encode.push_back({
					std::string(id), std::string(fields[1]), variants,
					detail::ParseUnits(fields[4]), detail::ParseBytes(fields[5]), lossy,
				});
			} else {
				throw std::runtime_error("unknown direction: " + std::string(fields[2]));
			}
		});
	return { std::move(decode), std::move(encode) };
}

} // namespace utf8_codec_corpus
