/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/output/OutputServiceTypes.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace workbench::output {

/*!
	@brief Provider-neutral advisory notification lifetime and delivery boundary.

	The owner supplies its model mutex and drain condition variable.  Methods
	whose name ends in `Locked` require that mutex to be held by the caller;
	`Drain`, `Subscribe`, `Unsubscribe`, and `WaitForDrain` acquire it as needed.
	The dispatcher deliberately owns no model state and never calls a listener
	while the owner mutex is held.  `StopLocked` closes the advisory boundary,
	clears queued work and listeners, and leaves an already-running callback to
	finish.  The provider remains responsible for its own terminal state and for
	combining this drain with any other callback source it owns.
*/
class OutputServiceNotificationDispatcher final {
public:
	struct Limits final {
		std::size_t maximumSubscriptions{ 256 };
		std::size_t maximumPendingNotifications{ 512 };
	};

	OutputServiceNotificationDispatcher(std::mutex& modelMutex, std::condition_variable& drainCondition,
		Limits limits = {});
	~OutputServiceNotificationDispatcher() = default;

	OutputServiceNotificationDispatcher(const OutputServiceNotificationDispatcher&) = delete;
	OutputServiceNotificationDispatcher& operator=(const OutputServiceNotificationDispatcher&) = delete;

	//! Registers an advisory listener.  A null listener or a bounded limit returns no ID.
	[[nodiscard]] std::optional<OutputServiceSubscriptionId> Subscribe(OutputServiceListener listener);
	//! Removes a listener without waiting for an already-copied callback to finish.
	void Unsubscribe(OutputServiceSubscriptionId subscriptionId) noexcept;

	/*!
		Queues one committed change while the provider model lock is held.
		The change is copied only after the bounded queue check, preserving the
		model's allocation boundary.  The return value tells the caller whether
		it owns the synchronous drain attempt.
	*/
	[[nodiscard]] bool QueueLocked(std::uint64_t revision, EOutputChangeKind kind,
		const std::optional<std::string>& channelId,
		const std::optional<std::string>& activeChannelId) noexcept;

	//! Closes the advisory boundary and drops not-yet-delivered notifications.
	//! The caller must hold the provider model lock.
	void StopLocked() noexcept;

	//! Starts/continues delivery.  Call only after releasing the provider lock.
	void Drain() noexcept;

	//! Reports whether an advisory listener is registered; caller holds the owner lock.
	[[nodiscard]] bool HasSubscriptionsLocked() const noexcept;

	//! Waits for an external caller to observe callback quiescence.  A callback
	//! on the dispatch thread returns true instead of waiting on itself.
	[[nodiscard]] bool WaitForDrain() noexcept;

	//! The following accessors require the provider model lock.
	[[nodiscard]] bool IsDrainingLocked() const noexcept;
	[[nodiscard]] bool IsDispatchThreadLocked() const noexcept;
	[[nodiscard]] std::uint64_t DroppedNotificationCountLocked() const noexcept;
	[[nodiscard]] std::uint64_t ListenerFailureCountLocked() const noexcept;

private:
	struct PendingNotification final {
		OutputServiceChange change;
		std::vector<OutputServiceSubscriptionId> subscriberIds;
	};

	static void SaturatingIncrement(std::uint64_t& value) noexcept;

	std::mutex& m_modelMutex;
	std::condition_variable& m_drainCondition;
	Limits m_limits;
	std::map<OutputServiceSubscriptionId, OutputServiceListener> m_subscriptions;
	std::deque<PendingNotification> m_pendingNotifications;
	std::uint64_t m_droppedNotificationCount{};
	std::uint64_t m_listenerFailureCount{};
	OutputServiceSubscriptionId m_nextSubscriptionId{ 1 };
	bool m_stopped{};
	bool m_draining{};
	std::thread::id m_dispatchThreadId;
};

} // namespace workbench::output
