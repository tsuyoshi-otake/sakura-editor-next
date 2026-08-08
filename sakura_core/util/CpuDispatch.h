/*! @file @brief Runtime selection of x64 SIMD implementations. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>

namespace CpuDispatch
{
enum class Isa : std::uint8_t {
	Avx = 1,
	Avx2 = 2,
	Avx512 = 3,
};

struct Capabilities {
	bool avx{};
	bool avx2{};
	bool avx512{};
};

using FindCrOrLfFunction = std::size_t (*)(const char* data, std::size_t length) noexcept;
using FindUtf16Function = std::size_t (*)(const wchar_t* data, std::size_t length) noexcept;

// Widens the longest leading run of ASCII bytes (values below 0x80) into
// UTF-16 units and returns the run length. Exactly that many units are
// written to `destination`; bytes at and after the first non-ASCII byte are
// neither read past nor converted.
using WidenAsciiToUtf16Function =
	std::size_t (*)(const char* source, std::size_t length, wchar_t* destination) noexcept;

// Returns the index of the first UTF-16 unit equal to `target`, or `length`.
using FindUtf16CharFunction =
	std::size_t (*)(const wchar_t* data, std::size_t length, wchar_t target) noexcept;

// Minimum input length at which delegating a UTF-16 scan to the dispatched
// function beats a caller-local scalar loop. Below the minimum the indirect
// call plus the implementation's own scalar fallback would only add overhead,
// so callers keep their local loop. The AVX-512 implementations accept any
// length via masked loads; their minimums are benchmark-derived, not safety
// bounds.
struct Utf16ScanPolicy {
	std::size_t crOrLfMinimumLength{64};
	std::size_t markdownInlineSpecialMinimumLength{64};
	std::size_t findCharMinimumLength{64};
};

// Minimum remaining source length at which delegating ASCII-prefix widening
// to the dispatched function beats a caller-local scalar widening loop. Same
// contract as Utf16ScanPolicy: a caller-local loop covers shorter runs.
struct Utf8ConversionPolicy {
	std::size_t widenAsciiMinimumLength{64};
};

struct Dispatch {
	Isa isa{Isa::Avx};
	Capabilities capabilities{};
	FindCrOrLfFunction findCrOrLf{};
	FindUtf16Function findCrOrLfUtf16{};
	FindUtf16Function findMarkdownInlineSpecialUtf16{};
	WidenAsciiToUtf16Function widenAsciiToUtf16{};
	FindUtf16CharFunction findUtf16Char{};
	Utf16ScanPolicy utf16ScanPolicy{};
	Utf8ConversionPolicy utf8ConversionPolicy{};
	std::int64_t initializationTicks{};
};

// Detects CPU and OS extended-state support once and freezes the process-wide
// dispatch table. Call during wWinMain startup before worker threads begin.
const Dispatch& Initialize() noexcept;
const Dispatch& Get() noexcept;

// Pure selection helpers used to lock the fallback order in unit tests.
Isa SelectBestIsa(const Capabilities& capabilities) noexcept;
Utf16ScanPolicy GetUtf16ScanPolicy(Isa isa) noexcept;
Utf8ConversionPolicy GetUtf8ConversionPolicy(Isa isa) noexcept;
const char* GetIsaName(Isa isa) noexcept;

namespace Testing
{
// Returns nullptr when the requested implementation is unsafe on this machine.
FindCrOrLfFunction GetSupportedFindCrOrLf(Isa isa) noexcept;
FindUtf16Function GetSupportedFindCrOrLfUtf16(Isa isa) noexcept;
FindUtf16Function GetSupportedFindMarkdownInlineSpecialUtf16(Isa isa) noexcept;
WidenAsciiToUtf16Function GetSupportedWidenAsciiToUtf16(Isa isa) noexcept;
FindUtf16CharFunction GetSupportedFindUtf16Char(Isa isa) noexcept;
}
}
