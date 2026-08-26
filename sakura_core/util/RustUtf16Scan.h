/*! @file @brief Panic-safe C ABI for the final native Rust static library. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <type_traits>

constexpr std::uint32_t SAKURA_NATIVE_ABI_VERSION_V1 = 1;

enum class SakuraStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	Unsupported = 2,
	NotInitialized = 3,
	ConflictingInitialization = 4,
	InternalError = 5,
};

enum class SakuraCpuFeature : std::uint64_t {
	Avx = std::uint64_t{1} << 0,
	Avx2 = std::uint64_t{1} << 1,
	Avx512F = std::uint64_t{1} << 2,
	Avx512Bw = std::uint64_t{1} << 3,
};

enum class SakuraOsExtendedState : std::uint64_t {
	Xmm = std::uint64_t{1} << 0,
	Ymm = std::uint64_t{1} << 1,
	Opmask = std::uint64_t{1} << 2,
	ZmmHi256 = std::uint64_t{1} << 3,
	Hi16Zmm = std::uint64_t{1} << 4,
};

enum class SakuraOperationId : std::uint32_t {
	FindCrOrLfUtf16 = 1,
	FindMarkdownSpecialUtf16 = 2,
	FindCharUtf16 = 3,
};

enum class SakuraImplementationId : std::uint32_t {
	CppAvx128 = 1,
	CppAvx2 = 2,
	CppAvx512Bw = 3,
	RustAvx128 = 101,
	RustAvx2 = 102,
	RustAvx512Bw = 103,
};

constexpr std::uint64_t SakuraAbiBit(SakuraCpuFeature bit) noexcept
{
	return static_cast<std::uint64_t>(bit);
}

constexpr std::uint64_t SakuraAbiBit(SakuraOsExtendedState bit) noexcept
{
	return static_cast<std::uint64_t>(bit);
}

// C++ is the sole CPUID/XGETBV owner. It copies one immutable snapshot into
// the Rust library during wWinMain startup before workers or Rust services can
// run. Raw CPUID bits stay separate from OS-enabled register state and from
// the selected operation policy. All reserved fields must be zero.
struct SakuraCpuCapabilitiesV1 {
	std::uint32_t structSize{};
	std::uint32_t abiVersion{SAKURA_NATIVE_ABI_VERSION_V1};
	std::uint64_t rawFeatureBits{};
	std::uint64_t osExtendedStateBits{};
	std::uint64_t reserved[4]{};
};

struct SakuraOperationPolicyV1 {
	std::uint32_t structSize{};
	std::uint32_t abiVersion{SAKURA_NATIVE_ABI_VERSION_V1};
	std::uint32_t operationId{};
	std::uint32_t implementationId{};
	std::uint64_t minimumLength{};
	std::uint64_t reserved[3]{};
};

static_assert(sizeof(SakuraStatus) == sizeof(std::uint32_t));
static_assert(sizeof(SakuraCpuCapabilitiesV1) == 56);
static_assert(sizeof(SakuraOperationPolicyV1) == 48);
static_assert(std::is_trivially_copyable_v<SakuraCpuCapabilitiesV1>);
static_assert(std::is_trivially_copyable_v<SakuraOperationPolicyV1>);

// Every export returns a typed status. The native workspace uses
// panic=unwind, and sakura_native_ffi catches all Rust panics before they can
// cross this C boundary. InternalError identifies a caught panic or violated
// internal invariant. OOM, explicit abort, and a double panic remain fatal.
//
// V2 UTF-16 scans use fixed-width lengths and an explicit caller-owned output.
// `resultIndex` is written with `length` before later validation so all
// failures after output validation fail closed. The unit is UTF-16 code units;
// `length` means not found. Zero length permits a null data pointer. Positive
// lengths require one immutable initialized allocation, two-byte alignment,
// a byte length no larger than INT64_MAX, and no address overflow. No pointer
// is retained. Raw NUL and unpaired surrogates remain ordinary code units.
extern "C"
{
SakuraStatus sakura_native_initialize_v1(
	const SakuraCpuCapabilitiesV1* capabilities,
	const SakuraOperationPolicyV1* policies,
	std::uint64_t policyCount) noexcept;

SakuraStatus sakura_utf16_find_cr_or_lf_avx128_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_markdown_special_avx128_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_char_avx128_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint16_t target,
	std::uint64_t* resultIndex) noexcept;

SakuraStatus sakura_utf16_find_cr_or_lf_avx2_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_markdown_special_avx2_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_char_avx2_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint16_t target,
	std::uint64_t* resultIndex) noexcept;

// The Rust AVX-512 kernels require AVX2 + AVX512F + AVX512BW and OS-enabled
// XMM/YMM/opmask/ZMM state. AVX2 remains an explicit Rust target-feature
// prerequisite independently of the C++ byte scanner's current tail design.
SakuraStatus sakura_utf16_find_cr_or_lf_avx512bw_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_markdown_special_avx512bw_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_utf16_find_char_avx512bw_v2(
	const std::uint16_t* data, std::uint64_t length, std::uint16_t target,
	std::uint64_t* resultIndex) noexcept;

// The byte CR/LF exports are direct comparison candidates only. They reuse
// the C++-owned capability snapshot for ISA/OS gating, but intentionally do
// not add a fourth operation-policy slot to ABI V1. The 512-byte handoff used
// inside the kernels is an implementation boundary, not a policy minimum.
SakuraStatus sakura_byte_find_cr_or_lf_avx128_candidate_v1(
	const std::uint8_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_byte_find_cr_or_lf_avx2_candidate_v1(
	const std::uint8_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
SakuraStatus sakura_byte_find_cr_or_lf_avx512bw_candidate_v1(
	const std::uint8_t* data, std::uint64_t length, std::uint64_t* resultIndex) noexcept;
}
