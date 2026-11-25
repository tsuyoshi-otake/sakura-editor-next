/*! @file @brief C ABI and C++ comparison adapter for the Rust UTF-8/CESU-8 conversion candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

#include "charset/charset.h"
#include "io/DocumentCodecAbi.h"

// Comparison-only (#443). C++ stays legacy-production: CUtf8 and CCesu8 decide every real conversion, and nothing outside
// tests calls this adapter. The structs mirror rust/native/sakura_native_ffi/src/utf8_codec.rs, where one generic request and
// one generic buffer serve both directions and this header names their fields per direction; both sides pin the same sizes,
// offsets and wire numbers, so a drift on either side fails to compile or fails a test. The ABI version and the typed status
// come from DocumentCodecAbi.h, which every document-codec candidate shares.

//! Longest input of one call, in bytes to decode or in UTF-16 units to encode: the int length domain of the C++ converters
//! (CMemory::GetRawLength and CNativeW::GetStringLength), not an adoption threshold.
constexpr std::uint64_t SAKURA_DOCUMENT_CODEC_UTF8_MAX_INPUT_V1 = 2147483647;

//! Decode request: UTF-8 or CESU-8 bytes. `variant` uses the ECodeType numbers (4 CODE_UTF8, 7 CODE_CESU8); any other value
//! is refused. A null data pointer is valid only with length 0.
struct SakuraDocumentCodecUtf8DecodeRequestV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	const std::uint8_t* data{};
	std::uint64_t length_bytes{};
	std::uint32_t variant{};
	std::uint32_t reserved0{};
	std::uint64_t reserved[2]{};
};

//! Encode request: UTF-16 units at a 2-byte aligned address, counted in units; otherwise as the decode request.
struct SakuraDocumentCodecUtf8EncodeRequestV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	const std::uint16_t* data{};
	std::uint64_t length_units{};
	std::uint32_t variant{};
	std::uint32_t reserved0{};
	std::uint64_t reserved[2]{};
};

//! Caller-owned UTF-16 output array of a decode. A null pointer is valid only when the capacity is zero; the candidate
//! never allocates for the caller. A write with too little capacity writes nothing, and a successful write leaves any
//! unused suffix of the array intact.
struct SakuraDocumentCodecUtf8DecodeBufferV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	std::uint16_t* units{};
	std::uint64_t capacity_units{};
	std::uint64_t reserved[2]{};
};

//! Caller-owned byte output array of an encode, with the rules of the decode buffer.
struct SakuraDocumentCodecUtf8EncodeBufferV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	std::uint8_t* bytes{};
	std::uint64_t capacity_bytes{};
	std::uint64_t reserved[2]{};
};

//! Result of all four calls. `output_length` counts output elements: UTF-16 units for a decode, bytes for an encode.
//! `lossy` is 0 or 1: always 0 for a decode, and 1 for an encode that replaced an unpaired surrogate outside
//! U+DC00..U+DCFF, which the converters report as RESULT_LOSESOME. There is no loss count or position. The measure and
//! write calls are independent; there is no receipt between them.
struct SakuraDocumentCodecUtf8ResultV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	std::uint64_t output_length{};
	std::uint32_t lossy{};
	std::uint32_t reserved0{};
	std::uint64_t reserved[1]{};
};

static_assert(sizeof(SakuraDocumentCodecUtf8DecodeRequestV1) == 48);
static_assert(sizeof(SakuraDocumentCodecUtf8EncodeRequestV1) == 48);
static_assert(sizeof(SakuraDocumentCodecUtf8DecodeBufferV1) == 40);
static_assert(sizeof(SakuraDocumentCodecUtf8EncodeBufferV1) == 40);
static_assert(sizeof(SakuraDocumentCodecUtf8ResultV1) == 32);
static_assert(alignof(SakuraDocumentCodecUtf8DecodeRequestV1) == 8);
static_assert(alignof(SakuraDocumentCodecUtf8EncodeRequestV1) == 8);
static_assert(alignof(SakuraDocumentCodecUtf8DecodeBufferV1) == 8);
static_assert(alignof(SakuraDocumentCodecUtf8EncodeBufferV1) == 8);
static_assert(alignof(SakuraDocumentCodecUtf8ResultV1) == 8);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecUtf8DecodeRequestV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecUtf8EncodeRequestV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecUtf8DecodeBufferV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecUtf8EncodeBufferV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecUtf8ResultV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecUtf8DecodeRequestV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecUtf8EncodeRequestV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecUtf8DecodeBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecUtf8EncodeBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecUtf8ResultV1>);

static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, data) == 8);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, length_bytes) == 16);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, variant) == 24);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, reserved0) == 28);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeRequestV1, reserved) == 32);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, data) == 8);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, length_units) == 16);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, variant) == 24);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, reserved0) == 28);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeRequestV1, reserved) == 32);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeBufferV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeBufferV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeBufferV1, units) == 8);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeBufferV1, capacity_units) == 16);
static_assert(offsetof(SakuraDocumentCodecUtf8DecodeBufferV1, reserved) == 24);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeBufferV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeBufferV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeBufferV1, bytes) == 8);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeBufferV1, capacity_bytes) == 16);
static_assert(offsetof(SakuraDocumentCodecUtf8EncodeBufferV1, reserved) == 24);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, output_length) == 8);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, lossy) == 16);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, reserved0) == 20);
static_assert(offsetof(SakuraDocumentCodecUtf8ResultV1, reserved) == 24);

// The wire numbers are the legacy enum values, so a legacy renumbering cannot silently change the ABI. The adapter reads
// and writes wchar_t as UTF-16 units, which holds on both Windows toolchains.
static_assert(static_cast<std::uint32_t>(CODE_UTF8) == 4);
static_assert(static_cast<std::uint32_t>(CODE_CESU8) == 7);
static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));

extern "C"
{
SakuraDocumentCodecStatus sakura_document_codec_utf8_decode_measure_v1(
	const SakuraDocumentCodecUtf8DecodeRequestV1* request,
	SakuraDocumentCodecUtf8ResultV1* output) noexcept;

SakuraDocumentCodecStatus sakura_document_codec_utf8_decode_write_v1(
	const SakuraDocumentCodecUtf8DecodeRequestV1* request,
	const SakuraDocumentCodecUtf8DecodeBufferV1* buffer,
	SakuraDocumentCodecUtf8ResultV1* output) noexcept;

SakuraDocumentCodecStatus sakura_document_codec_utf8_encode_measure_v1(
	const SakuraDocumentCodecUtf8EncodeRequestV1* request,
	SakuraDocumentCodecUtf8ResultV1* output) noexcept;

SakuraDocumentCodecStatus sakura_document_codec_utf8_encode_write_v1(
	const SakuraDocumentCodecUtf8EncodeRequestV1* request,
	const SakuraDocumentCodecUtf8EncodeBufferV1* buffer,
	SakuraDocumentCodecUtf8ResultV1* output) noexcept;
}

namespace document_codec::rust_candidate {

//! Copied candidate output of one decode: the UTF-16 units. A test/shadow value, not a production conversion result.
struct Utf8DecodeResult final {
	const std::optional<std::wstring> units;
	const SakuraDocumentCodecStatus status;

	explicit operator bool() const noexcept { return units.has_value(); }
};

//! Copied candidate output of one encode: the bytes and the loss flag (RESULT_LOSESOME in the converters).
struct Utf8EncodeResult final {
	const std::optional<std::string> bytes;
	const bool lossy;
	const SakuraDocumentCodecStatus status;

	explicit operator bool() const noexcept { return bytes.has_value(); }
};

//! Runs the two independent passes (measure, size the caller-owned array, write) over a whole buffer and copies the
//! result. `code` must be CODE_UTF8 or CODE_CESU8 and any other value is reported by the export as InvalidArgument; the
//! input is lengthBytes bytes at `bytes`, which may be null only when the length is 0. It is a shadow adapter: it
//! performs no file access and no provider selection.
[[nodiscard]] Utf8DecodeResult DecodeUtf8(ECodeType code, const void* bytes, std::size_t lengthBytes) noexcept;

//! The same for a whole buffer of UTF-16 units: lengthUnits units at `units`, which may be null only when the length is 0.
[[nodiscard]] Utf8EncodeResult EncodeUtf8(ECodeType code, const wchar_t* units, std::size_t lengthUnits) noexcept;

} // namespace document_codec::rust_candidate
