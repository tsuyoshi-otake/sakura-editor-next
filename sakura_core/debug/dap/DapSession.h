/*! @file
 * @brief Bounded, transport-injected Debug Adapter Protocol session owner.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "debug/dap/DapProtocolCodec.h"

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace debug::dap {

//! The session owns protocol state, never an adapter process or a native handle.
enum class EDapSessionState : std::uint8_t {
	Idle,
	Running,
	Failed,
	Closed,
	Stopped,
};

//! Terminal outcome remains observable after all pending requests are finalized.
enum class EDapSessionOutcome : std::uint8_t {
	None,
	CodecFailure,
	ProtocolViolation,
	TransportFailure,
	ClosedByCaller,
	StoppedByOwner,
};

enum class EDapSessionStatus : std::uint8_t {
	Started,
	Sent,
	Received,
	Cancelled,
	TimedOut,
	Closed,
	Stopped,
	NotStarted,
	AlreadyStarted,
	InvalidRequest,
	PendingRequestLimitExceeded,
	//! The configured monotonic outbound sequence space has been exhausted; no sequence is reused.
	OutgoingSequenceExhausted,
	//! A bounded state or observer record could not be allocated; the caller may retry or close explicitly.
	ResourceExhausted,
	UnknownRequest,
	TransportFailed,
	CodecFailed,
	ProtocolViolation,
	Terminal,
};

//! The only required adapter integration point. Implementations must not retain the passed view.
class IDapByteTransport {
public:
	virtual ~IDapByteTransport() = default;
	//! Returns false when the bytes were not accepted by the transport.
	virtual bool Send(std::string_view bytes) noexcept = 0;
	//! This is called at most once by a CDapSession terminal transition.
	virtual void Close() noexcept = 0;
};

struct DapSessionRequest final {
	std::string command;
	std::optional<std::string> argumentsJson;
	//! Caller-owned monotonic time; no session timer or background polling is created.
	std::optional<std::uint64_t> deadline;
};

struct DapPendingRequestSnapshot final {
	std::uint64_t sequence{};
	std::string command;
	std::optional<std::uint64_t> deadline;
};

//! Immutable terminal accounting survives notification queue overflow until its bounded history is evicted.
enum class EDapRequestCompletionKind : std::uint8_t {
	Responded,
	Cancelled,
	TimedOut,
	Rejected,
};

struct DapCompletedRequestSnapshot final {
	std::uint64_t sequence{};
	std::string command;
	EDapRequestCompletionKind kind{ EDapRequestCompletionKind::Rejected };
	EDapSessionOutcome outcome{ EDapSessionOutcome::None };
};

struct DapSessionSnapshot final {
	EDapSessionState state{ EDapSessionState::Idle };
	EDapSessionOutcome outcome{ EDapSessionOutcome::None };
	EDapProtocolCodecError codecError{ EDapProtocolCodecError::None };
	std::uint64_t nextOutgoingSequence{ 1 };
	bool transportClosed{};
	std::size_t droppedNotificationCount{};
	std::size_t droppedCompletedRequestCount{};
	std::vector<DapPendingRequestSnapshot> pendingRequests;
	std::vector<DapCompletedRequestSnapshot> completedRequests;
	std::vector<DapEvent> recentEvents;
};

enum class EDapSessionNotificationKind : std::uint8_t {
	RequestSent,
	ResponseReceived,
	ServerRequestReceived,
	EventReceived,
	RequestCancelled,
	RequestTimedOut,
	RequestRejected,
	LateResponseIgnored,
	Failed,
	Closed,
	Stopped,
};

//! Notification values are copies, safe to retain and valid after callbacks return.
struct DapSessionNotification final {
	EDapSessionNotificationKind kind{ EDapSessionNotificationKind::RequestSent };
	std::optional<std::uint64_t> requestSequence;
	std::optional<DapResponse> response;
	std::optional<DapRequest> serverRequest;
	std::optional<DapEvent> event;
	EDapSessionOutcome outcome{ EDapSessionOutcome::None };
	EDapProtocolCodecError codecError{ EDapProtocolCodecError::None };
};

using DapSessionSubscriptionId = std::uint64_t;
using DapSessionListener = std::function<void(const DapSessionNotification&)>;

struct DapSessionLimits final {
	std::size_t maximumPendingRequests{ 256 };
	std::size_t maximumCompletedRequests{ 1'024 };
	std::size_t maximumRecentEvents{ 512 };
	std::size_t maximumSubscriptions{ 128 };
	std::size_t maximumPendingNotifications{ 1'024 };
	//! Inclusive maximum allocated outgoing request sequence.  Lower values make exhaustion deterministic in tests.
	std::uint64_t maximumOutgoingSequence{ std::numeric_limits<std::uint64_t>::max() };
	DapProtocolCodecLimits codec;
};

struct DapSessionResult final {
	EDapSessionStatus status{ EDapSessionStatus::NotStarted };
	std::optional<std::uint64_t> requestSequence;
	//! True only when Close/Stop was called by the notification-dispatch thread itself.
	//! That caller owns completion of its current callback, so waiting would deadlock; an external Stop waits for it.
	bool callbackDrainDeferred{};

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EDapSessionStatus::Started || status == EDapSessionStatus::Sent || status == EDapSessionStatus::Received
			|| status == EDapSessionStatus::Cancelled || status == EDapSessionStatus::TimedOut || status == EDapSessionStatus::Closed
			|| status == EDapSessionStatus::Stopped;
	}
};

/*! 
 * @brief Thread-safe DAP session with explicit, caller-driven terminal state transitions.
 *
 * `Feed` may be called from a transport receive callback. `Expire` is deliberately
 * caller-driven so the owner controls time and scheduling. Listener callbacks occur
 * outside all session locks and exceptions from listeners are contained.
 */
class CDapSession final {
public:
	explicit CDapSession(IDapByteTransport& transport, DapSessionLimits limits = {});
	~CDapSession();
	CDapSession(const CDapSession&) = delete;
	CDapSession& operator=(const CDapSession&) = delete;

	[[nodiscard]] DapSessionResult Start();
	[[nodiscard]] DapSessionResult SendRequest(const DapSessionRequest& request);
	[[nodiscard]] DapSessionResult Feed(std::string_view bytes);
	[[nodiscard]] DapSessionResult Cancel(std::uint64_t requestSequence);
	//! Finalizes every request whose deadline is no later than callerSuppliedTime.
	[[nodiscard]] std::size_t Expire(std::uint64_t callerSuppliedTime);
	//! A transport owner uses this when its receive/send side reports a terminal error.
	[[nodiscard]] DapSessionResult NotifyTransportFailure();
	//! A callback-originated Close/Stop defers callback-drain completion to its safe outer dispatcher.
	//! Listeners borrow this session and must not destroy it from inside the callback.
	[[nodiscard]] DapSessionResult Close();
	[[nodiscard]] DapSessionResult Stop();

	[[nodiscard]] DapSessionSnapshot Snapshot() const;
	[[nodiscard]] std::optional<DapSessionSubscriptionId> Subscribe(DapSessionListener listener);
	void Unsubscribe(DapSessionSubscriptionId subscriptionId) noexcept;

private:
	struct PendingRequest final {
		std::string command;
		std::optional<std::uint64_t> deadline;
	};

	[[nodiscard]] bool IsRunningLocked() const noexcept;
	[[nodiscard]] DapSessionResult TerminalResultLocked() const noexcept;
	void EnqueueLocked(DapSessionNotification notification) noexcept;
	void RecordCompletionLocked(std::uint64_t sequence, const PendingRequest& request, EDapRequestCompletionKind kind,
		EDapSessionOutcome outcome = EDapSessionOutcome::None) noexcept;
	void FinalizePendingLocked(EDapSessionNotificationKind kind, EDapSessionOutcome outcome) noexcept;
	void FailLocked(EDapSessionOutcome outcome, EDapProtocolCodecError codecError = EDapProtocolCodecError::None) noexcept;
	[[nodiscard]] bool MarkTransportCloseNeededLocked() noexcept;
	void CloseTransportIfNeeded(bool closeNeeded) noexcept;
	//! Returns true only for a reentrant caller that is itself running a listener callback.
	[[nodiscard]] bool DrainNotifications() noexcept;
	//! An external owner waits until an already-running listener dispatch finishes.  The dispatching
	//! listener thread never waits on itself and receives callbackDrainDeferred instead.
	[[nodiscard]] bool WaitForNotificationDrain() noexcept;
	[[nodiscard]] bool IsNotificationDispatchThreadLocked() const noexcept;
	static void SaturatingIncrement(std::size_t& value) noexcept;

	IDapByteTransport& m_transport;
	DapSessionLimits m_limits;
	// Serializes the final Running check, physical Send, terminal transition,
	// and physical Close. Recursive ownership permits a transport callback to
	// close the session synchronously from inside Send without deadlocking.
	mutable std::recursive_mutex m_transportLifecycleMutex;
	mutable std::mutex m_mutex;
	CDapProtocolCodec m_codec;
	EDapSessionState m_state{ EDapSessionState::Idle };
	EDapSessionOutcome m_outcome{ EDapSessionOutcome::None };
	EDapProtocolCodecError m_codecError{ EDapProtocolCodecError::None };
	std::uint64_t m_nextOutgoingSequence{ 1 };
	std::map<std::uint64_t, PendingRequest> m_pendingRequests;
	std::deque<DapEvent> m_recentEvents;
	std::deque<DapCompletedRequestSnapshot> m_completedRequests;
	//! Only caller-cancelled/timed-out request IDs are temporarily tolerated if their adapter response races the caller.
	std::map<std::uint64_t, std::string> m_lateResponseTombstones;
	std::map<DapSessionSubscriptionId, DapSessionListener> m_listeners;
	DapSessionSubscriptionId m_nextSubscriptionId{ 1 };
	std::deque<DapSessionNotification> m_notifications;
	bool m_dispatchingNotifications{};
	std::thread::id m_notificationDispatchThreadId;
	std::condition_variable m_notificationDispatchChanged;
	bool m_clearListenersAfterDispatch{};
	bool m_transportClosed{};
	std::size_t m_droppedNotificationCount{};
	std::size_t m_droppedCompletedRequestCount{};
};

} // namespace debug::dap
