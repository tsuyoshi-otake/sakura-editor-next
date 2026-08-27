/* C ABI declarations for the callback-free Rust OutputService provider. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 = 1;

enum class SakuraOutputProviderStatus : std::uint32_t {
	Ok = 0,
	InvalidArgument = 1,
	InvalidHandle = 2,
	Stopped = 3,
	InsufficientCapacity = 4,
	InternalError = 5,
};

enum class SakuraOutputProviderOperationStatus : std::uint32_t {
	Succeeded = 0,
	Replayed = 1,
	NotApplicable = 2,
	Rejected = 3,
	Conflict = 4,
	StaleRevision = 5,
	RevisionExhausted = 6,
	Stopped = 7,
};

enum class SakuraOutputProviderReason : std::uint32_t {
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

constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_REQUEST_HAS_EXPECTED_REVISION = 1U << 0;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_REQUEST_PRESERVE_FOCUS = 1U << 1;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_REQUEST_LANGUAGE_PRESENT = 1U << 2;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_REQUEST_SOURCE_PRESENT = 1U << 3;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_LOG_SOURCE_PRESENT = 1U;

constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_CREATE_CHANNEL = 1;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_APPEND_OUTPUT = 2;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_REPLACE_OUTPUT = 3;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_APPEND_LOG = 4;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_CLEAR = 5;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_SHOW = 6;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_HIDE = 7;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_DISPOSE = 8;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_OP_DISPOSE_OWNER = 9;

constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_CHANNEL_OUTPUT = 0;
constexpr std::uint32_t SAKURA_OUTPUT_PROVIDER_CHANNEL_LOG = 1;

struct SakuraOutputProviderSpanV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	const std::uint8_t* data{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
};

struct SakuraOutputProviderLimitsV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint64_t maximum_owners{};
	std::uint64_t maximum_channels{};
	std::uint64_t maximum_text_bytes_per_channel{};
	std::uint64_t maximum_payload_bytes{};
	std::uint64_t maximum_log_entries_per_channel{};
	std::uint64_t maximum_remembered_operations{};
	std::uint64_t reserved[3]{};
};

struct SakuraOutputProviderLogEntryV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint32_t level{};
	std::uint32_t flags{};
	SakuraOutputProviderSpanV1 message{};
	SakuraOutputProviderSpanV1 source{};
	std::uint64_t reserved[2]{};
};

struct SakuraOutputProviderRequestV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint32_t operation_kind{};
	std::uint32_t channel_kind{};
	std::uint32_t flags{};
	SakuraOutputProviderSpanV1 operation_id{};
	std::uint64_t expected_revision{};
	SakuraOutputProviderSpanV1 owner_id{};
	std::uint64_t owner_generation{};
	SakuraOutputProviderSpanV1 channel_id{};
	SakuraOutputProviderSpanV1 label{};
	SakuraOutputProviderSpanV1 metadata_language_id{};
	SakuraOutputProviderSpanV1 metadata_source{};
	SakuraOutputProviderSpanV1 payload{};
	const SakuraOutputProviderLogEntryV1* log_entries{};
	std::uint64_t log_entry_count{};
	std::uint64_t reserved[4]{};
};

struct SakuraOutputProviderApplyResultV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint32_t status{};
	std::uint32_t reason{};
	std::uint64_t revision{};
	std::uint8_t callback_drain_deferred{};
	std::uint8_t reserved[7]{};
};

// ABI V1 receipt extension for the measure/write snapshot transaction.  The
// seven export names remain unchanged, but V1 now requires the exact struct
// sizes below (pre-receipt V1 descriptors are rejected).  All in-tree callers
// must use this layout atomically; a future incompatible layout needs a new
// ABI version.  The caller copies this fixed-width receipt from measure info
// into the write buffer.  The measurement id binds it to one provider token
// and permits multiple callers to have outstanding measurements without a
// last-measure race.  The provider contract advances revision for every
// accepted state mutation; the duplicated framing fields and advisory drop
// counter complete the identity check without retaining caller memory or
// encoding the snapshot during measurement.
struct SakuraOutputProviderSnapshotReceiptV1 final {
	std::uint64_t measurement_id{};
	std::uint64_t revision{};
	std::uint64_t dropped_notification_count{};
	std::uint64_t channel_count{};
	std::uint64_t encoded_size{};
	std::uint8_t stopped{};
	std::uint8_t active_channel_present{};
	std::uint8_t reserved[6]{};
};

struct SakuraOutputProviderSnapshotInfoV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint64_t revision{};
	std::uint8_t stopped{};
	std::uint8_t active_channel_present{};
	std::uint8_t reserved0[6]{};
	std::uint64_t dropped_notification_count{};
	std::uint64_t channel_count{};
	std::uint64_t encoded_size{};
	std::uint64_t reserved[2]{};
	SakuraOutputProviderSnapshotReceiptV1 receipt{};
};

struct SakuraOutputProviderSnapshotBufferV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint8_t* data{};
	std::uint64_t capacity{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
	SakuraOutputProviderSnapshotReceiptV1 receipt{};
};

//! Small post-commit metadata query used for advisory notifications.
//! It avoids copying the complete channel snapshot for every mutation.
struct SakuraOutputProviderActiveChannelV1 final {
	std::uint32_t struct_size{};
	std::uint32_t abi_version{ SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 };
	std::uint64_t revision{};
	std::uint8_t present{};
	std::uint8_t reserved0[7]{};
	std::uint8_t* data{};
	std::uint64_t capacity{};
	std::uint64_t length{};
	std::uint64_t reserved[2]{};
};

static_assert(std::is_standard_layout_v<SakuraOutputProviderSpanV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderLimitsV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderLogEntryV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderRequestV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderApplyResultV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderSnapshotReceiptV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderSnapshotInfoV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderSnapshotBufferV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderSpanV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderLimitsV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderLogEntryV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderRequestV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderApplyResultV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderSnapshotReceiptV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderSnapshotInfoV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderSnapshotBufferV1>);
static_assert(std::is_standard_layout_v<SakuraOutputProviderActiveChannelV1>);
static_assert(std::is_trivially_copyable_v<SakuraOutputProviderActiveChannelV1>);
static_assert(sizeof(SakuraOutputProviderSpanV1) == 40);
static_assert(sizeof(SakuraOutputProviderLimitsV1) == 80);
static_assert(sizeof(SakuraOutputProviderLogEntryV1) == 112);
static_assert(sizeof(SakuraOutputProviderRequestV1) == 368);
static_assert(sizeof(SakuraOutputProviderApplyResultV1) == 32);
static_assert(sizeof(SakuraOutputProviderSnapshotReceiptV1) == 48);
static_assert(sizeof(SakuraOutputProviderSnapshotInfoV1) == 112);
static_assert(sizeof(SakuraOutputProviderSnapshotBufferV1) == 96);
static_assert(sizeof(SakuraOutputProviderActiveChannelV1) == 64);
static_assert(offsetof(SakuraOutputProviderSpanV1, data) == 8);
static_assert(offsetof(SakuraOutputProviderSpanV1, length) == 16);
static_assert(offsetof(SakuraOutputProviderRequestV1, operation_id) == 24);
static_assert(offsetof(SakuraOutputProviderRequestV1, expected_revision) == 64);
static_assert(offsetof(SakuraOutputProviderRequestV1, log_entries) == 320);
static_assert(offsetof(SakuraOutputProviderApplyResultV1, revision) == 16);
static_assert(offsetof(SakuraOutputProviderSnapshotInfoV1, dropped_notification_count) == 24);
static_assert(offsetof(SakuraOutputProviderSnapshotInfoV1, receipt) == 64);
static_assert(offsetof(SakuraOutputProviderSnapshotReceiptV1, revision) == 8);
static_assert(offsetof(SakuraOutputProviderSnapshotReceiptV1, dropped_notification_count) == 16);
static_assert(offsetof(SakuraOutputProviderSnapshotReceiptV1, stopped) == 40);
static_assert(offsetof(SakuraOutputProviderSnapshotBufferV1, data) == 8);
static_assert(offsetof(SakuraOutputProviderActiveChannelV1, data) == 24);
static_assert(offsetof(SakuraOutputProviderActiveChannelV1, length) == 40);
static_assert(offsetof(SakuraOutputProviderSnapshotBufferV1, receipt) == 48);

extern "C"
{
SakuraOutputProviderStatus sakura_output_provider_create_v1(
	const SakuraOutputProviderLimitsV1* limits,
	std::uint64_t* token) noexcept;

SakuraOutputProviderStatus sakura_output_provider_apply_v1(
	std::uint64_t token,
	const SakuraOutputProviderRequestV1* request,
	SakuraOutputProviderApplyResultV1* result) noexcept;

SakuraOutputProviderStatus sakura_output_provider_snapshot_measure_v1(
	std::uint64_t token,
	SakuraOutputProviderSnapshotInfoV1* info) noexcept;

SakuraOutputProviderStatus sakura_output_provider_snapshot_write_v1(
	std::uint64_t token,
	SakuraOutputProviderSnapshotBufferV1* buffer) noexcept;

SakuraOutputProviderStatus sakura_output_provider_active_channel_v1(
	std::uint64_t token,
	SakuraOutputProviderActiveChannelV1* active) noexcept;

SakuraOutputProviderStatus sakura_output_provider_stop_v1(
	std::uint64_t token,
	SakuraOutputProviderApplyResultV1* result) noexcept;

SakuraOutputProviderStatus sakura_output_provider_destroy_v1(std::uint64_t* token) noexcept;
}
