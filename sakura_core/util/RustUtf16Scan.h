/*! @file @brief C ABI for the bounded, audited Rust UTF-16 scan kernels. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>

// ABI version 1 is a bounded, read-only UTF-16 span. The Rust side borrows
// data only for the duration of each call; no allocation crosses this ABI and
// no pointer or reference escapes it. The return value is always in
// [0, length], with length meaning "not found".
//
// A zero-length call returns zero without reading data, so both null and
// non-null pointers are valid in that case. For a positive length, the caller
// must pass one initialized, immutable allocation of UTF-16 code units whose
// byte size is at most INT64_MAX, whose address is non-null and 2-byte
// aligned, and whose address plus byte size does not overflow. A null,
// misaligned, oversized, or address-overflow span fails closed by returning
// length before dereference. Those representational checks do not make an
// invalid lifetime or freed allocation safe; callers still own that contract.
//
// Rust is built with panic=abort. These functions are noexcept on the C++
// side, retain no pointer, and do not permit C++ exceptions or Rust unwinding
// to cross the boundary.
//
// The caller must also prove the matching CPU and OS state before invoking a
// valid-span operation: AVX-128 requires AVX plus OSXSAVE/XCR0 XMM/YMM state;
// AVX2 additionally requires AVX2; AVX512BW requires AVX2, AVX512F, and
// AVX512BW plus OSXSAVE/XCR0 XMM/YMM/opmask/ZMM state. Invalid spans return
// before reaching the ISA kernel, but that fail-closed path is not a license
// to call a valid span on unsupported hardware.
extern "C"
{
std::size_t sakura_utf16_find_cr_or_lf_avx128_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_markdown_special_avx128_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_char_avx128_v1(
	const std::uint16_t* data, std::size_t length, std::uint16_t target) noexcept;

std::size_t sakura_utf16_find_cr_or_lf_avx2_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_markdown_special_avx2_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_char_avx2_v1(
	const std::uint16_t* data, std::size_t length, std::uint16_t target) noexcept;

// All three AVX512BW entry points below share the process-wide AVX-512 tier
// contract: AVX2 + AVX512F + AVX512BW with OSXSAVE/XCR0 XMM/YMM/opmask/ZMM
// state enabled. AVX2 remains required because that global tier also selects
// the C++ byte scanner; its AVX-512 tail delegates to `FindCrOrLfAvx2`.
std::size_t sakura_utf16_find_cr_or_lf_avx512bw_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_markdown_special_avx512bw_v1(
	const std::uint16_t* data, std::size_t length) noexcept;
std::size_t sakura_utf16_find_char_avx512bw_v1(
	const std::uint16_t* data, std::size_t length, std::uint16_t target) noexcept;
}
