/*! @file @brief Bounded, transport-free state model for a Debug Console. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "debug/console/DebugConsoleModel.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace debug::console {

namespace {

template <typename TValue>
constexpr TValue NonZeroLimit(const TValue value) noexcept
{
	return value == 0 ? TValue{ 1 } : value;
}

void SaturatingIncrement(std::size_t& value) noexcept
{
	if (value != (std::numeric_limits<std::size_t>::max)()) ++value;
}

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80U) {
			if (first == 0 || (!permitControls && (first < 0x20U || first == 0x7fU))) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2U && first <= 0xdfU) { continuationCount = 1; codePoint = first & 0x1fU; }
		else if (first >= 0xe0U && first <= 0xefU) { continuationCount = 2; codePoint = first & 0x0fU; }
		else if (first >= 0xf0U && first <= 0xf4U) { continuationCount = 3; codePoint = first & 0x07U; }
		else return false;
		if (continuationCount >= value.size() - index) return false;
		for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
			const auto continuation = static_cast<unsigned char>(value[index + offset]);
			if ((continuation & 0xc0U) != 0x80U) return false;
			codePoint = (codePoint << 6U) | (continuation & 0x3fU);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffffU || (codePoint >= 0xd800U && codePoint <= 0xdfffU)
			|| (!permitControls && codePoint >= 0x80U && codePoint <= 0x9fU)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidBoundedText(const std::string_view value, const std::size_t maximumBytes, const bool permitControls) noexcept
{
	return value.size() <= maximumBytes && IsValidUtf8(value, permitControls);
}

bool IsValidOutputCategory(const EDebugConsoleOutputCategory category) noexcept
{
	switch (category) {
	case EDebugConsoleOutputCategory::Console:
	case EDebugConsoleOutputCategory::Stdout:
	case EDebugConsoleOutputCategory::Stderr:
	case EDebugConsoleOutputCategory::Telemetry:
	case EDebugConsoleOutputCategory::Important:
		return true;
	}
	return false;
}

bool IsValidEvaluationTerminal(const EDebugConsoleEvaluationTerminal terminal) noexcept
{
	switch (terminal) {
	case EDebugConsoleEvaluationTerminal::Completed:
	case EDebugConsoleEvaluationTerminal::Failed:
	case EDebugConsoleEvaluationTerminal::Cancelled:
	case EDebugConsoleEvaluationTerminal::Expired:
		return true;
	}
	return false;
}

bool IsValidNotificationKind(const EDebugConsoleNotificationKind kind) noexcept
{
	switch (kind) {
	case EDebugConsoleNotificationKind::SessionStarted:
	case EDebugConsoleNotificationKind::OutputAppended:
	case EDebugConsoleNotificationKind::ReplSubmitted:
	case EDebugConsoleNotificationKind::EvaluationCompleted:
	case EDebugConsoleNotificationKind::EvaluationFailed:
	case EDebugConsoleNotificationKind::EvaluationCancelled:
	case EDebugConsoleNotificationKind::EvaluationExpired:
	case EDebugConsoleNotificationKind::SessionDisposed:
	case EDebugConsoleNotificationKind::Stopped:
		return true;
	}
	return false;
}

bool IsSequenceAvailable(const std::uint64_t next, const std::uint64_t maximum) noexcept
{
	return next != 0 && next <= maximum;
}

void ConsumeSequence(std::uint64_t& next, const std::uint64_t maximum) noexcept
{
	next = next == maximum ? 0 : next + 1;
}

EDebugConsoleNotificationKind NotificationFor(const EDebugConsoleEvaluationTerminal terminal) noexcept
{
	switch (terminal) {
	case EDebugConsoleEvaluationTerminal::Completed: return EDebugConsoleNotificationKind::EvaluationCompleted;
	case EDebugConsoleEvaluationTerminal::Failed: return EDebugConsoleNotificationKind::EvaluationFailed;
	case EDebugConsoleEvaluationTerminal::Cancelled: return EDebugConsoleNotificationKind::EvaluationCancelled;
	case EDebugConsoleEvaluationTerminal::Expired: return EDebugConsoleNotificationKind::EvaluationExpired;
	}
	return EDebugConsoleNotificationKind::EvaluationCancelled;
}

} // namespace

CDebugConsoleModel::CDebugConsoleModel(DebugConsoleLimits limits)
	: m_limits(std::move(limits))
{
	m_limits.maximumTranscriptEntries = NonZeroLimit(m_limits.maximumTranscriptEntries);
	m_limits.maximumHistoryEntries = NonZeroLimit(m_limits.maximumHistoryEntries);
	m_limits.maximumPendingEvaluations = NonZeroLimit(m_limits.maximumPendingEvaluations);
	m_limits.maximumCompletedEvaluations = NonZeroLimit(m_limits.maximumCompletedEvaluations);
	m_limits.maximumPendingNotifications = NonZeroLimit(m_limits.maximumPendingNotifications);
	m_limits.maximumSubscriptions = NonZeroLimit(m_limits.maximumSubscriptions);
	m_limits.maximumOperationIdLength = NonZeroLimit(m_limits.maximumOperationIdLength);
	m_limits.maximumExpressionLength = NonZeroLimit(m_limits.maximumExpressionLength);
	m_limits.maximumOutputTextLength = NonZeroLimit(m_limits.maximumOutputTextLength);
	m_limits.maximumCompletionDetailLength = NonZeroLimit(m_limits.maximumCompletionDetailLength);
	m_limits.maximumEvaluationRequestId = NonZeroLimit(m_limits.maximumEvaluationRequestId);
	m_limits.maximumTranscriptSequence = NonZeroLimit(m_limits.maximumTranscriptSequence);
	m_limits.maximumHistorySequence = NonZeroLimit(m_limits.maximumHistorySequence);
}

CDebugConsoleModel::~CDebugConsoleModel()
{
	try { (void)Stop(); }
	catch (...) { /* Destruction cannot propagate an allocation failure from best-effort finalization. */ }
}

DebugConsoleModelResult CDebugConsoleModel::SessionStatusLocked(const DebugConsoleSessionGeneration generation) const noexcept
{
	if (generation == 0) return { EDebugConsoleModelStatus::InvalidArgument };
	if (m_stopped) return { EDebugConsoleModelStatus::Stopped };
	if (!m_activeSessionGeneration) return { generation <= m_newestSessionGeneration ? EDebugConsoleModelStatus::SessionDisposed : EDebugConsoleModelStatus::NoActiveSession };
	return generation == *m_activeSessionGeneration ? DebugConsoleModelResult{ EDebugConsoleModelStatus::Accepted }
		: DebugConsoleModelResult{ EDebugConsoleModelStatus::StaleGeneration };
}

bool CDebugConsoleModel::IsValidOperationLocked(const std::string& operationId, const std::string& expression) const noexcept
{
	return !operationId.empty() && IsValidBoundedText(operationId, m_limits.maximumOperationIdLength, false)
		&& !expression.empty() && IsValidBoundedText(expression, m_limits.maximumExpressionLength, true);
}

void CDebugConsoleModel::EnqueueLocked(DebugConsoleNotification notification) noexcept
{
	if (!IsValidNotificationKind(notification.kind) || (notification.terminal && !IsValidEvaluationTerminal(*notification.terminal))) {
		SaturatingIncrement(m_droppedNotificationCount);
		return;
	}
	if (m_notifications.size() == m_limits.maximumPendingNotifications) {
		m_notifications.pop_front();
		SaturatingIncrement(m_droppedNotificationCount);
	}
	try {
		m_notifications.emplace_back(std::move(notification));
	} catch (...) {
		// If an allocation fails after evicting the oldest queued notification, both the old and the new
		// notification are accounted for.  State mutations remain committed and their loss is observable.
		SaturatingIncrement(m_droppedNotificationCount);
	}
}

void CDebugConsoleModel::RecordCompletionLocked(const DebugConsoleEvaluationRequestId requestId, const PendingEvaluation& evaluation,
	const EDebugConsoleEvaluationTerminal terminal, std::string detail)
{
	if (!IsValidEvaluationTerminal(terminal) || !m_activeSessionGeneration) return;
	DebugConsoleCompletedEvaluation completed{
		.sessionGeneration = *m_activeSessionGeneration,
		.requestId = requestId,
		.operationId = evaluation.operationId,
		.expression = evaluation.expression,
		.terminal = terminal,
		.detail = std::move(detail),
	};
	// Push before evicting so allocation failure preserves both retained completion records and operation replay state.
	m_completedEvaluations.push_back(std::move(completed));
	if (m_completedEvaluations.size() > m_limits.maximumCompletedEvaluations) {
		const auto& evicted = m_completedEvaluations.front();
		const auto operation = m_operations.find(evicted.operationId);
		if (operation != m_operations.end() && operation->second.generation == evicted.sessionGeneration
			&& operation->second.requestId == evicted.requestId) {
			m_operations.erase(operation);
		}
		m_completedEvaluations.pop_front();
		SaturatingIncrement(m_droppedCompletedEvaluationCount);
	}
	EnqueueLocked({ .kind = NotificationFor(terminal), .sessionGeneration = *m_activeSessionGeneration, .requestId = requestId, .terminal = terminal });
}

void CDebugConsoleModel::FinalizeAllPendingLocked(const EDebugConsoleEvaluationTerminal terminal, const std::string& detail)
{
	for (const auto& [requestId, evaluation] : m_pendingEvaluations) {
		RecordCompletionLocked(requestId, evaluation, terminal, detail);
	}
	m_pendingEvaluations.clear();
}

void CDebugConsoleModel::CompleteStopFinalizationLocked() noexcept
{
	if (!m_stopFinalizationInProgress || !m_stopped || m_dispatchingNotifications) return;
	m_stopFinalizationInProgress = false;
	m_stopFinalizationCompleted = true;
	m_stopCompletionCondition.notify_all();
}

void CDebugConsoleModel::DrainNotifications()
{
	std::unique_lock lock(m_mutex);
	if (m_dispatchingNotifications) return;
	m_dispatchingNotifications = true;
	m_dispatchThreadId = std::this_thread::get_id();
	while (!m_notifications.empty()) {
		auto notification = std::move(m_notifications.front());
		m_notifications.pop_front();
		std::vector<DebugConsoleListener> listeners;
		try {
			listeners.reserve(m_listeners.size());
			for (const auto& [id, listener] : m_listeners) {
				(void)id;
				listeners.push_back(listener);
			}
		} catch (...) {
			SaturatingIncrement(m_droppedNotificationCount);
			continue;
		}
		lock.unlock();
		for (const auto& listener : listeners) {
			try { listener(notification); }
			catch (...) { /* A host/UI listener cannot compromise model finalization. */ }
		}
		lock.lock();
	}
	m_dispatchingNotifications = false;
	m_dispatchThreadId = {};
	if (m_clearListenersAfterDispatch) {
		m_listeners.clear();
		m_clearListenersAfterDispatch = false;
	}
	CompleteStopFinalizationLocked();
	lock.unlock();
}

DebugConsoleModelResult CDebugConsoleModel::StartSession(const DebugConsoleSessionGeneration generation)
{
	if (generation == 0) return { EDebugConsoleModelStatus::InvalidArgument };
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return { EDebugConsoleModelStatus::Stopped };
		if (m_activeSessionGeneration) {
			return { *m_activeSessionGeneration == generation ? EDebugConsoleModelStatus::SessionActive : EDebugConsoleModelStatus::StaleGeneration };
		}
		if (generation <= m_newestSessionGeneration) return { EDebugConsoleModelStatus::StaleGeneration };
		m_newestSessionGeneration = generation;
		m_activeSessionGeneration = generation;
		EnqueueLocked({ .kind = EDebugConsoleNotificationKind::SessionStarted, .sessionGeneration = generation });
	}
	DrainNotifications();
	return { EDebugConsoleModelStatus::SessionStarted };
}

DebugConsoleModelResult CDebugConsoleModel::AppendOutput(const DebugConsoleSessionGeneration generation,
	const EDebugConsoleOutputCategory category, std::string text)
{
	if (!IsValidOutputCategory(category) || !IsValidBoundedText(text, m_limits.maximumOutputTextLength, true)) return { EDebugConsoleModelStatus::InvalidArgument };
	try {
		DebugConsoleOutput output{ .sessionGeneration = generation, .category = category, .text = std::move(text) };
		{
			std::lock_guard lock(m_mutex);
			auto status = SessionStatusLocked(generation);
			if (status.status != EDebugConsoleModelStatus::Accepted) return status;
			if (!IsSequenceAvailable(m_nextTranscriptSequence, m_limits.maximumTranscriptSequence)) return { EDebugConsoleModelStatus::SequenceExhausted };
			output.sequence = m_nextTranscriptSequence;
			// Push before evicting.  An allocation failure leaves the bounded transcript completely unchanged.
			m_transcript.push_back(std::move(output));
			if (m_transcript.size() > m_limits.maximumTranscriptEntries) {
				m_transcript.pop_front();
				SaturatingIncrement(m_droppedTranscriptEntryCount);
			}
			ConsumeSequence(m_nextTranscriptSequence, m_limits.maximumTranscriptSequence);
			EnqueueLocked({ .kind = EDebugConsoleNotificationKind::OutputAppended, .sessionGeneration = generation,
				.transcriptSequence = m_transcript.back().sequence });
		}
	} catch (...) {
		return { EDebugConsoleModelStatus::ResourceExhausted };
	}
	DrainNotifications();
	return { EDebugConsoleModelStatus::Accepted };
}

DebugConsoleModelResult CDebugConsoleModel::SubmitReplInput(const DebugConsoleSessionGeneration generation, std::string operationId,
	std::string expression, const std::optional<std::uint64_t> deadline)
{
	DebugConsoleEvaluationRequestId requestId{};
	try {
		std::lock_guard lock(m_mutex);
		if (!IsValidOperationLocked(operationId, expression)) return { EDebugConsoleModelStatus::InvalidArgument };
		auto status = SessionStatusLocked(generation);
		if (status.status != EDebugConsoleModelStatus::Accepted) return status;
		if (const auto existing = m_operations.find(operationId); existing != m_operations.end()) {
			if (existing->second.generation == generation && existing->second.expression == expression && existing->second.deadline == deadline) {
				return { EDebugConsoleModelStatus::Replayed, existing->second.requestId };
			}
			return { EDebugConsoleModelStatus::Conflict };
		}
		if (m_pendingEvaluations.size() == m_limits.maximumPendingEvaluations) {
			return { EDebugConsoleModelStatus::PendingLimitExceeded };
		}
		if (!IsSequenceAvailable(m_nextEvaluationRequestId, m_limits.maximumEvaluationRequestId)
			|| !IsSequenceAvailable(m_nextHistorySequence, m_limits.maximumHistorySequence)) return { EDebugConsoleModelStatus::SequenceExhausted };
		requestId = m_nextEvaluationRequestId;
		const auto historySequence = m_nextHistorySequence;
		// Allocate all value payloads before touching a container.  Every following failure rolls back
		// the insertion it owns, so resource exhaustion leaves the externally visible model unchanged.
		PendingEvaluation pending{ operationId, expression, deadline };
		OperationRecord operation{ generation, requestId, expression, deadline };
		DebugConsoleReplHistoryEntry history{ historySequence, generation, expression };
		m_history.push_back(std::move(history));
		try {
			const auto [pendingIt, pendingInserted] = m_pendingEvaluations.emplace(requestId, std::move(pending));
			if (!pendingInserted) {
				m_history.pop_back();
				return { EDebugConsoleModelStatus::ResourceExhausted };
			}
			try {
				const auto [operationIt, operationInserted] = m_operations.emplace(operationId, std::move(operation));
				(void)operationIt;
				if (!operationInserted) {
					m_pendingEvaluations.erase(pendingIt);
					m_history.pop_back();
					return { EDebugConsoleModelStatus::ResourceExhausted };
				}
			} catch (...) {
				m_pendingEvaluations.erase(pendingIt);
				m_history.pop_back();
				throw;
			}
		} catch (...) {
			if (!m_history.empty() && m_history.back().sequence == historySequence) m_history.pop_back();
			throw;
		}
		if (m_history.size() > m_limits.maximumHistoryEntries) {
			m_history.pop_front();
			SaturatingIncrement(m_droppedHistoryEntryCount);
		}
		ConsumeSequence(m_nextEvaluationRequestId, m_limits.maximumEvaluationRequestId);
		ConsumeSequence(m_nextHistorySequence, m_limits.maximumHistorySequence);
		EnqueueLocked({ .kind = EDebugConsoleNotificationKind::ReplSubmitted, .sessionGeneration = generation, .requestId = requestId });
	} catch (...) {
		return { EDebugConsoleModelStatus::ResourceExhausted };
	}
	DrainNotifications();
	return { EDebugConsoleModelStatus::Accepted, requestId };
}

DebugConsoleModelResult CDebugConsoleModel::FinalizeEvaluation(const DebugConsoleSessionGeneration generation,
	const DebugConsoleEvaluationRequestId requestId, const EDebugConsoleEvaluationTerminal terminal, std::string detail)
{
	if (!IsValidEvaluationTerminal(terminal) || !IsValidBoundedText(detail, m_limits.maximumCompletionDetailLength, true)) return { EDebugConsoleModelStatus::InvalidArgument };
	{
		std::lock_guard lock(m_mutex);
		auto status = SessionStatusLocked(generation);
		if (status.status != EDebugConsoleModelStatus::Accepted) return status;
		const auto evaluation = m_pendingEvaluations.find(requestId);
		if (evaluation == m_pendingEvaluations.end()) {
			const auto completed = std::find_if(m_completedEvaluations.begin(), m_completedEvaluations.end(),
				[requestId](const DebugConsoleCompletedEvaluation& item) { return item.requestId == requestId; });
			return { completed == m_completedEvaluations.end() ? EDebugConsoleModelStatus::UnknownRequest : EDebugConsoleModelStatus::AlreadyTerminal };
		}
		try {
			RecordCompletionLocked(requestId, evaluation->second, terminal, std::move(detail));
		} catch (...) {
			return { EDebugConsoleModelStatus::ResourceExhausted };
		}
		m_pendingEvaluations.erase(evaluation);
	}
	DrainNotifications();
	return { EDebugConsoleModelStatus::Accepted, requestId };
}

DebugConsoleModelResult CDebugConsoleModel::CompleteEvaluation(const DebugConsoleSessionGeneration generation,
	const DebugConsoleEvaluationRequestId requestId, std::string detail)
{
	return FinalizeEvaluation(generation, requestId, EDebugConsoleEvaluationTerminal::Completed, std::move(detail));
}

DebugConsoleModelResult CDebugConsoleModel::FailEvaluation(const DebugConsoleSessionGeneration generation,
	const DebugConsoleEvaluationRequestId requestId, std::string detail)
{
	return FinalizeEvaluation(generation, requestId, EDebugConsoleEvaluationTerminal::Failed, std::move(detail));
}

DebugConsoleModelResult CDebugConsoleModel::CancelEvaluation(const DebugConsoleSessionGeneration generation,
	const DebugConsoleEvaluationRequestId requestId, std::string detail)
{
	return FinalizeEvaluation(generation, requestId, EDebugConsoleEvaluationTerminal::Cancelled, std::move(detail));
}

DebugConsoleExpiryResult CDebugConsoleModel::ExpireEvaluations(const DebugConsoleSessionGeneration generation, const std::uint64_t callerSuppliedTime,
	std::string detail)
{
	if (!IsValidBoundedText(detail, m_limits.maximumCompletionDetailLength, true)) return { EDebugConsoleModelStatus::InvalidArgument };
	DebugConsoleExpiryResult result{ EDebugConsoleModelStatus::Accepted };
	{
		std::lock_guard lock(m_mutex);
		const auto sessionStatus = SessionStatusLocked(generation);
		if (sessionStatus.status != EDebugConsoleModelStatus::Accepted) return { sessionStatus.status };
		for (auto it = m_pendingEvaluations.begin(); it != m_pendingEvaluations.end();) {
			if (!it->second.deadline || *it->second.deadline > callerSuppliedTime) {
				++it;
				continue;
			}
			try {
				RecordCompletionLocked(it->first, it->second, EDebugConsoleEvaluationTerminal::Expired, detail);
			} catch (...) {
				result.status = EDebugConsoleModelStatus::ResourceExhausted;
				break;
			}
			it = m_pendingEvaluations.erase(it);
			++result.expiredCount;
		}
	}
	DrainNotifications();
	return result;
}

DebugConsoleModelResult CDebugConsoleModel::DisposeSession(const DebugConsoleSessionGeneration generation)
{
	DebugConsoleModelResult result{ EDebugConsoleModelStatus::SessionDisposed };
	{
		std::lock_guard lock(m_mutex);
		auto status = SessionStatusLocked(generation);
		if (status.status != EDebugConsoleModelStatus::Accepted) return status;
		for (auto it = m_pendingEvaluations.begin(); it != m_pendingEvaluations.end();) {
			try {
				RecordCompletionLocked(it->first, it->second, EDebugConsoleEvaluationTerminal::Cancelled, "session disposed");
			} catch (...) {
				// Completed entries already committed above remain terminal; every unprocessed evaluation stays
				// pending under the active session and is visible in Snapshot for a caller-controlled retry.
				result.status = EDebugConsoleModelStatus::ResourceExhausted;
				break;
			}
			it = m_pendingEvaluations.erase(it);
		}
		if (result.status == EDebugConsoleModelStatus::ResourceExhausted) {
			// Keep m_activeSessionGeneration and retained operation records intact: Dispose's release contract
			// applies only after every pending evaluation has reached a terminal completion record.
		} else {
			m_operations.clear();
			m_activeSessionGeneration.reset();
			EnqueueLocked({ .kind = EDebugConsoleNotificationKind::SessionDisposed, .sessionGeneration = generation });
		}
	}
	DrainNotifications();
	return result;
}

DebugConsoleModelResult CDebugConsoleModel::Stop()
{
	DebugConsoleModelResult firstStopResult{ EDebugConsoleModelStatus::Stopped };
	bool drainHere{};
	{
		std::unique_lock lock(m_mutex);
		if (m_stopFinalizationInProgress) {
			if (m_dispatchingNotifications && m_dispatchThreadId == std::this_thread::get_id()) {
				return { EDebugConsoleModelStatus::StopNotificationDrainDeferred };
			}
			m_stopCompletionCondition.wait(lock, [this] { return m_stopFinalizationCompleted; });
			return m_finalStopResult;
		}
		if (m_stopFinalizationCompleted) return m_finalStopResult;

		// Claim the finalization before publishing m_stopped.  Every subsequent non-reentrant Stop
		// therefore waits for the same final delivery/listener-cleanup boundary.
		m_stopFinalizationInProgress = true;
		try {
			if (m_activeSessionGeneration) {
				FinalizeAllPendingLocked(EDebugConsoleEvaluationTerminal::Cancelled, "model stopped");
				EnqueueLocked({ .kind = EDebugConsoleNotificationKind::Stopped, .sessionGeneration = *m_activeSessionGeneration });
				m_activeSessionGeneration.reset();
			}
		} catch (...) {
			m_pendingEvaluations.clear();
			m_operations.clear();
			m_activeSessionGeneration.reset();
			SaturatingIncrement(m_droppedNotificationCount);
			firstStopResult.status = EDebugConsoleModelStatus::StoppedWithDroppedState;
		}
		m_stopped = true;
		m_clearListenersAfterDispatch = true;
		m_finalStopResult = firstStopResult;
		if (m_dispatchingNotifications) {
			if (m_dispatchThreadId == std::this_thread::get_id()) return { EDebugConsoleModelStatus::StopNotificationDrainDeferred };
			m_stopCompletionCondition.wait(lock, [this] { return m_stopFinalizationCompleted; });
			return m_finalStopResult;
		}
		drainHere = true;
	}
	if (drainHere) DrainNotifications();
	{
		std::unique_lock lock(m_mutex);
		if (!m_stopFinalizationCompleted) {
			if (m_dispatchingNotifications && m_dispatchThreadId == std::this_thread::get_id()) {
				return { EDebugConsoleModelStatus::StopNotificationDrainDeferred };
			}
			m_stopCompletionCondition.wait(lock, [this] { return m_stopFinalizationCompleted; });
		}
	}
	return m_finalStopResult;
}

DebugConsoleSnapshot CDebugConsoleModel::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	DebugConsoleSnapshot snapshot;
	snapshot.stopped = m_stopped;
	snapshot.activeSessionGeneration = m_activeSessionGeneration;
	snapshot.newestSessionGeneration = m_newestSessionGeneration;
	snapshot.nextEvaluationRequestId = m_nextEvaluationRequestId;
	snapshot.nextTranscriptSequence = m_nextTranscriptSequence;
	snapshot.nextHistorySequence = m_nextHistorySequence;
	snapshot.droppedTranscriptEntryCount = m_droppedTranscriptEntryCount;
	snapshot.droppedHistoryEntryCount = m_droppedHistoryEntryCount;
	snapshot.droppedCompletedEvaluationCount = m_droppedCompletedEvaluationCount;
	snapshot.droppedNotificationCount = m_droppedNotificationCount;
	snapshot.transcript.assign(m_transcript.begin(), m_transcript.end());
	snapshot.history.assign(m_history.begin(), m_history.end());
	snapshot.pendingEvaluations.reserve(m_pendingEvaluations.size());
	for (const auto& [requestId, evaluation] : m_pendingEvaluations) {
		snapshot.pendingEvaluations.push_back({ requestId, evaluation.operationId, evaluation.expression, evaluation.deadline });
	}
	snapshot.completedEvaluations.assign(m_completedEvaluations.begin(), m_completedEvaluations.end());
	return snapshot;
}

std::optional<DebugConsoleSubscriptionId> CDebugConsoleModel::Subscribe(DebugConsoleListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_mutex);
	if (m_stopped || m_listeners.size() == m_limits.maximumSubscriptions || m_nextSubscriptionId == 0) return std::nullopt;
	const auto id = m_nextSubscriptionId++;
	m_listeners.emplace(id, std::move(listener));
	return id;
}

void CDebugConsoleModel::Unsubscribe(const DebugConsoleSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_mutex);
	m_listeners.erase(subscriptionId);
}

} // namespace debug::console
