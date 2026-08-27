/*!
 * @file
 * @brief Observational accepted-commit adapter for the Rust Output shadow.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "OutputServiceRustCandidate.h"
#include "workbench/output/OutputServiceRustSnapshotCodec.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace workbench::output {
namespace {

[[nodiscard]] bool SnapshotsEqualExceptAdvisoryDrop(
	const OutputServiceSnapshot& expected,
	const OutputServiceSnapshot& actual) noexcept
{
	if (expected.revision != actual.revision
		|| expected.stopped != actual.stopped
		|| expected.activeChannelId != actual.activeChannelId
		|| expected.channels.size() != actual.channels.size()) {
		return false;
	}
	for (std::size_t index = 0; index < expected.channels.size(); ++index) {
		const auto& left = expected.channels[index];
		const auto& right = actual.channels[index];
		if (left.channelId != right.channelId
			|| left.label != right.label
			|| left.owner != right.owner
			|| left.kind != right.kind
			|| left.metadata != right.metadata
			|| left.visible != right.visible
			|| left.lastShowPreservedFocus != right.lastShowPreservedFocus
			|| left.droppedCharacterCount != right.droppedCharacterCount
			|| left.text != right.text
			|| left.logEntries != right.logEntries
			|| left.projectedText != right.projectedText) {
			return false;
		}
	}
	return true;
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
	pending.raw = {};
	pending.raw.struct_size = sizeof(pending.raw);
	pending.raw.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
	pending.raw.operation_kind = operationKind;
	// Rust validates every span descriptor, including fields that are not used
	// by a particular operation.  Start all seven request spans as valid empty
	// descriptors, then replace the fields used below.
	const auto emptySpan = Span(std::string_view{});
	pending.raw.operation_id = emptySpan;
	pending.raw.owner_id = emptySpan;
	pending.raw.channel_id = emptySpan;
	pending.raw.label = emptySpan;
	pending.raw.metadata_language_id = emptySpan;
	pending.raw.metadata_source = emptySpan;
	pending.raw.payload = emptySpan;
	pending.raw.operation_id = Span(operation.operationId);
	pending.raw.owner_id = Span(owner.ownerId);
	pending.raw.owner_generation = owner.generation;
	pending.raw.channel_id = Span(channelId);
	if (operation.expectedRevision) {
		pending.raw.flags |= SAKURA_OUTPUT_SHADOW_REQUEST_HAS_EXPECTED_REVISION;
		pending.raw.expected_revision = *operation.expectedRevision;
	}
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

[[nodiscard]] bool BuildPendingRequest(
	const OutputAcceptedCommit& commit,
	PendingRequest& pending)
{
	switch (commit.kind) {
	case EOutputAcceptedCommitKind::CreateChannel:
		if (!std::holds_alternative<OutputCreateChannelRequest>(commit.data)) return false;
		FillCreate(pending, std::get<OutputCreateChannelRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::AppendOutput:
		if (!std::holds_alternative<OutputTextMutationRequest>(commit.data)) return false;
		FillText(pending, SAKURA_OUTPUT_SHADOW_OP_APPEND_OUTPUT,
			std::get<OutputTextMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::ReplaceOutput:
		if (!std::holds_alternative<OutputTextMutationRequest>(commit.data)) return false;
		FillText(pending, SAKURA_OUTPUT_SHADOW_OP_REPLACE_OUTPUT,
			std::get<OutputTextMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::AppendLog:
		if (!std::holds_alternative<OutputLogMutationRequest>(commit.data)) return false;
		FillLog(pending, std::get<OutputLogMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::Clear:
		if (!std::holds_alternative<OutputChannelMutationRequest>(commit.data)) return false;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_CLEAR,
			std::get<OutputChannelMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::Show:
		if (!std::holds_alternative<OutputShowChannelRequest>(commit.data)) return false;
		FillShow(pending, std::get<OutputShowChannelRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::Hide:
		if (!std::holds_alternative<OutputChannelMutationRequest>(commit.data)) return false;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_HIDE,
			std::get<OutputChannelMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::Dispose:
		if (!std::holds_alternative<OutputChannelMutationRequest>(commit.data)) return false;
		FillChannel(pending, SAKURA_OUTPUT_SHADOW_OP_DISPOSE,
			std::get<OutputChannelMutationRequest>(commit.data));
		return true;
	case EOutputAcceptedCommitKind::DisposeOwner:
		if (!std::holds_alternative<OutputDisposeOwnerRequest>(commit.data)) return false;
		FillDisposeOwner(pending, std::get<OutputDisposeOwnerRequest>(commit.data));
		return true;
	}
	return false;
}

[[nodiscard]] bool IsValidApplyResult(const SakuraOutputShadowApplyResultV1& result) noexcept
{
	return result.struct_size == sizeof(result)
		&& result.abi_version == SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
		&& result.status <= static_cast<std::uint32_t>(SakuraOutputShadowOperationStatus::Stopped)
		&& result.reason <= static_cast<std::uint32_t>(SakuraOutputShadowReason::ExpectedRevisionMismatch)
		&& result.callback_drain_deferred <= 1
		&& std::all_of(std::begin(result.reserved), std::end(result.reserved),
			[](const std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool IsValidSnapshotInfo(const SakuraOutputShadowSnapshotInfoV1& info) noexcept
{
	return info.struct_size == sizeof(info)
		&& info.abi_version == SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
		&& info.stopped <= 1
		&& info.active_channel_present <= 1
		&& std::all_of(std::begin(info.reserved0), std::end(info.reserved0),
			[](const std::uint8_t value) { return value == 0; })
		&& std::all_of(std::begin(info.reserved), std::end(info.reserved),
			[](const std::uint64_t value) { return value == 0; });
}

void SaturatingIncrement(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

} // namespace

struct OutputServiceRustCandidate::Control final {
	OutputService* service{};
	mutable std::mutex mutex;
	OutputServiceRustCandidateDiagnostics diagnostics{};
	OutputAcceptedCommitSubscriptionId subscriptionId{};
	std::uint64_t token{};
	bool subscribed{};
	bool shuttingDown{};
};

#if defined(SAKURA_UTF16_RUST_CANDIDATE)
namespace {

void InitializeAbiHeader(SakuraOutputShadowApplyResultV1& result) noexcept
{
	result = {};
	result.struct_size = sizeof(result);
	result.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
}

void InitializeAbiHeader(SakuraOutputShadowSnapshotInfoV1& info) noexcept
{
	info = {};
	info.struct_size = sizeof(info);
	info.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
}

void InitializeAbiHeader(SakuraOutputShadowSnapshotBufferV1& buffer) noexcept
{
	buffer = {};
	buffer.struct_size = sizeof(buffer);
	buffer.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
}

template <typename Control>
void UnsubscribeFeedLocked(Control& control) noexcept
{
	if (!control.subscribed) return;
	const auto id = control.subscriptionId;
	control.subscriptionId = 0;
	control.subscribed = false;
	if (control.service && id != 0) control.service->UnsubscribeAcceptedCommits(id);
}

template <typename Control>
bool StopRustLocked(Control& control) noexcept
{
	if (control.token == 0) return true;
	SakuraOutputShadowApplyResultV1 raw{};
	InitializeAbiHeader(raw);
	const auto status = sakura_output_shadow_stop_v1(control.token, &raw);
	control.diagnostics.lastFfiStatus = status;
	if (status != SakuraOutputShadowStatus::Ok || !IsValidApplyResult(raw)) return false;
	control.diagnostics.lastOperationStatus =
		static_cast<SakuraOutputShadowOperationStatus>(raw.status);
	control.diagnostics.lastOperationReason =
		static_cast<SakuraOutputShadowReason>(raw.reason);
	control.diagnostics.lastOperationRevision = raw.revision;
	return raw.status == static_cast<std::uint32_t>(SakuraOutputShadowOperationStatus::Succeeded)
		&& raw.reason == static_cast<std::uint32_t>(SakuraOutputShadowReason::None);
}

template <typename Control>
bool DestroyRustLocked(Control& control) noexcept
{
	if (control.token == 0) return true;
	const auto status = sakura_output_shadow_destroy_v1(&control.token);
	control.diagnostics.lastFfiStatus = status;
	return status == SakuraOutputShadowStatus::Ok && control.token == 0;
}

template <typename Control>
bool StopAndDestroyRustLocked(Control& control) noexcept
{
	const auto priorStatus = control.diagnostics.lastFfiStatus;
	const bool stopOk = StopRustLocked(control);
	const auto stopStatus = control.diagnostics.lastFfiStatus;
	const bool destroyOk = DestroyRustLocked(control);
	const auto destroyStatus = control.diagnostics.lastFfiStatus;
	if (priorStatus != SakuraOutputShadowStatus::Ok) control.diagnostics.lastFfiStatus = priorStatus;
	else if (!stopOk) control.diagnostics.lastFfiStatus = stopStatus;
	else if (!destroyOk) control.diagnostics.lastFfiStatus = destroyStatus;
	return stopOk && destroyOk;
}

template <typename Control>
void TerminalFaultLocked(
	Control& control,
	const EOutputServiceRustCandidateFault fault) noexcept
{
	const auto priorFfiStatus = control.diagnostics.lastFfiStatus;
	const auto priorOperationStatus = control.diagnostics.lastOperationStatus;
	const auto priorOperationReason = control.diagnostics.lastOperationReason;
	const auto priorOperationRevision = control.diagnostics.lastOperationRevision;
	UnsubscribeFeedLocked(control);
	const bool shutdownOk = StopAndDestroyRustLocked(control);
	// Stop/destroy is cleanup, not a new observed commit.  Preserve the
	// operation and (when it was already a failure) FFI diagnostics that caused
	// this terminal transition; otherwise a successful cleanup would hide the
	// original mismatch/fault.
	control.diagnostics.lastOperationStatus = priorOperationStatus;
	control.diagnostics.lastOperationReason = priorOperationReason;
	control.diagnostics.lastOperationRevision = priorOperationRevision;
	if (priorFfiStatus != SakuraOutputShadowStatus::Ok) {
		control.diagnostics.lastFfiStatus = priorFfiStatus;
	}
	control.diagnostics.state = EOutputServiceRustCandidateState::Faulted;
	control.diagnostics.fault = shutdownOk
		? fault : EOutputServiceRustCandidateFault::FfiFailure;
}

template <typename Control>
bool ReadRustSnapshotLocked(
	Control& control,
	OutputServiceSnapshot& snapshot)
{
	SakuraOutputShadowSnapshotInfoV1 info{};
	InitializeAbiHeader(info);
	const auto measured = sakura_output_shadow_snapshot_measure_v1(control.token, &info);
	control.diagnostics.lastFfiStatus = measured;
	if (measured != SakuraOutputShadowStatus::Ok || !IsValidSnapshotInfo(info)) {
		if (measured == SakuraOutputShadowStatus::Ok) {
			control.diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
		}
		return false;
	}
	if (info.encoded_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		control.diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
		return false;
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.encoded_size));
	SakuraOutputShadowSnapshotBufferV1 buffer{};
	InitializeAbiHeader(buffer);
	buffer.data = bytes.empty() ? nullptr : bytes.data();
	buffer.capacity = static_cast<std::uint64_t>(bytes.size());
	const auto written = sakura_output_shadow_snapshot_write_v1(control.token, &buffer);
	control.diagnostics.lastFfiStatus = written;
	if (written != SakuraOutputShadowStatus::Ok
		|| buffer.struct_size != sizeof(buffer)
		|| buffer.abi_version != SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1
		|| buffer.length != info.encoded_size
		|| !std::all_of(std::begin(buffer.reserved), std::end(buffer.reserved),
			[](const std::uint64_t value) { return value == 0; })) {
		if (written == SakuraOutputShadowStatus::Ok) {
			control.diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
		}
		return false;
	}
	const auto parsed = DecodeOutputServiceRustSnapshotV1(bytes);
	if (!parsed
		|| parsed->revision != info.revision
		|| parsed->stopped != (info.stopped != 0)
		|| parsed->droppedNotificationCount != info.dropped_notification_count
		|| parsed->channels.size() != info.channel_count
		|| parsed->activeChannelId.has_value() != (info.active_channel_present != 0)) {
		return false;
	}
	snapshot = *parsed;
	return true;
}

template <typename Control>
bool CheckFreshRustLocked(Control& control) noexcept
{
	SakuraOutputShadowSnapshotInfoV1 info{};
	InitializeAbiHeader(info);
	const auto status = sakura_output_shadow_snapshot_measure_v1(control.token, &info);
	control.diagnostics.lastFfiStatus = status;
	if (status != SakuraOutputShadowStatus::Ok || !IsValidSnapshotInfo(info)) {
		if (status == SakuraOutputShadowStatus::Ok) {
			control.diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
		}
		return false;
	}
	return info.revision == 1
		&& info.stopped == 0
		&& info.active_channel_present == 0
		&& info.channel_count == 0;
}

template <typename Control>
void HandleEventLocked(
	Control& control,
	const OutputAcceptedCommitEvent& event)
{
	if (control.shuttingDown
		|| control.diagnostics.state != EOutputServiceRustCandidateState::Live
		|| control.token == 0) {
		return;
	}

	if (event.state == EOutputAcceptedCommitFeedState::Gap) {
		control.diagnostics.lastCursor = event.cursor.sequence;
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::Gap);
		return;
	}
	if (event.state == EOutputAcceptedCommitFeedState::Stopped) {
		if (event.cursor.sequence != control.diagnostics.lastCursor) {
			control.diagnostics.lastCursor = event.cursor.sequence;
			TerminalFaultLocked(control, EOutputServiceRustCandidateFault::CursorMismatch);
			return;
		}
		UnsubscribeFeedLocked(control);
		if (!StopRustLocked(control)) {
			const auto failedStatus = control.diagnostics.lastFfiStatus;
			const bool destroyed = DestroyRustLocked(control);
			if (!destroyed) control.diagnostics.lastFfiStatus = failedStatus;
			control.diagnostics.state = EOutputServiceRustCandidateState::Faulted;
			control.diagnostics.fault = EOutputServiceRustCandidateFault::FfiFailure;
			return;
		}
		// Keep the stopped token until explicit teardown so a final snapshot can
		// still be compared after OutputService::Stop() has fenced callbacks.
		control.diagnostics.state = EOutputServiceRustCandidateState::Stopped;
		control.diagnostics.fault = EOutputServiceRustCandidateFault::None;
		return;
	}
	if (event.state != EOutputAcceptedCommitFeedState::Live || !event.commit) {
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::CallbackException);
		return;
	}

	const auto& commit = *event.commit;
	if (commit.sequence != event.cursor.sequence
		|| control.diagnostics.lastCursor == std::numeric_limits<std::uint64_t>::max()
		|| commit.sequence != control.diagnostics.lastCursor + 1) {
		control.diagnostics.lastCursor = event.cursor.sequence;
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::CursorMismatch);
		return;
	}
	if (commit.result.status != EOutputOperationStatus::Succeeded
		|| commit.result.reason != EOutputOperationReason::None
		|| commit.postCommitRevision != commit.result.revision) {
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::ResultMismatch);
		return;
	}

	PendingRequest pending;
	if (!BuildPendingRequest(commit, pending)) {
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::ResultMismatch);
		return;
	}
	SakuraOutputShadowApplyResultV1 raw{};
	InitializeAbiHeader(raw);
	const auto status = sakura_output_shadow_apply_v1(control.token, &pending.raw, &raw);
	control.diagnostics.lastFfiStatus = status;
	if (status != SakuraOutputShadowStatus::Ok || !IsValidApplyResult(raw)) {
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::FfiFailure);
		return;
	}
	control.diagnostics.lastOperationStatus =
		static_cast<SakuraOutputShadowOperationStatus>(raw.status);
	control.diagnostics.lastOperationReason =
		static_cast<SakuraOutputShadowReason>(raw.reason);
	control.diagnostics.lastOperationRevision = raw.revision;
	if (raw.status != static_cast<std::uint32_t>(SakuraOutputShadowOperationStatus::Succeeded)
		|| raw.reason != static_cast<std::uint32_t>(SakuraOutputShadowReason::None)
		|| raw.revision != commit.postCommitRevision
		|| raw.callback_drain_deferred != static_cast<std::uint8_t>(commit.result.callbackDrainDeferred)) {
		TerminalFaultLocked(control, EOutputServiceRustCandidateFault::ResultMismatch);
		return;
	}
	control.diagnostics.lastCursor = commit.sequence;
	SaturatingIncrement(control.diagnostics.appliedCommitCount);
}

template <typename ControlPtr>
void HandleEvent(
	const ControlPtr& control,
	const OutputAcceptedCommitEvent& event) noexcept
{
	try {
		std::lock_guard lock(control->mutex);
		HandleEventLocked(*control, event);
	}
	catch (...) {
		try {
			std::lock_guard lock(control->mutex);
			if (!control->shuttingDown
				&& control->diagnostics.state == EOutputServiceRustCandidateState::Live) {
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::CallbackException);
			}
		}
		catch (...) {
			// A candidate callback is observational.  Even an allocation or
			// diagnostic failure must never escape into OutputService authority.
		}
	}
}

} // namespace
#endif

OutputServiceRustCandidate::OutputServiceRustCandidate(
	OutputService& service,
	const OutputServiceLimits& limits) noexcept
{
	try {
		auto control = std::make_shared<Control>();
		control->service = &service;
		m_control = control;
		{
			std::lock_guard lock(control->mutex);
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
			control->diagnostics.availability =
				EOutputServiceRustCandidateAvailability::Available;
			control->diagnostics.state = EOutputServiceRustCandidateState::Attaching;
			control->diagnostics.fault = EOutputServiceRustCandidateFault::None;

			SakuraOutputShadowLimitsV1 rawLimits{};
			rawLimits.struct_size = sizeof(rawLimits);
			rawLimits.abi_version = SAKURA_OUTPUT_SHADOW_ABI_VERSION_V1;
			rawLimits.maximum_owners = static_cast<std::uint64_t>(limits.maximumOwners);
			rawLimits.maximum_channels = static_cast<std::uint64_t>(limits.maximumChannels);
			rawLimits.maximum_text_bytes_per_channel = static_cast<std::uint64_t>(limits.maximumTextBytesPerChannel);
			rawLimits.maximum_payload_bytes = static_cast<std::uint64_t>(limits.maximumPayloadBytes);
			rawLimits.maximum_log_entries_per_channel = static_cast<std::uint64_t>(limits.maximumLogEntriesPerChannel);
			rawLimits.maximum_remembered_operations = static_cast<std::uint64_t>(limits.maximumRememberedOperations);
			const auto createStatus = sakura_output_shadow_create_v1(&rawLimits, &control->token);
			control->diagnostics.lastFfiStatus = createStatus;
			if (createStatus != SakuraOutputShadowStatus::Ok || control->token == 0) {
				if (createStatus == SakuraOutputShadowStatus::Ok) {
					control->diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
				}
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::FfiFailure);
				return;
			}
			if (!CheckFreshRustLocked(*control)) {
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::BootstrapMismatch);
				return;
			}

			// The mutex remains held while SubscribeAcceptedCommits performs its
			// atomic snapshot/cursor/subscription operation.  A racing callback can
			// therefore wait until the Attaching state has become Live.
			const auto bootstrap = service.SubscribeAcceptedCommits(
				[control](const OutputAcceptedCommitEvent& event) noexcept {
					HandleEvent(control, event);
				});
			if (bootstrap && bootstrap->subscriptionId != 0) {
				// Record ownership before validating freshness.  A successful
				// subscription with an unsupported/non-fresh bootstrap must still be
				// detached so it cannot retain the feed journal after this fault.
				control->subscriptionId = bootstrap->subscriptionId;
				control->subscribed = true;
			}
			if (!bootstrap) {
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::SubscribeFailed);
				return;
			}
			if (bootstrap->state != EOutputAcceptedCommitFeedState::Live
				|| bootstrap->subscriptionId == 0
				|| bootstrap->cursor.sequence != 0
				|| bootstrap->snapshot.revision != 1
				|| bootstrap->snapshot.stopped
				|| bootstrap->snapshot.activeChannelId
				|| !bootstrap->snapshot.channels.empty()) {
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::BootstrapMismatch);
				return;
			}
			control->diagnostics.lastCursor = bootstrap->cursor.sequence;
			control->diagnostics.state = EOutputServiceRustCandidateState::Live;
			control->diagnostics.fault = EOutputServiceRustCandidateFault::None;
#else
			(void)limits;
			control->diagnostics.availability =
				EOutputServiceRustCandidateAvailability::Unavailable;
			control->diagnostics.state = EOutputServiceRustCandidateState::Unavailable;
			control->diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
			control->diagnostics.fault = EOutputServiceRustCandidateFault::Unavailable;
#endif
		}
	}
	catch (...) {
		if (m_control) {
			try {
				std::lock_guard lock(m_control->mutex);
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
				TerminalFaultLocked(*m_control, EOutputServiceRustCandidateFault::CallbackException);
#else
				m_control->diagnostics.availability =
					EOutputServiceRustCandidateAvailability::Unavailable;
				m_control->diagnostics.state = EOutputServiceRustCandidateState::Unavailable;
				m_control->diagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
				m_control->diagnostics.fault = EOutputServiceRustCandidateFault::Unavailable;
#endif
				m_fallbackDiagnostics = m_control->diagnostics;
			}
			catch (...) {
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
				m_fallbackDiagnostics.availability =
					EOutputServiceRustCandidateAvailability::Available;
				m_fallbackDiagnostics.state = EOutputServiceRustCandidateState::Faulted;
				m_fallbackDiagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
				m_fallbackDiagnostics.fault = EOutputServiceRustCandidateFault::CallbackException;
#endif
			}
		}
		else {
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
			m_fallbackDiagnostics.availability =
				EOutputServiceRustCandidateAvailability::Available;
			m_fallbackDiagnostics.state = EOutputServiceRustCandidateState::Faulted;
			m_fallbackDiagnostics.lastFfiStatus = SakuraOutputShadowStatus::InternalError;
			m_fallbackDiagnostics.fault = EOutputServiceRustCandidateFault::CallbackException;
#endif
		}
	}
}

OutputServiceRustCandidate::~OutputServiceRustCandidate() noexcept
{
	ShutdownAfterOutputServiceStop();
}

bool OutputServiceRustCandidate::IsAvailable() const noexcept
{
	return Diagnostics().availability == EOutputServiceRustCandidateAvailability::Available;
}

OutputServiceRustCandidateDiagnostics OutputServiceRustCandidate::Diagnostics() const noexcept
{
	const auto control = m_control;
	if (!control) return m_fallbackDiagnostics;
	try {
		std::lock_guard lock(control->mutex);
		return control->diagnostics;
	}
	catch (...) {
		return m_fallbackDiagnostics;
	}
}

bool OutputServiceRustCandidate::VerifySnapshot() noexcept
{
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
	const auto control = m_control;
	if (!control) return false;
	try {
		std::lock_guard lock(control->mutex);
		if (control->shuttingDown || control->token == 0
			|| (control->diagnostics.state != EOutputServiceRustCandidateState::Live
				&& control->diagnostics.state != EOutputServiceRustCandidateState::Stopped)) {
			return false;
		}
		if (!control->service) return false;
		const auto authority = control->service->Snapshot();
		OutputServiceSnapshot shadow;
		if (!ReadRustSnapshotLocked(*control, shadow)) {
			const auto fault = control->diagnostics.lastFfiStatus == SakuraOutputShadowStatus::Ok
				? EOutputServiceRustCandidateFault::SnapshotMismatch
				: EOutputServiceRustCandidateFault::FfiFailure;
			TerminalFaultLocked(*control, fault);
			return false;
		}
		if (!SnapshotsEqualExceptAdvisoryDrop(authority, shadow)) {
			TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::SnapshotMismatch);
			return false;
		}
		return true;
	}
	catch (...) {
		try {
			std::lock_guard lock(control->mutex);
			if (!control->shuttingDown
				&& control->token != 0
				&& control->diagnostics.state != EOutputServiceRustCandidateState::Faulted) {
				TerminalFaultLocked(*control, EOutputServiceRustCandidateFault::CallbackException);
			}
		}
		catch (...) {
		}
		return false;
	}
#else
	return false;
#endif
}

void OutputServiceRustCandidate::ShutdownAfterOutputServiceStop() noexcept
{
	const auto control = m_control;
	if (!control) return;
	OutputService* service{};
	OutputAcceptedCommitSubscriptionId subscriptionId{};
	try {
		{
			std::lock_guard lock(control->mutex);
			if (control->shuttingDown) return;
			control->shuttingDown = true;
			service = control->service;
			if (control->subscribed) {
				subscriptionId = control->subscriptionId;
				control->subscriptionId = 0;
				control->subscribed = false;
			}
		}
		// Unsubscribe is intentionally non-draining.  The second lock below is
		// the callback-state drain; OutputService::Stop remains the authority's
		// formal lifetime fence for the normal runtime path.
		if (service && subscriptionId != 0) service->UnsubscribeAcceptedCommits(subscriptionId);
		std::lock_guard lock(control->mutex);
#if defined(SAKURA_UTF16_RUST_CANDIDATE)
		const auto priorStatus = control->diagnostics.lastFfiStatus;
		const bool shutdownOk = StopAndDestroyRustLocked(*control);
		if (priorStatus != SakuraOutputShadowStatus::Ok) {
			control->diagnostics.lastFfiStatus = priorStatus;
		}
		if (!shutdownOk && control->diagnostics.fault == EOutputServiceRustCandidateFault::None) {
			control->diagnostics.fault = EOutputServiceRustCandidateFault::DestroyFailure;
			control->diagnostics.state = EOutputServiceRustCandidateState::Faulted;
		}
#endif
		if (control->diagnostics.state != EOutputServiceRustCandidateState::Faulted) {
			if (control->diagnostics.availability == EOutputServiceRustCandidateAvailability::Available) {
				control->diagnostics.state = EOutputServiceRustCandidateState::Stopped;
			}
		}
		control->service = nullptr;
	}
	catch (...) {
		// Destruction is best effort after the documented service lifetime fence.
		// Never let cleanup exceptions escape a destructor or authority teardown.
	}
}

} // namespace workbench::output
