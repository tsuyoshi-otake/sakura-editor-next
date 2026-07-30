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
	const __m512i cr = _mm512_set1_epi8('\r');
	const __m512i lf = _mm512_set1_epi8('\n');
	std::size_t offset = 0;
	for (; offset + 64 <= length; offset += 64) {
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
}
