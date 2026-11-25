/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "CUtf8TestAccess.h"
#include "Utf8CodecCorpus.h"

namespace {

using namespace utf8_codec_corpus;

std::wstring_view UnitsOf(const CNativeW& output)
{
	const auto length = static_cast<size_t>(output.GetStringLength());
	return length == 0 ? std::wstring_view{} : std::wstring_view{ output.GetStringPtr(), length };
}

std::string_view BytesOf(const CMemory& output)
{
	const auto length = static_cast<size_t>(output.GetRawLength());
	return length == 0 ? std::string_view{} : std::string_view{ reinterpret_cast<const char*>(output.GetRawPtr()), length };
}

// Visits every (case, mode) pair a row applies to, with the pair named in any failure it reports.
template <class Cases, class Body>
size_t ForEachApplicable(const Cases& cases, Body&& body)
{
	size_t visited = 0;
	for (const auto& testCase : cases) {
		for (const Mode mode : kModes) {
			if (Applies(testCase.variants, mode)) {
				SCOPED_TRACE(Describe(testCase.id, mode));
				body(testCase, mode);
				++visited;
			}
		}
	}
	return visited;
}

template <class Cases>
size_t ApplicablePairs(const Cases& cases)
{
	return static_cast<size_t>(std::ranges::count_if(cases, [](const auto& testCase) {
		return testCase.variants == Variants::Both;
	})) * kModes.size() + static_cast<size_t>(std::ranges::count_if(cases, [](const auto& testCase) {
		return testCase.variants != Variants::Both;
	}));
}

// The allocation-free production loops, run over caller-owned buffers that are reused between calls.
// Sweeps make millions of conversions and tests1 runs under full page heap, where one allocation per
// call would dominate. A guard of sentinel elements follows the documented output bound (one unit per
// input byte when decoding, three bytes per input unit when encoding), so a loop that writes past its
// bound is caught. The views a call returns stay valid until the next call of the same kind.
class RawLoops {
public:
	struct Decoded {
		const std::wstring_view units;
		const bool contained;
	};

	struct Encoded {
		const std::string_view bytes;
		const bool lossy;
		const bool contained;
	};

	Decoded Decode(std::string_view input, Mode mode)
	{
		m_units.assign(input.size() + kGuard, kUnitSentinel);
		const int length = CUtf8Access::DecodeForTest(
			input.data(), static_cast<int>(input.size()), m_units.data(), IsCesu8(mode));
		const bool contained = length >= 0 && static_cast<size_t>(length) <= input.size()
			&& GuardIntact(m_units, input.size(), kUnitSentinel);
		return { std::wstring_view{ m_units.data(), contained ? static_cast<size_t>(length) : 0 }, contained };
	}

	Encoded Encode(std::wstring_view input, Mode mode)
	{
		const size_t bound = input.size() * 3;
		m_bytes.assign(bound + kGuard, kByteSentinel);
		bool lossy = false;
		const int length = CUtf8Access::EncodeForTest(
			input.data(), static_cast<int>(input.size()), m_bytes.data(), &lossy, IsCesu8(mode));
		const bool contained = length >= 0 && static_cast<size_t>(length) <= bound
			&& GuardIntact(m_bytes, bound, kByteSentinel);
		return { std::string_view{ m_bytes.data(), contained ? static_cast<size_t>(length) : 0 }, lossy, contained };
	}

private:
	template <class Element>
	static bool GuardIntact(const std::vector<Element>& buffer, size_t bound, Element sentinel)
	{
		return std::all_of(buffer.begin() + static_cast<std::ptrdiff_t>(bound), buffer.end(),
			[sentinel](Element element) { return element == sentinel; });
	}

	static constexpr size_t kGuard = 8;
	static constexpr wchar_t kUnitSentinel = static_cast<wchar_t>(0xA5A5);
	static constexpr char kByteSentinel = static_cast<char>(0xA5);

	std::vector<wchar_t> m_units;
	std::vector<char> m_bytes;
};

// A sweep reports its first few mismatches and then only counts, so one wrong rule does not print a
// million lines.
class Mismatches {
public:
	void Report(const std::string& description)
	{
		++m_count;
		if (m_count <= kReported) {
			ADD_FAILURE() << description;
		}
	}

	size_t Count() const
	{
		return m_count;
	}

private:
	static constexpr size_t kReported = 8;
	size_t m_count = 0;
};

// One UTF-16 unit read as a three-byte form, and one supplementary scalar as a four-byte form.
std::string ThreeByteForm(unsigned unit)
{
	return {
		static_cast<char>(0xE0 | (unit >> 12)),
		static_cast<char>(0x80 | ((unit >> 6) & 0x3F)),
		static_cast<char>(0x80 | (unit & 0x3F)),
	};
}

std::string FourByteForm(unsigned scalar)
{
	return {
		static_cast<char>(0xF0 | (scalar >> 18)),
		static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)),
		static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)),
		static_cast<char>(0x80 | (scalar & 0x3F)),
	};
}

} // namespace

// The corpus freezes what the production converters CUtf8::Utf8ToUni and CUtf8::UniToUtf8 do today
// (specification, not refactoring). A candidate converter is compared against the same rows, so a row
// must never record anything production does not do: these tests prove every row against production,
// through the whole-buffer entry points the document code calls and through the raw loops they wrap.

TEST(Utf8CodecContract, CorpusIsWellFormedAndCoversTheRequiredClasses)
{
	const auto corpus = LoadCorpus();

	// Adding or removing a row is a deliberate change of these two numbers.
	EXPECT_EQ(130u, corpus.decode.size());
	EXPECT_EQ(68u, corpus.encode.size());

	std::set<std::string> decodeClasses;
	std::set<std::string> encodeClasses;
	std::set<Variants> variants;
	std::set<bool> lossy;
	for (const auto& testCase : corpus.decode) {
		decodeClasses.insert(testCase.cls);
		variants.insert(testCase.variants);
	}
	for (const auto& testCase : corpus.encode) {
		encodeClasses.insert(testCase.cls);
		variants.insert(testCase.variants);
		lossy.insert(testCase.lossy);
	}
	const std::set<std::string> expectedDecodeClasses{
		"empty", "ascii", "nul", "two-byte", "three-byte", "four-byte", "overlong", "out-of-range",
		"surrogate-encoding", "cesu8-pair", "cesu8-lone", "truncated", "bad-continuation",
		"lone-continuation", "resume", "mixed", "bom", "non-character",
	};
	const std::set<std::string> expectedEncodeClasses{
		"empty", "ascii", "bmp", "pair", "binary-low", "lone-low-lossy", "lone-high-lossy",
		"surrogate-edges", "mixed-lossy", "round-trip",
	};
	EXPECT_EQ(expectedDecodeClasses, decodeClasses);
	EXPECT_EQ(expectedEncodeClasses, encodeClasses);
	EXPECT_EQ((std::set<Variants>{ Variants::Both, Variants::Utf8, Variants::Cesu8 }), variants);
	EXPECT_EQ((std::set<bool>{ false, true }), lossy);
}

TEST(Utf8CodecContract, WholeBufferDecodeMatchesTheCorpusInBothModes)
{
	const auto corpus = LoadCorpus();
	const size_t compared = ForEachApplicable(corpus.decode, [](const DecodeCase& testCase, Mode mode) {
		const CMemory source(testCase.input.data(), testCase.input.size());
		CNativeW output;
		EXPECT_EQ(RESULT_COMPLETE, CUtf8::_UTF8ToUnicode(source, &output, IsCesu8(mode)));
		EXPECT_EQ(FormatUnits(testCase.expected), FormatUnits(UnitsOf(output)));
	});
	EXPECT_EQ(ApplicablePairs(corpus.decode), compared);
}

TEST(Utf8CodecContract, WholeBufferEncodeMatchesTheCorpusInBothModes)
{
	const auto corpus = LoadCorpus();
	const size_t compared = ForEachApplicable(corpus.encode, [](const EncodeCase& testCase, Mode mode) {
		const CNativeW source(testCase.input.data(), testCase.input.size());
		CMemory output;
		EXPECT_EQ(testCase.lossy ? RESULT_LOSESOME : RESULT_COMPLETE,
			CUtf8::_UnicodeToUTF8(source, &output, IsCesu8(mode)));
		EXPECT_EQ(FormatBytes(testCase.expected), FormatBytes(BytesOf(output)));
	});
	EXPECT_EQ(ApplicablePairs(corpus.encode), compared);
}

TEST(Utf8CodecContract, RawLoopsMatchTheCorpusWithinTheDocumentedOutputBounds)
{
	const auto corpus = LoadCorpus();
	RawLoops loops;
	const size_t decoded = ForEachApplicable(corpus.decode, [&](const DecodeCase& testCase, Mode mode) {
		const auto result = loops.Decode(testCase.input, mode);
		EXPECT_TRUE(result.contained) << "the decoder wrote past one unit per input byte";
		EXPECT_EQ(FormatUnits(testCase.expected), FormatUnits(result.units));
	});
	const size_t encoded = ForEachApplicable(corpus.encode, [&](const EncodeCase& testCase, Mode mode) {
		const auto result = loops.Encode(testCase.input, mode);
		EXPECT_TRUE(result.contained) << "the encoder wrote past three bytes per input unit";
		EXPECT_EQ(testCase.lossy, result.lossy);
		EXPECT_EQ(FormatBytes(testCase.expected), FormatBytes(result.bytes));
	});
	EXPECT_EQ(ApplicablePairs(corpus.decode), decoded);
	EXPECT_EQ(ApplicablePairs(corpus.encode), encoded);
}

// Every byte that does not start a complete valid sequence becomes one binary unit, and a binary unit
// is written back as the byte it came from, so nothing a decoder reads is lost. This is the property
// that lets a candidate decoder report no loss at all.
TEST(Utf8CodecContract, EveryByteAndBytePairDecodesWithinTheBoundAndRoundTrips)
{
	RawLoops loops;
	Mismatches mismatches;
	size_t examined = 0;
	std::string input;
	for (const Mode mode : kModes) {
		for (unsigned first = 0; first < 0x100; ++first) {
			// second == 0x100 stands for the single byte on its own.
			for (unsigned second = 0; second <= 0x100; ++second) {
				input.assign(1, static_cast<char>(first));
				if (second < 0x100) {
					input.push_back(static_cast<char>(second));
				}
				++examined;
				const auto decoded = loops.Decode(input, mode);
				const auto encoded = loops.Encode(decoded.units, mode);
				if (!decoded.contained || !encoded.contained || encoded.lossy || encoded.bytes != input) {
					mismatches.Report(std::string(Name(mode)) + ": " + FormatBytes(input) + " -> "
						+ FormatUnits(decoded.units) + " -> " + FormatBytes(encoded.bytes));
				}
			}
		}
	}
	EXPECT_EQ(2u * 256u * 257u, examined);
	EXPECT_EQ(0u, mismatches.Count());
}

// A high surrogate followed by any low surrogate, including one in the binary range U+DC80..U+DCFF, is
// a pair: UTF-8 writes the scalar as one four-byte form, CESU-8 writes the two units as two three-byte
// forms.
TEST(Utf8CodecContract, EveryValidSurrogatePairEncodesAsTheScalarOrTheTwoThreeByteForms)
{
	RawLoops loops;
	Mismatches mismatches;
	size_t examined = 0;
	std::wstring pair(2, L'\0');
	for (unsigned high = 0xD800; high <= 0xDBFF; ++high) {
		const std::string highForm = ThreeByteForm(high);
		for (unsigned low = 0xDC00; low <= 0xDFFF; ++low) {
			pair[0] = static_cast<wchar_t>(high);
			pair[1] = static_cast<wchar_t>(low);
			const unsigned scalar = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
			const std::string utf8 = FourByteForm(scalar);
			const std::string cesu8 = highForm + ThreeByteForm(low);
			for (const Mode mode : kModes) {
				++examined;
				const auto encoded = loops.Encode(pair, mode);
				if (!encoded.contained || encoded.lossy || encoded.bytes != (IsCesu8(mode) ? cesu8 : utf8)) {
					mismatches.Report(std::string(Name(mode)) + ": " + FormatUnits(pair) + " -> "
						+ FormatBytes(encoded.bytes));
				}
			}
		}
	}
	EXPECT_EQ(1024u * 1024u * 2u, examined);
	EXPECT_EQ(0u, mismatches.Count());
}

// A surrogate that is not half of a pair is a binary unit when it lies in U+DC00..U+DCFF (it becomes
// its low eight bits as one byte, without loss); any other one becomes '?' and the conversion reports
// loss. Neighbouring ASCII units never change that.
TEST(Utf8CodecContract, EveryLoneSurrogateEncodesAsItsBinaryByteOrAQuestionMarkWithLoss)
{
	RawLoops loops;
	Mismatches mismatches;
	size_t examined = 0;
	for (const Mode mode : kModes) {
		for (unsigned unit = 0xD800; unit <= 0xDFFF; ++unit) {
			const bool binary = unit >= 0xDC00 && unit <= 0xDCFF;
			const std::string expected(1, binary ? static_cast<char>(unit & 0xFF) : '?');
			const wchar_t lone = static_cast<wchar_t>(unit);
			const std::array<std::wstring, 3> inputs{
				std::wstring{ lone },
				std::wstring{ L'A', lone },
				std::wstring{ lone, L'A' },
			};
			const std::array<std::string, 3> outputs{ expected, "A" + expected, expected + "A" };
			for (size_t shape = 0; shape < inputs.size(); ++shape) {
				++examined;
				const auto encoded = loops.Encode(inputs[shape], mode);
				if (!encoded.contained || encoded.lossy == binary || encoded.bytes != outputs[shape]) {
					mismatches.Report(std::string(Name(mode)) + ": " + FormatUnits(inputs[shape]) + " -> "
						+ FormatBytes(encoded.bytes) + (encoded.lossy ? " (lossy)" : ""));
				}
			}
		}
	}
	EXPECT_EQ(2u * 2048u * 3u, examined);
	EXPECT_EQ(0u, mismatches.Count());
}
