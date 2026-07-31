/*! @file @brief AVX-512F/BW CR/LF scanner isolated from baseline code generation. */
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
std::size_t FindCrOrLfAvx512(const char* data, std::size_t length) noexcept
{
	constexpr std::size_t vectorWidth = 64;
	constexpr std::size_t unalignedWarmupBytes = 512;
	const __m512i cr = _mm512_set1_epi8('\r');
	const __m512i lf = _mm512_set1_epi8('\n');
	std::size_t offset = 0;
	// Keep the common short-line path on unaligned loads; align only after a
	// delimiter-free warmup is long enough to amortize the masked prefix.
	if (length < unalignedWarmupBytes) {
		for (; offset + vectorWidth <= length; offset += vectorWidth) {
			const __m512i bytes = _mm512_loadu_si512(data + offset);
			const std::uint64_t mask =
				_mm512_cmpeq_epi8_mask(bytes, cr) | _mm512_cmpeq_epi8_mask(bytes, lf);
			if (mask != 0) {
				unsigned long index{};
#if defined(_MSC_VER)
				_BitScanForward64(&index, mask);
#else
				index = static_cast<unsigned long>(__builtin_ctzll(mask));
#endif
				return offset + index;
			}
		}
		return FindCrOrLfAvx2(data + offset, length - offset) + offset;
	}

	for (; offset < unalignedWarmupBytes; offset += vectorWidth) {
		const __m512i bytes = _mm512_loadu_si512(data + offset);
		const std::uint64_t mask =
			_mm512_cmpeq_epi8_mask(bytes, cr) | _mm512_cmpeq_epi8_mask(bytes, lf);
		if (mask != 0) {
			unsigned long index{};
#if defined(_MSC_VER)
			_BitScanForward64(&index, mask);
#else
			index = static_cast<unsigned long>(__builtin_ctzll(mask));
#endif
			return offset + index;
		}
	}

	const std::size_t misalignment =
		reinterpret_cast<std::uintptr_t>(data + offset) & (vectorWidth - 1);
	if (misalignment != 0) {
		const std::size_t prefixLength = vectorWidth - misalignment;
		if (prefixLength <= length - offset) {
			const __mmask64 prefixMask =
				(static_cast<__mmask64>(1) << prefixLength) - 1;
			const __m512i bytes = _mm512_maskz_loadu_epi8(prefixMask, data + offset);
			const std::uint64_t mask =
				_mm512_cmpeq_epi8_mask(bytes, cr) | _mm512_cmpeq_epi8_mask(bytes, lf);
			if (mask != 0) {
				unsigned long index{};
#if defined(_MSC_VER)
				_BitScanForward64(&index, mask);
#else
				index = static_cast<unsigned long>(__builtin_ctzll(mask));
#endif
				return offset + index;
			}
			offset += prefixLength;
		}
	}

	for (; offset + vectorWidth <= length; offset += vectorWidth) {
		const __m512i bytes = _mm512_load_si512(data + offset);
		const std::uint64_t mask =
			_mm512_cmpeq_epi8_mask(bytes, cr) | _mm512_cmpeq_epi8_mask(bytes, lf);
		if (mask != 0) {
			unsigned long index{};
#if defined(_MSC_VER)
			_BitScanForward64(&index, mask);
#else
			index = static_cast<unsigned long>(__builtin_ctzll(mask));
#endif
			return offset + index;
		}
	}
	return FindCrOrLfAvx2(data + offset, length - offset) + offset;
}
}
