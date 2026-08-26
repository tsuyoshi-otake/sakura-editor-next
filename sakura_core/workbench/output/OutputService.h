/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace workbench::output {

//! Explicit owner generations prevent a reloaded extension from mutating an older channel.
struct OutputOwner {
	std::string ownerId;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const OutputOwner&) const noexcept = default;
};

enum class EOutputChannelKind : std::uint8_t {
	Output,
	Log,
};

enum class EOutputLogLevel : std::uint8_t {
	Trace,
	Debug,
	Info,
	Warning,
	Error,
};

//! Optional metadata is descriptive only; it never introduces a filesystem or editor dependency.
struct OutputChannelMetadata {
	std::optional<std::string> languageId;
	std::optional<std::string> source;
	[[nodiscard]] bool operator==(const OutputChannelMetadata&) const noexcept = default;
};

struct OutputLogEntry {
	EOutputLogLevel level{ EOutputLogLevel::Info };
	std::string message;
	std::optional<std::string> source;
	[[nodiscard]] bool operator==(const OutputLogEntry&) const noexcept = default;
};

struct OutputOperation {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

struct OutputCreateChannelRequest {
	OutputOperation operation;
	OutputOwner owner;
	std::string channelId;
	std::string label;
	EOutputChannelKind kind{ EOutputChannelKind::Output };
	OutputChannelMetadata metadata;
};

struct OutputTextMutationRequest {
	OutputOperation operation;
	OutputOwner owner;
	std::string channelId;
	std::string text;
};

struct OutputLogMutationRequest {
	OutputOperation operation;
	OutputOwner owner;
	std::string channelId;
	std::vector<OutputLogEntry> entries;
};

struct OutputChannelMutationRequest {
	OutputOperation operation;
	OutputOwner owner;
	std::string channelId;
};

struct OutputShowChannelRequest {
	OutputOperation operation;
	OutputOwner owner;
	std::string channelId;
	//! A projection adapter uses this value to avoid stealing native input focus.
	bool preserveFocus{};
};

struct OutputDisposeOwnerRequest {
	OutputOperation operation;
	OutputOwner owner;
};

enum class EOutputOperationStatus : std::uint8_t {
	Succeeded,
	Replayed,
	NotApplicable,
	Rejected,
	Conflict,
	StaleRevision,
	RevisionExhausted,
	Stopped,
};

enum class EOutputOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	InvalidOwner,
	InvalidChannelId,
	InvalidLabel,
	InvalidMetadata,
	InvalidPayload,
	PayloadLimitExceeded,
	//! Owner-generation fences are lifetime-bounded and never evicted before Stop().
	OwnerLimitExceeded,
	ChannelLimitExceeded,
	TextLimitExceeded,
	LogEntryLimitExceeded,
	ChannelNotFound,
	OwnerGenerationConflict,
	ChannelKindMismatch,
	OperationIdConflict,
	ExpectedRevisionMismatch,
};

struct OutputOperationResult {
	EOutputOperationStatus status{ EOutputOperationStatus::Rejected };
	EOutputOperationReason reason{ EOutputOperationReason::None };
	std::uint64_t revision{};
	bool callbackDrainDeferred{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EOutputOperationStatus::Succeeded || status == EOutputOperationStatus::Replayed;
	}
};

//! Snapshot data is immutable by value. `text` is populated only for Output channels and `logEntries` only for Log channels.
struct OutputChannelSnapshot {
	std::string channelId;
	std::string label;
	OutputOwner owner;
	EOutputChannelKind kind{ EOutputChannelKind::Output };
	OutputChannelMetadata metadata;
	bool visible{};
	bool lastShowPreservedFocus{};
	std::uint64_t droppedCharacterCount{};
	std::string text;
	std::vector<OutputLogEntry> logEntries;
	//! A bounded deterministic rendering for a native Output view. It does not replace structured log entries.
	std::string projectedText;
};

struct OutputServiceSnapshot {
	std::uint64_t revision{};
	bool stopped{};
	std::uint64_t droppedNotificationCount{};
	std::optional<std::string> activeChannelId;
	std::vector<OutputChannelSnapshot> channels;
};

//! The accepted-commit feed has its own sequence space, independent of model revisions.
struct OutputAcceptedCommitCursor {
	std::uint64_t sequence{};

	[[nodiscard]] bool operator==(const OutputAcceptedCommitCursor&) const noexcept = default;
};

enum class EOutputAcceptedCommitFeedState : std::uint8_t {
	Live,
	Gap,
	Stopped,
};

enum class EOutputAcceptedCommitKind : std::uint8_t {
	CreateChannel,
	AppendOutput,
	ReplaceOutput,
	AppendLog,
	Clear,
	Show,
	Hide,
	Dispose,
	DisposeOwner,
};

//! Request data is copied into the feed; the kind disambiguates shared request DTOs such as text mutations.
using OutputAcceptedCommitData = std::variant<
	OutputCreateChannelRequest,
	OutputTextMutationRequest,
	OutputLogMutationRequest,
	OutputChannelMutationRequest,
	OutputShowChannelRequest,
	OutputDisposeOwnerRequest>;

//! One fresh successful mutation, with no borrow into the service or caller-owned request.
struct OutputAcceptedCommit {
	std::uint64_t sequence{};
	EOutputAcceptedCommitKind kind{ EOutputAcceptedCommitKind::CreateChannel };
	OutputAcceptedCommitData data;
	OutputOperationResult result;
	std::uint64_t postCommitRevision{};
};

//! Feed callbacks carry one copied commit, an explicit terminal gap, or terminal stopped state.
struct OutputAcceptedCommitEvent {
	EOutputAcceptedCommitFeedState state{ EOutputAcceptedCommitFeedState::Live };
	std::optional<OutputAcceptedCommit> commit;
	OutputAcceptedCommitCursor cursor;
	std::uint64_t missingFromSequence{};
	std::uint64_t missingToSequence{};
};

using OutputAcceptedCommitSubscriptionId = std::uint64_t;
using OutputAcceptedCommitListener = std::function<void(const OutputAcceptedCommitEvent&)>;

//! Snapshot and accepted-feed bootstrap are captured and subscribed under one model lock.
struct OutputAcceptedCommitBootstrap {
	OutputServiceSnapshot snapshot;
	OutputAcceptedCommitCursor cursor;
	EOutputAcceptedCommitFeedState state{ EOutputAcceptedCommitFeedState::Live };
	OutputAcceptedCommitSubscriptionId subscriptionId{};
};

enum class EOutputChangeKind : std::uint8_t {
	ChannelCreated,
	ContentAppended,
	ContentReplaced,
	ContentCleared,
	ChannelShown,
	ChannelHidden,
	ChannelDisposed,
	OwnerDisposed,
};

struct OutputServiceChange {
	std::uint64_t revision{};
	EOutputChangeKind kind{ EOutputChangeKind::ChannelCreated };
	std::optional<std::string> channelId;
	std::optional<std::string> activeChannelId;
};

using OutputServiceSubscriptionId = std::uint64_t;
using OutputServiceListener = std::function<void(const OutputServiceChange&)>;

//! Limits are deliberately public so adapters can enforce a smaller resource budget for constrained hosts.
struct OutputServiceLimits {
	std::size_t maximumOwners{ 128 };
	std::size_t maximumChannels{ 128 };
	std::size_t maximumTextBytesPerChannel{ 1U << 20 };
	std::size_t maximumPayloadBytes{ 64U << 10 };
	std::size_t maximumLogEntriesPerChannel{ 4'096 };
	std::size_t maximumSubscriptions{ 256 };
	std::size_t maximumRememberedOperations{ 512 };
	std::size_t maximumPendingNotifications{ 512 };
	std::size_t maximumAcceptedCommitFeedEntries{ 512 };
};

/*!
	@brief Thread-safe pure output-channel model, intentionally independent of HWNDs, files and extension RPC.

	Each accepted mutation has a caller-supplied operation ID and optional expected service revision. Exact replay
	is accepted once remembered; reuse of an operation ID with a different request is a conflict. Notifications are
	queued under the model lock and delivered after unlocking, so listeners can safely mutate the service.
*/
class OutputService final {
public:
	explicit OutputService(OutputServiceLimits limits = {});
	~OutputService();

	OutputService(const OutputService&) = delete;
	OutputService& operator=(const OutputService&) = delete;

	[[nodiscard]] OutputOperationResult CreateChannel(const OutputCreateChannelRequest& request);
	[[nodiscard]] OutputOperationResult AppendOutput(const OutputTextMutationRequest& request);
	[[nodiscard]] OutputOperationResult ReplaceOutput(const OutputTextMutationRequest& request);
	[[nodiscard]] OutputOperationResult AppendLog(const OutputLogMutationRequest& request);
	[[nodiscard]] OutputOperationResult Clear(const OutputChannelMutationRequest& request);
	[[nodiscard]] OutputOperationResult Show(const OutputShowChannelRequest& request);
	[[nodiscard]] OutputOperationResult Hide(const OutputChannelMutationRequest& request);
	[[nodiscard]] OutputOperationResult Dispose(const OutputChannelMutationRequest& request);
	[[nodiscard]] OutputOperationResult DisposeOwner(const OutputDisposeOwnerRequest& request);
	//! External callers wait for active listener callbacks; a reentrant listener Stop returns deferred.
	//! A listener borrows this service and must not destroy it from inside the callback.
	[[nodiscard]] OutputOperationResult Stop() noexcept;

	[[nodiscard]] OutputServiceSnapshot Snapshot() const;
	[[nodiscard]] std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener listener);
	void Unsubscribe(OutputServiceSubscriptionId subscriptionId) noexcept;
	//! Atomically captures a service snapshot, current feed cursor, and a future-only feed subscription.
	//! A returned Live subscription starts after the returned cursor. A Gap event is terminal and requires
	//! resnapshot/rebootstrap; it is never silently converted into a partial stream.
	[[nodiscard]] std::optional<OutputAcceptedCommitBootstrap> SubscribeAcceptedCommits(OutputAcceptedCommitListener listener);
	//! Removes a feed subscription without waiting. Stop() is the lifetime fence for an owner that is
	//! destroying a callback target; an already-active copied callback may finish after this returns.
	void UnsubscribeAcceptedCommits(OutputAcceptedCommitSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;
	[[nodiscard]] static bool IsValidOperationId(std::string_view value) noexcept;

private:
	struct Impl;
	Impl* m_impl;
};

} // namespace workbench::output
