/* Differential tests for the replay-only Rust OutputService shadow. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "OutputServiceRustShadowAbi.h"
#include "workbench/output/OutputService.h"
#include "workbench/output/OutputServiceRustCandidate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::output {
namespace {

#if defined(SAKURA_UTF16_RUST_CANDIDATE)

constexpr std::string_view kSnapshotMagic("SAKURA_OUTPUT_SHADOW_V1\0", 24);

struct RustApplyResponse final {
	SakuraOutputShadowStatus abiStatus{ SakuraOutputShadowStatus::InternalError };
	OutputOperationResult operation{};
};

struct SnapshotReader final {
	const std::vector<std::uint8_t>& bytes;
	std::size_t offset{};

	[[nodiscard]] std::size_t Remaining() const noexcept
	{
		return offset <= bytes.size() ? bytes.size() - offset : 0;
	}

	[[nodiscard]] bool ReadByte(std::uint8_t& value) noexcept
	{
		if (Remaining() < 1) return false;
		value = bytes[offset++];
		return true;
	}

	[[nodiscard]] bool ReadU32(std::uint32_t& value) noexcept
	{
		if (Remaining() < sizeof(value)) return false;
		value = static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
		offset += sizeof(value);
		return true;
	}

	[[nodiscard]] bool ReadU64(std::uint64_t& value) noexcept
	{
		if (Remaining() < sizeof(value)) return false;
		value = 0;
		for (std::size_t index = 0; index < sizeof(value); ++index) {
			value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
		}
		offset += sizeof(value);
		return true;
	}

	[[nodiscard]] bool ReadBytes(std::string& value)
	{
		std::uint64_t length{};
		if (!ReadU64(length) || length > Remaining()
			|| length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			return false;
		}
		const auto size = static_cast<std::size_t>(length);
		value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
		offset += size;
		return true;
	}

	[[nodiscard]] bool ReadOptionalBytes(std::optional<std::string>& value)
	{
		std::uint8_t present{};
		if (!ReadByte(present) || present > 1) return false;
		if (present == 0) {
			value.reset();
			return true;
		}
		std::string copied;
		if (!ReadBytes(copied)) return false;
		value = std::move(copied);
		return true;
	}
};

[[nodiscard]] std::optional<OutputServiceSnapshot> ParseSnapshot(const std::vector<std::uint8_t>& bytes)
{
	SnapshotReader reader{ bytes };
	if (reader.Remaining() < kSnapshotMagic.size()
		|| !std::equal(kSnapshotMagic.begin(), kSnapshotMagic.end(), bytes.begin())) {
		return std::nullopt;
	}
	reader.offset = kSnapshotMagic.size();

	OutputServiceSnapshot snapshot;
	if (!reader.ReadU64(snapshot.revision)) return std::nullopt;
	std::uint8_t stopped{};
	if (!reader.ReadByte(stopped) || stopped > 1) return std::nullopt;
	snapshot.stopped = stopped != 0;
	if (!reader.ReadU64(snapshot.droppedNotificationCount)) return std::nullopt;

	std::uint8_t activePresent{};
	if (!reader.ReadByte(activePresent) || activePresent > 1) return std::nullopt;
	if (activePresent != 0) {
		std::string active;
		if (!reader.ReadBytes(active)) return std::nullopt;
		snapshot.activeChannelId = std::move(active);
	}

	std::uint64_t channelCount{};
	if (!reader.ReadU64(channelCount)
		|| channelCount > reader.Remaining()) {
		return std::nullopt;
	}
	if (channelCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return std::nullopt;
	snapshot.channels.reserve(static_cast<std::size_t>(channelCount));
	for (std::uint64_t index = 0; index < channelCount; ++index) {
		OutputChannelSnapshot channel;
		if (!reader.ReadBytes(channel.channelId)
			|| !reader.ReadBytes(channel.label)
			|| !reader.ReadBytes(channel.owner.ownerId)
			|| !reader.ReadU64(channel.owner.generation)) {
			return std::nullopt;
		}
		std::uint8_t kind{};
		if (!reader.ReadByte(kind) || kind > SAKURA_OUTPUT_SHADOW_CHANNEL_LOG) return std::nullopt;
		channel.kind = static_cast<EOutputChannelKind>(kind);
		if (!reader.ReadOptionalBytes(channel.metadata.languageId)
			|| !reader.ReadOptionalBytes(channel.metadata.source)) {
			return std::nullopt;
		}
		std::uint8_t visible{};
		std::uint8_t preservedFocus{};
		if (!reader.ReadByte(visible) || visible > 1
			|| !reader.ReadByte(preservedFocus) || preservedFocus > 1
			|| !reader.ReadU64(channel.droppedCharacterCount)
			|| !reader.ReadBytes(channel.text)) {
			return std::nullopt;
		}
		channel.visible = visible != 0;
		channel.lastShowPreservedFocus = preservedFocus != 0;

		std::uint64_t logCount{};
		if (!reader.ReadU64(logCount) || logCount > reader.Remaining()
			|| logCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			return std::nullopt;
		}
		channel.logEntries.reserve(static_cast<std::size_t>(logCount));
		for (std::uint64_t logIndex = 0; logIndex < logCount; ++logIndex) {
			std::uint32_t level{};
			OutputLogEntry entry;
			if (!reader.ReadU32(level) || level > static_cast<std::uint32_t>(EOutputLogLevel::Error)
				|| !reader.ReadBytes(entry.message)
				|| !reader.ReadOptionalBytes(entry.source)) {
				return std::nullopt;
			}
			entry.level = static_cast<EOutputLogLevel>(level);
			channel.logEntries.push_back(std::move(entry));
		}
		if (!reader.ReadBytes(channel.projectedText)) return std::nullopt;
		snapshot.channels.push_back(std::move(channel));
	}
	if (reader.Remaining() != 0) return std::nullopt;
	return snapshot;
}

[[nodiscard]] SakuraOutputShadowSpanV1 Span(const std::string_view value) noexcept
{
	SakuraOutputShadowSpanV1 span{};
	span.struct_size = sizeof(span);
	span.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
	span.data = reinterpret_cast<const std::uint8_t*>(value.data());
	span.length = static_cast<std::uint64_t>(value.size());
	return span;
}

struct PendingRequest final {
	SakuraOutputShadowRequestV1 raw{};
	std::vector<SakuraOutputShadowLogEntryV1> logEntries;
};

void FillCommon(
	PendingRequest& pending,
	const std::uint32_t operationKind,
	const OutputOperation& operation,
	const OutputOwner& owner,
	const std::string_view channelId)
{
	pending.raw.struct_size = sizeof(pending.raw);
	pending.raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
	const auto emptySpan = Span(std::string_view{});
	pending.raw.operation_id = emptySpan;
	pending.raw.owner_id = emptySpan;
	pending.raw.channel_id = emptySpan;
	pending.raw.label = emptySpan;
	pending.raw.metadata_language_id = emptySpan;
	pending.raw.metadata_source = emptySpan;
	pending.raw.payload = emptySpan;
	pending.raw.operation_kind = operationKind;
	pending.raw.operation_id = Span(operation.operationId);
	pending.raw.flags = operation.expectedRevision
		? SAKURA_OUTPUT_SHADOW_REQUEST_HAS_EXPECTED_REVISION
		: 0;
	pending.raw.expected_revision = operation.expectedRevision.value_or(0);
	pending.raw.owner_id = Span(owner.ownerId);
	pending.raw.owner_generation = owner.generation;
	pending.raw.channel_id = Span(channelId);
}

void FillCreate(PendingRequest& pending, const OutputCreateChannelRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_SHADOW_OP_CREATE_CHANNEL, request.operation,
		request.owner, request.channelId);
	pending.raw.channel_kind = static_cast<std::uint32_t>(request.kind);
	pending.raw.label = Span(request.label);
	if (request.metadata.languageId) {
		pending.raw.flags |= SAKURA_OUTPUT_SHADOW_REQUEST_LANGUAGE_PRESENT;
		pending.raw.metadata_language_id = Span(*request.metadata.languageId);
	}
	if (request.metadata.source) {
		pending.raw.flags |= SAKURA_OUTPUT_SHADOW_REQUEST_SOURCE_PRESENT;
		pending.raw.metadata_source = Span(*request.metadata.source);
	}
}

void FillText(PendingRequest& pending, const std::uint32_t operationKind, const OutputTextMutationRequest& request)
{
	FillCommon(pending, operationKind, request.operation, request.owner, request.channelId);
	pending.raw.payload = Span(request.text);
}

void FillLog(PendingRequest& pending, const OutputLogMutationRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_SHADOW_OP_APPEND_LOG, request.operation,
		request.owner, request.channelId);
	pending.logEntries.reserve(request.entries.size());
	for (const auto& entry : request.entries) {
		SakuraOutputShadowLogEntryV1 raw{};
		raw.struct_size = sizeof(raw);
		raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		raw.level = static_cast<std::uint32_t>(entry.level);
		raw.message = Span(entry.message);
		if (entry.source) {
			raw.flags = SAKURA_OUTPUT_SHADOW_LOG_SOURCE_PRESENT;
			raw.source = Span(*entry.source);
		}
		pending.logEntries.push_back(raw);
	}
	pending.raw.log_entries = pending.logEntries.empty() ? nullptr : pending.logEntries.data();
	pending.raw.log_entry_count = static_cast<std::uint64_t>(pending.logEntries.size());
}

void FillChannel(
	PendingRequest& pending,
	const std::uint32_t operationKind,
	const OutputChannelMutationRequest& request)
{
	FillCommon(pending, operationKind, request.operation, request.owner, request.channelId);
}

void FillShow(PendingRequest& pending, const OutputShowChannelRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_SHADOW_OP_SHOW, request.operation,
		request.owner, request.channelId);
	if (request.preserveFocus) pending.raw.flags |= SAKURA_OUTPUT_SHADOW_REQUEST_PRESERVE_FOCUS;
}

void FillDisposeOwner(PendingRequest& pending, const OutputDisposeOwnerRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_SHADOW_OP_DISPOSE_OWNER, request.operation,
		request.owner, {});
}

[[nodiscard]] bool IsValidApplyResult(const SakuraOutputShadowApplyResultV1& result) noexcept
{
	return result.struct_size == sizeof(result)
		&& result.abi_version == SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
		&& result.status <= static_cast<std::uint32_t>(SakuraOutputShadowOperationStatus::Stopped)
		&& result.reason <= static_cast<std::uint32_t>(SakuraOutputShadowReason::ExpectedRevisionMismatch)
		&& result.callback_drain_deferred <= 1
		&& std::all_of(std::begin(result.reserved), std::end(result.reserved), [](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] OutputOperationResult ToOperationResult(const SakuraOutputShadowApplyResultV1& result) noexcept
{
	return {
		.status = static_cast<EOutputOperationStatus>(result.status),
		.reason = static_cast<EOutputOperationReason>(result.reason),
		.revision = result.revision,
		.callbackDrainDeferred = result.callback_drain_deferred != 0,
	};
}

class RustShadowAdapter final {
public:
	explicit RustShadowAdapter(const OutputServiceLimits& limits)
	{
		SakuraOutputShadowLimitsV1 raw{};
		raw.struct_size = sizeof(raw);
		raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		raw.maximum_owners = static_cast<std::uint64_t>(limits.maximumOwners);
		raw.maximum_channels = static_cast<std::uint64_t>(limits.maximumChannels);
		raw.maximum_text_bytes_per_channel = static_cast<std::uint64_t>(limits.maximumTextBytesPerChannel);
		raw.maximum_payload_bytes = static_cast<std::uint64_t>(limits.maximumPayloadBytes);
		raw.maximum_log_entries_per_channel = static_cast<std::uint64_t>(limits.maximumLogEntriesPerChannel);
		raw.maximum_remembered_operations = static_cast<std::uint64_t>(limits.maximumRememberedOperations);
		createStatus_ = sakura_output_shadow_create_v1(&raw, &token_);
	}

	~RustShadowAdapter()
	{
		if (token_ != 0) (void)sakura_output_shadow_destroy_v1(&token_);
	}

	RustShadowAdapter(const RustShadowAdapter&) = delete;
	RustShadowAdapter& operator=(const RustShadowAdapter&) = delete;

	[[nodiscard]] SakuraOutputShadowStatus CreateStatus() const noexcept { return createStatus_; }
	[[nodiscard]] std::uint64_t Token() const noexcept { return token_; }

	[[nodiscard]] RustApplyResponse Apply(const OutputCreateChannelRequest& request)
	{
		PendingRequest pending;
		FillCreate(pending, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse AppendOutput(const OutputTextMutationRequest& request)
	{
		PendingRequest pending;
		FillText(pending, SAKURA_OUTPUT_SHADOW_OP_APPEND_OUTPUT, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse ReplaceOutput(const OutputTextMutationRequest& request)
	{
		PendingRequest pending;
		FillText(pending, SAKURA_OUTPUT_SHADOW_OP_REPLACE_OUTPUT, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse AppendLog(const OutputLogMutationRequest& request)
	{
		PendingRequest pending;
		FillLog(pending, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse Clear(const OutputChannelMutationRequest& request)
	{
		PendingRequest pending;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_CLEAR, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse Show(const OutputShowChannelRequest& request)
	{
		PendingRequest pending;
		FillShow(pending, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse Hide(const OutputChannelMutationRequest& request)
	{
		PendingRequest pending;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_HIDE, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse Dispose(const OutputChannelMutationRequest& request)
	{
		PendingRequest pending;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_DISPOSE, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse DisposeOwner(const OutputDisposeOwnerRequest& request)
	{
		PendingRequest pending;
		FillDisposeOwner(pending, request);
		return Invoke(pending);
	}

	[[nodiscard]] RustApplyResponse Stop()
	{
		SakuraOutputShadowApplyResultV1 raw{};
		raw.struct_size = sizeof(raw);
		raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		const auto status = sakura_output_shadow_stop_v1(token_, &raw);
		return MakeResponse(status, raw);
	}

	[[nodiscard]] SakuraOutputShadowStatus Destroy() noexcept
	{
		return sakura_output_shadow_destroy_v1(&token_);
	}

	[[nodiscard]] SakuraOutputShadowStatus Snapshot(OutputServiceSnapshot& snapshot)
	{
		SakuraOutputShadowSnapshotInfoV1 info{};
		info.struct_size = sizeof(info);
		info.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		const auto measured = sakura_output_shadow_snapshot_measure_v1(token_, &info);
		if (measured != SakuraOutputShadowStatus::Ok) return measured;
		if (info.struct_size != sizeof(info)
			|| info.abi_version != SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
			|| info.stopped > 1
			|| info.active_channel_present > 1
			|| !std::all_of(std::begin(info.reserved0), std::end(info.reserved0), [](const std::uint8_t value) { return value == 0; })
			|| !std::all_of(std::begin(info.reserved), std::end(info.reserved), [](const std::uint64_t value) { return value == 0; })) {
			return SakuraOutputShadowStatus::InternalError;
		}
		if (info.encoded_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) return SakuraOutputShadowStatus::InternalError;
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.encoded_size));
		SakuraOutputShadowSnapshotBufferV1 buffer{};
		buffer.struct_size = sizeof(buffer);
		buffer.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		buffer.data = bytes.empty() ? nullptr : bytes.data();
		buffer.capacity = static_cast<std::uint64_t>(bytes.size());
		const auto written = sakura_output_shadow_snapshot_write_v1(token_, &buffer);
		if (written != SakuraOutputShadowStatus::Ok) return written;
		if (buffer.struct_size != sizeof(buffer)
			|| buffer.abi_version != SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
			|| buffer.length != info.encoded_size
			|| !std::all_of(std::begin(buffer.reserved), std::end(buffer.reserved), [](const std::uint64_t value) { return value == 0; })) {
			return SakuraOutputShadowStatus::InternalError;
		}
		const auto parsed = ParseSnapshot(bytes);
		if (!parsed || parsed->revision != info.revision
			|| parsed->stopped != (info.stopped != 0)
			|| parsed->droppedNotificationCount != info.dropped_notification_count
			|| parsed->channels.size() != info.channel_count
			|| parsed->activeChannelId.has_value() != (info.active_channel_present != 0)) {
			return SakuraOutputShadowStatus::InternalError;
		}
		snapshot = *parsed;
		return SakuraOutputShadowStatus::Ok;
	}

private:
	[[nodiscard]] static RustApplyResponse MakeResponse(
		const SakuraOutputShadowStatus abiStatus,
		const SakuraOutputShadowApplyResultV1& raw) noexcept
	{
		RustApplyResponse response{ .abiStatus = abiStatus };
		if (abiStatus == SakuraOutputShadowStatus::Ok
			|| abiStatus == SakuraOutputShadowStatus::Stopped) {
			if (IsValidApplyResult(raw)) response.operation = ToOperationResult(raw);
		}
		return response;
	}

	[[nodiscard]] RustApplyResponse Invoke(const PendingRequest& pending)
	{
		SakuraOutputShadowApplyResultV1 raw{};
		raw.struct_size = sizeof(raw);
		raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
		const auto status = sakura_output_shadow_apply_v1(token_, &pending.raw, &raw);
		return MakeResponse(status, raw);
	}

	SakuraOutputShadowStatus createStatus_{ SakuraOutputShadowStatus::InternalError };
	std::uint64_t token_{};
};

OutputOwner Owner(std::string id, const std::uint64_t generation = 1)
{
	return { .ownerId = std::move(id), .generation = generation };
}

OutputOperation Operation(std::string id, const std::optional<std::uint64_t> revision = std::nullopt)
{
	return { .operationId = std::move(id), .expectedRevision = revision };
}

OutputCreateChannelRequest Create(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const EOutputChannelKind kind = EOutputChannelKind::Output)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.label = "Test channel",
		.kind = kind,
		.metadata = { .languageId = std::string("plaintext"), .source = std::string("tests") },
	};
}

OutputTextMutationRequest Text(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	std::string value,
	const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return {
		.operation = Operation(std::move(operationId), expectedRevision),
		.owner = std::move(owner),
		.channelId = std::move(channelId),
		.text = std::move(value),
	};
}

OutputChannelMutationRequest Channel(std::string operationId, OutputOwner owner, std::string channelId)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner), .channelId = std::move(channelId) };
}

OutputShowChannelRequest Show(
	std::string operationId,
	OutputOwner owner,
	std::string channelId,
	const bool preserveFocus = false)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner),
		.channelId = std::move(channelId), .preserveFocus = preserveFocus };
}

OutputDisposeOwnerRequest DisposeOwner(std::string operationId, OutputOwner owner)
{
	return { .operation = Operation(std::move(operationId)), .owner = std::move(owner) };
}

void ExpectSnapshotsEqual(const OutputServiceSnapshot& expected, const OutputServiceSnapshot& actual)
{
	ASSERT_EQ(expected.revision, actual.revision);
	ASSERT_EQ(expected.stopped, actual.stopped);
	ASSERT_EQ(expected.droppedNotificationCount, actual.droppedNotificationCount);
	ASSERT_EQ(expected.activeChannelId, actual.activeChannelId);
	ASSERT_EQ(expected.channels.size(), actual.channels.size());
	for (std::size_t index = 0; index < expected.channels.size(); ++index) {
		const auto& left = expected.channels[index];
		const auto& right = actual.channels[index];
		EXPECT_EQ(left.channelId, right.channelId);
		EXPECT_EQ(left.label, right.label);
		EXPECT_EQ(left.owner, right.owner);
		EXPECT_EQ(left.kind, right.kind);
		EXPECT_EQ(left.metadata, right.metadata);
		EXPECT_EQ(left.visible, right.visible);
		EXPECT_EQ(left.lastShowPreservedFocus, right.lastShowPreservedFocus);
		EXPECT_EQ(left.droppedCharacterCount, right.droppedCharacterCount);
		EXPECT_EQ(left.text, right.text);
		EXPECT_EQ(left.logEntries, right.logEntries);
		EXPECT_EQ(left.projectedText, right.projectedText);
	}
}

template <typename CppCall, typename RustCall>
void ExpectEquivalentAfterMutation(
	OutputService& cpp,
	RustShadowAdapter& rust,
	CppCall&& cppCall,
	RustCall&& rustCall)
{
	const auto expected = cppCall();
	const auto actual = rustCall();
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, actual.abiStatus);
	EXPECT_EQ(expected.status, actual.operation.status);
	EXPECT_EQ(expected.reason, actual.operation.reason);
	EXPECT_EQ(expected.revision, actual.operation.revision);
	EXPECT_EQ(expected.callbackDrainDeferred, actual.operation.callbackDrainDeferred);
	OutputServiceSnapshot shadowSnapshot;
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.Snapshot(shadowSnapshot));
	ExpectSnapshotsEqual(cpp.Snapshot(), shadowSnapshot);
}

void ExpectStopEquivalent(OutputService& cpp, RustShadowAdapter& rust)
{
	const auto expected = cpp.Stop();
	const auto actual = rust.Stop();
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, actual.abiStatus);
	EXPECT_EQ(expected.status, actual.operation.status);
	EXPECT_EQ(expected.reason, actual.operation.reason);
	EXPECT_EQ(expected.revision, actual.operation.revision);
	EXPECT_EQ(expected.callbackDrainDeferred, actual.operation.callbackDrainDeferred);
	OutputServiceSnapshot shadowSnapshot;
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.Snapshot(shadowSnapshot));
	ExpectSnapshotsEqual(cpp.Snapshot(), shadowSnapshot);
}

TEST(OutputServiceRustCandidate, FullOperationScriptMatchesCppAuthority)
{
	OutputServiceLimits limits;
	limits.maximumPayloadBytes = 32;
	limits.maximumTextBytesPerChannel = 8;
	limits.maximumLogEntriesPerChannel = 2;
	limits.maximumRememberedOperations = 64;
	OutputService cpp(limits);
	RustShadowAdapter rust(limits);
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.CreateStatus());
	ASSERT_NE(0U, rust.Token());

	const auto ownerA = Owner("owner.a", 1);
	const auto ownerB = Owner("owner.b", 1);
	const auto createOutput = Create("create-output", ownerA, "z.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(createOutput); },
		[&] { return rust.Apply(createOutput); });
	const auto createLog = Create("create-log", ownerA, "a.log", EOutputChannelKind::Log);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(createLog); },
		[&] { return rust.Apply(createLog); });
	const auto createOther = Create("create-other", ownerB, "b.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(createOther); },
		[&] { return rust.Apply(createOther); });

	const auto append = Text("append-output", ownerA, "z.output", "abcdef");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(append); },
		[&] { return rust.AppendOutput(append); });
	const auto replace = Text("replace-output", ownerA, "z.output", "xy");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.ReplaceOutput(replace); },
		[&] { return rust.ReplaceOutput(replace); });

	const OutputLogMutationRequest appendLog{
		.operation = Operation("append-log"),
		.owner = ownerA,
		.channelId = "a.log",
		.entries = {
			{ .level = EOutputLogLevel::Warning, .message = "first", .source = std::string("compiler") },
			{ .level = EOutputLogLevel::Error, .message = "second" },
		},
	};
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendLog(appendLog); },
		[&] { return rust.AppendLog(appendLog); });
	const auto show = Show("show-output", ownerA, "z.output", true);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.Show(show); },
		[&] { return rust.Show(show); });
	const auto hide = Channel("hide-output", ownerA, "z.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.Hide(hide); },
		[&] { return rust.Hide(hide); });
	const auto clearLog = Channel("clear-log", ownerA, "a.log");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.Clear(clearLog); },
		[&] { return rust.Clear(clearLog); });
	const auto disposeLog = Channel("dispose-log", ownerA, "a.log");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.Dispose(disposeLog); },
		[&] { return rust.Dispose(disposeLog); });
	const auto disposeOtherOwner = DisposeOwner("dispose-other-owner", ownerB);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.DisposeOwner(disposeOtherOwner); },
		[&] { return rust.DisposeOwner(disposeOtherOwner); });

	const auto newGeneration = Owner("owner.a", 2);
	const auto replaceGeneration = Create("create-new-generation", newGeneration, "new.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(replaceGeneration); },
		[&] { return rust.Apply(replaceGeneration); });
	const auto staleGenerationText = Text("stale-generation-text", ownerA, "new.output", "old");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(staleGenerationText); },
		[&] { return rust.AppendOutput(staleGenerationText); });
	const auto replay = Text("replay-output", newGeneration, "new.output", "replay");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(replay); },
		[&] { return rust.AppendOutput(replay); });
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(replay); },
		[&] { return rust.AppendOutput(replay); });
	const auto conflict = Text("replay-output", newGeneration, "new.output", "different");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(conflict); },
		[&] { return rust.AppendOutput(conflict); });

	const auto disposeNewOwner = DisposeOwner("dispose-new-owner", newGeneration);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.DisposeOwner(disposeNewOwner); },
		[&] { return rust.DisposeOwner(disposeNewOwner); });
	const auto tombstoneConflict = Create("tombstone-conflict", newGeneration, "late.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(tombstoneConflict); },
		[&] { return rust.Apply(tombstoneConflict); });
	const auto newestGeneration = Create("create-newest-generation", Owner("owner.a", 3), "latest.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(newestGeneration); },
		[&] { return rust.Apply(newestGeneration); });

	ExpectStopEquivalent(cpp, rust);
	const auto afterStop = Text("after-stop", Owner("owner.a", 3), "latest.output", "ignored");
	const auto stoppedCpp = cpp.AppendOutput(afterStop);
	const auto stoppedRust = rust.AppendOutput(afterStop);
	EXPECT_EQ(EOutputOperationStatus::Stopped, stoppedCpp.status);
	EXPECT_EQ(EOutputOperationStatus::Stopped, stoppedRust.operation.status);
	EXPECT_EQ(stoppedCpp.revision, stoppedRust.operation.revision);
	EXPECT_EQ(SakuraOutputShadowStatus::Stopped, stoppedRust.abiStatus);
	OutputServiceSnapshot stoppedSnapshot;
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.Snapshot(stoppedSnapshot));
	ExpectSnapshotsEqual(cpp.Snapshot(), stoppedSnapshot);

	ExpectStopEquivalent(cpp, rust);
	const auto staleToken = rust.Token();
	EXPECT_EQ(SakuraOutputShadowStatus::Ok, rust.Destroy());
	EXPECT_EQ(0U, rust.Token());
	SakuraOutputShadowSnapshotInfoV1 staleInfo{};
	EXPECT_EQ(SakuraOutputShadowStatus::InvalidHandle,
		sakura_output_shadow_snapshot_measure_v1(staleToken, &staleInfo));
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(), staleInfo.revision);
	auto staleDestroyToken = staleToken;
	EXPECT_EQ(SakuraOutputShadowStatus::InvalidHandle,
		sakura_output_shadow_destroy_v1(&staleDestroyToken));
	EXPECT_EQ(staleToken, staleDestroyToken);
	EXPECT_EQ(SakuraOutputShadowStatus::InvalidHandle, rust.Destroy());
}

TEST(OutputServiceRustCandidate, RejectsStaleGenerationAndBoundedMutationsLikeCpp)
{
	OutputServiceLimits limits;
	limits.maximumOwners = 1;
	limits.maximumChannels = 4;
	limits.maximumPayloadBytes = 5;
	limits.maximumTextBytesPerChannel = 4;
	limits.maximumLogEntriesPerChannel = 1;
	limits.maximumRememberedOperations = 64;
	OutputService cpp(limits);
	RustShadowAdapter rust(limits);
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.CreateStatus());
	const auto owner = Owner("bounded.owner", 1);
	const auto create = Create("create-bounded", owner, "bounded.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(create); },
		[&] { return rust.Apply(create); });
	const auto oversized = Text("oversized", owner, "bounded.output", "123456");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(oversized); },
		[&] { return rust.AppendOutput(oversized); });
	const auto stale = Text("stale-revision", owner, "bounded.output", "ok", 1);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(stale); },
		[&] { return rust.AppendOutput(stale); });
	const auto wrongGeneration = Text("wrong-generation", Owner("bounded.owner", 2), "bounded.output", "ok");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(wrongGeneration); },
		[&] { return rust.AppendOutput(wrongGeneration); });
	const auto ownerLimit = Create("owner-limit", Owner("other.owner", 1), "other.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(ownerLimit); },
		[&] { return rust.Apply(ownerLimit); });

	const auto appendText = Text("append-four", owner, "bounded.output", "1234");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(appendText); },
		[&] { return rust.AppendOutput(appendText); });
	const auto truncateText = Text("append-five", owner, "bounded.output", "5");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(truncateText); },
		[&] { return rust.AppendOutput(truncateText); });

	const auto createLog = Create("create-bounded-log", owner, "bounded.log", EOutputChannelKind::Log);
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(createLog); },
		[&] { return rust.Apply(createLog); });
	const OutputLogMutationRequest tooManyLogs{
		.operation = Operation("too-many-logs"),
		.owner = owner,
		.channelId = "bounded.log",
		.entries = { { .message = "one" }, { .message = "two" } },
	};
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendLog(tooManyLogs); },
		[&] { return rust.AppendLog(tooManyLogs); });
	const OutputLogMutationRequest oneLog{
		.operation = Operation("one-log"),
		.owner = owner,
		.channelId = "bounded.log",
		.entries = { { .level = EOutputLogLevel::Info, .message = "one", .source = std::string("test") } },
	};
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendLog(oneLog); },
		[&] { return rust.AppendLog(oneLog); });

	const auto newGeneration = Create("adopt-generation", Owner("bounded.owner", 2), "adopted.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(newGeneration); },
		[&] { return rust.Apply(newGeneration); });
	const auto lateOld = Create("late-old", owner, "late-old.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(lateOld); },
		[&] { return rust.Apply(lateOld); });
	const auto dispose = DisposeOwner("dispose-adopted", Owner("bounded.owner", 2));
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.DisposeOwner(dispose); },
		[&] { return rust.DisposeOwner(dispose); });
	const auto lateSame = Create("late-same", Owner("bounded.owner", 2), "late-same.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(lateSame); },
		[&] { return rust.Apply(lateSame); });
}

TEST(OutputServiceRustCandidate, ReplaysOnlyWithinTheBoundedRememberedWindow)
{
	OutputServiceLimits limits;
	limits.maximumRememberedOperations = 2;
	OutputService cpp(limits);
	RustShadowAdapter rust(limits);
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.CreateStatus());
	const auto owner = Owner("eviction.owner");
	const auto create = Create("evict-create", owner, "evict.output");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.CreateChannel(create); },
		[&] { return rust.Apply(create); });
	const auto first = Text("evict-first", owner, "evict.output", "a");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(first); },
		[&] { return rust.AppendOutput(first); });
	const auto second = Text("evict-second", owner, "evict.output", "b");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(second); },
		[&] { return rust.AppendOutput(second); });
	const auto third = Text("evict-third", owner, "evict.output", "c");
	ExpectEquivalentAfterMutation(cpp, rust,
		[&] { return cpp.AppendOutput(third); },
		[&] { return rust.AppendOutput(third); });

	const auto firstAgain = Text("evict-first", owner, "evict.output", "a");
	const auto expected = cpp.AppendOutput(firstAgain);
	const auto actual = rust.AppendOutput(firstAgain);
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, actual.abiStatus);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, expected.status);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, actual.operation.status);
	EXPECT_EQ(expected.revision, actual.operation.revision);
	OutputServiceSnapshot shadowSnapshot;
	ASSERT_EQ(SakuraOutputShadowStatus::Ok, rust.Snapshot(shadowSnapshot));
	ExpectSnapshotsEqual(cpp.Snapshot(), shadowSnapshot);
}

TEST(OutputServiceRustCandidate, LiveCandidateCoversEveryAcceptedCommitKind)
{
	OutputServiceLimits limits;
	limits.maximumPayloadBytes = 32;
	limits.maximumTextBytesPerChannel = 32;
	limits.maximumLogEntriesPerChannel = 4;
	limits.maximumRememberedOperations = 32;
	OutputService authority(limits);
	OutputServiceRustCandidate candidate(authority, limits);
	ASSERT_TRUE(candidate.IsAvailable());
	ASSERT_EQ(EOutputServiceRustCandidateState::Live, candidate.Diagnostics().state);

	const auto owner = Owner("all-kinds.owner");
	const auto outputChannel = "all-kinds.output";
	const auto logChannel = "all-kinds.log";
	const auto expectLiveMatch = [&](const std::uint64_t expectedSequence) {
		const auto diagnostics = candidate.Diagnostics();
		EXPECT_EQ(EOutputServiceRustCandidateState::Live, diagnostics.state);
		EXPECT_EQ(EOutputServiceRustCandidateFault::None, diagnostics.fault);
		EXPECT_EQ(expectedSequence, diagnostics.lastCursor);
		EXPECT_EQ(expectedSequence, diagnostics.appliedCommitCount);
		EXPECT_TRUE(candidate.VerifySnapshot());
	};

	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.CreateChannel(Create("all-kinds-create-output", owner, outputChannel)).status);
	expectLiveMatch(1);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.CreateChannel(Create("all-kinds-create-log", owner, logChannel, EOutputChannelKind::Log)).status);
	expectLiveMatch(2);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("all-kinds-append", owner, outputChannel, "first")).status);
	expectLiveMatch(3);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.ReplaceOutput(Text("all-kinds-replace", owner, outputChannel, "second")).status);
	expectLiveMatch(4);

	const OutputLogMutationRequest appendLog{
		.operation = Operation("all-kinds-append-log"),
		.owner = owner,
		.channelId = logChannel,
		.entries = {
			{ .level = EOutputLogLevel::Warning, .message = "warning", .source = std::string("tests") },
		},
	};
	ASSERT_EQ(EOutputOperationStatus::Succeeded, authority.AppendLog(appendLog).status);
	expectLiveMatch(5);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.Clear(Channel("all-kinds-clear", owner, logChannel)).status);
	expectLiveMatch(6);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.Show(Show("all-kinds-show", owner, outputChannel, true)).status);
	expectLiveMatch(7);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.Hide(Channel("all-kinds-hide", owner, outputChannel)).status);
	expectLiveMatch(8);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.Dispose(Channel("all-kinds-dispose", owner, outputChannel)).status);
	expectLiveMatch(9);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.DisposeOwner(DisposeOwner("all-kinds-dispose-owner", owner)).status);
	expectLiveMatch(10);

	const auto diagnostics = candidate.Diagnostics();
	EXPECT_EQ(10U, diagnostics.lastCursor);
	EXPECT_EQ(10U, diagnostics.appliedCommitCount);
	EXPECT_EQ(EOutputServiceRustCandidateFault::None, diagnostics.fault);
	EXPECT_TRUE(candidate.VerifySnapshot());
	EXPECT_TRUE(authority.Snapshot().channels.empty());
	ASSERT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
}

TEST(OutputServiceRustCandidate, LiveFeedTracksOnlyFreshAcceptedCommits)
{
	OutputServiceLimits limits;
	limits.maximumRememberedOperations = 16;
	OutputService authority(limits);
	OutputServiceRustCandidate candidate(authority, limits);
	ASSERT_TRUE(candidate.IsAvailable());
	ASSERT_EQ(EOutputServiceRustCandidateState::Live, candidate.Diagnostics().state);

	const auto owner = Owner("live.owner");
	const auto create = Create("live-create", owner, "live.output");
	ASSERT_EQ(EOutputOperationStatus::Succeeded, authority.CreateChannel(create).status);
	const auto afterCreate = candidate.Diagnostics();
	EXPECT_EQ(1U, afterCreate.lastCursor);
	EXPECT_EQ(1U, afterCreate.appliedCommitCount);
	EXPECT_EQ(SakuraOutputShadowOperationStatus::Succeeded, afterCreate.lastOperationStatus);
	EXPECT_EQ(SakuraOutputShadowReason::None, afterCreate.lastOperationReason);
	EXPECT_EQ(2U, afterCreate.lastOperationRevision);

	EXPECT_EQ(EOutputOperationStatus::Replayed, authority.CreateChannel(create).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		authority.CreateChannel(Create("live-create", owner, "other.output")).status);
	EXPECT_EQ(EOutputOperationStatus::Rejected, authority.AppendOutput({
		.operation = Operation("live-invalid-owner"),
		.owner = {},
		.channelId = "live.output",
		.text = "rejected",
	}).status);
	EXPECT_EQ(EOutputOperationStatus::Conflict,
		authority.AppendOutput(Text("live-wrong-generation", Owner("live.owner", 2),
			"live.output", "conflict")).status);
	EXPECT_EQ(EOutputOperationStatus::StaleRevision,
		authority.AppendOutput(Text("live-stale", owner, "live.output", "stale", 1)).status);
	EXPECT_EQ(EOutputOperationStatus::NotApplicable,
		authority.AppendOutput(Text("live-empty", owner, "live.output", "")).status);
	EXPECT_EQ(1U, candidate.Diagnostics().appliedCommitCount);

	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("live-append", owner, "live.output", "accepted")).status);
	const auto accepted = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateState::Live, accepted.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::None, accepted.fault);
	EXPECT_EQ(2U, accepted.lastCursor);
	EXPECT_EQ(2U, accepted.appliedCommitCount);
	EXPECT_EQ(3U, accepted.lastOperationRevision);
	ASSERT_TRUE(candidate.VerifySnapshot());
	const auto authoritativeSnapshot = authority.Snapshot();
	ASSERT_EQ(1U, authoritativeSnapshot.channels.size());
	EXPECT_EQ("accepted", authoritativeSnapshot.channels.front().text);

	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	EXPECT_TRUE(candidate.VerifySnapshot());
	EXPECT_EQ(EOutputServiceRustCandidateState::Stopped, candidate.Diagnostics().state);
	candidate.ShutdownAfterOutputServiceStop();
	EXPECT_EQ(EOutputServiceRustCandidateState::Stopped, candidate.Diagnostics().state);
}

TEST(OutputServiceRustCandidate, NonFreshBootstrapFaultsWithoutRetainingFeedOwnership)
{
	OutputServiceLimits limits;
	limits.maximumSubscriptions = 1;
	OutputService authority(limits);
	const auto owner = Owner("bootstrap.owner");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.CreateChannel(Create("bootstrap-create", owner, "bootstrap.output")).status);

	OutputServiceRustCandidate candidate(authority, limits);
	ASSERT_TRUE(candidate.IsAvailable());
	const auto diagnostics = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, diagnostics.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::BootstrapMismatch, diagnostics.fault);
	EXPECT_EQ(0U, diagnostics.appliedCommitCount);

	// The failed candidate must release the only feed slot immediately. This is
	// also an observable proxy for not retaining future journal DTOs.
	const auto probe = authority.SubscribeAcceptedCommits([](const OutputAcceptedCommitEvent&) {});
	ASSERT_TRUE(probe.has_value());
	authority.UnsubscribeAcceptedCommits(probe->subscriptionId);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("bootstrap-later", owner, "bootstrap.output", "cpp-only")).status);
	EXPECT_EQ(0U, candidate.Diagnostics().appliedCommitCount);
	ASSERT_EQ(1U, authority.Snapshot().channels.size());
	EXPECT_EQ("cpp-only", authority.Snapshot().channels.front().text);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
}

TEST(OutputServiceRustCandidate, SubscriptionFailureIsTypedAndTerminal)
{
	OutputServiceLimits limits;
	limits.maximumSubscriptions = 1;
	OutputService authority(limits);
	const auto occupied = authority.SubscribeAcceptedCommits([](const OutputAcceptedCommitEvent&) {});
	ASSERT_TRUE(occupied.has_value());

	OutputServiceRustCandidate candidate(authority, limits);
	ASSERT_TRUE(candidate.IsAvailable());
	const auto diagnostics = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, diagnostics.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::SubscribeFailed, diagnostics.fault);
	EXPECT_EQ(0U, diagnostics.appliedCommitCount);
	authority.UnsubscribeAcceptedCommits(occupied->subscriptionId);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, candidate.Diagnostics().state);
}

TEST(OutputServiceRustCandidate, BoundedFeedGapIsTerminalAndNeverResynchronizes)
{
	OutputServiceLimits limits;
	limits.maximumAcceptedCommitFeedEntries = 1;
	limits.maximumSubscriptions = 4;
	OutputService authority(limits);

	std::mutex gateMutex;
	std::condition_variable gate;
	bool firstCallbackEntered{};
	bool releaseFirstCallback{};
	const auto blocker = authority.SubscribeAcceptedCommits(
		[&](const OutputAcceptedCommitEvent& event) {
			if (event.state != EOutputAcceptedCommitFeedState::Live
				|| !event.commit || event.commit->sequence != 1) return;
			std::unique_lock lock(gateMutex);
			firstCallbackEntered = true;
			gate.notify_all();
			gate.wait(lock, [&] { return releaseFirstCallback; });
		});
	ASSERT_TRUE(blocker.has_value());
	OutputServiceRustCandidate candidate(authority, limits);
	ASSERT_TRUE(candidate.IsAvailable());

	const auto owner = Owner("gap.owner");
	OutputOperationResult firstResult;
	std::thread firstMutation([&] {
		firstResult = authority.CreateChannel(Create("gap-create", owner, "gap.output"));
	});
	bool enteredInTime{};
	{
		std::unique_lock lock(gateMutex);
		enteredInTime = gate.wait_for(lock, std::chrono::seconds(5), [&] { return firstCallbackEntered; });
		if (!enteredInTime) releaseFirstCallback = true;
	}
	if (!enteredInTime) {
		gate.notify_all();
		firstMutation.join();
		authority.UnsubscribeAcceptedCommits(blocker->subscriptionId);
		(void)authority.Stop();
		candidate.ShutdownAfterOutputServiceStop();
		FAIL() << "accepted-commit blocker did not start within the bounded wait";
		return;
	}

	const auto secondResult = authority.AppendOutput(
		Text("gap-append", owner, "gap.output", "accepted by C++"));
	EXPECT_EQ(EOutputOperationStatus::Succeeded, secondResult.status);
	{
		std::lock_guard lock(gateMutex);
		releaseFirstCallback = true;
	}
	gate.notify_all();
	firstMutation.join();
	EXPECT_EQ(EOutputOperationStatus::Succeeded, firstResult.status);

	const auto gap = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, gap.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::Gap, gap.fault);
	EXPECT_EQ(0U, gap.appliedCommitCount);
	EXPECT_EQ(1U, gap.lastCursor);
	authority.UnsubscribeAcceptedCommits(blocker->subscriptionId);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("gap-after-terminal", owner, "gap.output", " later")).status);
	EXPECT_EQ(0U, candidate.Diagnostics().appliedCommitCount);
	const auto authoritySnapshot = authority.Snapshot();
	ASSERT_EQ(1U, authoritySnapshot.channels.size());
	EXPECT_EQ("accepted by C++ later", authoritySnapshot.channels.front().text);
	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
}

TEST(OutputServiceRustCandidate, SnapshotMismatchFaultsWithoutChangingCppAuthority)
{
	OutputServiceLimits authorityLimits;
	authorityLimits.maximumPayloadBytes = 16;
	authorityLimits.maximumTextBytesPerChannel = 4;
	OutputServiceLimits divergentCandidateLimits = authorityLimits;
	divergentCandidateLimits.maximumTextBytesPerChannel = 8;
	OutputService authority(authorityLimits);
	// A deliberately divergent test-only limit injects a deterministic model
	// mismatch without exposing or mutating the candidate's opaque Rust token.
	OutputServiceRustCandidate candidate(authority, divergentCandidateLimits);
	ASSERT_TRUE(candidate.IsAvailable());
	const auto owner = Owner("mismatch.owner");
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.CreateChannel(Create("mismatch-create", owner, "mismatch.output")).status);
	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("mismatch-append", owner, "mismatch.output", "abcdef")).status);
	ASSERT_EQ(EOutputServiceRustCandidateState::Live, candidate.Diagnostics().state);

	EXPECT_FALSE(candidate.VerifySnapshot());
	const auto mismatch = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, mismatch.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::SnapshotMismatch, mismatch.fault);
	EXPECT_EQ(2U, mismatch.lastCursor);
	EXPECT_EQ(2U, mismatch.appliedCommitCount);
	const auto authoritySnapshot = authority.Snapshot();
	ASSERT_EQ(1U, authoritySnapshot.channels.size());
	EXPECT_EQ("cdef", authoritySnapshot.channels.front().text);
	EXPECT_EQ(2U, authoritySnapshot.channels.front().droppedCharacterCount);

	ASSERT_EQ(EOutputOperationStatus::Succeeded,
		authority.AppendOutput(Text("mismatch-after-terminal", owner, "mismatch.output", "z")).status);
	EXPECT_EQ(2U, candidate.Diagnostics().appliedCommitCount);
	EXPECT_FALSE(candidate.VerifySnapshot());
	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
	EXPECT_EQ(EOutputServiceRustCandidateState::Faulted, candidate.Diagnostics().state);
}

#else

TEST(OutputServiceRustCandidate, IsExplicitlyUnavailableWithoutNativeRustArchive)
{
	OutputService authority;
	OutputServiceRustCandidate candidate(authority, {});
	EXPECT_FALSE(OutputServiceRustCandidate::IsCompiledIn());
	EXPECT_FALSE(candidate.IsAvailable());
	const auto diagnostics = candidate.Diagnostics();
	EXPECT_EQ(EOutputServiceRustCandidateAvailability::Unavailable, diagnostics.availability);
	EXPECT_EQ(EOutputServiceRustCandidateState::Unavailable, diagnostics.state);
	EXPECT_EQ(EOutputServiceRustCandidateFault::Unavailable, diagnostics.fault);
	EXPECT_FALSE(candidate.VerifySnapshot());
	EXPECT_EQ(EOutputOperationStatus::Succeeded, authority.Stop().status);
	candidate.ShutdownAfterOutputServiceStop();
	EXPECT_EQ(EOutputServiceRustCandidateState::Unavailable, candidate.Diagnostics().state);
}

#endif

} // namespace
} // namespace workbench::output
