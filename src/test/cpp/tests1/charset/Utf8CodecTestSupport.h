/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "CUtf8TestAccess.h"
#include "Utf8CodecCorpus.h"
#include "mem/CMemory.h"
#include "mem/CNativeW.h"

// Shared helpers of the UTF-8 / CESU-8 suites (#443): views over what a converter produced, the walk over the
// corpus rows, the production loops run over reused guarded buffers, and the mismatch counter of the sweeps.
// They live in utf8_codec_corpus next to the corpus loader, as the encoded-EOL helpers do in encoded_eol_corpus.

namespace utf8_codec_corpus {

inline std::wstring_view UnitsOf(const CNativeW& output)
{
	const auto length = static_cast<size_t>(output.GetStringLength());
	return length == 0 ? std::wstring_view{} : std::wstring_view{ output.GetStringPtr(), length };
}

inline std::string_view BytesOf(const CMemory& output)
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

} // namespace utf8_codec_corpus
