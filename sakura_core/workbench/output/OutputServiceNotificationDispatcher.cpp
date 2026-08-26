/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/output/OutputServiceNotificationDispatcher.h"

#include <limits>
#include <utility>

namespace workbench::output {

OutputServiceNotificationDispatcher::OutputServiceNotificationDispatcher(
	std::mutex& modelMutex, std::condition_variable& drainCondition, Limits limits)
	: m_modelMutex(modelMutex)
	, m_drainCondition(drainCondition)
	, m_limits(limits)
{
	if (m_limits.maximumSubscriptions == 0) m_limits.maximumSubscriptions = 1;
	if (m_limits.maximumPendingNotifications == 0) m_limits.maximumPendingNotifications = 1;
}

void OutputServiceNotificationDispatcher::SaturatingIncrement(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

std::optional<OutputServiceSubscriptionId> OutputServiceNotificationDispatcher::Subscribe(OutputServiceListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_modelMutex);
	if (m_stopped || m_subscriptions.size() == m_limits.maximumSubscriptions || m_nextSubscriptionId == 0) {
		return std::nullopt;
	}
	const auto subscriptionId = m_nextSubscriptionId++;
	m_subscriptions.emplace(subscriptionId, std::move(listener));
	return subscriptionId;
}

void OutputServiceNotificationDispatcher::Unsubscribe(const OutputServiceSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_modelMutex);
	m_subscriptions.erase(subscriptionId);
}

bool OutputServiceNotificationDispatcher::QueueLocked(const std::uint64_t revision, const EOutputChangeKind kind,
	const std::optional<std::string>& channelId, const std::optional<std::string>& activeChannelId) noexcept
{
	try {
		if (m_stopped) return false;
		if (m_pendingNotifications.size() >= m_limits.maximumPendingNotifications) {
			SaturatingIncrement(m_droppedNotificationCount);
			return false;
		}
		PendingNotification pending{ .change = { .revision = revision, .kind = kind, .channelId = channelId, .activeChannelId = activeChannelId } };
		pending.subscriberIds.reserve(m_subscriptions.size());
		for (const auto& [subscriptionId, ignored] : m_subscriptions) {
			(void)ignored;
			pending.subscriberIds.push_back(subscriptionId);
		}
		m_pendingNotifications.push_back(std::move(pending));
		if (m_draining) return false;
		m_draining = true;
		return true;
	}
	catch (...) {
		// A notification is advisory.  Allocation failure must not roll back the
		// already accepted provider mutation.
		SaturatingIncrement(m_droppedNotificationCount);
		return false;
	}
}

void OutputServiceNotificationDispatcher::StopLocked() noexcept
{
	m_stopped = true;
	m_subscriptions.clear();
	m_pendingNotifications.clear();
}

bool OutputServiceNotificationDispatcher::IsDrainingLocked() const noexcept
{
	return m_draining;
}

bool OutputServiceNotificationDispatcher::IsDispatchThreadLocked() const noexcept
{
	return m_draining && m_dispatchThreadId == std::this_thread::get_id();
}

std::uint64_t OutputServiceNotificationDispatcher::DroppedNotificationCountLocked() const noexcept
{
	return m_droppedNotificationCount;
}

bool OutputServiceNotificationDispatcher::WaitForDrain() noexcept
{
	std::unique_lock lock(m_modelMutex);
	if (IsDispatchThreadLocked()) return true;
	try {
		m_drainCondition.wait(lock, [this] { return !m_draining; });
	}
	catch (...) {
		return m_draining;
	}
	return false;
}

void OutputServiceNotificationDispatcher::Drain() noexcept
{
	{
		std::lock_guard lock(m_modelMutex);
		if (!m_draining || m_dispatchThreadId != std::thread::id{}) return;
		m_dispatchThreadId = std::this_thread::get_id();
	}

	for (;;) {
		PendingNotification pending;
		{
			std::lock_guard lock(m_modelMutex);
			if (m_pendingNotifications.empty()) {
				m_draining = false;
				m_dispatchThreadId = {};
				m_drainCondition.notify_all();
				return;
			}
			pending = std::move(m_pendingNotifications.front());
			m_pendingNotifications.pop_front();
		}

		for (const auto subscriptionId : pending.subscriberIds) {
			OutputServiceListener listener;
			bool listenerCopyFailed{};
			{
				std::lock_guard lock(m_modelMutex);
				const auto found = m_subscriptions.find(subscriptionId);
				if (found != m_subscriptions.end()) {
					try {
						listener = found->second;
					}
					catch (...) {
						// Copying a listener is advisory work too. Keep the provider
						// alive when a stateful target rejects the copy, and let later
						// subscribers and queued notifications continue.
						SaturatingIncrement(m_droppedNotificationCount);
						listenerCopyFailed = true;
					}
				}
			}
			if (listenerCopyFailed) continue;
			if (!listener) continue;
			try {
				// Callbacks are deliberately outside the provider model lock.  They
				// may reenter, unsubscribe, or request a deferred Stop.
				listener(pending.change);
			}
			catch (...) {
				// An advisory observer cannot suppress later observers or mutate the
				// terminal state of the provider by throwing.
			}
		}
	}
}

} // namespace workbench::output
