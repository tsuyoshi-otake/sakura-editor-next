/*! @file @brief C++ adapters for the Rust UTF-16 scan kernels. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include <climits>

#include "util/CpuDispatchInternal.h"
#include "util/RustUtf16Scan.h"

#if defined(SAKURA_UTF16_BACKEND_RUST)
namespace CpuDispatch::Internal
{
namespace
{
static_assert(CHAR_BIT == 8,
	"The Rust UTF-16 scanner requires 8-bit bytes");
static_assert(sizeof(wchar_t) == sizeof(std::uint16_t),
	"The Rust UTF-16 scanner requires 16-bit wchar_t");
static_assert(alignof(wchar_t) == alignof(std::uint16_t),
	"The Rust UTF-16 scanner requires matching wchar_t alignment");

const std::uint16_t* AsUtf16(const wchar_t* data) noexcept
{
	return reinterpret_cast<const std::uint16_t*>(data);
}
}

std::size_t FindCrOrLfUtf16RustAvx128(const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_cr_or_lf_avx128_v1(AsUtf16(data), length);
}

std::size_t FindCrOrLfUtf16RustAvx2(const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_cr_or_lf_avx2_v1(AsUtf16(data), length);
}

std::size_t FindCrOrLfUtf16RustAvx512Bw(const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_cr_or_lf_avx512bw_v1(AsUtf16(data), length);
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx128(
	const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_markdown_special_avx128_v1(AsUtf16(data), length);
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx2(
	const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_markdown_special_avx2_v1(AsUtf16(data), length);
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx512Bw(
	const wchar_t* data, std::size_t length) noexcept
{
	return sakura_utf16_find_markdown_special_avx512bw_v1(AsUtf16(data), length);
}

std::size_t FindUtf16CharRustAvx128(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return sakura_utf16_find_char_avx128_v1(AsUtf16(data), length, static_cast<std::uint16_t>(target));
}

std::size_t FindUtf16CharRustAvx2(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return sakura_utf16_find_char_avx2_v1(AsUtf16(data), length, static_cast<std::uint16_t>(target));
}

std::size_t FindUtf16CharRustAvx512Bw(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return sakura_utf16_find_char_avx512bw_v1(AsUtf16(data), length, static_cast<std::uint16_t>(target));
}
}
#endif
