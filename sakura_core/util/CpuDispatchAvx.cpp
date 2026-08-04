/*! @file @brief AVX-baseline CR/LF scanner. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "util/CpuDispatchInternal.h"

#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace CpuDispatch::Internal
{
namespace
{
constexpr std::size_t kUtf16VectorThreshold = 64;

[[nodiscard]] bool IsMarkdownInlineSpecial(wchar_t value) noexcept
{
	switch (value) {
	case L'\\':
	case L'`':
	case L'!':
	case L'[':
	case L'*':
	case L'_':
	case L'~':
	case L'<':
	case L'&':
	case L'$':
		return true;
	default:
		return false;
	}
}

template <bool MarkdownInlineSpecial>
std::size_t FindUtf16Scalar(const wchar_t* data, std::size_t length) noexcept
{
	std::size_t offset = 0;
	for (; offset < length; ++offset) {
		const wchar_t value = data[offset];
		if constexpr (MarkdownInlineSpecial) {
			if (IsMarkdownInlineSpecial(value)) {
				break;
			}
		} else if (value == L'\r' || value == L'\n') {
			break;
		}
	}
	return offset;
}

[[nodiscard]] unsigned long FirstUtf16Lane(unsigned long byteMask) noexcept
{
	unsigned long byteIndex{};
#if defined(_MSC_VER)
	_BitScanForward(&byteIndex, byteMask);
#else
	byteIndex = static_cast<unsigned long>(__builtin_ctzl(byteMask));
#endif
	return byteIndex / sizeof(wchar_t);
}

[[nodiscard]] __m128i MatchMarkdownInlineSpecial(__m128i units) noexcept
{
	__m128i matches = _mm_cmpeq_epi16(units, _mm_set1_epi16(L'\\'));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'`')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'!')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'[')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'*')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'_')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'~')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'<')));
	matches = _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'&')));
	return _mm_or_si128(matches, _mm_cmpeq_epi16(units, _mm_set1_epi16(L'$')));
}
}

std::size_t FindCrOrLfAvx(const char* data, std::size_t length) noexcept
{
	constexpr std::size_t vectorWidth = 16;
	constexpr std::size_t unalignedWarmupBytes = 512;
	const __m128i cr = _mm_set1_epi8('\r');
	const __m128i lf = _mm_set1_epi8('\n');
	std::size_t offset = 0;
	if (length < unalignedWarmupBytes) {
		for (; offset + vectorWidth <= length; offset += vectorWidth) {
			const __m128i bytes = _mm_loadu_si128(
				reinterpret_cast<const __m128i*>(data + offset));
			const __m128i matches = _mm_or_si128(
				_mm_cmpeq_epi8(bytes, cr),
				_mm_cmpeq_epi8(bytes, lf));
			const unsigned long mask = static_cast<unsigned long>(_mm_movemask_epi8(matches));
			if (mask != 0) {
				unsigned long index{};
#if defined(_MSC_VER)
				_BitScanForward(&index, mask);
#else
				index = static_cast<unsigned long>(__builtin_ctzl(mask));
#endif
				return offset + index;
			}
		}
		for (; offset < length; ++offset) {
			if (data[offset] == '\r' || data[offset] == '\n') {
				break;
			}
		}
		return offset;
	}

	for (; offset < unalignedWarmupBytes; offset += vectorWidth) {
		const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + offset));
		const __m128i matches = _mm_or_si128(
			_mm_cmpeq_epi8(bytes, cr),
			_mm_cmpeq_epi8(bytes, lf));
		const unsigned long mask = static_cast<unsigned long>(_mm_movemask_epi8(matches));
		if (mask != 0) {
			unsigned long index{};
#if defined(_MSC_VER)
			_BitScanForward(&index, mask);
#else
			index = static_cast<unsigned long>(__builtin_ctzl(mask));
#endif
			return offset + index;
		}
	}

	// The AVX-only path uses 16-byte integer vectors. Pay the scalar alignment
	// prefix only after a long delimiter-free run and only when an aligned
	// vector remains to amortize it.
	const std::size_t misalignment =
		reinterpret_cast<std::uintptr_t>(data + offset) & (vectorWidth - 1);
	if (misalignment != 0) {
		const std::size_t prefixLength = vectorWidth - misalignment;
		if (prefixLength + vectorWidth <= length - offset) {
			const std::size_t prefixEnd = offset + prefixLength;
			for (; offset < prefixEnd; ++offset) {
				if (data[offset] == '\r' || data[offset] == '\n') {
					return offset;
				}
			}
		}
	}

	if ((reinterpret_cast<std::uintptr_t>(data + offset) & (vectorWidth - 1)) == 0) {
		for (; offset + vectorWidth <= length; offset += vectorWidth) {
			const __m128i bytes = _mm_load_si128(
				reinterpret_cast<const __m128i*>(data + offset));
			const __m128i matches = _mm_or_si128(
				_mm_cmpeq_epi8(bytes, cr),
				_mm_cmpeq_epi8(bytes, lf));
			const unsigned long mask = static_cast<unsigned long>(_mm_movemask_epi8(matches));
			if (mask != 0) {
				unsigned long index{};
#if defined(_MSC_VER)
				_BitScanForward(&index, mask);
#else
				index = static_cast<unsigned long>(__builtin_ctzl(mask));
#endif
				return offset + index;
			}
		}
	} else {
		for (; offset + vectorWidth <= length; offset += vectorWidth) {
			const __m128i bytes = _mm_loadu_si128(
				reinterpret_cast<const __m128i*>(data + offset));
			const __m128i matches = _mm_or_si128(
				_mm_cmpeq_epi8(bytes, cr),
				_mm_cmpeq_epi8(bytes, lf));
			const unsigned long mask = static_cast<unsigned long>(_mm_movemask_epi8(matches));
			if (mask != 0) {
				unsigned long index{};
#if defined(_MSC_VER)
				_BitScanForward(&index, mask);
#else
				index = static_cast<unsigned long>(__builtin_ctzl(mask));
#endif
				return offset + index;
			}
		}
	}
	for (; offset < length; ++offset) {
		if (data[offset] == '\r' || data[offset] == '\n') {
			break;
		}
	}
	return offset;
}

std::size_t FindCrOrLfUtf16Avx(const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	if (length < kUtf16VectorThreshold) {
		return FindUtf16Scalar<false>(data, length);
	}

	constexpr std::size_t vectorWidth = 8;
	const __m128i cr = _mm_set1_epi16(L'\r');
	const __m128i lf = _mm_set1_epi16(L'\n');
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m128i units = _mm_loadu_si128(
			reinterpret_cast<const __m128i*>(data + offset));
		const __m128i matches = _mm_or_si128(
			_mm_cmpeq_epi16(units, cr),
			_mm_cmpeq_epi16(units, lf));
		const unsigned long mask = static_cast<unsigned long>(_mm_movemask_epi8(matches));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	return offset + FindUtf16Scalar<false>(data + offset, length - offset);
}

std::size_t FindMarkdownInlineSpecialUtf16Avx(
	const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	if (length < kUtf16VectorThreshold) {
		return FindUtf16Scalar<true>(data, length);
	}

	constexpr std::size_t vectorWidth = 8;
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m128i units = _mm_loadu_si128(
			reinterpret_cast<const __m128i*>(data + offset));
		const unsigned long mask = static_cast<unsigned long>(
			_mm_movemask_epi8(MatchMarkdownInlineSpecial(units)));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	return offset + FindUtf16Scalar<true>(data + offset, length - offset);
}
}
