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
std::size_t FindCrOrLfAvx(const char* data, std::size_t length) noexcept
{
	const __m128i cr = _mm_set1_epi8('\r');
	const __m128i lf = _mm_set1_epi8('\n');
	std::size_t offset = 0;
	for (; offset + 16 <= length; offset += 16) {
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
	for (; offset < length; ++offset) {
		if (data[offset] == '\r' || data[offset] == '\n') {
			break;
		}
	}
	return offset;
}
}
