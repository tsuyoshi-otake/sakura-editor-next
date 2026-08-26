/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/output/OutputServiceRustProvider.h"
#include "workbench/output/OutputServiceNotificationDispatcher.h"

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::output {
namespace {

constexpr std::string_view kSnapshotMagic("SAKURA_OUTPUT_MODEL_V1\0", 23);
constexpr std::size_t kMaximumStableIdBytes = 160;
constexpr std::size_t kMaximumLabelBytes = 512;
constexpr std::size_t kMaximumMetadataBytes = 512;

static_assert(static_cast<std::uint32_t>(EOutputChannelKind::Output)
	== SAKURA_OUTPUT_PROVIDER_CHANNEL_OUTPUT);
static_assert(static_cast<std::uint32_t>(EOutputChannelKind::Log)
	== SAKURA_OUTPUT_PROVIDER_CHANNEL_LOG);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Trace) == 0);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Error) == 4);
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Succeeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Succeeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Stopped)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Stopped));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::None)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::None));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::ExpectedRevisionMismatch)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::ExpectedRevisionMismatch));

OutputServiceLimits NormalizeLimits(OutputServiceLimits limits) noexcept
{
	if (limits.maximumOwners == 0) limits.maximumOwners = 1;
	if (limits.maximumChannels == 0) limits.maximumChannels = 1;
	if (limits.maximumTextBytesPerChannel == 0) limits.maximumTextBytesPerChannel = 1;
	if (limits.maximumPayloadBytes == 0) limits.maximumPayloadBytes = 1;
	if (limits.maximumLogEntriesPerChannel == 0) limits.maximumLogEntriesPerChannel = 1;
	if (limits.maximumSubscriptions == 0) limits.maximumSubscriptions = 1;
	if (limits.maximumRememberedOperations == 0) limits.maximumRememberedOperations = 1;
	if (limits.maximumPendingNotifications == 0) limits.maximumPendingNotifications = 1;
	if (limits.maximumAcceptedCommitFeedEntries == 0) limits.maximumAcceptedCommitFeedEntries = 1;
	return limits;
}

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if ((!permitControls && (first < 0x20 || first == 0x7f)) || first == 0) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) { continuationCount = 1; codePoint = first & 0x1f; }
		else if (first >= 0xe0 && first <= 0xef) { continuationCount = 2; codePoint = first & 0x0f; }
		else if (first >= 0xf0 && first <= 0xf4) { continuationCount = 3; codePoint = first & 0x07; }
		else return false;
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		if (!permitControls && (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidOutputStableId(const std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumStableIdBytes || !IsValidUtf8(value, false)) return false;
	return std::none_of(value.begin(), value.end(), [](const unsigned char character) {
		return character <= 0x20 || character == 0x7f;
	});
}

bool IsValidMetadataValue(const std::optional<std::string>& value) noexcept
{
	return !value || (!value->empty() && value->size() <= kMaximumMetadataBytes
		&& IsValidUtf8(*value, false));
}

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
		if (!ReadU64(length)
			|| length > Remaining()
			|| length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			return false;
		}
		const auto size = static_cast<std::size_t>(length);
		value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
		offset += size;
		return IsValidUtf8(value, true);
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

[[nodiscard]] std::optional<OutputServiceSnapshot> ParseSnapshot(
	const std::vector<std::uint8_t>& bytes)
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
		if (!reader.ReadBytes(active) || !IsValidOutputStableId(active)) return std::nullopt;
		snapshot.activeChannelId = std::move(active);
	}

	std::uint64_t channelCount{};
	if (!reader.ReadU64(channelCount)
		|| channelCount > reader.Remaining()
		|| channelCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		return std::nullopt;
	}
	snapshot.channels.reserve(static_cast<std::size_t>(channelCount));
	for (std::uint64_t index = 0; index < channelCount; ++index) {
		OutputChannelSnapshot channel;
		if (!reader.ReadBytes(channel.channelId)
			|| !reader.ReadBytes(channel.label)
			|| !reader.ReadBytes(channel.owner.ownerId)
			|| !reader.ReadU64(channel.owner.generation)) {
			return std::nullopt;
		}
		if (!IsValidOutputStableId(channel.channelId)
			|| !IsValidOutputStableId(channel.owner.ownerId)
			|| channel.owner.generation == 0
			|| channel.label.empty()
			|| channel.label.size() > kMaximumLabelBytes
			|| !IsValidUtf8(channel.label, false)) {
			return std::nullopt;
		}
		std::uint8_t kind{};
		if (!reader.ReadByte(kind) || kind > SAKURA_OUTPUT_PROVIDER_CHANNEL_LOG) return std::nullopt;
		channel.kind = static_cast<EOutputChannelKind>(kind);
		if (!reader.ReadOptionalBytes(channel.metadata.languageId)
			|| !reader.ReadOptionalBytes(channel.metadata.source)
			|| !IsValidMetadataValue(channel.metadata.languageId)
			|| !IsValidMetadataValue(channel.metadata.source)) {
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
		if (!reader.ReadU64(logCount)
			|| logCount > reader.Remaining()
			|| logCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			return std::nullopt;
		}
		channel.logEntries.reserve(static_cast<std::size_t>(logCount));
		for (std::uint64_t logIndex = 0; logIndex < logCount; ++logIndex) {
			std::uint32_t level{};
			OutputLogEntry entry;
			if (!reader.ReadU32(level)
				|| level > static_cast<std::uint32_t>(EOutputLogLevel::Error)
				|| !reader.ReadBytes(entry.message)
				|| entry.message.empty()
				|| !IsValidUtf8(entry.message, true)
				|| !reader.ReadOptionalBytes(entry.source)
				|| !IsValidMetadataValue(entry.source)) {
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

[[nodiscard]] SakuraOutputProviderSpanV1 Span(const std::string_view value) noexcept
{
	SakuraOutputProviderSpanV1 span{};
	span.struct_size = sizeof(span);
	span.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
	span.data = reinterpret_cast<const std::uint8_t*>(value.data());
	span.length = static_cast<std::uint64_t>(value.size());
	return span;
}

struct PendingRequest final {
	SakuraOutputProviderRequestV1 raw{};
	std::vector<SakuraOutputProviderLogEntryV1> logEntries;
};

void FillCommon(
	PendingRequest& pending,
	const std::uint32_t operationKind,
	const OutputOperation& operation,
	const OutputOwner& owner,
	const std::string_view channelId)
{
	pending.raw = {};
	pending.raw.struct_size = sizeof(pending.raw);
	pending.raw.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
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
	pending.raw.owner_id = Span(owner.ownerId);
	pending.raw.owner_generation = owner.generation;
	pending.raw.channel_id = Span(channelId);
	if (operation.expectedRevision) {
		pending.raw.flags |= SAKURA_OUTPUT_PROVIDER_REQUEST_HAS_EXPECTED_REVISION;
		pending.raw.expected_revision = *operation.expectedRevision;
	}
}

void FillCreate(PendingRequest& pending, const OutputCreateChannelRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_PROVIDER_OP_CREATE_CHANNEL, request.operation,
		request.owner, request.channelId);
	pending.raw.channel_kind = static_cast<std::uint32_t>(request.kind);
	pending.raw.label = Span(request.label);
	if (request.metadata.languageId) {
		pending.raw.flags |= SAKURA_OUTPUT_PROVIDER_REQUEST_LANGUAGE_PRESENT;
		pending.raw.metadata_language_id = Span(*request.metadata.languageId);
	}
	if (request.metadata.source) {
		pending.raw.flags |= SAKURA_OUTPUT_PROVIDER_REQUEST_SOURCE_PRESENT;
		pending.raw.metadata_source = Span(*request.metadata.source);
	}
}

void FillText(
	PendingRequest& pending,
	const std::uint32_t operationKind,
	const OutputTextMutationRequest& request)
{
	FillCommon(pending, operationKind, request.operation, request.owner, request.channelId);
	pending.raw.payload = Span(request.text);
}

void FillLog(PendingRequest& pending, const OutputLogMutationRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_PROVIDER_OP_APPEND_LOG, request.operation,
		request.owner, request.channelId);
	pending.logEntries.reserve(request.entries.size());
	for (const auto& entry : request.entries) {
		SakuraOutputProviderLogEntryV1 raw{};
		raw.struct_size = sizeof(raw);
		raw.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
		raw.level = static_cast<std::uint32_t>(entry.level);
		raw.message = Span(entry.message);
		raw.source = Span(std::string_view{});
		if (entry.source) {
			raw.flags = SAKURA_OUTPUT_PROVIDER_LOG_SOURCE_PRESENT;
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
	FillCommon(pending, SAKURA_OUTPUT_PROVIDER_OP_SHOW, request.operation,
		request.owner, request.channelId);
	if (request.preserveFocus) pending.raw.flags |= SAKURA_OUTPUT_PROVIDER_REQUEST_PRESERVE_FOCUS;
}

void FillDisposeOwner(PendingRequest& pending, const OutputDisposeOwnerRequest& request)
{
	FillCommon(pending, SAKURA_OUTPUT_PROVIDER_OP_DISPOSE_OWNER, request.operation,
		request.owner, {});
}

[[nodiscard]] bool IsValidApplyResult(const SakuraOutputProviderApplyResultV1& result) noexcept
{
	return result.struct_size == sizeof(result)
		&& result.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& result.status <= static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Stopped)
		&& result.reason <= static_cast<std::uint32_t>(SakuraOutputProviderReason::ExpectedRevisionMismatch)
		&& result.callback_drain_deferred <= 1
		&& std::all_of(std::begin(result.reserved), std::end(result.reserved),
			[](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool IsValidSnapshotInfo(const SakuraOutputProviderSnapshotInfoV1& info) noexcept
{
	return info.struct_size == sizeof(info)
		&& info.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& info.stopped <= 1
		&& info.active_channel_present <= 1
		&& std::all_of(std::begin(info.reserved0), std::end(info.reserved0),
			[](const std::uint8_t value) { return value == 0; })
		&& std::all_of(std::begin(info.reserved), std::end(info.reserved),
			[](const std::uint64_t value) { return value == 0; });
}

[[nodiscard]] bool IsValidSnapshotBuffer(
	const SakuraOutputProviderSnapshotBufferV1& buffer) noexcept
{
	return buffer.struct_size == sizeof(buffer)
		&& buffer.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& std::all_of(std::begin(buffer.reserved), std::end(buffer.reserved),
			[](const std::uint64_t value) { return value == 0; });
}

[[nodiscard]] bool IsValidActiveChannelHeader(
	const SakuraOutputProviderActiveChannelV1& active) noexcept
{
	return active.struct_size == sizeof(active)
		&& active.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& active.present <= 1
		&& std::all_of(std::begin(active.reserved0), std::end(active.reserved0),
			[](const std::uint8_t value) { return value == 0; })
		&& std::all_of(std::begin(active.reserved), std::end(active.reserved),
			[](const std::uint64_t value) { return value == 0; });
}

void InitializeAbiHeader(SakuraOutputProviderApplyResultV1& result) noexcept
{
	result = {};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
}

void InitializeAbiHeader(SakuraOutputProviderSnapshotInfoV1& info) noexcept
{
	info = {};
	info.struct_size = sizeof(info);
	info.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
}

void InitializeAbiHeader(SakuraOutputProviderSnapshotBufferV1& buffer) noexcept
{
	buffer = {};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
}

} // namespace

struct OutputServiceRustProvider::Control final {
	explicit Control(OutputServiceLimits initialLimits)
		: limits(NormalizeLimits(std::move(initialLimits)))
		, notificationDispatcher(modelMutex, drainCondition,
			OutputServiceNotificationDispatcher::Limits{
				.maximumSubscriptions = limits.maximumSubscriptions,
				.maximumPendingNotifications = limits.maximumPendingNotifications })
	{
	}

	OutputServiceLimits limits;
	mutable std::mutex modelMutex;
	std::condition_variable drainCondition;
	OutputServiceNotificationDispatcher notificationDispatcher;
	std::mutex mutationMutex;
	std::uint64_t token{};
	std::uint64_t lastRevision{ 1 };
	bool authorityStopped{};
	bool pendingDestroy{};
	bool terminalSnapshotAvailable{};
	OutputServiceSnapshot terminalSnapshot;
	OutputServiceRustProviderDiagnostics diagnostics{};
};

namespace {

void SaturatingIncrement(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

template <typename Control>
void RecordBoundaryCall(
	Control& control,
	const EOutputProviderBoundary boundary,
	std::uint64_t OutputProviderHealthCounters::* const counter) noexcept
{
	std::lock_guard lock(control.modelMutex);
	SaturatingIncrement(control.diagnostics.counters.ffiCalls);
	if (counter) SaturatingIncrement(control.diagnostics.counters.*counter);
	control.diagnostics.lastBoundary = boundary;
}

template <typename Control>
void RecordBoundaryStatus(
	Control& control,
	const EOutputProviderBoundary boundary,
	const SakuraOutputProviderStatus status) noexcept
{
	std::lock_guard lock(control.modelMutex);
	control.diagnostics.lastBoundary = boundary;
	control.diagnostics.lastFfiStatus = status;
}

template <typename Control>
void SetFault(
	Control& control,
	const EOutputServiceRustProviderFault fault,
	const EOutputProviderBoundary boundary,
	const SakuraOutputProviderStatus ffiStatus) noexcept
{
	std::lock_guard lock(control.modelMutex);
	control.diagnostics.availability =
		control.token == 0 || control.authorityStopped ? EOutputServiceRustProviderAvailability::Unavailable
			: EOutputServiceRustProviderAvailability::Available;
	if (!control.authorityStopped) control.diagnostics.state = EOutputServiceRustProviderState::Faulted;
	control.diagnostics.fault = fault;
	control.diagnostics.lastBoundary = boundary;
	control.diagnostics.failureBoundary = boundary;
	control.diagnostics.lastFfiStatus = ffiStatus;
	SaturatingIncrement(control.diagnostics.counters.boundaryFailures);
}

template <typename Control>
void RecordPublicResultLocked(
	Control& control,
	const OutputOperationResult& result,
	const bool countMutation) noexcept
{
	control.diagnostics.hasLastOperation = true;
	control.diagnostics.lastOperationStatus =
		static_cast<SakuraOutputProviderOperationStatus>(result.status);
	control.diagnostics.lastOperationReason =
		static_cast<SakuraOutputProviderReason>(result.reason);
	control.diagnostics.lastOperationRevision = result.revision;
	// A replay carries the original commit revision and must not make the
	// provider's cached current revision move backwards after later commits.
	if (result.revision > control.lastRevision) control.lastRevision = result.revision;
	if (!countMutation) return;
	switch (result.status) {
	case EOutputOperationStatus::Succeeded:
		SaturatingIncrement(control.diagnostics.counters.acceptedOperations);
		break;
	case EOutputOperationStatus::Replayed:
		SaturatingIncrement(control.diagnostics.counters.replayedOperations);
		break;
	default:
		SaturatingIncrement(control.diagnostics.counters.rejectedOperations);
		break;
	}
}

template <typename Control>
void RecordMutationAttempt(Control& control) noexcept
{
	std::lock_guard lock(control.modelMutex);
	SaturatingIncrement(control.diagnostics.counters.mutationCalls);
}

template <typename Control>
void RecordOperation(
	Control& control,
	const SakuraOutputProviderStatus ffiStatus,
	const SakuraOutputProviderApplyResultV1& raw,
	const bool countMutation = true) noexcept
{
	std::lock_guard lock(control.modelMutex);
	control.diagnostics.lastBoundary = EOutputProviderBoundary::Apply;
	control.diagnostics.lastFfiStatus = ffiStatus;
	RecordPublicResultLocked(control, OutputOperationResult{
		static_cast<EOutputOperationStatus>(raw.status),
		static_cast<EOutputOperationReason>(raw.reason),
		raw.revision,
		raw.callback_drain_deferred != 0,
	}, countMutation);
}

template <typename Control>
[[nodiscard]] OutputOperationResult ProviderUnavailable(
	Control& control,
	const bool countMutation = false) noexcept
{
	std::lock_guard lock(control.modelMutex);
	OutputOperationResult result;
	if (control.authorityStopped) {
		result = { EOutputOperationStatus::Stopped, EOutputOperationReason::None, control.lastRevision };
	} else {
		result = { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, control.lastRevision };
	}
	RecordPublicResultLocked(control, result, countMutation);
	return result;
}

[[nodiscard]] EOutputProviderBoundaryStatus ToBoundaryStatus(
	const SakuraOutputProviderStatus status) noexcept
{
	switch (status) {
	case SakuraOutputProviderStatus::Ok: return EOutputProviderBoundaryStatus::Ok;
	case SakuraOutputProviderStatus::InvalidArgument: return EOutputProviderBoundaryStatus::InvalidArgument;
	case SakuraOutputProviderStatus::InvalidHandle: return EOutputProviderBoundaryStatus::InvalidHandle;
	case SakuraOutputProviderStatus::Stopped: return EOutputProviderBoundaryStatus::Stopped;
	case SakuraOutputProviderStatus::InsufficientCapacity: return EOutputProviderBoundaryStatus::InsufficientCapacity;
	case SakuraOutputProviderStatus::InternalError: return EOutputProviderBoundaryStatus::InternalError;
	default: return EOutputProviderBoundaryStatus::InternalError;
	}
}

[[nodiscard]] EOutputProviderLifecycle ToProviderLifecycle(
	const EOutputServiceRustProviderState state) noexcept
{
	switch (state) {
	case EOutputServiceRustProviderState::Unavailable: return EOutputProviderLifecycle::Unavailable;
	case EOutputServiceRustProviderState::Ready: return EOutputProviderLifecycle::Ready;
	case EOutputServiceRustProviderState::Faulted: return EOutputProviderLifecycle::Faulted;
	case EOutputServiceRustProviderState::Stopped: return EOutputProviderLifecycle::Stopped;
	default: return EOutputProviderLifecycle::Faulted;
	}
}

[[nodiscard]] EOutputProviderFault ToProviderFault(
	const EOutputServiceRustProviderFault fault) noexcept
{
	switch (fault) {
	case EOutputServiceRustProviderFault::None: return EOutputProviderFault::None;
	case EOutputServiceRustProviderFault::Unavailable: return EOutputProviderFault::Unavailable;
	case EOutputServiceRustProviderFault::AbiFailure: return EOutputProviderFault::AbiContract;
	case EOutputServiceRustProviderFault::FfiFailure: return EOutputProviderFault::Ffi;
	case EOutputServiceRustProviderFault::SnapshotFailure: return EOutputProviderFault::Snapshot;
	case EOutputServiceRustProviderFault::CallbackFailure: return EOutputProviderFault::Callback;
	case EOutputServiceRustProviderFault::DestroyFailure: return EOutputProviderFault::Destroy;
	default: return EOutputProviderFault::Ffi;
	}
}

#if defined(SAKURA_OUTPUT_BACKEND_RUST)

template <typename Control>
[[nodiscard]] std::optional<OutputServiceSnapshot> ReadSnapshot(
	Control& control)
{
	SakuraOutputProviderSnapshotInfoV1 info{};
	InitializeAbiHeader(info);
	RecordBoundaryCall(control, EOutputProviderBoundary::SnapshotMeasure, nullptr);
	const auto measured = sakura_output_provider_snapshot_measure_v1(control.token, &info);
	RecordBoundaryStatus(control, EOutputProviderBoundary::SnapshotMeasure, measured);
	if (measured != SakuraOutputProviderStatus::Ok || !IsValidSnapshotInfo(info)) {
		SetFault(control, measured == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::SnapshotMeasure, measured);
		return std::nullopt;
	}
	if (info.encoded_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		SetFault(control, EOutputServiceRustProviderFault::SnapshotFailure,
			EOutputProviderBoundary::SnapshotMeasure, SakuraOutputProviderStatus::InternalError);
		return std::nullopt;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.encoded_size));
	SakuraOutputProviderSnapshotBufferV1 buffer{};
	InitializeAbiHeader(buffer);
	buffer.data = bytes.empty() ? nullptr : bytes.data();
	buffer.capacity = static_cast<std::uint64_t>(bytes.size());
	const auto expectedData = buffer.data;
	const auto expectedCapacity = buffer.capacity;
	RecordBoundaryCall(control, EOutputProviderBoundary::SnapshotWrite, nullptr);
	const auto written = sakura_output_provider_snapshot_write_v1(control.token, &buffer);
	RecordBoundaryStatus(control, EOutputProviderBoundary::SnapshotWrite, written);
	if (written != SakuraOutputProviderStatus::Ok
		|| !IsValidSnapshotBuffer(buffer)
		|| buffer.data != expectedData
		|| buffer.capacity != expectedCapacity
		|| buffer.length != info.encoded_size) {
		SetFault(control, written == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::SnapshotWrite, written);
		return std::nullopt;
	}
	if (buffer.length != static_cast<std::uint64_t>(bytes.size())) {
		SetFault(control, EOutputServiceRustProviderFault::SnapshotFailure,
			EOutputProviderBoundary::SnapshotWrite, SakuraOutputProviderStatus::InternalError);
		return std::nullopt;
	}
	auto snapshot = ParseSnapshot(bytes);
	if (!snapshot
		|| snapshot->revision != info.revision
		|| snapshot->stopped != (info.stopped != 0)
		|| snapshot->droppedNotificationCount != info.dropped_notification_count
		|| snapshot->channels.size() != static_cast<std::size_t>(info.channel_count)
		|| snapshot->activeChannelId.has_value() != (info.active_channel_present != 0)) {
		SetFault(control, EOutputServiceRustProviderFault::SnapshotFailure,
			EOutputProviderBoundary::SnapshotDecode, SakuraOutputProviderStatus::InternalError);
		return std::nullopt;
	}
	{
		std::lock_guard lock(control.modelMutex);
		snapshot->droppedNotificationCount = control.notificationDispatcher.DroppedNotificationCountLocked();
		control.lastRevision = snapshot->revision;
	}
	return snapshot;
}

template <typename Control>
[[nodiscard]] bool ReadActiveChannel(
	Control& control,
	const std::uint64_t expectedRevision,
	std::optional<std::string>& activeChannelId)
{
	SakuraOutputProviderActiveChannelV1 active{};
	active.struct_size = sizeof(active);
	active.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
	active.capacity = 0;
	RecordBoundaryCall(control, EOutputProviderBoundary::ActiveChannelQuery,
		&OutputProviderHealthCounters::activeChannelCalls);
	const auto measured = sakura_output_provider_active_channel_v1(control.token, &active);
	RecordBoundaryStatus(control, EOutputProviderBoundary::ActiveChannelQuery, measured);
	if (measured == SakuraOutputProviderStatus::Ok) {
		if (!IsValidActiveChannelHeader(active) || active.present != 0 || active.length != 0
			|| active.data != nullptr || active.capacity != 0
			|| active.revision != expectedRevision) {
			SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
				EOutputProviderBoundary::ActiveChannelQuery, measured);
			return false;
		}
		activeChannelId.reset();
		return true;
	}
	if (measured != SakuraOutputProviderStatus::InsufficientCapacity
		|| !IsValidActiveChannelHeader(active)
		|| active.present != 1
		|| active.data != nullptr || active.capacity != 0
		|| active.revision != expectedRevision
		|| active.length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		SetFault(control, measured == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::ActiveChannelQuery, measured);
		return false;
	}
	const auto length = static_cast<std::size_t>(active.length);
	std::string value(length, '\0');
	const auto expectedData = value.empty()
		? nullptr
		: reinterpret_cast<std::uint8_t*>(value.data());
	active.data = expectedData;
	active.capacity = static_cast<std::uint64_t>(value.size());
	RecordBoundaryCall(control, EOutputProviderBoundary::ActiveChannelQuery,
		&OutputProviderHealthCounters::activeChannelCalls);
	const auto written = sakura_output_provider_active_channel_v1(control.token, &active);
	RecordBoundaryStatus(control, EOutputProviderBoundary::ActiveChannelQuery, written);
	if (written != SakuraOutputProviderStatus::Ok
		|| !IsValidActiveChannelHeader(active)
		|| active.data != expectedData
		|| active.capacity != static_cast<std::uint64_t>(value.size())
		|| active.revision != expectedRevision
		|| active.length != static_cast<std::uint64_t>(value.size())) {
		SetFault(control, written == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::ActiveChannelQuery, written);
		return false;
	}
	if (active.present == 0) {
		if (!value.empty()) {
			SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
				EOutputProviderBoundary::ActiveChannelQuery, SakuraOutputProviderStatus::InternalError);
			return false;
		}
		activeChannelId.reset();
		return true;
	}
	if (!IsValidOutputStableId(value)) {
		SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
			EOutputProviderBoundary::ActiveChannelQuery, SakuraOutputProviderStatus::InternalError);
		return false;
	}
	activeChannelId = std::move(value);
	return true;
}

template <typename Control>
[[nodiscard]] OutputOperationResult ApplyPending(
	Control& control,
	PendingRequest& pending,
	const EOutputChangeKind changeKind,
	const std::optional<std::string>& channelId)
{
	std::unique_lock mutationLock(control.mutationMutex);
	bool ready{};
	{
		std::lock_guard lock(control.modelMutex);
		ready = !control.authorityStopped
			&& control.diagnostics.state == EOutputServiceRustProviderState::Ready;
	}
	if (!ready) return ProviderUnavailable(control, true);
	SakuraOutputProviderApplyResultV1 raw{};
	InitializeAbiHeader(raw);
	RecordBoundaryCall(control, EOutputProviderBoundary::Apply, nullptr);
	const auto ffiStatus = sakura_output_provider_apply_v1(control.token, &pending.raw, &raw);
	RecordBoundaryStatus(control, EOutputProviderBoundary::Apply, ffiStatus);
	const auto operationStatus = static_cast<SakuraOutputProviderOperationStatus>(raw.status);
	if (ffiStatus != SakuraOutputProviderStatus::Ok
		&& !(ffiStatus == SakuraOutputProviderStatus::Stopped
			&& operationStatus == SakuraOutputProviderOperationStatus::Stopped)) {
		SetFault(control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, ffiStatus);
		return ProviderUnavailable(control, true);
	}
	if (!IsValidApplyResult(raw)) {
		SetFault(control, ffiStatus == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, ffiStatus);
		return ProviderUnavailable(control, true);
	}
	RecordOperation(control, ffiStatus, raw);
	if (operationStatus == SakuraOutputProviderOperationStatus::Stopped) {
		std::lock_guard lock(control.modelMutex);
		control.authorityStopped = true;
		control.diagnostics.state = EOutputServiceRustProviderState::Stopped;
		control.notificationDispatcher.StopLocked();
	}
	OutputOperationResult result{
		static_cast<EOutputOperationStatus>(raw.status),
		static_cast<EOutputOperationReason>(raw.reason),
		raw.revision,
		raw.callback_drain_deferred != 0 };
	if (operationStatus == SakuraOutputProviderOperationStatus::Succeeded) {
		std::optional<std::string> activeChannelId;
		const auto activeQuerySucceeded = ReadActiveChannel(control, result.revision, activeChannelId);
		bool drain{};
		if (activeQuerySucceeded) {
			std::lock_guard lock(control.modelMutex);
			if (control.diagnostics.state == EOutputServiceRustProviderState::Ready) {
				drain = control.notificationDispatcher.QueueLocked(
					result.revision, changeKind, channelId, activeChannelId);
			}
		}
		mutationLock.unlock();
		if (drain) control.notificationDispatcher.Drain();
	}
	return result;
}

#endif

template <typename Control>
void CacheTerminalSnapshot(
	Control& control,
	std::optional<OutputServiceSnapshot> snapshot,
	const std::uint64_t revision) noexcept
{
	std::lock_guard lock(control.modelMutex);
	try {
		if (snapshot) {
			control.terminalSnapshot = std::move(*snapshot);
		} else {
			control.terminalSnapshot.channels.clear();
			control.terminalSnapshot.activeChannelId.reset();
		}
	} catch (...) {
		// A terminal snapshot is an observation cache. Preserve the terminal
		// fence even if copying the complete Rust snapshot fails under pressure.
		control.terminalSnapshot.channels.clear();
		control.terminalSnapshot.activeChannelId.reset();
	}
	control.terminalSnapshot.revision = revision;
	control.terminalSnapshot.stopped = true;
	control.terminalSnapshot.droppedNotificationCount =
		control.notificationDispatcher.DroppedNotificationCountLocked();
	control.terminalSnapshotAvailable = true;
	control.lastRevision = revision;
}

template <typename Control>
[[nodiscard]] bool DestroyToken(Control& control) noexcept
{
	std::uint64_t token{};
	{
		std::lock_guard lock(control.modelMutex);
		token = control.token;
		if (token == 0) {
			control.pendingDestroy = false;
			return true;
		}
		control.pendingDestroy = true;
	}
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	RecordBoundaryCall(control, EOutputProviderBoundary::Destroy,
		&OutputProviderHealthCounters::destroyCalls);
	// Keep the ABI's writable token slot private to this serialized call. The
	// control token is also observed under modelMutex by health/fault paths, so
	// passing its address directly would race with those observers while Rust
	// consumes the slot on success.
	std::uint64_t destroyToken = token;
	const auto status = sakura_output_provider_destroy_v1(&destroyToken);
	RecordBoundaryStatus(control, EOutputProviderBoundary::Destroy, status);
	if (status != SakuraOutputProviderStatus::Ok) {
		SetFault(control, EOutputServiceRustProviderFault::DestroyFailure,
			EOutputProviderBoundary::Destroy, status);
		return false;
	}
#else
	(void)token;
#endif
	{
		std::lock_guard lock(control.modelMutex);
		control.token = 0;
		control.pendingDestroy = false;
	}
	return true;
}

} // namespace

OutputServiceRustProvider::OutputServiceRustProvider(OutputServiceLimits limits) noexcept
{
	try {
		m_control = std::make_unique<Control>(std::move(limits));
		auto& control = *m_control;
		control.diagnostics.initializationStage =
			EOutputProviderInitializationStage::ProviderConstruction;
		control.diagnostics.counters.initializationAttempts = 1;
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		SakuraOutputProviderLimitsV1 rawLimits{};
		rawLimits.struct_size = sizeof(rawLimits);
		rawLimits.abi_version = SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1;
		rawLimits.maximum_owners = static_cast<std::uint64_t>(control.limits.maximumOwners);
		rawLimits.maximum_channels = static_cast<std::uint64_t>(control.limits.maximumChannels);
		rawLimits.maximum_text_bytes_per_channel = static_cast<std::uint64_t>(control.limits.maximumTextBytesPerChannel);
		rawLimits.maximum_payload_bytes = static_cast<std::uint64_t>(control.limits.maximumPayloadBytes);
		rawLimits.maximum_log_entries_per_channel = static_cast<std::uint64_t>(control.limits.maximumLogEntriesPerChannel);
		rawLimits.maximum_remembered_operations = static_cast<std::uint64_t>(control.limits.maximumRememberedOperations);
		control.diagnostics.initializationStage = EOutputProviderInitializationStage::AbiCreate;
		RecordBoundaryCall(control, EOutputProviderBoundary::Create, nullptr);
		const auto status = sakura_output_provider_create_v1(&rawLimits, &control.token);
		RecordBoundaryStatus(control, EOutputProviderBoundary::Create, status);
		if (status != SakuraOutputProviderStatus::Ok || control.token == 0) {
			control.token = 0;
			control.diagnostics.availability = EOutputServiceRustProviderAvailability::Unavailable;
			control.diagnostics.state = EOutputServiceRustProviderState::Unavailable;
			control.diagnostics.fault = status == SakuraOutputProviderStatus::Ok
				? EOutputServiceRustProviderFault::AbiFailure
				: EOutputServiceRustProviderFault::Unavailable;
			control.diagnostics.failureBoundary = EOutputProviderBoundary::Create;
			SaturatingIncrement(control.diagnostics.counters.boundaryFailures);
			return;
		}
		control.diagnostics.availability = EOutputServiceRustProviderAvailability::Available;
		control.diagnostics.state = EOutputServiceRustProviderState::Ready;
		control.diagnostics.fault = EOutputServiceRustProviderFault::None;
		control.diagnostics.initializationStage = EOutputProviderInitializationStage::Ready;
#else
		control.diagnostics.availability = EOutputServiceRustProviderAvailability::Unavailable;
		control.diagnostics.state = EOutputServiceRustProviderState::Unavailable;
		control.diagnostics.fault = EOutputServiceRustProviderFault::Unavailable;
		control.diagnostics.lastFfiStatus = SakuraOutputProviderStatus::InternalError;
		control.diagnostics.failureBoundary = EOutputProviderBoundary::Factory;
		SaturatingIncrement(control.diagnostics.counters.boundaryFailures);
#endif
	}
	catch (...) {
		if (!m_control) {
			try {
				m_control = std::make_unique<Control>(OutputServiceLimits{});
			} catch (...) {
				return;
			}
		}
		auto& control = *m_control;
		control.diagnostics.initializationStage =
			EOutputProviderInitializationStage::ProviderConstruction;
		control.diagnostics.counters.initializationAttempts = 1;
		control.token = 0;
		control.diagnostics.availability = EOutputServiceRustProviderAvailability::Unavailable;
		control.diagnostics.state = EOutputServiceRustProviderState::Unavailable;
		control.diagnostics.fault = EOutputServiceRustProviderFault::Unavailable;
		control.diagnostics.lastFfiStatus = SakuraOutputProviderStatus::InternalError;
		control.diagnostics.failureBoundary = EOutputProviderBoundary::Factory;
		SaturatingIncrement(control.diagnostics.counters.boundaryFailures);
	}
}

OutputServiceRustProvider::~OutputServiceRustProvider()
{
	if (!m_control) return;
	(void)Stop();
	std::unique_lock mutationLock(m_control->mutationMutex);
	(void)DestroyToken(*m_control);
}

bool OutputServiceRustProvider::IsAvailable() const noexcept
{
	if (!m_control) return false;
	const auto diagnostics = Diagnostics();
	return diagnostics.availability == EOutputServiceRustProviderAvailability::Available
		&& diagnostics.state == EOutputServiceRustProviderState::Ready;
}

OutputServiceRustProviderDiagnostics OutputServiceRustProvider::Diagnostics() const noexcept
{
	if (!m_control) return {};
	std::lock_guard lock(m_control->modelMutex);
	auto diagnostics = m_control->diagnostics;
	diagnostics.counters.advisoryDroppedNotifications =
		m_control->notificationDispatcher.DroppedNotificationCountLocked();
	diagnostics.counters.advisoryListenerFailures =
		m_control->notificationDispatcher.ListenerFailureCountLocked();
	return diagnostics;
}

OutputProviderHealthSnapshot OutputServiceRustProvider::Health() const noexcept
{
	OutputProviderHealthSnapshot health;
	health.kind = EOutputProviderKind::Rust;
	health.compiledIn = IsCompiledIn();
	health.abiVersion = IsCompiledIn() ? SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1 : 0;
	if (!m_control) {
		health.factoryStatus = EOutputProviderFactoryStatus::Unavailable;
		health.lifecycle = EOutputProviderLifecycle::Unavailable;
		health.fault = EOutputProviderFault::Initialization;
		health.failureBoundary = EOutputProviderBoundary::Factory;
		health.counters.initializationAttempts = 1;
		health.counters.boundaryFailures = 1;
		return health;
	}
	std::lock_guard lock(m_control->modelMutex);
	const auto& diagnostics = m_control->diagnostics;
	health.factoryStatus = diagnostics.initializationStage == EOutputProviderInitializationStage::Ready
		? EOutputProviderFactoryStatus::Created
		: EOutputProviderFactoryStatus::Unavailable;
	health.lifecycle = ToProviderLifecycle(diagnostics.state);
	health.initializationStage = diagnostics.initializationStage;
	health.fault = ToProviderFault(diagnostics.fault);
	health.lastBoundary = diagnostics.lastBoundary;
	health.failureBoundary = diagnostics.failureBoundary;
	health.lastBoundaryStatus = diagnostics.lastBoundary == EOutputProviderBoundary::None
		? EOutputProviderBoundaryStatus::NotCalled
		: ToBoundaryStatus(diagnostics.lastFfiStatus);
	health.available = diagnostics.availability == EOutputServiceRustProviderAvailability::Available
		&& diagnostics.state == EOutputServiceRustProviderState::Ready;
	health.hasLastOperation = diagnostics.hasLastOperation;
	health.lastOperationStatus = static_cast<EOutputOperationStatus>(diagnostics.lastOperationStatus);
	health.lastOperationReason = static_cast<EOutputOperationReason>(diagnostics.lastOperationReason);
	health.lastOperationRevision = diagnostics.lastOperationRevision;
	health.currentRevision = m_control->lastRevision;
	health.counters = diagnostics.counters;
	health.counters.advisoryDroppedNotifications =
		m_control->notificationDispatcher.DroppedNotificationCountLocked();
	health.counters.advisoryListenerFailures =
		m_control->notificationDispatcher.ListenerFailureCountLocked();
	return health;
}

OutputOperationResult OutputServiceRustProvider::CreateChannel(
	const OutputCreateChannelRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillCreate(pending, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ChannelCreated, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::AppendOutput(
	const OutputTextMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillText(pending, SAKURA_OUTPUT_PROVIDER_OP_APPEND_OUTPUT, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ContentAppended, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::ReplaceOutput(
	const OutputTextMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillText(pending, SAKURA_OUTPUT_PROVIDER_OP_REPLACE_OUTPUT, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ContentReplaced, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::AppendLog(
	const OutputLogMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillLog(pending, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ContentAppended, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::Clear(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_CLEAR, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ContentCleared, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::Show(
	const OutputShowChannelRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillShow(pending, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ChannelShown, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::Hide(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_HIDE, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ChannelHidden, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::Dispose(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_DISPOSE, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::ChannelDisposed, request.channelId);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::DisposeOwner(
	const OutputDisposeOwnerRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	RecordMutationAttempt(*m_control);
	PendingRequest pending;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		FillDisposeOwner(pending, request);
		return ApplyPending(*m_control, pending, EOutputChangeKind::OwnerDisposed, std::nullopt);
#else
		(void)request;
		return ProviderUnavailable(*m_control, true);
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(*m_control, true);
	}
}

OutputOperationResult OutputServiceRustProvider::Stop() noexcept
{
	if (!m_control) return { EOutputOperationStatus::Succeeded, EOutputOperationReason::None, 0 };
	{
		std::lock_guard lock(m_control->modelMutex);
		SaturatingIncrement(m_control->diagnostics.counters.stopCalls);
	}
	OutputOperationResult result{ EOutputOperationStatus::Succeeded, EOutputOperationReason::None, 1 };
	{
		std::unique_lock mutationLock(m_control->mutationMutex);
		bool alreadyStopped{};
		bool stopSucceeded = true;
		{
			std::lock_guard lock(m_control->modelMutex);
			alreadyStopped = m_control->authorityStopped;
			result.revision = m_control->lastRevision;
		}

		if (alreadyStopped) {
			bool pendingDestroy{};
			{
				std::lock_guard lock(m_control->modelMutex);
				pendingDestroy = m_control->pendingDestroy;
			}
			if (pendingDestroy && !DestroyToken(*m_control)) {
				result = { EOutputOperationStatus::Rejected, EOutputOperationReason::None, result.revision };
			}
			{
				std::lock_guard lock(m_control->modelMutex);
				result.revision = m_control->lastRevision;
				RecordPublicResultLocked(*m_control, result, false);
			}
		} else {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
			if (m_control->token != 0) {
				SakuraOutputProviderApplyResultV1 raw{};
				InitializeAbiHeader(raw);
				RecordBoundaryCall(*m_control, EOutputProviderBoundary::Stop, nullptr);
				const auto status = sakura_output_provider_stop_v1(m_control->token, &raw);
				RecordBoundaryStatus(*m_control, EOutputProviderBoundary::Stop, status);
				if (status != SakuraOutputProviderStatus::Ok
					|| !IsValidApplyResult(raw)
					|| raw.status != static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Succeeded)) {
					SetFault(*m_control, status == SakuraOutputProviderStatus::Ok
						? EOutputServiceRustProviderFault::AbiFailure
						: EOutputServiceRustProviderFault::FfiFailure,
						EOutputProviderBoundary::Stop, status);
					result = ProviderUnavailable(*m_control);
					stopSucceeded = false;
				} else {
					result = { static_cast<EOutputOperationStatus>(raw.status),
						static_cast<EOutputOperationReason>(raw.reason), raw.revision,
						raw.callback_drain_deferred != 0 };
					std::optional<OutputServiceSnapshot> terminalSnapshot;
					try {
						terminalSnapshot = ReadSnapshot(*m_control);
					} catch (...) {
						SetFault(*m_control, EOutputServiceRustProviderFault::SnapshotFailure,
							EOutputProviderBoundary::SnapshotDecode,
							SakuraOutputProviderStatus::InternalError);
					}
					CacheTerminalSnapshot(*m_control, std::move(terminalSnapshot), result.revision);
				}
			} else {
				CacheTerminalSnapshot(*m_control, std::nullopt, result.revision);
			}
#else
			CacheTerminalSnapshot(*m_control, std::nullopt, result.revision);
#endif
			{
				std::lock_guard lock(m_control->modelMutex);
				m_control->notificationDispatcher.StopLocked();
				if (stopSucceeded) {
					m_control->authorityStopped = true;
					m_control->diagnostics.availability =
						EOutputServiceRustProviderAvailability::Unavailable;
					m_control->diagnostics.state = EOutputServiceRustProviderState::Stopped;
				}
			}
			if (stopSucceeded && !DestroyToken(*m_control)) {
				result = { EOutputOperationStatus::Rejected, EOutputOperationReason::None, result.revision };
			}
			{
				std::lock_guard lock(m_control->modelMutex);
				result.revision = m_control->lastRevision;
				RecordPublicResultLocked(*m_control, result, false);
			}
		}
	}
	result.callbackDrainDeferred = m_control->notificationDispatcher.WaitForDrain();
	return result;
}

OutputServiceSnapshot OutputServiceRustProvider::Snapshot() const
{
	if (!m_control) return {};
	{
		std::lock_guard lock(m_control->modelMutex);
		SaturatingIncrement(m_control->diagnostics.counters.snapshotCalls);
	}
	try {
		std::unique_lock mutationLock(m_control->mutationMutex);
		{
			std::lock_guard lock(m_control->modelMutex);
			if (m_control->authorityStopped && m_control->terminalSnapshotAvailable) {
				return m_control->terminalSnapshot;
			}
			if (m_control->token == 0
				|| m_control->diagnostics.state == EOutputServiceRustProviderState::Unavailable
				|| m_control->diagnostics.state == EOutputServiceRustProviderState::Faulted) {
				return {};
			}
		}
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		if (const auto snapshot = ReadSnapshot(*m_control)) return *snapshot;
#endif
	} catch (...) {
		SetFault(*m_control, EOutputServiceRustProviderFault::SnapshotFailure,
			EOutputProviderBoundary::SnapshotDecode, SakuraOutputProviderStatus::InternalError);
	}
	return {};
}

std::optional<OutputServiceSubscriptionId> OutputServiceRustProvider::Subscribe(
	OutputServiceListener listener)
{
	if (!m_control || !listener) return std::nullopt;
	try {
		std::lock_guard lock(m_control->modelMutex);
		if (m_control->diagnostics.state != EOutputServiceRustProviderState::Ready) return std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	return m_control->notificationDispatcher.Subscribe(std::move(listener));
#else
	return std::nullopt;
#endif
}

void OutputServiceRustProvider::Unsubscribe(
	const OutputServiceSubscriptionId subscriptionId) noexcept
{
	if (!m_control) return;
	try {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		m_control->notificationDispatcher.Unsubscribe(subscriptionId);
#else
		(void)subscriptionId;
#endif
	} catch (...) {
	}
}

} // namespace workbench::output
