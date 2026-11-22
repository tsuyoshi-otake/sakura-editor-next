/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "UnicodeLowercase.h"
#include "UnicodeLowercaseData.h"
#include "UriIdentityInternal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace platform::uri::internal {
namespace {

using unicode_lowercase_data::kCased;
using unicode_lowercase_data::kCaseIgnorable;
using unicode_lowercase_data::kSimpleLowercase;
using unicode_lowercase_data::kSpecialLowercase;

static_assert(sizeof(wchar_t) == 2);

constexpr bool IsHighSurrogate(std::uint32_t value) noexcept
{
	return value >= 0xD800 && value <= 0xDBFF;
}

constexpr bool IsLowSurrogate(std::uint32_t value) noexcept
{
	return value >= 0xDC00 && value <= 0xDFFF;
}

constexpr std::uint32_t DecodePair(std::uint32_t high, std::uint32_t low) noexcept
{
	return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
}

std::uint32_t DecodeNext(std::wstring_view utf16, std::size_t& index) noexcept
{
	const auto first = static_cast<std::uint16_t>(utf16[index++]);
	if (IsHighSurrogate(first) && index < utf16.size()) {
		const auto second = static_cast<std::uint16_t>(utf16[index]);
		if (IsLowSurrogate(second)) {
			++index;
			return DecodePair(first, second);
		}
	}
	return first;
}

std::uint32_t DecodePrevious(std::wstring_view utf16, std::size_t& index) noexcept
{
	const auto last = static_cast<std::uint16_t>(utf16[--index]);
	if (IsLowSurrogate(last) && index != 0) {
		const auto previous = static_cast<std::uint16_t>(utf16[index - 1]);
		if (IsHighSurrogate(previous)) {
			--index;
			return DecodePair(previous, last);
		}
	}
	return last;
}

void AppendScalar(std::wstring& output, std::uint32_t value)
{
	if (value <= 0xFFFF) {
		output.push_back(static_cast<wchar_t>(value));
	} else {
		value -= 0x10000;
		output.push_back(static_cast<wchar_t>(0xD800 + (value >> 10)));
		output.push_back(static_cast<wchar_t>(0xDC00 + (value & 0x3FF)));
	}
}

template <std::size_t Rows, std::size_t Columns>
const std::uint32_t* FindMapping(const std::uint32_t (&table)[Rows][Columns], std::uint32_t scalar) noexcept
{
	const auto end = table + Rows;
	const auto found = std::lower_bound(table, end, scalar,
		[](const auto& row, std::uint32_t value) { return row[0] < value; });
	return found != end && (*found)[0] == scalar ? *found : nullptr;
}

template <std::size_t Rows>
bool InRanges(const std::uint32_t (&table)[Rows][2], std::uint32_t scalar) noexcept
{
	const auto found = std::upper_bound(table, table + Rows, scalar,
		[](std::uint32_t value, const auto& row) { return value < row[0]; });
	return found != table && scalar <= found[-1][1];
}

bool IsFinalSigma(std::wstring_view utf16, std::size_t before, std::size_t after) noexcept
{
	// Total work is O(n) because U+03A3 is Cased and not Case_Ignorable, so each scan stops at a neighbouring sigma.
	bool precededByCased = false;
	while (before != 0) {
		const auto scalar = DecodePrevious(utf16, before);
		if (!InRanges(kCaseIgnorable, scalar)) {
			precededByCased = InRanges(kCased, scalar);
			break;
		}
	}
	if (!precededByCased) {
		return false;
	}
	while (after < utf16.size()) {
		const auto scalar = DecodeNext(utf16, after);
		if (!InRanges(kCaseIgnorable, scalar)) {
			return !InRanges(kCased, scalar);
		}
	}
	return true;
}

} // namespace

std::wstring ToUnicodeLowercase(std::wstring_view utf16)
{
	std::wstring result;
	result.reserve(utf16.size());
	for (std::size_t index = 0; index < utf16.size();) {
		const auto first = static_cast<std::uint16_t>(utf16[index]);
		if (first <= 0x7F) {
			result.push_back(ToLowerAscii(utf16[index++]));
			continue;
		}

		const auto start = index;
		const auto scalar = DecodeNext(utf16, index);
		if (scalar == 0x03A3 && IsFinalSigma(utf16, start, index)) {
			AppendScalar(result, 0x03C2);
		} else if (const auto* special = FindMapping(kSpecialLowercase, scalar)) {
			for (std::uint32_t offset = 0; offset < special[1]; ++offset) {
				AppendScalar(result, special[offset + 2]);
			}
		} else {
			const auto* simple = FindMapping(kSimpleLowercase, scalar);
			AppendScalar(result, simple ? simple[1] : scalar);
		}
	}
	return result;
}

} // namespace platform::uri::internal
