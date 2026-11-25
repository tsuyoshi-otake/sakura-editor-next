/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "charset/CCodeFactory.h"

#include "CodecGoldenCorpus.h"

namespace {

using namespace codec_golden_corpus;
using namespace std::string_view_literals;

struct Codec {
	const std::string_view name;
	const ECodeType type;
	const int flags = 0;
};

constexpr std::array kCodecs{
	Codec{ "SJIS", CODE_SJIS },
	Codec{ "EUC", CODE_EUC },
	Codec{ "JIS-0", CODE_JIS, 0 },
	Codec{ "JIS-1", CODE_JIS, 1 },
	Codec{ "UTF8", CODE_UTF8 },
	Codec{ "CESU8", CODE_CESU8 },
	Codec{ "UTF7", CODE_UTF7 },
	Codec{ "UTF16LE", CODE_UNICODE },
	Codec{ "UTF16BE", CODE_UNICODEBE },
	Codec{ "Latin1", CODE_LATIN1 },
	Codec{ "CP932", static_cast<ECodeType>(932) },
	Codec{ "CP1252", static_cast<ECodeType>(1252) },
	Codec{ "CP12000", CODE_UTF32LE },
	Codec{ "CP12001", CODE_UTF32BE },
};

std::wstring_view OutputView(const CNativeW& output)
{
	const auto length = static_cast<size_t>(output.GetStringLength());
	return length == 0 ? std::wstring_view{} : std::wstring_view{ output.GetStringPtr(), length };
}

std::string_view OutputView(const CMemory& output)
{
	const auto length = static_cast<size_t>(output.GetRawLength());
	return length == 0 ? std::string_view{} : std::string_view{ reinterpret_cast<const char*>(output.GetRawPtr()), length };
}

void AppendDecodeRows(const Codec& variant, CCodeBase& codec, std::vector<GoldenRow>& rows)
{
	const auto measure = [&](std::string_view group, const auto& inputs) {
		SummaryBuilder summary;
		for (const auto& input : inputs) {
			const CMemory source(input.data(), input.size());
			CNativeW output;
			const auto result = codec.CodeToUnicode(source, &output);
			summary.Add(result, OutputView(output));
		}
		rows.push_back({ variant.name, "decode", group, summary.Build() });
	};

	measure("SingleBytes", SingleBytes());
	switch (variant.type) {
	case CODE_SJIS:
	case CODE_EUC:
	case static_cast<ECodeType>(932):
		measure("HighBytePairs", HighBytePairs());
		measure("TruncatedHighByteTails", TruncatedHighByteTails());
		break;
	case CODE_JIS:
		measure("JisEscapes", kJisEscapes);
		measure("JisMimeHeaders", kJisMimeHeaders);
		break;
	case CODE_UTF8:
	case CODE_CESU8:
		measure("Utf8Forms", kUtf8Forms);
		break;
	case CODE_UTF7:
		measure("Utf7Base64Runs", kUtf7Base64Runs);
		break;
	case CODE_UNICODE:
		measure("Utf16LeForms", kUtf16LeForms);
		break;
	case CODE_UNICODEBE:
		measure("Utf16BeForms", kUtf16BeForms);
		break;
	case CODE_UTF32LE:
		measure("Utf32LeForms", kUtf32LeForms);
		break;
	case CODE_UTF32BE:
		measure("Utf32BeForms", kUtf32BeForms);
		break;
	default:
		break;
	}
}

void AppendEncodeRows(const Codec& variant, CCodeBase& codec, std::vector<GoldenRow>& rows)
{
	const auto measure = [&](std::string_view group, const auto& inputs) {
		SummaryBuilder summary;
		for (const auto& input : inputs) {
			const CNativeW source(input.data(), input.size());
			CMemory output;
			const auto result = codec.UnicodeToCode(source, &output);
			summary.Add(result, OutputView(output));
		}
		rows.push_back({ variant.name, "encode", group, summary.Build() });
	};

	measure("BmpUnits", BmpUnits());
	measure("SurrogatePairs", kSurrogatePairs);
	measure("TextSamples", kTextSamples);
}

std::string FormatRow(const GoldenRow& row)
{
	const auto& summary = row.summary;
	return std::format("\t{{ \"{}\", \"{}\", \"{}\", {{ {}, {}, {}, {}, 0x{:016X}ull }} }},\n",
		row.codec, row.direction, row.group, summary.complete, summary.losesome,
		summary.failure, summary.totalLength, summary.digest);
}

TEST(CodecGoldenCorpus, MatchesRecordedRows)
{
	constexpr wchar_t kRecordVariable[] = L"SAKURA_CODEC_GOLDEN_RECORD";
	::SetLastError(ERROR_SUCCESS);
	const DWORD required = ::GetEnvironmentVariableW(kRecordVariable, nullptr, 0);
	std::wstring recordPath;
	if (required == 0) {
		const DWORD error = ::GetLastError();
		ASSERT_EQ(static_cast<DWORD>(ERROR_ENVVAR_NOT_FOUND), error)
			<< "record mode: SAKURA_CODEC_GOLDEN_RECORD must contain a file path";
	} else {
		recordPath.resize(required);
		const DWORD length = ::GetEnvironmentVariableW(kRecordVariable, recordPath.data(), required);
		ASSERT_GT(length, 0u) << "record mode: SAKURA_CODEC_GOLDEN_RECORD must contain a file path";
		ASSERT_LT(length, required) << "record mode: SAKURA_CODEC_GOLDEN_RECORD changed during lookup";
		recordPath.resize(length);
	}

	std::vector<GoldenRow> rows;
	for (const auto& variant : kCodecs) {
		SCOPED_TRACE(variant.name);
		if (variant.type >= CODE_CODEMAX) {
			ASSERT_TRUE(IsValidCodePageEx(variant.type))
				<< "Required explicit code page " << static_cast<int>(variant.type) << " is unavailable";
		}
		const std::unique_ptr<CCodeBase> codec(CCodeFactory::CreateCodeBase(variant.type, variant.flags));
		ASSERT_NE(codec, nullptr) << "CCodeFactory could not create the required codec";
		AppendDecodeRows(variant, *codec, rows);
		AppendEncodeRows(variant, *codec, rows);
	}

	if (!recordPath.empty()) {
		std::ofstream output(std::filesystem::path(recordPath), std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(output.is_open()) << "record mode: could not open the output file";
		for (const auto& row : rows) {
			output << FormatRow(row);
		}
		output.close();
		ASSERT_TRUE(output.good()) << "record mode: could not write the output file";
		FAIL() << "record mode: wrote " << rows.size() << " golden rows";
	}

	ASSERT_EQ(kExpectedRows.size(), rows.size()) << "Golden table row count differs";
	auto expected = kExpectedRows.begin();
	for (const auto& actual : rows) {
		SCOPED_TRACE(FormatRow(actual));
		EXPECT_EQ(expected->codec, actual.codec);
		EXPECT_EQ(expected->direction, actual.direction);
		EXPECT_EQ(expected->group, actual.group);
		EXPECT_EQ(expected->summary.complete, actual.summary.complete);
		EXPECT_EQ(expected->summary.losesome, actual.summary.losesome);
		EXPECT_EQ(expected->summary.failure, actual.summary.failure);
		EXPECT_EQ(expected->summary.totalLength, actual.summary.totalLength);
		EXPECT_EQ(expected->summary.digest, actual.summary.digest);
		++expected;
	}
}

TEST(CodecGoldenCorpus, DecodesLiteralVectors)
{
	const struct {
		Codec codec;
		std::string_view input;
		std::wstring_view expected;
		EConvertResult result;
	} vectors[]{
		{ { "SJIS hiragana", CODE_SJIS }, "\x82\xA0"sv, L"\u3042"sv, RESULT_COMPLETE },
		{ { "SJIS undecodable byte", CODE_SJIS }, "\x80"sv, L"\xDC80"sv, RESULT_COMPLETE },
		{ { "SJIS truncated lead", CODE_SJIS }, "A\x82"sv, L"A\xDC82"sv, RESULT_COMPLETE },
		{ { "UTF8 overlong NUL", CODE_UTF8 }, "\xC0\x80"sv, L"\xDCC0\xDC80"sv, RESULT_COMPLETE },
		{ { "UTF8 four-byte scalar", CODE_UTF8 }, "\xF0\x9F\x98\x80"sv, L"\xD83D\xDE00"sv, RESULT_COMPLETE },
		{ { "CESU8 surrogate pair", CODE_CESU8 }, "\xED\xA0\xBD\xED\xB8\x80"sv, L"\xD83D\xDE00"sv, RESULT_COMPLETE },
		{ { "JIS MIME on", CODE_JIS, 1 }, kMimeHeader, L"From: \u30B5\u30AF\u30E9"sv, RESULT_COMPLETE },
		{ { "JIS MIME off", CODE_JIS, 0 }, kMimeHeader, L"From: =?iso-2022-jp?B?GyRCJTUlLyVpGyhC?="sv, RESULT_COMPLETE },
		{ { "UTF16LE odd length", CODE_UNICODE }, "\x42\x30\x41"sv, L"\u3042A"sv, RESULT_LOSESOME },
		{ { "UTF16BE isolated surrogate", CODE_UNICODEBE }, "\xD8\x00"sv, L"\xD800"sv, RESULT_COMPLETE },
	};

	for (const auto& item : vectors) {
		SCOPED_TRACE(item.codec.name);
		const std::unique_ptr<CCodeBase> codec(CCodeFactory::CreateCodeBase(item.codec.type, item.codec.flags));
		ASSERT_NE(codec, nullptr);
		const CMemory source(item.input.data(), item.input.size());
		CNativeW output;
		EXPECT_EQ(item.result, codec->CodeToUnicode(source, &output));
		EXPECT_EQ(item.expected, OutputView(output));
	}
}

TEST(CodecGoldenCorpus, EncodesSjisBinaryMappedUnit)
{
	const auto codec = CCodeFactory::CreateCodeBase(CODE_SJIS);
	ASSERT_NE(codec, nullptr);
	const CNativeW source(L"\xDC80", 1);
	CMemory output;
	EXPECT_EQ(RESULT_COMPLETE, codec->UnicodeToCode(source, &output));
	EXPECT_EQ("\x80"sv, OutputView(output));
}

} // namespace
