/* C ABI declarations for the replay-only Rust OutputService shadow. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 = 1;

enum class SakuraOutputShadowStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	InvalidHandle = 2,
	Stopped = 3,
	InsufficientCapacity = 4,
	InternalError = 5,
};

enum class SakuraOutputShadowOperationStatus : std::uint32_t {
	Succeeded = 0,
	Replayed = 1,
	NotApplicable = 2,
	Rejected = 3,
	Conflict = 4,
	StaleRevision = 5,
	RevisionExhausted = 6,
	Stopped = 7,
};

enum class SakuraOutputShadowReason : std::uint32_t {
	None = 0,
	InvalidOperationId = 1,
	InvalidOwner = 2,
	InvalidChannelId = 3,
	InvalidLabel = 4,
	InvalidMetadata = 5,
	InvalidPayload = 6,
	PayloadLimitExceeded = 7,
	OwnerLimitExceeded = 8,
	ChannelLimitExceeded = 9,
	TextLimitExceeded = 10,
	LogEntryLimitExceeded = 11,
	ChannelNotFound = 12,
	OwnerGenerationConflict = 13,
	ChannelKindMismatch = 14,
	OperationIdConflict = 15,
	ExpectedRevisionMismatch = 16,
};

constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_REQUEST_HAS_EXPECTED_REVISION = 1U << 0;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_REQUEST_PRESERVE_FOCUS = 1U << 1;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_REQUEST_LANGUAGE_PRESENT = 1U << 2;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_REQUEST_SOURCE_PRESENT = 1U << 3;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_LOG_SOURCE_PRESENT = 1U;

constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_CREATE_CHANNEL = 1;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_APPEND_OUTPUT = 2;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_REPLACE_OUTPUT = 3;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_APPEND_LOG = 4;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_CLEAR = 5;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_SHOW = 6;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_HIDE = 7;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_DISPOSE = 8;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_OP_DISPOSE_OWNER = 9;

constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_CHANNEL_OUTPUT = 0;
constexpr std::uint32_t SAKURA_OUTPUT_SHADOW_CHANNEL_LOG = 1;

struct SakuraOutputShadowSpanV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	const std::uint8_t* data{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
};

struct SakuraOutputShadowLimitsV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint64_t maximum_owners{};
	std::uint64_t maximum_channels{};
	std::uint64_t maximum_text_bytes_per_channel{};
	std::uint64_t maximum_payload_bytes{};
	std::uint64_t maximum_log_entries_per_channel{};
	std::uint64_t maximum_remembered_operations{};
	std::uint64_t reserved[3]{};
};

struct SakuraOutputShadowLogEntryV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint32_t level{};
	std::uint32_t flags{};
	SakuraOutputShadowSpanV1 message{};
	SakuraOutputShadowSpanV1 source{};
	std::uint64_t reserved[2]{};
};

struct SakuraOutputShadowRequestV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint32_t operation_kind{};
	std::uint32_t channel_kind{};
	std::uint32_t flags{};
	SakuraOutputShadowSpanV1 operation_id{};
	std::uint64_t expected_revision{};
	SakuraOutputShadowSpanV1 owner_id{};
	std::uint64_t owner_generation{};
	SakuraOutputShadowSpanV1 channel_id{};
	SakuraOutputShadowSpanV1 label{};
	SakuraOutputShadowSpanV1 metadata_language_id{};
	SakuraOutputShadowSpanV1 metadata_source{};
	SakuraOutputShadowSpanV1 payload{};
	const SakuraOutputShadowLogEntryV1* log_entries{};
	std::uint64_t log_entry_count{};
	std::uint64_t reserved[4]{};
};

struct SakuraOutputShadowApplyResultV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint32_t status{};
	std::uint32_t reason{};
	std::uint64_t revision{};
	std::uint8_t callback_drain_deferred{};
	std::uint8_t reserved[7]{};
};

struct SakuraOutputShadowSnapshotInfoV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint64_t revision{};
	std::uint8_t stopped{};
	std::uint8_t active_channel_present{};
	std::uint8_t reserved0[6]{};
	std::uint64_t dropped_notification_count{};
	std::uint64_t channel_count{};
	std::uint64_t encoded_size{};
	std::uint64_t reserved[2]{};
};

struct SakuraOutputShadowSnapshotBufferV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1 };
	std::uint8_t* data{};
	std::uint64_t capacity{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
};

static_assert(std::is_standard_layout_v<SakuraOutputShadowSpanV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowLimitsV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowLogEntryV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowRequestV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowApplyResultV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowSnapshotInfoV1>);
static_assert(std::is_standard_layout_v<SakuraOutputShadowSnapshotBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowSpanV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowLimitsV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowLogEntryV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowRequestV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowApplyResultV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowSnapshotInfoV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputShadowSnapshotBufferV1>);
static_assert(sizeof(SakuraOutputShadowSpanV1) == 40);
static_assert(sizeof(SakuraOutputShadowLimitsV1) == 80);
static_assert(sizeof(SakuraOutputShadowLogEntryV1) == 112);
static_assert(sizeof(SakuraOutputShadowRequestV1) == 368);
static_assert(sizeof(SakuraOutputShadowApplyResultV1) == 32);
static_assert(sizeof(SakuraOutputShadowSnapshotInfoV1) == 64);
static_assert(sizeof(SakuraOutputShadowSnapshotBufferV1) == 48);
static_assert(offsetof(SakuraOutputShadowSpanV1, data) == 8);
static_assert(offsetof(SakuraOutputShadowSpanV1, length) == 16);
static_assert(offsetof(SakuraOutputShadowRequestV1, operation_id) == 24);
static_assert(offsetof(SakuraOutputShadowRequestV1, expected_revision) == 64);
static_assert(offsetof(SakuraOutputShadowRequestV1, log_entries) == 320);
static_assert(offsetof(SakuraOutputShadowApplyResultV1, revision) == 16);
static_assert(offsetof(SakuraOutputShadowSnapshotInfoV1, dropped_notification_count) == 24);
static_assert(offsetof(SakuraOutputShadowSnapshotBufferV1, data) == 8);

extern "C"
{
SakuraOutputShadowStatus sakura_output_shadow_create_v1(
	const SakuraOutputShadowLimitsV1* limits,
	std::uint64_t* token) noexcept;

SakuraOutputShadowStatus sakura_output_shadow_apply_v1(
	std::uint64_t token,
	const SakuraOutputShadowRequestV1* request,
	SakuraOutputShadowApplyResultV1* result) noexcept;

SakuraOutputShadowStatus sakura_output_shadow_snapshot_measure_v1(
	std::uint64_t token,
	SakuraOutputShadowSnapshotInfoV1* info) noexcept;

SakuraOutputShadowStatus sakura_output_shadow_snapshot_write_v1(
	std::uint64_t token,
	SakuraOutputShadowSnapshotBufferV1* buffer) noexcept;

SakuraOutputShadowStatus sakura_output_shadow_stop_v1(
	std::uint64_t token,
	SakuraOutputShadowApplyResultV1* result) noexcept;

SakuraOutputShadowStatus sakura_output_shadow_destroy_v1(std::uint64_t* token) noexcept;
}
