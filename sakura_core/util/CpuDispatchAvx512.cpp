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
namespace
{
[[nodiscard]] unsigned long FirstUtf16Lane(std::uint32_t laneMask) noexcept
{
	unsigned long index{};
#if defined(_MSC_VER)
	_BitScanForward(&index, static_cast<unsigned long>(laneMask));
#else
	index = static_cast<unsigned long>(__builtin_ctz(laneMask));
#endif
	return index;
}

// Low `count` bits set. A width-exact shift (`1 << 32`) is undefined
// behavior, so both edges are handled explicitly.
[[nodiscard]] __mmask32 LowBitsMask32(std::size_t count) noexcept
{
	if (count >= 32) {
		return static_cast<__mmask32>(0xffffffffu);
	}
	return static_cast<__mmask32>((std::uint32_t{1} << count) - 1u);
}

[[nodiscard]] __mmask64 LowBitsMask64(std::size_t count) noexcept
{
	if (count >= 64) {
		return static_cast<__mmask64>(~std::uint64_t{0});
	}
	return static_cast<__mmask64>((std::uint64_t{1} << count) - 1u);
}

[[nodiscard]] unsigned long FirstByteLane(std::uint64_t byteMask) noexcept
{
	unsigned long index{};
#if defined(_MSC_VER)
	_BitScanForward64(&index, byteMask);
#else
	index = static_cast<unsigned long>(__builtin_ctzll(byteMask));
#endif
	return index;
}

// Widens the low `count` bytes of one loaded vector. The masked stores write
// exactly `count` UTF-16 units, so a destination sized to the ASCII run is
// never touched past its end.
std::size_t WidenAsciiPartial(__m512i bytes, std::size_t count, wchar_t* destination) noexcept
{
	_mm512_mask_storeu_epi16(
		destination,
		LowBitsMask32(count),
		_mm512_cvtepu8_epi16(_mm512_castsi512_si256(bytes)));
	if (count > 32) {
		_mm512_mask_storeu_epi16(
			destination + 32,
			LowBitsMask32(count - 32),
			_mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(bytes, 1)));
	}
	return count;
}

[[nodiscard]] __mmask32 MatchMarkdownInlineSpecial(__m512i units) noexcept
{
	__mmask32 matches = _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'\\'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'`'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'!'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'['));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'*'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'_'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'~'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'<'));
	matches |= _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'&'));
	return matches | _mm512_cmpeq_epi16_mask(units, _mm512_set1_epi16(L'$'));
}
}

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

std::size_t FindCrOrLfUtf16Avx512(const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 32;
	const __m512i cr = _mm512_set1_epi16(L'\r');
	const __m512i lf = _mm512_set1_epi16(L'\n');
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m512i units = _mm512_loadu_si512(data + offset);
		const std::uint32_t mask = static_cast<std::uint32_t>(
			_mm512_cmpeq_epi16_mask(units, cr) | _mm512_cmpeq_epi16_mask(units, lf));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	// The fault-suppressed masked load covers the 1..31-unit tail (and short
	// inputs entirely) without touching bytes past `length`. Invalid lanes are
	// excluded from the match mask by contract even though zeroed lanes cannot
	// equal CR or LF.
	const std::size_t remaining = length - offset;
	if (remaining == 0) {
		return length;
	}
	const __mmask32 valid = LowBitsMask32(remaining);
	const __m512i units = _mm512_maskz_loadu_epi16(valid, data + offset);
	const std::uint32_t mask = static_cast<std::uint32_t>(
		(_mm512_cmpeq_epi16_mask(units, cr) | _mm512_cmpeq_epi16_mask(units, lf))
		& valid);
	return mask != 0 ? offset + FirstUtf16Lane(mask) : length;
}

std::size_t FindMarkdownInlineSpecialUtf16Avx512(
	const wchar_t* data, std::size_t length) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 32;
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m512i units = _mm512_loadu_si512(data + offset);
		const std::uint32_t mask =
			static_cast<std::uint32_t>(MatchMarkdownInlineSpecial(units));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	const std::size_t remaining = length - offset;
	if (remaining == 0) {
		return length;
	}
	const __mmask32 valid = LowBitsMask32(remaining);
	const __m512i units = _mm512_maskz_loadu_epi16(valid, data + offset);
	const std::uint32_t mask = static_cast<std::uint32_t>(
		MatchMarkdownInlineSpecial(units) & valid);
	return mask != 0 ? offset + FirstUtf16Lane(mask) : length;
}

std::size_t WidenAsciiToUtf16Avx512(
	const char* source, std::size_t length, wchar_t* destination) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The widening kernel requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 64;
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const __m512i bytes = _mm512_loadu_si512(source + offset);
		const std::uint64_t nonAscii =
			static_cast<std::uint64_t>(_mm512_movepi8_mask(bytes));
		if (nonAscii != 0) {
			return offset
				+ WidenAsciiPartial(bytes, FirstByteLane(nonAscii), destination + offset);
		}
		_mm512_storeu_si512(
			destination + offset,
			_mm512_cvtepu8_epi16(_mm512_castsi512_si256(bytes)));
		_mm512_storeu_si512(
			destination + offset + vectorWidth / 2,
			_mm512_cvtepu8_epi16(_mm512_extracti64x4_epi64(bytes, 1)));
	}
	// The fault-suppressed masked load covers the 1..63-byte tail (and short
	// inputs entirely) without touching bytes past `length`. Masked-off lanes
	// load as zero and can never raise the sign bit, so the non-ASCII mask
	// needs no additional clamp.
	const std::size_t remaining = length - offset;
	if (remaining == 0) {
		return offset;
	}
	const __m512i bytes = _mm512_maskz_loadu_epi8(LowBitsMask64(remaining), source + offset);
	const std::uint64_t nonAscii = static_cast<std::uint64_t>(_mm512_movepi8_mask(bytes));
	const std::size_t run = nonAscii != 0 ? FirstByteLane(nonAscii) : remaining;
	return offset + WidenAsciiPartial(bytes, run, destination + offset);
}

std::size_t FindUtf16CharAvx512(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	static_assert(sizeof(wchar_t) == 2, "The UTF-16 scanner requires 16-bit wchar_t");
	constexpr std::size_t vectorWidth = 32;
	const __m512i needle = _mm512_set1_epi16(static_cast<short>(target));
	std::size_t offset = 0;
	for (; length - offset >= vectorWidth; offset += vectorWidth) {
		const std::uint32_t mask = static_cast<std::uint32_t>(
			_mm512_cmpeq_epi16_mask(_mm512_loadu_si512(data + offset), needle));
		if (mask != 0) {
			return offset + FirstUtf16Lane(mask);
		}
	}
	// Invalid lanes load as zero, which would match a NUL target, so the match
	// mask is clamped to the valid lanes.
	const std::size_t remaining = length - offset;
	if (remaining == 0) {
		return length;
	}
	const __mmask32 valid = LowBitsMask32(remaining);
	const __m512i units = _mm512_maskz_loadu_epi16(valid, data + offset);
	const std::uint32_t mask =
		static_cast<std::uint32_t>(_mm512_cmpeq_epi16_mask(units, needle) & valid);
	return mask != 0 ? offset + FirstUtf16Lane(mask) : length;
}
}
