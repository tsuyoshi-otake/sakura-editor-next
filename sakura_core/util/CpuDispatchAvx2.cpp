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
	const __m256i cr = _mm256_set1_epi8('\r');
	const __m256i lf = _mm256_set1_epi8('\n');
	std::size_t offset = 0;
	for (; offset + 32 <= length; offset += 32) {
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
	return FindCrOrLfAvx(data + offset, length - offset) + offset;
}
}
