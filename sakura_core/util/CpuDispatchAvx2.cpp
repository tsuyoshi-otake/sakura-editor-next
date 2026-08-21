/*! @file @brief AVX2 CR/LF scanner isolated from baseline code generation. */
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
#if !defined(SAKURA_UTF16_BACKEND_RUST)
// One 256-bit vector holds 16 UTF-16 units; below that only the scalar loop
// can run. Callers consult Utf16ScanPolicy before delegating this short.
constexpr std::size_t kUtf16VectorThreshold = 16;

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

[[nodiscard]] __m256i MatchMarkdownInlineSpecial(__m256i units) noexcept
{
	__m256i matches = _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'\\'));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'`')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'!')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'[')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'*')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'_')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'~')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'<')));
	matches = _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'&')));
	return _mm256_or_si256(matches, _mm256_cmpeq_epi16(units, _mm256_set1_epi16(L'$')));
}
#endif
}

std::size_t FindCrOrLfAvx2(const char* data, std::size_t length) noexcept
{
	constexpr std::size_t vectorWidth = 32;
	constexpr std::size_t unalignedWarmupBytes = 512;
	constexpr std::size_t unrolledWidth = vectorWidth * 2;
	const __m256i cr = _mm256_set1_epi8('\r');
	const __m256i lf = _mm256_set1_epi8('\n');
	std::size_t offset = 0;
	if (length < unalignedWarmupBytes) {
		for (; offset + vectorWidth <= length; offset += vectorWidth) {
			const __m256i bytes = _mm256_loadu_si256(
				reinterpret_cast<const __m256i*>(data + offset));
			const __m256i matches = _mm256_or_si256(
				_mm256_cmpeq_epi8(bytes, cr),
				_mm256_cmpeq_epi8(bytes, lf));
			const unsigned long mask =
				static_cast<unsigned long>(_mm256_movemask_epi8(matches));
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
		return FindCrOrLfAvx(data + offset, length - offset) + offset;
	}

	for (; offset < unalignedWarmupBytes; offset += vectorWidth) {
		const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + offset));
		const __m256i matches = _mm256_or_si256(
			_mm256_cmpeq_epi8(bytes, cr),
			_mm256_cmpeq_epi8(bytes, lf));
		const unsigned long mask = static_cast<unsigned long>(_mm256_movemask_epi8(matches));
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

	// Keep the loads unaligned: MSVC folds them into the comparisons, while an
	// alignment peel adds control flow without a cheaper AVX2 instruction.
	// After a long delimiter-free run, scan two vectors per iteration. Combining
	// the match vectors first keeps the no-match hot path to one movemask and
	// one branch per 64 bytes; an individual mask is needed only on a match.
	for (; length - offset >= unrolledWidth; offset += unrolledWidth) {
		const __m256i bytes0 = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset));
		const __m256i bytes1 = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset + vectorWidth));
		const __m256i matches0 = _mm256_or_si256(
			_mm256_cmpeq_epi8(bytes0, cr),
			_mm256_cmpeq_epi8(bytes0, lf));
		const __m256i matches1 = _mm256_or_si256(
			_mm256_cmpeq_epi8(bytes1, cr),
			_mm256_cmpeq_epi8(bytes1, lf));
		const unsigned long aggregateMask = static_cast<unsigned long>(
			_mm256_movemask_epi8(_mm256_or_si256(matches0, matches1)));
		if (aggregateMask != 0) {
			const unsigned long mask0 =
				static_cast<unsigned long>(_mm256_movemask_epi8(matches0));
			unsigned long index{};
			if (mask0 != 0) {
#if defined(_MSC_VER)
				_BitScanForward(&index, mask0);
#else
				index = static_cast<unsigned long>(__builtin_ctzl(mask0));
#endif
				return offset + index;
			}
#if defined(_MSC_VER)
			_BitScanForward(&index, aggregateMask);
#else
			index = static_cast<unsigned long>(__builtin_ctzl(aggregateMask));
#endif
			return offset + vectorWidth + index;
		}
	}

	for (; offset + vectorWidth <= length; offset += vectorWidth) {
		const __m256i bytes = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset));
		const __m256i matches = _mm256_or_si256(
			_mm256_cmpeq_epi8(bytes, cr),
			_mm256_cmpeq_epi8(bytes, lf));
		const unsigned long mask =
			static_cast<unsigned long>(_mm256_movemask_epi8(matches));
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
	return FindCrOrLfAvx(data + offset, length - offset) + offset;
}

#if !defined(SAKURA_UTF16_BACKEND_RUST)
std::size_t FindCrOrLfUtf16Avx2(const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	if (length < kUtf16VectorThreshold) {
		return FindUtf16Scalar<false>(data, length);
	}

	constexpr std::size_t vectorWidth = 16;
	const __m256i cr = _mm256_set1_epi16(L'\r');
	const __m256i lf = _mm256_set1_epi16(L'\n');
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m256i units = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset));
		const __m256i matches = _mm256_or_si256(
			_mm256_cmpeq_epi16(units, cr),
			_mm256_cmpeq_epi16(units, lf));
		const unsigned long mask =
			static_cast<unsigned long>(_mm256_movemask_epi8(matches));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	return offset + FindUtf16Scalar<false>(data + offset, length - offset);
}

std::size_t FindMarkdownInlineSpecialUtf16Avx2(
	const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	if (length < kUtf16VectorThreshold) {
		return FindUtf16Scalar<true>(data, length);
	}

	constexpr std::size_t vectorWidth = 16;
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m256i units = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset));
		const unsigned long mask = static_cast<unsigned long>(
			_mm256_movemask_epi8(MatchMarkdownInlineSpecial(units)));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	return offset + FindUtf16Scalar<true>(data + offset, length - offset);
}
#endif

std::size_t WidenAsciiToUtf16Avx2(
	const char* source, std::size_t length, wchar_t* destination) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The widening kernel requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 32;
	std::size_t offset = 0;
	for (; offset + vectorWidth <= length; offset += vectorWidth) {
		const __m256i bytes = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(source + offset));
		if (_mm256_movemask_epi8(bytes) != 0) {
			break;
		}
		// Both stores land entirely inside the returned ASCII run, so a
		// destination sized to the run is never overwritten past its end.
		_mm256_storeu_si256(
			reinterpret_cast<__m256i*>(destination + offset),
			_mm256_cvtepu8_epi16(_mm256_castsi256_si128(bytes)));
		_mm256_storeu_si256(
			reinterpret_cast<__m256i*>(destination + offset + vectorWidth / 2),
			_mm256_cvtepu8_epi16(_mm256_extracti128_si256(bytes, 1)));
	}
	for (; offset < length; ++offset) {
		const unsigned char byte = static_cast<unsigned char>(source[offset]);
		if (byte >= 0x80) {
			break;
		}
		destination[offset] = static_cast<wchar_t>(byte);
	}
	return offset;
}

#if !defined(SAKURA_UTF16_BACKEND_RUST)
std::size_t FindUtf16CharAvx2(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 16;
	const __m256i needle = _mm256_set1_epi16(static_cast<short>(target));
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m256i units = _mm256_loadu_si256(
			reinterpret_cast<const __m256i*>(data + offset));
		const unsigned long mask = static_cast<unsigned long>(
			_mm256_movemask_epi8(_mm256_cmpeq_epi16(units, needle)));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	for (; offset < length; ++offset) {
		if (data[offset] == target) {
			break;
		}
	}
	return offset;
}
#endif
}
