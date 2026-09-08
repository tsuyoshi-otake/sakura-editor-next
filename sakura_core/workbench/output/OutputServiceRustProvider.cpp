/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/output/OutputServiceRustProvider.h"
#include "workbench/output/OutputServiceNotificationDispatcher.h"
#include "workbench/output/OutputServiceRustSnapshotCodec.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::output {
namespace {

// The provider archive and its fixed-width descriptors are an x64-only ABI.
// Keep this assertion close to the conversions so an accidental Win32 build
// fails with an explicit contract error instead of relying on layout fallout.
static_assert(sizeof(void*) == sizeof(std::uint64_t),
	"Sakura Output provider ABI V1 requires 64-bit pointers");

// These assertions are deliberately exhaustive.  The provider ABI carries
// operation/channel/log discriminants as uint32_t values, while the C++ model
// uses smaller scoped enums.  Any insertion, reordering, or ABI renumbering
// must therefore fail at this boundary rather than silently changing model
// semantics.
static_assert(static_cast<std::uint32_t>(EOutputChannelKind::Output)
	== SAKURA_OUTPUT_PROVIDER_CHANNEL_OUTPUT);
static_assert(static_cast<std::uint32_t>(EOutputChannelKind::Log)
	== SAKURA_OUTPUT_PROVIDER_CHANNEL_LOG);

static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Trace) == 0);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Debug) == 1);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Info) == 2);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Warning) == 3);
static_assert(static_cast<std::uint32_t>(EOutputLogLevel::Error) == 4);

static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Succeeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Succeeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Replayed)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Replayed));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::NotApplicable)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::NotApplicable));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Rejected)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Rejected));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Conflict)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Conflict));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::StaleRevision)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::StaleRevision));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::RevisionExhausted)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::RevisionExhausted));
static_assert(static_cast<std::uint32_t>(EOutputOperationStatus::Stopped)
	== static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Stopped));

static_assert(static_cast<std::uint32_t>(EOutputOperationReason::None)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::None));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidOperationId)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidOperationId));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidOwner)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidOwner));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidChannelId)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidChannelId));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidLabel)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidLabel));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidMetadata)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidMetadata));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::InvalidPayload)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::InvalidPayload));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::PayloadLimitExceeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::PayloadLimitExceeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::OwnerLimitExceeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::OwnerLimitExceeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::ChannelLimitExceeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::ChannelLimitExceeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::TextLimitExceeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::TextLimitExceeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::LogEntryLimitExceeded)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::LogEntryLimitExceeded));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::ChannelNotFound)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::ChannelNotFound));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::OwnerGenerationConflict)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::OwnerGenerationConflict));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::ChannelKindMismatch)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::ChannelKindMismatch));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::OperationIdConflict)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::OperationIdConflict));
static_assert(static_cast<std::uint32_t>(EOutputOperationReason::ExpectedRevisionMismatch)
	== static_cast<std::uint32_t>(SakuraOutputProviderReason::ExpectedRevisionMismatch));

// Accepted-commit operation kinds use zero-based C++ discriminants and
// one-based provider request operation codes.
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::CreateChannel) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_CREATE_CHANNEL);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::AppendOutput) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_APPEND_OUTPUT);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::ReplaceOutput) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_REPLACE_OUTPUT);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::AppendLog) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_APPEND_LOG);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::Clear) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_CLEAR);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::Show) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_SHOW);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::Hide) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_HIDE);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::Dispose) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_DISPOSE);
static_assert(static_cast<std::uint32_t>(EOutputAcceptedCommitKind::DisposeOwner) + 1U
	== SAKURA_OUTPUT_PROVIDER_OP_DISPOSE_OWNER);

// ToBoundaryStatus intentionally reserves NotCalled as the zero value, so
// every ABI status maps to the following provider-health status.
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::Ok) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::Ok));
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::InvalidArgument) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::InvalidArgument));
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::InvalidHandle) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::InvalidHandle));
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::Stopped) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::Stopped));
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::InsufficientCapacity) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::InsufficientCapacity));
static_assert(static_cast<std::uint32_t>(SakuraOutputProviderStatus::InternalError) + 1U
	== static_cast<std::uint32_t>(EOutputProviderBoundaryStatus::InternalError));

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
	const auto& receipt = info.receipt;
	const auto receiptValid = receipt.measurement_id != 0
		&& receipt.stopped <= 1
		&& receipt.active_channel_present <= 1
		&& std::all_of(std::begin(receipt.reserved), std::end(receipt.reserved),
			[](const std::uint8_t value) { return value == 0; });
	return info.struct_size == sizeof(info)
		&& info.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& info.stopped <= 1
		&& info.active_channel_present <= 1
		&& std::all_of(std::begin(info.reserved0), std::end(info.reserved0),
			[](const std::uint8_t value) { return value == 0; })
		&& std::all_of(std::begin(info.reserved), std::end(info.reserved),
			[](const std::uint64_t value) { return value == 0; })
		&& receiptValid
		&& receipt.revision == info.revision
		&& receipt.stopped == info.stopped
		&& receipt.active_channel_present == info.active_channel_present
		&& receipt.dropped_notification_count == info.dropped_notification_count
		&& receipt.channel_count == info.channel_count
		&& receipt.encoded_size == info.encoded_size;
}

[[nodiscard]] bool IsValidSnapshotBuffer(
	const SakuraOutputProviderSnapshotBufferV1& buffer) noexcept
{
	const auto& receipt = buffer.receipt;
	return buffer.struct_size == sizeof(buffer)
		&& buffer.abi_version == SAKURA_OUTPUT_PROVIDER_ABI_VERSION_V1
		&& std::all_of(std::begin(buffer.reserved), std::end(buffer.reserved),
			[](const std::uint64_t value) { return value == 0; })
		&& receipt.measurement_id != 0
		&& receipt.stopped <= 1
		&& receipt.active_channel_present <= 1
		&& std::all_of(std::begin(receipt.reserved), std::end(receipt.reserved),
			[](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool IsSameSnapshotReceipt(
	const SakuraOutputProviderSnapshotReceiptV1& left,
	const SakuraOutputProviderSnapshotReceiptV1& right) noexcept
{
	return left.measurement_id == right.measurement_id
		&& left.revision == right.revision
		&& left.dropped_notification_count == right.dropped_notification_count
		&& left.channel_count == right.channel_count
		&& left.encoded_size == right.encoded_size
		&& left.stopped == right.stopped
		&& left.active_channel_present == right.active_channel_present
		&& std::equal(std::begin(left.reserved), std::end(left.reserved),
			std::begin(right.reserved));
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
	// Rust's active channel is observable only through this adapter. Keep a
	// small provider-local fact so content-only commits do not cross the FFI
	// boundary just to reproduce an unchanged advisory field. An operation
	// whose fallback selection is not knowable marks this fact unknown until a
	// listener requires a fresh query (or Snapshot() refreshes it).
	std::optional<std::string> knownActiveChannelId;
	bool activeChannelKnown{ true };
	// This is a bounded, immutable observation cache only. The valid bit is
	// cleared before every fallible authority mutation, so a saturated revision
	// or partial mutation can never make an old observation look current.
	std::shared_ptr<const OutputServiceSnapshot> snapshotCache;
	bool snapshotCacheValid{};
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
void RecordBoundaryLocked(
	Control& control,
	const EOutputProviderBoundary boundary,
	std::uint64_t OutputProviderHealthCounters::* const counter,
	const SakuraOutputProviderStatus status) noexcept
{
	SaturatingIncrement(control.diagnostics.counters.ffiCalls);
	if (counter) SaturatingIncrement(control.diagnostics.counters.*counter);
	control.diagnostics.lastBoundary = boundary;
	control.diagnostics.lastFfiStatus = status;
}

template <typename Control, typename Invoke>
[[nodiscard]] SakuraOutputProviderStatus InvokeBoundary(
	Control& control,
	const EOutputProviderBoundary boundary,
	std::uint64_t OutputProviderHealthCounters::* const counter,
	Invoke&& invoke) noexcept
{
	// The native provider exports are callback-free and do not call back into
	// Control. Keep modelMutex out of the FFI call; the diagnostic update is a
	// single short critical section after the boundary returns.
	const auto status = invoke();
	{
		std::lock_guard lock(control.modelMutex);
		RecordBoundaryLocked(control, boundary, counter, status);
	}
	return status;
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
	control.snapshotCacheValid = false;
	control.knownActiveChannelId.reset();
	control.activeChannelKnown = false;
	control.diagnostics.lastBoundary = boundary;
	control.diagnostics.failureBoundary = boundary;
	control.diagnostics.lastFfiStatus = ffiStatus;
	SaturatingIncrement(control.diagnostics.counters.boundaryFailures);
}

//! Updates the adapter's advisory active-channel fact after an accepted commit.
//! Returns true when the post-commit value is known without an ABI query.
template <typename Control>
[[nodiscard]] bool UpdateKnownActiveChannelAfterAcceptedLocked(
	Control& control,
	const EOutputChangeKind changeKind,
	const std::optional<std::string_view> channelId)
{
	switch (changeKind) {
	case EOutputChangeKind::ContentAppended:
	case EOutputChangeKind::ContentReplaced:
	case EOutputChangeKind::ContentCleared:
		// These commits never alter Rust's active-channel selection.
		return control.activeChannelKnown;
	case EOutputChangeKind::ChannelShown:
		if (!channelId) break;
		control.knownActiveChannelId = *channelId;
		control.activeChannelKnown = true;
		return true;
	case EOutputChangeKind::ChannelHidden:
	case EOutputChangeKind::ChannelDisposed:
		// Hiding or disposing a non-active channel leaves selection unchanged;
		// removing the active channel may select a fallback that only Rust knows.
		if (control.activeChannelKnown
			&& (!control.knownActiveChannelId
				|| (channelId && *control.knownActiveChannelId != *channelId))) {
			return true;
		}
		break;
	case EOutputChangeKind::ChannelCreated:
		// Rust's fallback invariant is active == none iff there are no
		// channels. A successful create from that observed empty state leaves
		// exactly the submitted channel, even when adopting a new generation.
		// This is an advisory fact from an accepted transition, not C++ channel
		// authority; nonempty/unknown states still query Rust's fallback.
		if (control.activeChannelKnown && !control.knownActiveChannelId && channelId) {
			control.knownActiveChannelId = *channelId;
			return true;
		}
		break;
	case EOutputChangeKind::OwnerDisposed:
		// Owner disposal always runs Rust's fallback selector.
		break;
	}
	control.knownActiveChannelId.reset();
	control.activeChannelKnown = false;
	return false;
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
	const auto measured = InvokeBoundary(control, EOutputProviderBoundary::SnapshotMeasure,
		nullptr, [&]() noexcept {
			return sakura_output_provider_snapshot_measure_v1(control.token, &info);
		});
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
	buffer.receipt = info.receipt;
	const auto expectedData = buffer.data;
	const auto expectedCapacity = buffer.capacity;
	const auto expectedReceipt = buffer.receipt;
	const auto written = InvokeBoundary(control, EOutputProviderBoundary::SnapshotWrite,
		nullptr, [&]() noexcept {
			return sakura_output_provider_snapshot_write_v1(control.token, &buffer);
		});
	if (written != SakuraOutputProviderStatus::Ok
		|| !IsValidSnapshotBuffer(buffer)
		|| buffer.data != expectedData
		|| buffer.capacity != expectedCapacity
		|| !IsSameSnapshotReceipt(buffer.receipt, expectedReceipt)
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
	auto snapshot = DecodeOutputServiceRustSnapshotV1(bytes);
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
		control.knownActiveChannelId = snapshot->activeChannelId;
		control.activeChannelKnown = true;
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
	std::array<std::uint8_t, kMaximumOutputStableIdBytes> inlineStorage{};
	active.data = inlineStorage.data();
	active.capacity = static_cast<std::uint64_t>(inlineStorage.size());
	const auto measured = InvokeBoundary(control, EOutputProviderBoundary::ActiveChannelQuery,
		&OutputProviderHealthCounters::activeChannelCalls, [&]() noexcept {
			return sakura_output_provider_active_channel_v1(control.token, &active);
		});
	if (measured == SakuraOutputProviderStatus::Ok) {
		if (!IsValidActiveChannelHeader(active)
			|| active.data != inlineStorage.data()
			|| active.capacity != static_cast<std::uint64_t>(inlineStorage.size())
			|| active.revision != expectedRevision
			|| active.length > static_cast<std::uint64_t>(inlineStorage.size())) {
			SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
				EOutputProviderBoundary::ActiveChannelQuery, measured);
			return false;
		}
		if (active.present == 0) {
			if (active.length != 0) {
				SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
					EOutputProviderBoundary::ActiveChannelQuery, measured);
				return false;
			}
			activeChannelId.reset();
			return true;
		}
		const auto length = static_cast<std::size_t>(active.length);
		const std::string_view value(
			reinterpret_cast<const char*>(inlineStorage.data()), length);
		if (!IsValidOutputStableId(value)) {
			SetFault(control, EOutputServiceRustProviderFault::AbiFailure,
				EOutputProviderBoundary::ActiveChannelQuery, measured);
			return false;
		}
		activeChannelId = std::string(value);
		return true;
	}
	if (measured != SakuraOutputProviderStatus::InsufficientCapacity
		|| !IsValidActiveChannelHeader(active)
		|| active.present != 1
		|| active.data != inlineStorage.data()
		|| active.capacity != static_cast<std::uint64_t>(inlineStorage.size())
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
	const auto written = InvokeBoundary(control, EOutputProviderBoundary::ActiveChannelQuery,
		&OutputProviderHealthCounters::activeChannelCalls, [&]() noexcept {
			return sakura_output_provider_active_channel_v1(control.token, &active);
		});
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
	const std::optional<std::string_view> channelId,
	std::unique_lock<std::mutex>& mutationLock)
{
	SakuraOutputProviderApplyResultV1 raw{};
	InitializeAbiHeader(raw);
	// Apply is callback-free, but keep the foreign boundary outside modelMutex.
	// The complete diagnostic/result update is published in one post-call
	// critical section below.
	const auto ffiStatus = sakura_output_provider_apply_v1(control.token, &pending.raw, &raw);
	const auto operationStatus = static_cast<SakuraOutputProviderOperationStatus>(raw.status);
	if (ffiStatus != SakuraOutputProviderStatus::Ok
		&& !(ffiStatus == SakuraOutputProviderStatus::Stopped
			&& operationStatus == SakuraOutputProviderOperationStatus::Stopped)) {
		{
			std::lock_guard lock(control.modelMutex);
			RecordBoundaryLocked(control, EOutputProviderBoundary::Apply, nullptr, ffiStatus);
		}
		SetFault(control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, ffiStatus);
		return ProviderUnavailable(control, true);
	}
	if (!IsValidApplyResult(raw)) {
		{
			std::lock_guard lock(control.modelMutex);
			RecordBoundaryLocked(control, EOutputProviderBoundary::Apply, nullptr, ffiStatus);
		}
		SetFault(control, ffiStatus == SakuraOutputProviderStatus::Ok
			? EOutputServiceRustProviderFault::AbiFailure
			: EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, ffiStatus);
		return ProviderUnavailable(control, true);
	}
	OutputOperationResult result{
		static_cast<EOutputOperationStatus>(raw.status),
		static_cast<EOutputOperationReason>(raw.reason),
		raw.revision,
		raw.callback_drain_deferred != 0 };
	bool activeQueryNeeded{};
	bool lateSubscriptionCheckNeeded{};
	bool drain{};
	{
		std::lock_guard lock(control.modelMutex);
		RecordBoundaryLocked(control, EOutputProviderBoundary::Apply, nullptr, ffiStatus);
		RecordPublicResultLocked(control, result, true);
		if (operationStatus != SakuraOutputProviderOperationStatus::Succeeded
			&& operationStatus != SakuraOutputProviderOperationStatus::Stopped) {
			// The validated non-accepted result does not change Rust state. Keep
			// the decoded observation allocated and publish it again only after
			// this boundary established that no accepted mutation occurred.
			if (!control.authorityStopped
				&& control.diagnostics.state == EOutputServiceRustProviderState::Ready
				&& control.snapshotCache) {
				control.snapshotCacheValid = true;
				if (!control.activeChannelKnown) {
					control.knownActiveChannelId = control.snapshotCache->activeChannelId;
					control.activeChannelKnown = true;
				}
			}
		}
		if (operationStatus == SakuraOutputProviderOperationStatus::Stopped) {
			control.authorityStopped = true;
			control.diagnostics.state = EOutputServiceRustProviderState::Stopped;
			control.knownActiveChannelId.reset();
			control.activeChannelKnown = true;
			control.notificationDispatcher.StopLocked();
		}
		if (operationStatus == SakuraOutputProviderOperationStatus::Succeeded) {
			const auto activeKnown = UpdateKnownActiveChannelAfterAcceptedLocked(
				control, changeKind, channelId);
			const auto hasSubscriptions = control.notificationDispatcher.HasSubscriptionsLocked();
			activeQueryNeeded = hasSubscriptions && !activeKnown;
			lateSubscriptionCheckNeeded = !hasSubscriptions && !activeKnown;
			if (hasSubscriptions && activeKnown) {
				drain = control.notificationDispatcher.QueueLocked(
					result.revision, changeKind, channelId, control.knownActiveChannelId);
			}
		}
	}
	if (operationStatus == SakuraOutputProviderOperationStatus::Succeeded) {
		bool activeQuerySucceeded = true;
		while (activeQueryNeeded || lateSubscriptionCheckNeeded) {
			if (activeQueryNeeded) {
				std::optional<std::string> queriedActiveChannelId;
				activeQuerySucceeded = ReadActiveChannel(
					control, result.revision, queriedActiveChannelId);
				if (!activeQuerySucceeded) break;
				{
					std::lock_guard lock(control.modelMutex);
					control.knownActiveChannelId = std::move(queriedActiveChannelId);
					control.activeChannelKnown = true;
				}
				activeQueryNeeded = false;
			}
			if (!activeQuerySucceeded) break;
			std::lock_guard lock(control.modelMutex);
			lateSubscriptionCheckNeeded = false;
			if (!control.notificationDispatcher.HasSubscriptionsLocked()
				|| control.diagnostics.state != EOutputServiceRustProviderState::Ready) {
				break;
			}
			if (!control.activeChannelKnown) {
				// A listener may have registered after the first check. Re-query
				// before queueing so that this accepted commit retains advisory
				// snapshot parity even in that registration race.
				activeQueryNeeded = true;
				continue;
			}
			drain = control.notificationDispatcher.QueueLocked(
				result.revision, changeKind, channelId, control.knownActiveChannelId);
			break;
		}
		mutationLock.unlock();
		if (drain) control.notificationDispatcher.Drain();
	}
	return result;
}

#endif

template <typename Control, typename FillRequest>
[[nodiscard]] OutputOperationResult ExecuteMutation(
	Control& control,
	const EOutputChangeKind changeKind,
	const std::optional<std::string_view> channelId,
	FillRequest&& fillRequest)
{
	// One transaction owns admission, conversion, the authority call and any
	// terminal fault. Do not reacquire modelMutex just to count the same entry.
	std::unique_lock mutationLock(control.mutationMutex);
	bool ready{};
	{
		std::lock_guard lock(control.modelMutex);
		SaturatingIncrement(control.diagnostics.counters.mutationCalls);
		ready = !control.authorityStopped
			&& control.diagnostics.state == EOutputServiceRustProviderState::Ready;
		if (ready) control.snapshotCacheValid = false;
	}
	if (!ready) return ProviderUnavailable(control, true);
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
	try {
		PendingRequest pending;
		fillRequest(pending);
		return ApplyPending(control, pending, changeKind, channelId, mutationLock);
	} catch (...) {
		// Conversion or post-commit observation failure is finalized before
		// another mutation can enter; no failed intermediate state is published
		// as a ready authority. Advisory callbacks remain outside both locks.
		SetFault(control, EOutputServiceRustProviderFault::FfiFailure,
			EOutputProviderBoundary::Apply, SakuraOutputProviderStatus::InternalError);
		return ProviderUnavailable(control, true);
	}
#else
	(void)changeKind;
	(void)channelId;
	(void)fillRequest;
	return ProviderUnavailable(control, true);
#endif
}

template <typename Control>
std::shared_ptr<const OutputServiceSnapshot> CacheTerminalSnapshotLocked(
	Control& control,
	const std::uint64_t revision) noexcept
{
	control.snapshotCacheValid = false;
	// Keep the old live observation out of the terminal transition while the
	// lock is held, but release its potentially large allocation afterward.
	auto previousSnapshot = std::move(control.snapshotCache);
	control.terminalSnapshot.channels.clear();
	control.terminalSnapshot.activeChannelId.reset();
	control.terminalSnapshot.revision = revision;
	control.terminalSnapshot.stopped = true;
	control.terminalSnapshot.droppedNotificationCount =
		control.notificationDispatcher.DroppedNotificationCountLocked();
	control.terminalSnapshotAvailable = true;
	control.knownActiveChannelId.reset();
	control.activeChannelKnown = true;
	control.lastRevision = revision;
	return previousSnapshot;
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
	// Keep the ABI's writable token slot private to this serialized call. The
	// control token is also observed under modelMutex by health/fault paths, so
	// passing its address directly would race with those observers while Rust
	// consumes the slot on success.
	std::uint64_t destroyToken = token;
	const auto status = sakura_output_provider_destroy_v1(&destroyToken);
	{
		std::lock_guard lock(control.modelMutex);
		RecordBoundaryLocked(control, EOutputProviderBoundary::Destroy,
			&OutputProviderHealthCounters::destroyCalls, status);
		if (status == SakuraOutputProviderStatus::Ok) {
			control.token = 0;
			control.pendingDestroy = false;
		}
	}
	if (status != SakuraOutputProviderStatus::Ok) {
		SetFault(control, EOutputServiceRustProviderFault::DestroyFailure,
			EOutputProviderBoundary::Destroy, status);
		return false;
	}
#else
	(void)token;
	{
		std::lock_guard lock(control.modelMutex);
		control.token = 0;
		control.pendingDestroy = false;
	}
#endif
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
		const auto status = InvokeBoundary(control, EOutputProviderBoundary::Create,
			nullptr, [&]() noexcept {
				return sakura_output_provider_create_v1(&rawLimits, &control.token);
			});
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
	return ExecuteMutation(*m_control, EOutputChangeKind::ChannelCreated, request.channelId,
		[&request](PendingRequest& pending) { FillCreate(pending, request); });
}

OutputOperationResult OutputServiceRustProvider::AppendOutput(
	const OutputTextMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ContentAppended, request.channelId,
		[&request](PendingRequest& pending) { FillText(pending, SAKURA_OUTPUT_PROVIDER_OP_APPEND_OUTPUT, request); });
}

OutputOperationResult OutputServiceRustProvider::ReplaceOutput(
	const OutputTextMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ContentReplaced, request.channelId,
		[&request](PendingRequest& pending) { FillText(pending, SAKURA_OUTPUT_PROVIDER_OP_REPLACE_OUTPUT, request); });
}

OutputOperationResult OutputServiceRustProvider::AppendLog(
	const OutputLogMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ContentAppended, request.channelId,
		[&request](PendingRequest& pending) { FillLog(pending, request); });
}

OutputOperationResult OutputServiceRustProvider::Clear(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ContentCleared, request.channelId,
		[&request](PendingRequest& pending) { FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_CLEAR, request); });
}

OutputOperationResult OutputServiceRustProvider::Show(
	const OutputShowChannelRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ChannelShown, request.channelId,
		[&request](PendingRequest& pending) { FillShow(pending, request); });
}

OutputOperationResult OutputServiceRustProvider::Hide(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ChannelHidden, request.channelId,
		[&request](PendingRequest& pending) { FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_HIDE, request); });
}

OutputOperationResult OutputServiceRustProvider::Dispose(
	const OutputChannelMutationRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::ChannelDisposed, request.channelId,
		[&request](PendingRequest& pending) { FillChannel(pending, SAKURA_OUTPUT_PROVIDER_OP_DISPOSE, request); });
}

OutputOperationResult OutputServiceRustProvider::DisposeOwner(
	const OutputDisposeOwnerRequest& request)
{
	if (!m_control) return { EOutputOperationStatus::Rejected, EOutputOperationReason::InvalidPayload, 0 };
	return ExecuteMutation(*m_control, EOutputChangeKind::OwnerDisposed, std::nullopt,
		[&request](PendingRequest& pending) { FillDisposeOwner(pending, request); });
}

OutputOperationResult OutputServiceRustProvider::Stop() noexcept
{
	if (!m_control) return { EOutputOperationStatus::Succeeded, EOutputOperationReason::None, 0 };
	OutputOperationResult result{ EOutputOperationStatus::Succeeded, EOutputOperationReason::None, 1 };
	{
		std::unique_lock mutationLock(m_control->mutationMutex);
		bool alreadyStopped{};
		bool pendingDestroy{};
		bool stopSucceeded = true;
		{
			std::lock_guard lock(m_control->modelMutex);
			SaturatingIncrement(m_control->diagnostics.counters.stopCalls);
			alreadyStopped = m_control->authorityStopped;
			pendingDestroy = m_control->pendingDestroy;
			result.revision = m_control->lastRevision;
			if (alreadyStopped && !pendingDestroy) {
				RecordPublicResultLocked(*m_control, result, false);
			} else if (!alreadyStopped) {
				// Invalidate before the fallible terminal ABI transition.
				m_control->snapshotCacheValid = false;
			}
		}

		if (alreadyStopped) {
			if (pendingDestroy) {
				if (!DestroyToken(*m_control)) {
					result = { EOutputOperationStatus::Rejected, EOutputOperationReason::None, result.revision };
				}
				std::lock_guard lock(m_control->modelMutex);
				result.revision = m_control->lastRevision;
				RecordPublicResultLocked(*m_control, result, false);
			}
		} else {
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
			bool publishSuccessfulStopBoundary{};
			if (m_control->token != 0) {
				SakuraOutputProviderApplyResultV1 raw{};
				InitializeAbiHeader(raw);
				const auto status = sakura_output_provider_stop_v1(m_control->token, &raw);
				if (status != SakuraOutputProviderStatus::Ok
					|| !IsValidApplyResult(raw)
					|| raw.status != static_cast<std::uint32_t>(SakuraOutputProviderOperationStatus::Succeeded)) {
					{
						std::lock_guard lock(m_control->modelMutex);
						RecordBoundaryLocked(*m_control, EOutputProviderBoundary::Stop, nullptr, status);
					}
					SetFault(*m_control, status == SakuraOutputProviderStatus::Ok
						? EOutputServiceRustProviderFault::AbiFailure
						: EOutputServiceRustProviderFault::FfiFailure,
						EOutputProviderBoundary::Stop, status);
					result = ProviderUnavailable(*m_control);
					stopSucceeded = false;
				} else {
					publishSuccessfulStopBoundary = true;
					result = { static_cast<EOutputOperationStatus>(raw.status),
						static_cast<EOutputOperationReason>(raw.reason), raw.revision,
						raw.callback_drain_deferred != 0 };
					// A validated Rust Stop is the terminal transition receipt: Rust
					// clears every channel and active selection before returning it.
					// Synthesize the provider-neutral stopped snapshot from that receipt
					// instead of crossing the two-call snapshot ABI while callbacks may
					// still be borrowing this provider.
				}
			}
#endif
			std::shared_ptr<const OutputServiceSnapshot> previousSnapshot;
			{
				std::lock_guard lock(m_control->modelMutex);
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
				if (publishSuccessfulStopBoundary) {
					RecordBoundaryLocked(*m_control, EOutputProviderBoundary::Stop,
						nullptr, SakuraOutputProviderStatus::Ok);
				}
#endif
				if (stopSucceeded) {
					previousSnapshot = CacheTerminalSnapshotLocked(*m_control, result.revision);
				}
				m_control->notificationDispatcher.StopLocked();
				if (stopSucceeded) {
					m_control->authorityStopped = true;
					m_control->diagnostics.availability =
						EOutputServiceRustProviderAvailability::Unavailable;
					m_control->diagnostics.state = EOutputServiceRustProviderState::Stopped;
				}
			}
			// Retire the potentially large live observation outside modelMutex.
			previousSnapshot.reset();
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
	try {
		std::unique_lock mutationLock(m_control->mutationMutex);
		std::shared_ptr<const OutputServiceSnapshot> cachedObservation;
		{
			std::lock_guard lock(m_control->modelMutex);
			SaturatingIncrement(m_control->diagnostics.counters.snapshotCalls);
			if (m_control->authorityStopped && m_control->terminalSnapshotAvailable) {
				return m_control->terminalSnapshot;
			}
			if (m_control->token == 0
				|| m_control->diagnostics.state == EOutputServiceRustProviderState::Unavailable
				|| m_control->diagnostics.state == EOutputServiceRustProviderState::Faulted) {
				return {};
			}
			const auto droppedNotificationCount =
				m_control->notificationDispatcher.DroppedNotificationCountLocked();
			if (m_control->snapshotCacheValid && m_control->snapshotCache) {
				if (!m_control->snapshotCache->stopped
					&& m_control->snapshotCache->revision == m_control->lastRevision
					&& m_control->snapshotCache->droppedNotificationCount == droppedNotificationCount) {
					cachedObservation = m_control->snapshotCache;
				}
				// A revision or dispatcher drop-count change makes the old
				// observation permanently stale, even if the next call races with
				// no further mutation.
				if (!cachedObservation) m_control->snapshotCacheValid = false;
			}
		}
		if (cachedObservation) {
			// Validation above is the read's linearization point. Shared ownership
			// pins that immutable revision even if a mutation or Stop retires the
			// cache. Do not serialize the O(N) caller-owned copy (or its retained
			// observation's eventual destruction) with other reads or mutations.
			mutationLock.unlock();
			return *cachedObservation;
		}
#if defined(SAKURA_OUTPUT_BACKEND_RUST)
		if (auto snapshot = ReadSnapshot(*m_control)) {
			const auto observation = std::make_shared<const OutputServiceSnapshot>(std::move(*snapshot));
			std::shared_ptr<const OutputServiceSnapshot> previousSnapshot;
			{
				std::lock_guard lock(m_control->modelMutex);
				if (!m_control->authorityStopped
					&& m_control->token != 0
					&& m_control->diagnostics.state == EOutputServiceRustProviderState::Ready
					&& !observation->stopped) {
					previousSnapshot = std::move(m_control->snapshotCache);
					m_control->snapshotCache = observation;
					m_control->snapshotCacheValid = true;
				}
			}
			mutationLock.unlock();
			previousSnapshot.reset();
			return *observation;
		}
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
