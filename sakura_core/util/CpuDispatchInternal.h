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

std::size_t FindCrOrLfUtf16Avx(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16Avx2(const wchar_t* data, std::size_t length) noexcept;
std::size_t FindCrOrLfUtf16Avx512(const wchar_t* data, std::size_t length) noexcept;

std::size_t FindMarkdownInlineSpecialUtf16Avx(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16Avx2(
	const wchar_t* data, std::size_t length) noexcept;
std::size_t FindMarkdownInlineSpecialUtf16Avx512(
	const wchar_t* data, std::size_t length) noexcept;
}
