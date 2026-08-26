/*! @file @brief Internal SIMD implementation declarations. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "util/CpuDispatch.h"

namespace CpuDispatch::Internal
{
std::size_t FindCrOrLfAvx(const char* data, std::size_t length) noexcept;
std::size_t FindCrOrLfAvx2(const char* data, std::size_t length) noexcept;
std::size_t FindCrOrLfAvx512(const char* data, std::size_t length) noexcept;

#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
std::size_t FindCrOrLfRustAvx128(const char* data, std::size_t length) noexcept;
std::size_t FindCrOrLfRustAvx2(const char* data, std::size_t length) noexcept;
std::size_t FindCrOrLfRustAvx512Bw(const char* data, std::size_t length) noexcept;
#endif

std::size_t FindCrOrLfUtf16Avx(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16Avx2(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16Avx512(const wchar_t* data, std::size_t length) noexcept;

std::size_t FindMarkdownInlineSpecialUtf16Avx(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16Avx2(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16Avx512(
	const wchar_t* data, std::size_t length) noexcept;

std::size_t WidenAsciiToUtf16Avx(
	const char* source, std::size_t length, wchar_t* destination) noexcept;
std::size_t WidenAsciiToUtf16Avx2(
	const char* source, std::size_t length, wchar_t* destination) noexcept;
std::size_t WidenAsciiToUtf16Avx512(
	const char* source, std::size_t length, wchar_t* destination) noexcept;

std::size_t FindUtf16CharAvx(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;
std::size_t FindUtf16CharAvx2(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;
std::size_t FindUtf16CharAvx512(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;

#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
std::size_t FindCrOrLfUtf16RustAvx128(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16RustAvx2(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16RustAvx512Bw(const wchar_t* data, std::size_t length) noexcept;

std::size_t FindMarkdownInlineSpecialUtf16RustAvx128(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16RustAvx2(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16RustAvx512Bw(
	const wchar_t* data, std::size_t length) noexcept;

std::size_t FindUtf16CharRustAvx128(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;
std::size_t FindUtf16CharRustAvx2(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;
std::size_t FindUtf16CharRustAvx512Bw(
	const wchar_t* data, std::size_t length, wchar_t target) noexcept;
#endif
}
