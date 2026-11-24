/*! @file @brief C ABI and C++ comparison adapter for the Rust encoded-EOL candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include "basis/CEol.h"
#include "charset/CCodePage.h"

// Comparison-only (#441). C++ stays legacy-production: CFileLoad and the CReadManager partition arithmetic decide every
// real load, and nothing outside tests calls this adapter. The structs mirror rust/native/sakura_native_ffi/src/encoded_eol.rs;
// both sides pin the same sizes, offsets and wire numbers, so a drift on either side fails to compile or fails a test.

constexpr std::uint32_t SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1 = 1;

//! Comparison-only bound of one call (16 MiB of staged records), not an adoption threshold.
constexpr std::uint64_t SAKURA_DOCUMENT_CODEC_ENCODED_EOL_MAX_RECORDS_V1 = 1048576;

//! Typed status of the encoded-EOL exports. The numbers share the candidate status space of the URI candidate.
enum class SakuraDocumentCodecStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	InvalidCapacity = 2,
	InternalError = 12,
	// Adapter-only result when the Rust candidate is not linked. The Rust exports never return this value.
	Unsupported = 13,
};

//! One marker. Offsets are bytes from data[0]; the width is in bytes; a CR LF pair is one marker of two units.
//! `kind` uses the EEolType numbers (1 CRLF, 2 LF, 3 CR, 4 NEL, 5 LS, 6 PS).
struct SakuraDocumentCodecEolRecordV1 {
	std::uint64_t delimiter_byte_offset{};
	std::uint32_t byte_width{};
	std::uint32_t kind{};
};

//! Read-only scan request. The scan range is [start_byte_offset, length_bytes) and the end is exclusive. A null data
//! pointer is valid only with length 0. `format` uses the EEncodingTrait numbers (2 UTF-16LE, 3 UTF-16BE, 4 UTF-32LE,
//! 5 UTF-32BE); `extended_eol` is 0 or 1 and enables NEL, LS and PS.
struct SakuraDocumentCodecEolRequestV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	const std::uint8_t* data{};
	std::uint64_t length_bytes{};
	std::uint64_t start_byte_offset{};
	std::uint32_t format{};
	std::uint32_t extended_eol{};
	std::uint64_t reserved[2]{};
};

//! Caller-owned record array. A null pointer is valid only when the capacity is zero; the candidate never allocates for
//! the caller. A write with too little capacity writes nothing.
struct SakuraDocumentCodecEolBufferV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	SakuraDocumentCodecEolRecordV1* records{};
	std::uint64_t capacity_records{};
	std::uint64_t reserved[2]{};
};

//! Result of both calls: the marker count and the finish offset, which is the supplied exclusive end (trailing content
//! included). The two calls are independent; there is no receipt between them.
struct SakuraDocumentCodecEolMeasureV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_DOCUMENT_CODEC_ABI_VERSION_V1};
	std::uint64_t record_count{};
	std::uint64_t finish_byte_offset{};
	std::uint64_t reserved[2]{};
};

static_assert(sizeof(SakuraDocumentCodecEolRecordV1) == 16);
static_assert(sizeof(SakuraDocumentCodecEolRequestV1) == 56);
static_assert(sizeof(SakuraDocumentCodecEolBufferV1) == 40);
static_assert(sizeof(SakuraDocumentCodecEolMeasureV1) == 40);
static_assert(alignof(SakuraDocumentCodecEolRecordV1) == 8);
static_assert(alignof(SakuraDocumentCodecEolRequestV1) == 8);
static_assert(alignof(SakuraDocumentCodecEolBufferV1) == 8);
static_assert(alignof(SakuraDocumentCodecEolMeasureV1) == 8);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecEolRecordV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecEolRequestV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecEolBufferV1>);
static_assert(std::is_standard_layout_v<SakuraDocumentCodecEolMeasureV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecEolRecordV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecEolRequestV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecEolBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraDocumentCodecEolMeasureV1>);

static_assert(offsetof(SakuraDocumentCodecEolRecordV1, delimiter_byte_offset) == 0);
static_assert(offsetof(SakuraDocumentCodecEolRecordV1, byte_width) == 8);
static_assert(offsetof(SakuraDocumentCodecEolRecordV1, kind) == 12);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, data) == 8);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, length_bytes) == 16);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, start_byte_offset) == 24);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, format) == 32);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, extended_eol) == 36);
static_assert(offsetof(SakuraDocumentCodecEolRequestV1, reserved) == 40);
static_assert(offsetof(SakuraDocumentCodecEolBufferV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecEolBufferV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecEolBufferV1, records) == 8);
static_assert(offsetof(SakuraDocumentCodecEolBufferV1, capacity_records) == 16);
static_assert(offsetof(SakuraDocumentCodecEolBufferV1, reserved) == 24);
static_assert(offsetof(SakuraDocumentCodecEolMeasureV1, struct_size) == 0);
static_assert(offsetof(SakuraDocumentCodecEolMeasureV1, abi_version) == 4);
static_assert(offsetof(SakuraDocumentCodecEolMeasureV1, record_count) == 8);
static_assert(offsetof(SakuraDocumentCodecEolMeasureV1, finish_byte_offset) == 16);
static_assert(offsetof(SakuraDocumentCodecEolMeasureV1, reserved) == 24);

// The wire numbers are the legacy enum values, so a legacy renumbering cannot silently change the ABI.
static_assert(static_cast<std::uint32_t>(ENCODING_TRAIT_UTF16LE) == 2);
static_assert(static_cast<std::uint32_t>(ENCODING_TRAIT_UTF16BE) == 3);
static_assert(static_cast<std::uint32_t>(ENCODING_TRAIT_UTF32LE) == 4);
static_assert(static_cast<std::uint32_t>(ENCODING_TRAIT_UTF32BE) == 5);
static_assert(static_cast<std::uint32_t>(EEolType::cr_and_lf) == 1);
static_assert(static_cast<std::uint32_t>(EEolType::line_feed) == 2);
static_assert(static_cast<std::uint32_t>(EEolType::carriage_return) == 3);
static_assert(static_cast<std::uint32_t>(EEolType::next_line) == 4);
static_assert(static_cast<std::uint32_t>(EEolType::line_separator) == 5);
static_assert(static_cast<std::uint32_t>(EEolType::paragraph_separator) == 6);

extern "C"
{
SakuraDocumentCodecStatus sakura_document_codec_scan_eol_measure_v1(
	const SakuraDocumentCodecEolRequestV1* request,
	SakuraDocumentCodecEolMeasureV1* output) noexcept;

SakuraDocumentCodecStatus sakura_document_codec_scan_eol_write_v1(
	const SakuraDocumentCodecEolRequestV1* request,
	const SakuraDocumentCodecEolBufferV1* buffer,
	SakuraDocumentCodecEolMeasureV1* output) noexcept;
}

namespace document_codec::rust_candidate {

//! One marker found by the Rust candidate, in the legacy EOL vocabulary. Offsets and widths are bytes.
struct EolRecord final {
	const std::uint64_t byteOffset;
	const std::uint32_t byteWidth;
	const EEolType kind;
};

//! Copied candidate output of one scan. A test/shadow value, not a production scan result.
struct EolScan final {
	const std::vector<EolRecord> records;
	const std::uint64_t finishByteOffset;
};

struct EolResult final {
	const std::optional<EolScan> scan;
	const SakuraDocumentCodecStatus status;

	explicit operator bool() const noexcept { return scan.has_value(); }
};

//! Runs the two independent passes (measure, size the caller-owned array, write) over [startByteOffset, lengthBytes) of
//! data and copies the result. `format` must be one of the four UTF-16/UTF-32 traits and startByteOffset must be on a
//! unit boundary unless it equals lengthBytes; any other value is reported by the export as InvalidArgument. It is a
//! shadow adapter: it performs no file access, no line splitting and no provider selection.
[[nodiscard]] EolResult ScanEncodedEol(
	EEncodingTrait format,
	bool extendedEol,
	const void* data,
	std::size_t lengthBytes,
	std::size_t startByteOffset) noexcept;

} // namespace document_codec::rust_candidate
