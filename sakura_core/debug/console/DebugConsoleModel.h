/*! @file
 * @brief Bounded, transport-free state model for a Debug Console.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace debug::console {

using DebugConsoleSessionGeneration = std::uint64_t;
using DebugConsoleEvaluationRequestId = std::uint64_t;
using DebugConsoleSubscriptionId = std::uint64_t;

//! Matches the stable output channels exposed by VS Code's Debug Console without importing DAP types.
enum class EDebugConsoleOutputCategory : std::uint8_t {
	Console,
	Stdout,
	Stderr,
	Telemetry,
	Important,
};

enum class EDebugConsoleEvaluationTerminal : std::uint8_t {
	Completed,
	Failed,
	Cancelled,
	Expired,
};

enum class EDebugConsoleModelStatus : std::uint8_t {
	SessionStarted,
	Accepted,
	Replayed,
	Conflict,
	InvalidArgument,
	StaleGeneration,
	NoActiveSession,
	SessionActive,
	SessionDisposed,
	Stopped,
	PendingLimitExceeded,
	UnknownRequest,
	AlreadyTerminal,
	SubscriptionLimitExceeded,
	SequenceExhausted,
	ResourceExhausted,
	StopNotificationDrainDeferred,
	StoppedWithDroppedState,
};

struct DebugConsoleOutput final {
	std::uint64_t sequence{};
	DebugConsoleSessionGeneration sessionGeneration{};
	EDebugConsoleOutputCategory category{ EDebugConsoleOutputCategory::Console };
	std::string text;
};

struct DebugConsoleReplHistoryEntry final {
	std::uint64_t sequence{};
	DebugConsoleSessionGeneration sessionGeneration{};
	std::string expression;
};

struct DebugConsolePendingEvaluation final {
	DebugConsoleEvaluationRequestId requestId{};
	std::string operationId;
	std::string expression;
	std::optional<std::uint64_t> deadline;
};

struct DebugConsoleCompletedEvaluation final {
	DebugConsoleSessionGeneration sessionGeneration{};
	DebugConsoleEvaluationRequestId requestId{};
	std::string operationId;
	std::string expression;
	EDebugConsoleEvaluationTerminal terminal{ EDebugConsoleEvaluationTerminal::Cancelled };
	std::string detail;
};

enum class EDebugConsoleNotificationKind : std::uint8_t {
	SessionStarted,
	OutputAppended,
	ReplSubmitted,
	EvaluationCompleted,
	EvaluationFailed,
	EvaluationCancelled,
	EvaluationExpired,
	SessionDisposed,
	Stopped,
};

//! Values are copied before delivery and callbacks run outside the model mutex.
struct DebugConsoleNotification final {
	EDebugConsoleNotificationKind kind{ EDebugConsoleNotificationKind::SessionStarted };
	DebugConsoleSessionGeneration sessionGeneration{};
	std::optional<DebugConsoleEvaluationRequestId> requestId;
	std::optional<std::uint64_t> transcriptSequence;
	std::optional<EDebugConsoleEvaluationTerminal> terminal;
};

using DebugConsoleListener = std::function<void(const DebugConsoleNotification&)>;

struct DebugConsoleLimits final {
	std::size_t maximumTranscriptEntries{ 2'048 };
	std::size_t maximumHistoryEntries{ 512 };
	std::size_t maximumPendingEvaluations{ 256 };
	std::size_t maximumCompletedEvaluations{ 1'024 };
	std::size_t maximumPendingNotifications{ 1'024 };
	std::size_t maximumSubscriptions{ 128 };
	std::size_t maximumOperationIdLength{ 256 };
	std::size_t maximumExpressionLength{ 16'384 };
	std::size_t maximumOutputTextLength{ 65'536 };
	std::size_t maximumCompletionDetailLength{ 16'384 };
	//! Testable ceilings for monotonic identifiers.  A value of zero is normalized to one.
	std::uint64_t maximumEvaluationRequestId{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumTranscriptSequence{ (std::numeric_limits<std::uint64_t>::max)() };
	std::uint64_t maximumHistorySequence{ (std::numeric_limits<std::uint64_t>::max)() };
};

struct DebugConsoleModelResult final {
	EDebugConsoleModelStatus status{ EDebugConsoleModelStatus::InvalidArgument };
	std::optional<DebugConsoleEvaluationRequestId> requestId;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EDebugConsoleModelStatus::SessionStarted || status == EDebugConsoleModelStatus::Accepted
			|| status == EDebugConsoleModelStatus::Replayed || status == EDebugConsoleModelStatus::SessionDisposed
			|| status == EDebugConsoleModelStatus::Stopped || status == EDebugConsoleModelStatus::StopNotificationDrainDeferred
			|| status == EDebugConsoleModelStatus::StoppedWithDroppedState;
	}
};

//! Expiry can make bounded partial progress; the snapshot exposes every evaluation still pending after a failure.
struct DebugConsoleExpiryResult final {
	EDebugConsoleModelStatus status{ EDebugConsoleModelStatus::InvalidArgument };
	std::size_t expiredCount{};
};

struct DebugConsoleSnapshot final {
	bool stopped{};
	std::optional<DebugConsoleSessionGeneration> activeSessionGeneration;
	DebugConsoleSessionGeneration newestSessionGeneration{};
	DebugConsoleEvaluationRequestId nextEvaluationRequestId{ 1 };
	std::uint64_t nextTranscriptSequence{ 1 };
	std::uint64_t nextHistorySequence{ 1 };
	std::size_t droppedTranscriptEntryCount{};
	std::size_t droppedHistoryEntryCount{};
	std::size_t droppedCompletedEvaluationCount{};
	std::size_t droppedNotificationCount{};
	std::vector<DebugConsoleOutput> transcript;
	std::vector<DebugConsoleReplHistoryEntry> history;
	std::vector<DebugConsolePendingEvaluation> pendingEvaluations;
	std::vector<DebugConsoleCompletedEvaluation> completedEvaluations;
};

/*! 
 * @brief A deterministic Debug Console state model; it never creates a transport, thread, process, or UI object.
 *
 * Session generation is an ownership fence.  Every session-scoped mutation must present the current
 * generation.  An integration adapter owns forwarding accepted evaluation requests and routing the
 * terminal calls back to this class.
 */
class CDebugConsoleModel final {
public:
	explicit CDebugConsoleModel(DebugConsoleLimits limits = {});
	~CDebugConsoleModel();
	CDebugConsoleModel(const CDebugConsoleModel&) = delete;
	CDebugConsoleModel& operator=(const CDebugConsoleModel&) = delete;

	[[nodiscard]] DebugConsoleModelResult StartSession(DebugConsoleSessionGeneration generation);
	[[nodiscard]] DebugConsoleModelResult AppendOutput(DebugConsoleSessionGeneration generation, EDebugConsoleOutputCategory category,
		std::string text);
	//! Reusing an operation id is accepted only when its session and expression match exactly.
	[[nodiscard]] DebugConsoleModelResult SubmitReplInput(DebugConsoleSessionGeneration generation, std::string operationId,
		std::string expression, std::optional<std::uint64_t> deadline = std::nullopt);
	[[nodiscard]] DebugConsoleModelResult CompleteEvaluation(DebugConsoleSessionGeneration generation, DebugConsoleEvaluationRequestId requestId,
		std::string detail = {});
	[[nodiscard]] DebugConsoleModelResult FailEvaluation(DebugConsoleSessionGeneration generation, DebugConsoleEvaluationRequestId requestId,
		std::string detail);
	[[nodiscard]] DebugConsoleModelResult CancelEvaluation(DebugConsoleSessionGeneration generation, DebugConsoleEvaluationRequestId requestId,
		std::string detail = {});
	//! Finalizes pending evaluations whose deadline is no later than callerSuppliedTime, reporting partial resource failure explicitly.
	[[nodiscard]] DebugConsoleExpiryResult ExpireEvaluations(DebugConsoleSessionGeneration generation, std::uint64_t callerSuppliedTime,
		std::string detail = "expired");
	//! Cancels every pending evaluation in the supplied generation and releases the active session.
	[[nodiscard]] DebugConsoleModelResult DisposeSession(DebugConsoleSessionGeneration generation);
	/*! @brief Globally finalizes the model.
	 *
	 * A call from a notification callback returns @c StopNotificationDrainDeferred: the current notification
	 * dispatcher owns the remaining final delivery and listener cleanup.  A call from any other thread waits
	 * until a callback already in progress has returned, so that thread never observes Stop complete while a
	 * callback is still using the model.  This is explicit because destruction is not a user-visible lifecycle event.
	 *
	 * The owner must keep this object alive until every public call and notification callback has returned.
	 * In particular, a listener must never destroy this model from its callback: callbacks borrow the model's
	 * lifetime and self-destruction would be use-after-free.  Arrange disposal on an owning dispatcher after Stop.
	 */
	[[nodiscard]] DebugConsoleModelResult Stop();

	[[nodiscard]] DebugConsoleSnapshot Snapshot() const;
	[[nodiscard]] std::optional<DebugConsoleSubscriptionId> Subscribe(DebugConsoleListener listener);
	void Unsubscribe(DebugConsoleSubscriptionId subscriptionId) noexcept;

private:
	struct PendingEvaluation final {
		std::string operationId;
		std::string expression;
		std::optional<std::uint64_t> deadline;
	};
	struct OperationRecord final {
		DebugConsoleSessionGeneration generation{};
		DebugConsoleEvaluationRequestId requestId{};
		std::string expression;
		std::optional<std::uint64_t> deadline;
	};

	[[nodiscard]] DebugConsoleModelResult SessionStatusLocked(DebugConsoleSessionGeneration generation) const noexcept;
	[[nodiscard]] bool IsValidOperationLocked(const std::string& operationId, const std::string& expression) const noexcept;
	void EnqueueLocked(DebugConsoleNotification notification) noexcept;
	void RecordCompletionLocked(DebugConsoleEvaluationRequestId requestId, const PendingEvaluation& evaluation,
		EDebugConsoleEvaluationTerminal terminal, std::string detail);
	[[nodiscard]] DebugConsoleModelResult FinalizeEvaluation(DebugConsoleSessionGeneration generation,
		DebugConsoleEvaluationRequestId requestId, EDebugConsoleEvaluationTerminal terminal, std::string detail);
	void FinalizeAllPendingLocked(EDebugConsoleEvaluationTerminal terminal, const std::string& detail);
	void DrainNotifications();
	void CompleteStopFinalizationLocked() noexcept;

	DebugConsoleLimits m_limits;
	mutable std::mutex m_mutex;
	std::condition_variable m_stopCompletionCondition;
	bool m_stopped{};
	std::optional<DebugConsoleSessionGeneration> m_activeSessionGeneration;
	DebugConsoleSessionGeneration m_newestSessionGeneration{};
	DebugConsoleEvaluationRequestId m_nextEvaluationRequestId{ 1 };
	std::uint64_t m_nextTranscriptSequence{ 1 };
	std::uint64_t m_nextHistorySequence{ 1 };
	std::map<DebugConsoleEvaluationRequestId, PendingEvaluation> m_pendingEvaluations;
	std::deque<DebugConsoleCompletedEvaluation> m_completedEvaluations;
	std::map<std::string, OperationRecord> m_operations;
	std::deque<DebugConsoleOutput> m_transcript;
	std::deque<DebugConsoleReplHistoryEntry> m_history;
	std::map<DebugConsoleSubscriptionId, DebugConsoleListener> m_listeners;
	DebugConsoleSubscriptionId m_nextSubscriptionId{ 1 };
	std::deque<DebugConsoleNotification> m_notifications;
	bool m_dispatchingNotifications{};
	std::thread::id m_dispatchThreadId;
	bool m_clearListenersAfterDispatch{};
	bool m_stopFinalizationInProgress{};
	bool m_stopFinalizationCompleted{};
	DebugConsoleModelResult m_finalStopResult{ EDebugConsoleModelStatus::Stopped };
	std::size_t m_droppedTranscriptEntryCount{};
	std::size_t m_droppedHistoryEntryCount{};
	std::size_t m_droppedCompletedEvaluationCount{};
	std::size_t m_droppedNotificationCount{};
};

} // namespace debug::console
