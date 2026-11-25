/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <ranges>
#include <string_view>

#include <gtest/gtest.h>

#include "charset/CCodeBase.h"

// The recorded codec corpus (#426): the inputs every codec is driven with and the
// per-group summaries the production converters produced for them.
// CodecGoldenCorpusTest.cpp pins the C++ converters to kExpectedRows; a candidate
// compared against the same rows is driven with the same inputs.
namespace codec_golden_corpus {

using namespace std::string_view_literals;

static_assert(sizeof(wchar_t) == 2);

inline constexpr auto kMimeHeader = "From: =?iso-2022-jp?B?GyRCJTUlLyVpGyhC?="sv;

inline constexpr std::array kJisEscapes{
	"\x1B(BABC"sv, "\x1B(JABC"sv, "\x1B(HABC"sv, "\x1B(I6E\x1B(B"sv,
	"\x1B$B$\"\x1B(B"sv, "\x1B$@$\"\x1B(B"sv, "\x1B&@\x1B$B$\"\x1B(B"sv,
	"A\x1B"sv, "A\x1B$"sv, "A\x1B("sv, "A\x1B(X\x80"sv,
	"\x1B$(D$\"\x1B(B"sv, "\x1B$B$"sv, "\x1B$B\xFF\xFF\x1B(B"sv,
};

inline constexpr std::array kJisMimeHeaders{
	kMimeHeader,
	"=?iso-2022-jp?Q?=1B=24=42=24=22=1B=28=42?="sv,
	"=?iso-2022-jp?B?GyRCJTUlLyVpGyhC"sv,
	"=?utf-8?B?44GC?="sv,
};

inline constexpr std::array kUtf8Forms{
	"\xC2\x80"sv, "\xE0\xA0\x80"sv, "\xE3\x81\x82"sv,
	"\xC0\x80"sv, "\xC1\xBF"sv, "\xE0\x80\x80"sv, "\xF0\x80\x80\x80"sv,
	"\xED\x9F\xBF"sv, "\xED\xA0\x80"sv, "\xED\xB0\x80"sv, "\xEE\x80\x80"sv,
	"\xED\xA0\xBD\xED\xB8\x80"sv, "\xED\xB8\x80\xED\xA0\xBD"sv,
	"\xF0\x90\x80\x80"sv, "\xF0\x9F\x98\x80"sv, "\xF4\x8F\xBF\xBF"sv, "\xF4\x90\x80\x80"sv,
	"A\xC2"sv, "A\xE3"sv, "A\xE3\x81"sv,
	"A\xF0"sv, "A\xF0\x9F"sv, "A\xF0\x9F\x98"sv,
	"\xED\xA0\xBD\xED\xB8"sv, "\xE3" "A\x82"sv,
};

inline constexpr std::array kUtf7Base64Runs{
	"+-"sv, "+MEI-"sv, "A+MEI-B+2D3eAA-"sv, "+AAA-"sv,
	"+MEI"sv, "+"sv, "+M-"sv, "+ME-"sv, "+MEJ-"sv, "+MEI=-"sv, "+!"sv,
};

inline constexpr std::array kUtf16LeForms{
	"\x42\x30"sv, "\x42\x30\x41"sv, "\x00\xD8"sv, "\x80\xDC"sv, "\xFF\xDF"sv,
	"\x3D\xD8\x00\xDE"sv, "\x41\x00\x00\x00\x42\x00"sv,
};

inline constexpr std::array kUtf16BeForms{
	"\x30\x42"sv, "\x30\x42\x41"sv, "\xD8\x00"sv, "\xDC\x80"sv, "\xDF\xFF"sv,
	"\xD8\x3D\xDE\x00"sv, "\x00\x41\x00\x00\x00\x42"sv,
};

inline constexpr std::array kUtf32LeForms{
	"\x42\x30\x00\x00"sv, "\x00\xF6\x01\x00"sv, "\x00\x00\x11\x00"sv, "\x00\xD8\x00\x00"sv,
	"\x42\x30"sv, "\x42\x30\x00"sv,
};

inline constexpr std::array kUtf32BeForms{
	"\x00\x00\x30\x42"sv, "\x00\x01\xF6\x00"sv, "\x00\x11\x00\x00"sv, "\x00\x00\xD8\x00"sv,
	"\x00\x00"sv, "\x00\x00\x30"sv,
};

inline constexpr std::array kSurrogatePairs{
	L"\U00010000"sv, L"\U0001D11E"sv, L"\U0001F600"sv, L"\U0010FFFF"sv,
};

inline constexpr std::array kTextSamples{
	L"\0"sv,
	L"A\0B"sv,
	L"\uFF76\uFF85\u304B\u306A\u30AB\u30CA\u6F22\u5B57"sv,
};

constexpr auto SingleBytes()
{
	return std::views::iota(0u, 0x100u) | std::views::transform([](unsigned byte) {
		return std::array{ static_cast<char>(byte) };
	});
}

constexpr auto HighBytePairs()
{
	return std::views::iota(0x8000u, 0x10000u) | std::views::transform([](unsigned pair) {
		return std::array{ static_cast<char>(pair >> 8), static_cast<char>(pair & 0xFF) };
	});
}

constexpr auto TruncatedHighByteTails()
{
	// Exercise each possible high byte alone after an ASCII prefix.
	return std::views::iota(0x80u, 0x100u) | std::views::transform([](unsigned byte) {
		return std::array{ 'A', static_cast<char>(byte) };
	});
}

constexpr auto BmpUnits()
{
	return std::views::iota(0u, 0x10000u) | std::views::transform([](unsigned unit) {
		return std::array{ static_cast<wchar_t>(unit) };
	});
}

// The result of one recorded group: how many cases ended COMPLETE, LOSESOME and FAILURE, the total
// output length (UTF-16 units for decode, bytes for encode) and the FNV-1a digest over every case.
// Immutable: SummaryBuilder accumulates it and kExpectedRows records it.
struct Summary {
	const std::uint64_t complete;
	const std::uint64_t losesome;
	const std::uint64_t failure;
	const std::uint64_t totalLength;
	const std::uint64_t digest;
};

class SummaryBuilder {
public:
	template<class Unit>
	void Add(EConvertResult result, std::basic_string_view<Unit> output)
	{
		switch (result) {
		case RESULT_COMPLETE: ++m_complete; break;
		case RESULT_LOSESOME: ++m_losesome; break;
		case RESULT_FAILURE: ++m_failure; break;
		default: ADD_FAILURE() << "Unexpected conversion result " << static_cast<int>(result); break;
		}

		// FNV-1a input per case: result (1 byte), length (8 bytes, little endian),
		// then the output (UTF-16LE units for decode, raw bytes for encode).
		// Lengths count UTF-16 code units for decode and bytes for encode.
		const std::uint64_t length = output.size();
		m_totalLength += length;
		HashByte(static_cast<std::uint8_t>(result));
		for (unsigned shift = 0; shift < 64; shift += 8) {
			HashByte(static_cast<std::uint8_t>(length >> shift));
		}
		for (const auto unit : output) {
			HashByte(static_cast<std::uint8_t>(unit));
			if constexpr (sizeof(Unit) == 2) {
				HashByte(static_cast<std::uint8_t>(static_cast<std::uint16_t>(unit) >> 8));
			}
		}
	}

	Summary Build() const
	{
		return { m_complete, m_losesome, m_failure, m_totalLength, m_digest };
	}

private:
	void HashByte(std::uint8_t byte)
	{
		m_digest = (m_digest ^ byte) * 1099511628211ull;
	}

	std::uint64_t m_complete = 0;
	std::uint64_t m_losesome = 0;
	std::uint64_t m_failure = 0;
	std::uint64_t m_totalLength = 0;
	std::uint64_t m_digest = 14695981039346656037ull;
};

struct GoldenRow {
	const std::string_view codec;
	const std::string_view direction;
	const std::string_view group;
	const Summary summary;
};

inline constexpr std::initializer_list<GoldenRow> kExpectedRows{
	{ "SJIS", "decode", "SingleBytes", { 256, 0, 0, 256, 0x22C4DB35B3F88BB6ull } },
	{ "SJIS", "decode", "HighBytePairs", { 32768, 0, 0, 56330, 0xBF2B725D4259A227ull } },
	{ "SJIS", "decode", "TruncatedHighByteTails", { 128, 0, 0, 256, 0x6B492C559A1B4922ull } },
	{ "SJIS", "encode", "BmpUnits", { 9660, 55876, 0, 74744, 0x8B5F36F061E390FBull } },
	{ "SJIS", "encode", "SurrogatePairs", { 0, 4, 0, 4, 0xAB7EB171F28A04FDull } },
	{ "SJIS", "encode", "TextSamples", { 3, 0, 0, 18, 0x07A6EA9521CE0BD0ull } },
	{ "EUC", "decode", "SingleBytes", { 256, 0, 0, 256, 0xDB326A0D21AD7E25ull } },
	{ "EUC", "decode", "HighBytePairs", { 32768, 0, 0, 58147, 0x67EA2D849161A77Cull } },
	{ "EUC", "decode", "TruncatedHighByteTails", { 128, 0, 0, 256, 0x2DFB5D9167A5E5A5ull } },
	{ "EUC", "encode", "BmpUnits", { 7775, 57761, 0, 72927, 0x4EEF7F533BC7FF7Bull } },
	{ "EUC", "encode", "SurrogatePairs", { 0, 4, 0, 4, 0xAB7EB171F28A04FDull } },
	{ "EUC", "encode", "TextSamples", { 3, 0, 0, 20, 0xB93D35B1803BDC7Dull } },
	{ "JIS-0", "decode", "SingleBytes", { 128, 128, 0, 255, 0x0FB9268D029B537Full } },
	{ "JIS-0", "decode", "JisEscapes", { 9, 5, 0, 25, 0x392779587326870Eull } },
	{ "JIS-0", "decode", "JisMimeHeaders", { 4, 0, 0, 130, 0x80919B9A728686B3ull } },
	{ "JIS-0", "encode", "BmpUnits", { 7524, 58012, 0, 130400, 0xC5CBB38DDFE308EEull } },
	{ "JIS-0", "encode", "SurrogatePairs", { 0, 4, 0, 4, 0xAB7EB171F28A04FDull } },
	{ "JIS-0", "encode", "TextSamples", { 3, 0, 0, 27, 0x7F37B5A6FD08E970ull } },
	{ "JIS-1", "decode", "SingleBytes", { 128, 128, 0, 255, 0x0FB9268D029B537Full } },
	{ "JIS-1", "decode", "JisEscapes", { 9, 5, 0, 25, 0x392779587326870Eull } },
	{ "JIS-1", "decode", "JisMimeHeaders", { 4, 0, 0, 58, 0x55415B5E59D61E1Dull } },
	{ "JIS-1", "encode", "BmpUnits", { 7524, 58012, 0, 130400, 0xC5CBB38DDFE308EEull } },
	{ "JIS-1", "encode", "SurrogatePairs", { 0, 4, 0, 4, 0xAB7EB171F28A04FDull } },
	{ "JIS-1", "encode", "TextSamples", { 3, 0, 0, 27, 0x7F37B5A6FD08E970ull } },
	{ "UTF8", "decode", "SingleBytes", { 256, 0, 0, 256, 0xDB326A0D21AD7E25ull } },
	{ "UTF8", "decode", "Utf8Forms", { 25, 0, 0, 68, 0xF0F5E1E413E70D70ull } },
	{ "UTF8", "encode", "BmpUnits", { 63744, 1792, 0, 190336, 0xB2335CA0C500E025ull } },
	{ "UTF8", "encode", "SurrogatePairs", { 4, 0, 0, 16, 0x2C9E0FBAE8178D28ull } },
	{ "UTF8", "encode", "TextSamples", { 3, 0, 0, 28, 0xCFA52F752CCAF730ull } },
	{ "CESU8", "decode", "SingleBytes", { 256, 0, 0, 256, 0xDB326A0D21AD7E25ull } },
	{ "CESU8", "decode", "Utf8Forms", { 25, 0, 0, 70, 0x133F3F909A7605E9ull } },
	{ "CESU8", "encode", "BmpUnits", { 63744, 1792, 0, 190336, 0xB2335CA0C500E025ull } },
	{ "CESU8", "encode", "SurrogatePairs", { 4, 0, 0, 24, 0xB360BCF57CE892D0ull } },
	{ "CESU8", "encode", "TextSamples", { 3, 0, 0, 28, 0xCFA52F752CCAF730ull } },
	{ "UTF7", "decode", "SingleBytes", { 97, 159, 0, 254, 0x759E6C369CD14C87ull } },
	{ "UTF7", "decode", "Utf7Base64Runs", { 8, 3, 0, 15, 0x83994C903AE9C461ull } },
	{ "UTF7", "encode", "BmpUnits", { 65536, 0, 0, 327377, 0xF3B4CB55B78754BBull } },
	{ "UTF7", "encode", "SurrogatePairs", { 4, 0, 0, 32, 0x61DC78D8A22026D6ull } },
	{ "UTF7", "encode", "TextSamples", { 3, 0, 0, 36, 0x8331BE4A54318B37ull } },
	{ "UTF16LE", "decode", "SingleBytes", { 0, 256, 0, 256, 0x9618DD91AB613325ull } },
	{ "UTF16LE", "decode", "Utf16LeForms", { 6, 1, 0, 11, 0x9356C095284116BCull } },
	{ "UTF16LE", "encode", "BmpUnits", { 65536, 0, 0, 131072, 0xB11683461269EF25ull } },
	{ "UTF16LE", "encode", "SurrogatePairs", { 4, 0, 0, 16, 0x0987F704BABEE0D5ull } },
	{ "UTF16LE", "encode", "TextSamples", { 3, 0, 0, 24, 0x6AC78B8034AF94E8ull } },
	{ "UTF16BE", "decode", "SingleBytes", { 0, 256, 0, 256, 0xCAFD28EDC4DC2925ull } },
	{ "UTF16BE", "decode", "Utf16BeForms", { 6, 1, 0, 11, 0x8814459B8F6DB43Eull } },
	{ "UTF16BE", "encode", "BmpUnits", { 65536, 0, 0, 131072, 0x48BF2CD43F768B25ull } },
	{ "UTF16BE", "encode", "SurrogatePairs", { 4, 0, 0, 16, 0x20DF27B36AD7F759ull } },
	{ "UTF16BE", "encode", "TextSamples", { 3, 0, 0, 24, 0x094174BCD0AF8D0Eull } },
	{ "Latin1", "decode", "SingleBytes", { 256, 0, 0, 256, 0xA5B49D91D1E8CFC4ull } },
	{ "Latin1", "encode", "BmpUnits", { 964, 64572, 0, 65536, 0x2E58CE5780EB2997ull } },
	{ "Latin1", "encode", "SurrogatePairs", { 0, 4, 0, 4, 0xAB7EB171F28A04FDull } },
	{ "Latin1", "encode", "TextSamples", { 2, 1, 0, 12, 0x6B8E702CD99910F1ull } },
	{ "CP932", "decode", "SingleBytes", { 256, 0, 0, 256, 0x5E73B29A12E60032ull } },
	{ "CP932", "decode", "HighBytePairs", { 32768, 0, 0, 50236, 0x57597895AB9A2E00ull } },
	{ "CP932", "decode", "TruncatedHighByteTails", { 128, 0, 0, 256, 0x054FC5A97F5AD3EAull } },
	{ "CP932", "encode", "BmpUnits", { 65536, 0, 0, 74754, 0x630C67A341E55FDBull } },
	{ "CP932", "encode", "SurrogatePairs", { 4, 0, 0, 8, 0xE3A9B58BEC7A611Dull } },
	{ "CP932", "encode", "TextSamples", { 3, 0, 0, 18, 0x07A6EA9521CE0BD0ull } },
	{ "CP1252", "decode", "SingleBytes", { 256, 0, 0, 256, 0xA5B49D91D1E8CFC4ull } },
	{ "CP1252", "encode", "BmpUnits", { 65536, 0, 0, 65536, 0x351F86EE9207710Bull } },
	{ "CP1252", "encode", "SurrogatePairs", { 4, 0, 0, 8, 0xE3A9B58BEC7A611Dull } },
	{ "CP1252", "encode", "TextSamples", { 3, 0, 0, 12, 0x59B6CC1274EE666Eull } },
	{ "CP12000", "decode", "SingleBytes", { 256, 0, 0, 256, 0x19C5BCF6F7DF0325ull } },
	{ "CP12000", "decode", "Utf32LeForms", { 5, 0, 1, 12, 0x3B846DD9A6EF5A7Eull } },
	{ "CP12000", "encode", "BmpUnits", { 65280, 0, 256, 261120, 0x71E201E42E49A525ull } },
	{ "CP12000", "encode", "SurrogatePairs", { 4, 0, 0, 16, 0x948F9723279A2749ull } },
	{ "CP12000", "encode", "TextSamples", { 3, 0, 0, 48, 0x4BFB82475022B274ull } },
	{ "CP12001", "decode", "SingleBytes", { 256, 0, 0, 256, 0x19C5BCF6F7DF0325ull } },
	{ "CP12001", "decode", "Utf32BeForms", { 6, 0, 0, 16, 0xF0E3AC2323B2DFBBull } },
	{ "CP12001", "encode", "BmpUnits", { 65280, 0, 256, 261120, 0x83D4B05D4E05FA25ull } },
	{ "CP12001", "encode", "SurrogatePairs", { 4, 0, 0, 16, 0x46AD4512F3183641ull } },
	{ "CP12001", "encode", "TextSamples", { 3, 0, 0, 48, 0xAFAF5DD792BC69C2ull } },
};

} // namespace codec_golden_corpus
