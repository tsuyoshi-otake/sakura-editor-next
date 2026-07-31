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
}
