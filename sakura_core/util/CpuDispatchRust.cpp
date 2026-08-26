/*! @file @brief C++ adapters for the Rust UTF-16 scan kernels. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include <climits>

#include "util/CpuDispatchInternal.h"
#include "util/RustUtf16Scan.h"

#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
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

const std::uint8_t* AsBytes(const char* data) noexcept
{
	return reinterpret_cast<const std::uint8_t*>(data);
}

template<typename Operation>
std::size_t InvokeRustScan(std::size_t length, Operation operation) noexcept
{
	static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
	std::uint64_t result = static_cast<std::uint64_t>(length);
	const SakuraStatus status = operation(&result);
	if (status != SakuraStatus::Ok || result > static_cast<std::uint64_t>(length)) {
		return length;
	}
	return static_cast<std::size_t>(result);
}
}

std::size_t FindCrOrLfRustAvx128(const char* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_byte_find_cr_or_lf_avx128_candidate_v1(
			AsBytes(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindCrOrLfRustAvx2(const char* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_byte_find_cr_or_lf_avx2_candidate_v1(
			AsBytes(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindCrOrLfRustAvx512Bw(const char* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_byte_find_cr_or_lf_avx512bw_candidate_v1(
			AsBytes(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindCrOrLfUtf16RustAvx128(const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_cr_or_lf_avx128_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindCrOrLfUtf16RustAvx2(const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_cr_or_lf_avx2_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindCrOrLfUtf16RustAvx512Bw(const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_cr_or_lf_avx512bw_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx128(
	const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_markdown_special_avx128_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx2(
	const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_markdown_special_avx2_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindMarkdownInlineSpecialUtf16RustAvx512Bw(
	const wchar_t* data, std::size_t length) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_markdown_special_avx512bw_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length), result);
	});
}

std::size_t FindUtf16CharRustAvx128(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_char_avx128_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length),
			static_cast<std::uint16_t>(target), result);
	});
}

std::size_t FindUtf16CharRustAvx2(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_char_avx2_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length),
			static_cast<std::uint16_t>(target), result);
	});
}

std::size_t FindUtf16CharRustAvx512Bw(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept
{
	return InvokeRustScan(length, [&](std::uint64_t* result) noexcept {
		return sakura_utf16_find_char_avx512bw_v2(
			AsUtf16(data), static_cast<std::uint64_t>(length),
			static_cast<std::uint16_t>(target), result);
	});
}
}
#endif
