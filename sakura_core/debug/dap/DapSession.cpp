/*! @file @brief Bounded, transport-injected Debug Adapter Protocol session owner. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "debug/dap/DapSession.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace debug::dap {

CDapSession::CDapSession(IDapByteTransport& transport, DapSessionLimits limits)
	: m_transport(transport)
	, m_limits(limits)
	, m_codec(m_limits.codec)
{
	if (m_limits.maximumPendingRequests == 0) m_limits.maximumPendingRequests = 1;
	if (m_limits.maximumCompletedRequests == 0) m_limits.maximumCompletedRequests = 1;
	if (m_limits.maximumRecentEvents == 0) m_limits.maximumRecentEvents = 1;
	if (m_limits.maximumSubscriptions == 0) m_limits.maximumSubscriptions = 1;
	if (m_limits.maximumPendingNotifications == 0) m_limits.maximumPendingNotifications = 1;
	if (m_limits.maximumOutgoingSequence == 0) m_limits.maximumOutgoingSequence = 1;
}

CDapSession::~CDapSession()
{
	(void)Stop();
}

bool CDapSession::IsRunningLocked() const noexcept
{
	return m_state == EDapSessionState::Running;
}

DapSessionResult CDapSession::TerminalResultLocked() const noexcept
{
	if (m_state == EDapSessionState::Stopped) return { EDapSessionStatus::Stopped };
	if (m_state == EDapSessionState::Closed) return { EDapSessionStatus::Closed };
	if (m_state == EDapSessionState::Failed) {
		if (m_outcome == EDapSessionOutcome::CodecFailure) return { EDapSessionStatus::CodecFailed };
		if (m_outcome == EDapSessionOutcome::TransportFailure) return { EDapSessionStatus::TransportFailed };
		return { EDapSessionStatus::ProtocolViolation };
	}
	return { EDapSessionStatus::NotStarted };
}

void CDapSession::EnqueueLocked(DapSessionNotification notification) noexcept
{
	try {
		if (m_notifications.size() == m_limits.maximumPendingNotifications) {
			m_notifications.pop_front();
			SaturatingIncrement(m_droppedNotificationCount);
		}
		m_notifications.emplace_back(std::move(notification));
	} catch (...) {
		// Terminal transitions, including destruction, must not terminate because a
		// best-effort observer record could not be allocated.
		SaturatingIncrement(m_droppedNotificationCount);
	}
}

void CDapSession::RecordCompletionLocked(const std::uint64_t sequence, const PendingRequest& request,
	const EDapRequestCompletionKind kind, const EDapSessionOutcome outcome) noexcept
{
	try {
		if (m_completedRequests.size() == m_limits.maximumCompletedRequests) {
			const auto& evicted = m_completedRequests.front();
			if (evicted.kind == EDapRequestCompletionKind::Cancelled || evicted.kind == EDapRequestCompletionKind::TimedOut) {
				m_lateResponseTombstones.erase(evicted.sequence);
			}
			m_completedRequests.pop_front();
			SaturatingIncrement(m_droppedCompletedRequestCount);
		}
		m_completedRequests.emplace_back(sequence, request.command, kind, outcome);
		if (kind == EDapRequestCompletionKind::Cancelled || kind == EDapRequestCompletionKind::TimedOut) {
			// Missing a tombstone only means a racing late response is rejected as a
			// protocol violation; it can never resurrect a finalized request.
			try { m_lateResponseTombstones.insert_or_assign(sequence, request.command); }
			catch (...) { }
		}
	} catch (...) {
		SaturatingIncrement(m_droppedCompletedRequestCount);
	}
}

void CDapSession::SaturatingIncrement(std::size_t& value) noexcept
{
	if (value != std::numeric_limits<std::size_t>::max()) ++value;
}

void CDapSession::FinalizePendingLocked(const EDapSessionNotificationKind kind, const EDapSessionOutcome outcome) noexcept
{
	for (const auto& [sequence, pending] : m_pendingRequests) {
		RecordCompletionLocked(sequence, pending, EDapRequestCompletionKind::Rejected, outcome);
		EnqueueLocked({ .kind = kind, .requestSequence = sequence, .outcome = outcome, .codecError = m_codecError });
	}
	m_pendingRequests.clear();
}

void CDapSession::FailLocked(const EDapSessionOutcome outcome, const EDapProtocolCodecError codecError) noexcept
{
	if (!IsRunningLocked()) return;
	m_state = EDapSessionState::Failed;
	m_outcome = outcome;
	m_codecError = codecError;
	FinalizePendingLocked(EDapSessionNotificationKind::RequestRejected, outcome);
	EnqueueLocked({ .kind = EDapSessionNotificationKind::Failed, .outcome = outcome, .codecError = codecError });
}

bool CDapSession::MarkTransportCloseNeededLocked() noexcept
{
	if (m_transportClosed) return false;
	m_transportClosed = true;
	return true;
}

void CDapSession::CloseTransportIfNeeded(const bool closeNeeded) noexcept
{
	if (closeNeeded) {
		std::lock_guard transportLock(m_transportLifecycleMutex);
		try { m_transport.Close(); }
		catch (...) { /* external transport is not permitted to break lifecycle finalization */ }
	}
}

bool CDapSession::IsNotificationDispatchThreadLocked() const noexcept
{
	return m_dispatchingNotifications && m_notificationDispatchThreadId == std::this_thread::get_id();
}

bool CDapSession::DrainNotifications() noexcept
{
	std::unique_lock lock(m_mutex);
	if (m_dispatchingNotifications) return IsNotificationDispatchThreadLocked();
	m_dispatchingNotifications = true;
	m_notificationDispatchThreadId = std::this_thread::get_id();
	while (!m_notifications.empty()) {
		auto notification = std::move(m_notifications.front());
		m_notifications.pop_front();
		std::vector<DapSessionListener> listeners;
		try {
			listeners.reserve(m_listeners.size());
			for (const auto& [id, listener] : m_listeners) {
				(void)id;
				listeners.emplace_back(listener);
			}
		} catch (...) {
			// Listener copying is an observer concern; the committed state transition
			// and its terminal cleanup still have to finish.
			listeners.clear();
		}
		lock.unlock();
		for (const auto& listener : listeners) {
			try { listener(notification); }
			catch (...) { /* Extension/native callbacks are fault-contained at the session edge. */ }
		}
		lock.lock();
	}
	m_dispatchingNotifications = false;
	m_notificationDispatchThreadId = {};
	if (m_clearListenersAfterDispatch) {
		m_listeners.clear();
		m_clearListenersAfterDispatch = false;
	}
	m_notificationDispatchChanged.notify_all();
	return false;
}

bool CDapSession::WaitForNotificationDrain() noexcept
{
	std::unique_lock lock(m_mutex);
	if (IsNotificationDispatchThreadLocked()) return true;
	try {
		m_notificationDispatchChanged.wait(lock, [this] { return !m_dispatchingNotifications; });
	} catch (...) {
		// std::condition_variable::wait only throws for an OS synchronization
		// failure. Preserve destruction safety; the terminal state remains committed.
		return m_dispatchingNotifications;
	}
	return false;
}

DapSessionResult CDapSession::Start()
{
	{
		std::lock_guard lock(m_mutex);
		if (m_state == EDapSessionState::Idle) {
			m_state = EDapSessionState::Running;
			return { EDapSessionStatus::Started };
		}
		if (m_state == EDapSessionState::Running) return { EDapSessionStatus::AlreadyStarted };
		return TerminalResultLocked();
	}
}

DapSessionResult CDapSession::SendRequest(const DapSessionRequest& request)
{
	if (request.command.empty() || request.command.size() > 1'024 || (request.argumentsJson && request.argumentsJson->size() > m_limits.codec.maximumBodyBytes)) {
		return { EDapSessionStatus::InvalidRequest };
	}

	DapRequest outbound;
	{
		std::lock_guard lock(m_mutex);
		if (!IsRunningLocked()) return TerminalResultLocked();
		if (m_nextOutgoingSequence == 0 || m_nextOutgoingSequence > m_limits.maximumOutgoingSequence) {
			return { EDapSessionStatus::OutgoingSequenceExhausted };
		}
		if (m_pendingRequests.size() >= m_limits.maximumPendingRequests) return { EDapSessionStatus::PendingRequestLimitExceeded };
		try {
			outbound.seq = m_nextOutgoingSequence;
			outbound.command = request.command;
			outbound.argumentsJson = request.argumentsJson;
			m_pendingRequests.emplace(outbound.seq, PendingRequest{ request.command, request.deadline });
			if (m_nextOutgoingSequence == m_limits.maximumOutgoingSequence) m_nextOutgoingSequence = 0;
			else ++m_nextOutgoingSequence;
		} catch (...) {
			return { EDapSessionStatus::ResourceExhausted };
		}
	}

	std::string frame;
	DapProtocolCodecResult encoded;
	{
		std::lock_guard lock(m_mutex);
		if (!IsRunningLocked()) {
			auto terminal = TerminalResultLocked();
			terminal.requestSequence = outbound.seq;
			return terminal;
		}
		try { encoded = m_codec.Encode(outbound, frame); }
		catch (...) { encoded = { EDapProtocolCodecStatus::Failed, EDapProtocolCodecError::InvalidJson }; }
	}
	if (!encoded.Succeeded()) {
		bool closeNeeded = false;
		DapSessionResult failure{ EDapSessionStatus::CodecFailed, outbound.seq };
		{
			std::lock_guard transportLock(m_transportLifecycleMutex);
			{
				std::lock_guard lock(m_mutex);
				if (IsRunningLocked()) { FailLocked(EDapSessionOutcome::CodecFailure, encoded.error); closeNeeded = MarkTransportCloseNeededLocked(); }
				else { failure = TerminalResultLocked(); failure.requestSequence = outbound.seq; }
			}
			CloseTransportIfNeeded(closeNeeded);
		}
		(void)DrainNotifications();
		return failure;
	}

	DapSessionResult sendResult{ EDapSessionStatus::Sent, outbound.seq };
	{
		std::lock_guard transportLock(m_transportLifecycleMutex);
		{
			std::lock_guard lock(m_mutex);
			if (!IsRunningLocked()) {
				sendResult = TerminalResultLocked();
				sendResult.requestSequence = outbound.seq;
				return sendResult;
			}
		}

		bool sent = false;
		try { sent = m_transport.Send(frame); }
		catch (...) { sent = false; }
		if (!sent) {
			bool closeNeeded = false;
			sendResult = { EDapSessionStatus::TransportFailed, outbound.seq };
			{
				std::lock_guard lock(m_mutex);
				if (IsRunningLocked()) {
					FailLocked(EDapSessionOutcome::TransportFailure);
					closeNeeded = MarkTransportCloseNeededLocked();
				} else {
					sendResult = TerminalResultLocked();
					sendResult.requestSequence = outbound.seq;
				}
			}
			CloseTransportIfNeeded(closeNeeded);
		} else {
			std::lock_guard lock(m_mutex);
			if (!IsRunningLocked()) {
				sendResult = TerminalResultLocked();
				sendResult.requestSequence = outbound.seq;
			} else {
				EnqueueLocked({ .kind = EDapSessionNotificationKind::RequestSent, .requestSequence = outbound.seq });
			}
		}
	}
	(void)DrainNotifications();
	return sendResult;
}

DapSessionResult CDapSession::Feed(const std::string_view bytes)
{
	std::vector<DapMessage> messages;
	bool closeNeeded = false;
	DapSessionResult result{ EDapSessionStatus::Received };
	{
		std::lock_guard transportLock(m_transportLifecycleMutex);
		{
			std::lock_guard lock(m_mutex);
			if (!IsRunningLocked()) return TerminalResultLocked();
			const auto fed = m_codec.Feed(bytes, messages);
			if (!fed.Succeeded()) {
				FailLocked(EDapSessionOutcome::CodecFailure, fed.error);
				closeNeeded = MarkTransportCloseNeededLocked();
				result.status = EDapSessionStatus::CodecFailed;
			} else {
				for (const auto& message : messages) {
					if (const auto* response = std::get_if<DapResponse>(&message)) {
						const auto found = m_pendingRequests.find(response->requestSeq);
						if (found == m_pendingRequests.end()) {
							const auto retired = m_lateResponseTombstones.find(response->requestSeq);
							if (retired != m_lateResponseTombstones.end() && retired->second == response->command) {
								m_lateResponseTombstones.erase(retired);
								EnqueueLocked({ .kind = EDapSessionNotificationKind::LateResponseIgnored, .requestSequence = response->requestSeq, .response = *response });
								continue;
							}
							FailLocked(EDapSessionOutcome::ProtocolViolation);
							closeNeeded = MarkTransportCloseNeededLocked();
							result.status = EDapSessionStatus::ProtocolViolation;
							break;
						}
						if (found->second.command != response->command) {
							FailLocked(EDapSessionOutcome::ProtocolViolation);
							closeNeeded = MarkTransportCloseNeededLocked();
							result.status = EDapSessionStatus::ProtocolViolation;
							break;
						}
						RecordCompletionLocked(response->requestSeq, found->second, EDapRequestCompletionKind::Responded);
						m_pendingRequests.erase(found);
						EnqueueLocked({ .kind = EDapSessionNotificationKind::ResponseReceived, .requestSequence = response->requestSeq, .response = *response });
					} else if (const auto* request = std::get_if<DapRequest>(&message)) {
						EnqueueLocked({ .kind = EDapSessionNotificationKind::ServerRequestReceived, .requestSequence = request->seq, .serverRequest = *request });
					} else {
						const auto& event = std::get<DapEvent>(message);
						if (m_recentEvents.size() == m_limits.maximumRecentEvents) m_recentEvents.pop_front();
						m_recentEvents.emplace_back(event);
						EnqueueLocked({ .kind = EDapSessionNotificationKind::EventReceived, .event = event });
					}
				}
			}
		}
		CloseTransportIfNeeded(closeNeeded);
	}
	(void)DrainNotifications();
	return result;
}

DapSessionResult CDapSession::Cancel(const std::uint64_t requestSequence)
{
	{
		std::lock_guard lock(m_mutex);
		if (!IsRunningLocked()) return TerminalResultLocked();
		const auto found = m_pendingRequests.find(requestSequence);
		if (found == m_pendingRequests.end()) return { EDapSessionStatus::UnknownRequest, requestSequence };
		RecordCompletionLocked(requestSequence, found->second, EDapRequestCompletionKind::Cancelled);
		m_pendingRequests.erase(found);
		EnqueueLocked({ .kind = EDapSessionNotificationKind::RequestCancelled, .requestSequence = requestSequence });
	}
	(void)DrainNotifications();
	return { EDapSessionStatus::Cancelled, requestSequence };
}

std::size_t CDapSession::Expire(const std::uint64_t callerSuppliedTime)
{
	std::size_t expired = 0;
	{
		std::lock_guard lock(m_mutex);
		if (!IsRunningLocked()) return 0;
		for (auto it = m_pendingRequests.begin(); it != m_pendingRequests.end();) {
			if (it->second.deadline && *it->second.deadline <= callerSuppliedTime) {
				RecordCompletionLocked(it->first, it->second, EDapRequestCompletionKind::TimedOut);
				EnqueueLocked({ .kind = EDapSessionNotificationKind::RequestTimedOut, .requestSequence = it->first });
				it = m_pendingRequests.erase(it);
				++expired;
			} else ++it;
		}
	}
	(void)DrainNotifications();
	return expired;
}

DapSessionResult CDapSession::NotifyTransportFailure()
{
	bool closeNeeded = false;
	{
		std::lock_guard transportLock(m_transportLifecycleMutex);
		{
			std::lock_guard lock(m_mutex);
			if (!IsRunningLocked()) return TerminalResultLocked();
			FailLocked(EDapSessionOutcome::TransportFailure);
			closeNeeded = MarkTransportCloseNeededLocked();
		}
		CloseTransportIfNeeded(closeNeeded);
	}
	(void)DrainNotifications();
	return { EDapSessionStatus::TransportFailed };
}

DapSessionResult CDapSession::Close()
{
	bool closeNeeded = false;
	DapSessionResult result{ EDapSessionStatus::Closed };
	{
		std::lock_guard transportLock(m_transportLifecycleMutex);
		{
			std::lock_guard lock(m_mutex);
			if (!IsRunningLocked()) {
				result = TerminalResultLocked();
			} else {
				m_state = EDapSessionState::Closed;
				m_outcome = EDapSessionOutcome::ClosedByCaller;
				m_codec.Stop();
				FinalizePendingLocked(EDapSessionNotificationKind::RequestRejected, m_outcome);
				EnqueueLocked({ .kind = EDapSessionNotificationKind::Closed, .outcome = m_outcome });
				closeNeeded = MarkTransportCloseNeededLocked();
			}
		}
		CloseTransportIfNeeded(closeNeeded);
	}
	result.callbackDrainDeferred = DrainNotifications();
	if (!result.callbackDrainDeferred) result.callbackDrainDeferred = WaitForNotificationDrain();
	return result;
}

DapSessionResult CDapSession::Stop()
{
	bool closeNeeded = false;
	{
		std::lock_guard transportLock(m_transportLifecycleMutex);
		{
			std::lock_guard lock(m_mutex);
			if (m_state != EDapSessionState::Stopped) {
				m_state = EDapSessionState::Stopped;
				m_outcome = EDapSessionOutcome::StoppedByOwner;
				m_codec.Stop();
				FinalizePendingLocked(EDapSessionNotificationKind::RequestRejected, m_outcome);
				EnqueueLocked({ .kind = EDapSessionNotificationKind::Stopped, .outcome = m_outcome });
				m_clearListenersAfterDispatch = true;
				closeNeeded = MarkTransportCloseNeededLocked();
			}
		}
		CloseTransportIfNeeded(closeNeeded);
	}
	DapSessionResult result{ EDapSessionStatus::Stopped };
	result.callbackDrainDeferred = DrainNotifications();
	if (!result.callbackDrainDeferred) result.callbackDrainDeferred = WaitForNotificationDrain();
	return result;
}

DapSessionSnapshot CDapSession::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	DapSessionSnapshot snapshot{ .state = m_state, .outcome = m_outcome, .codecError = m_codecError, .nextOutgoingSequence = m_nextOutgoingSequence,
		.transportClosed = m_transportClosed, .droppedNotificationCount = m_droppedNotificationCount,
		.droppedCompletedRequestCount = m_droppedCompletedRequestCount };
	snapshot.pendingRequests.reserve(m_pendingRequests.size());
	for (const auto& [sequence, request] : m_pendingRequests) snapshot.pendingRequests.emplace_back(sequence, request.command, request.deadline);
	snapshot.completedRequests.assign(m_completedRequests.begin(), m_completedRequests.end());
	snapshot.recentEvents.assign(m_recentEvents.begin(), m_recentEvents.end());
	return snapshot;
}

std::optional<DapSessionSubscriptionId> CDapSession::Subscribe(DapSessionListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_mutex);
	if (!IsRunningLocked() || m_listeners.size() >= m_limits.maximumSubscriptions || m_nextSubscriptionId == 0) return std::nullopt;
	const auto id = m_nextSubscriptionId++;
	m_listeners.emplace(id, std::move(listener));
	return id;
}

void CDapSession::Unsubscribe(const DapSessionSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_mutex);
	m_listeners.erase(subscriptionId);
}

} // namespace debug::dap
