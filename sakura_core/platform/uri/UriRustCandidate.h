/*! @file @brief C ABI and C++ test adapter for the Rust URI shadow candidate. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

constexpr std::uint32_t SAKURA_URI_CANDIDATE_ABI_VERSION_V1 = 1;

//! Typed status for the stateless Rust URI candidate.  URI parse errors are
//! deliberately distinct from ABI, capacity, and panic failures.
enum class SakuraUriCandidateStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	InvalidCapacity = 2,
	EmptyInput = 3,
	MissingScheme = 4,
	InvalidScheme = 5,
	InvalidAuthority = 6,
	InvalidPath = 7,
	InvalidQuery = 8,
	InvalidFragment = 9,
	InvalidPercentEncoding = 10,
	InvalidUtf8 = 11,
	InternalError = 12,
	// Adapter-only result when the Rust candidate is not linked (for example,
	// the MinGW compatibility build). Rust ABI exports never return this value.
	Unsupported = 13,
};

//! A copied-input UTF-16 span.  `length` is measured in code units, not bytes.
struct SakuraUriCandidateSpanV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_URI_CANDIDATE_ABI_VERSION_V1};
	const std::uint16_t* data{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
};

//! Decoded component inputs for FromComponents.  Query and fragment presence
//! are explicit so an empty component is not confused with an absent one.
struct SakuraUriCandidateComponentsV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_URI_CANDIDATE_ABI_VERSION_V1};
	SakuraUriCandidateSpanV1 scheme{};
	SakuraUriCandidateSpanV1 authority{};
	SakuraUriCandidateSpanV1 path{};
	SakuraUriCandidateSpanV1 query{};
	SakuraUriCandidateSpanV1 fragment{};
	std::uint32_t has_authority{};
	std::uint32_t has_query{};
	std::uint32_t has_fragment{};
	std::uint32_t reserved{};
	std::uint64_t reserved64[2]{};
};

//! Caller-owned UTF-16 output buffer.  A null pointer is valid only when the
//! capacity is zero; the candidate never allocates on behalf of the caller.
struct SakuraUriCandidateBufferV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_URI_CANDIDATE_ABI_VERSION_V1};
	std::uint16_t* data{};
	std::uint64_t capacity{};
	std::uint64_t reserved[2]{};
};

//! All component and serialized output buffers for one write pass.
struct SakuraUriCandidateBuffersV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_URI_CANDIDATE_ABI_VERSION_V1};
	SakuraUriCandidateBufferV1 scheme{};
	SakuraUriCandidateBufferV1 authority{};
	SakuraUriCandidateBufferV1 path{};
	SakuraUriCandidateBufferV1 query{};
	SakuraUriCandidateBufferV1 fragment{};
	SakuraUriCandidateBufferV1 serialized{};
	std::uint64_t reserved64[2]{};
};

//! Result lengths and presence bits returned by both passes.
struct SakuraUriCandidateMeasureV1 {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{SAKURA_URI_CANDIDATE_ABI_VERSION_V1};
	std::uint64_t scheme_length{};
	std::uint64_t authority_length{};
	std::uint64_t path_length{};
	std::uint64_t query_length{};
	std::uint64_t fragment_length{};
	std::uint64_t serialized_length{};
	std::uint32_t has_authority{};
	std::uint32_t has_query{};
	std::uint32_t has_fragment{};
	std::uint32_t reserved{};
	std::uint64_t reserved64[2]{};
};

using SakuraUriCandidateOutputV1 = SakuraUriCandidateMeasureV1;

static_assert(sizeof(SakuraUriCandidateSpanV1) == 40);
static_assert(sizeof(SakuraUriCandidateComponentsV1) == 240);
static_assert(sizeof(SakuraUriCandidateBufferV1) == 40);
static_assert(sizeof(SakuraUriCandidateBuffersV1) == 264);
static_assert(sizeof(SakuraUriCandidateMeasureV1) == 88);
static_assert(std::is_standard_layout_v<SakuraUriCandidateSpanV1>);
static_assert(std::is_standard_layout_v<SakuraUriCandidateComponentsV1>);
static_assert(std::is_standard_layout_v<SakuraUriCandidateBufferV1>);
static_assert(std::is_standard_layout_v<SakuraUriCandidateBuffersV1>);
static_assert(std::is_standard_layout_v<SakuraUriCandidateMeasureV1>);
static_assert(std::is_trivially_copyable_v<SakuraUriCandidateSpanV1>);
static_assert(std::is_trivially_copyable_v<SakuraUriCandidateComponentsV1>);
static_assert(std::is_trivially_copyable_v<SakuraUriCandidateBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraUriCandidateBuffersV1>);
static_assert(std::is_trivially_copyable_v<SakuraUriCandidateMeasureV1>);

extern "C"
{
SakuraUriCandidateStatus sakura_uri_candidate_parse_measure_v1(
	const SakuraUriCandidateSpanV1* input,
	SakuraUriCandidateMeasureV1* output) noexcept;

SakuraUriCandidateStatus sakura_uri_candidate_parse_write_v1(
	const SakuraUriCandidateSpanV1* input,
	const SakuraUriCandidateBuffersV1* buffers,
	SakuraUriCandidateOutputV1* output) noexcept;

SakuraUriCandidateStatus sakura_uri_candidate_from_components_measure_v1(
	const SakuraUriCandidateComponentsV1* input,
	SakuraUriCandidateMeasureV1* output) noexcept;

SakuraUriCandidateStatus sakura_uri_candidate_from_components_write_v1(
	const SakuraUriCandidateComponentsV1* input,
	const SakuraUriCandidateBuffersV1* buffers,
	SakuraUriCandidateOutputV1* output) noexcept;
}

namespace platform::uri::rust_candidate {

//! Copied candidate output.  This is a test/shadow value and is not a
//! production URI identity authority.
struct Value final {
	std::wstring scheme;
	std::wstring authority;
	std::wstring path;
	std::optional<std::wstring> query;
	std::optional<std::wstring> fragment;
	bool hasAuthority = false;
	std::wstring serialized;
};

struct Result final {
	std::optional<Value> value;
	SakuraUriCandidateStatus status = SakuraUriCandidateStatus::InternalError;

	explicit operator bool() const noexcept { return value.has_value(); }
};

//! Runs the two-pass Rust parse candidate.  It is only a shadow adapter; it
//! performs no filesystem lookup, identity comparison, or provider selection.
[[nodiscard]] Result Parse(std::wstring_view text) noexcept;

//! Runs the two-pass Rust FromComponents candidate with copied UTF-16 inputs.
[[nodiscard]] Result FromComponents(
	std::wstring scheme,
	std::wstring authority,
	std::wstring path,
	std::optional<std::wstring> query = std::nullopt,
	std::optional<std::wstring> fragment = std::nullopt,
	bool hasAuthority = false) noexcept;

} // namespace platform::uri::rust_candidate
