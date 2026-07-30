/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionNotificationCenter.h"

#include <algorithm>
#include <mutex>

namespace {

bool ValidText(std::wstring_view value, std::size_t maximum, bool allowEmpty = false)
{
	return (allowEmpty || !value.empty()) && value.size() <= maximum &&
		value.find(L'\0') == std::wstring_view::npos;
}
} // namespace

CExtensionNotificationCenter::CExtensionNotificationCenter(std::size_t maximumPending)
	: m_maximumPending((std::max<std::size_t>)(1, maximumPending))
{
}

std::optional<std::uint64_t> CExtensionNotificationCenter::Show(SExtensionNotification notification)
{
	if (!ValidText(notification.extensionId, 255) || notification.generation == 0 ||
		!ValidText(notification.message, 16384) || !ValidText(notification.detail, 65536, true) ||
		notification.actions.size() > 32 || std::any_of(notification.actions.begin(), notification.actions.end(),
			[](const auto& action) { return !ValidText(action, 2048); })) return std::nullopt;
	std::unique_lock lock(m_mutex);
	const auto pendingCount = std::count_if(m_notifications.begin(), m_notifications.end(), [](const auto& pair) {
		return pair.second.state == EExtensionNotificationState::Pending;
	});
	if (static_cast<std::size_t>(pendingCount) >= m_maximumPending) {
		const auto evict = std::find_if(m_order.begin(), m_order.end(), [this](std::uint64_t id) {
			const auto found = m_notifications.find(id);
			return found != m_notifications.end() && found->second.state == EExtensionNotificationState::Pending &&
				!found->second.modal;
		});
		if (evict == m_order.end()) return std::nullopt;
		m_notifications[*evict].state = EExtensionNotificationState::Dismissed;
	}
	notification.id = m_nextId++;
	notification.state = EExtensionNotificationState::Pending;
	notification.selectedAction.reset();
	const auto id = notification.id;
	m_notifications.emplace(id, std::move(notification));
	m_order.push_back(id);
	return id;
}

bool CExtensionNotificationCenter::Resolve(std::uint64_t id, std::optional<std::size_t> selectedAction)
{
	std::unique_lock lock(m_mutex);
	return FinishLocked(id, selectedAction ? EExtensionNotificationState::Resolved : EExtensionNotificationState::Dismissed,
		selectedAction);
}

bool CExtensionNotificationCenter::Cancel(std::uint64_t id)
{
	std::unique_lock lock(m_mutex);
	return FinishLocked(id, EExtensionNotificationState::Cancelled, std::nullopt);
}

void CExtensionNotificationCenter::NotifyHostLost(std::wstring_view extensionId, std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	for (auto& [id, notification] : m_notifications) {
		if (notification.state == EExtensionNotificationState::Pending && notification.extensionId == extensionId &&
			(generation == 0 || notification.generation == generation)) {
			notification.state = EExtensionNotificationState::HostLost;
			notification.selectedAction.reset();
		}
	}
}

void CExtensionNotificationCenter::Clear()
{
	std::unique_lock lock(m_mutex);
	m_notifications.clear();
	m_order.clear();
}

std::vector<SExtensionNotification> CExtensionNotificationCenter::Pending() const
{
	std::shared_lock lock(m_mutex);
	std::vector<SExtensionNotification> result;
	for (const auto id : m_order) {
		const auto found = m_notifications.find(id);
		if (found != m_notifications.end() && found->second.state == EExtensionNotificationState::Pending) {
			result.push_back(found->second);
		}
	}
	return result;
}

std::optional<SExtensionNotification> CExtensionNotificationCenter::TakeCompletion(std::uint64_t id)
{
	std::unique_lock lock(m_mutex);
	const auto found = m_notifications.find(id);
	if (found == m_notifications.end() || found->second.state == EExtensionNotificationState::Pending) return std::nullopt;
	auto result = std::move(found->second);
	m_notifications.erase(found);
	m_order.erase(std::remove(m_order.begin(), m_order.end(), id), m_order.end());
	return result;
}

bool CExtensionNotificationCenter::FinishLocked(
	std::uint64_t id,
	EExtensionNotificationState state,
	std::optional<std::size_t> selectedAction)
{
	const auto found = m_notifications.find(id);
	if (found == m_notifications.end() || found->second.state != EExtensionNotificationState::Pending) return false;
	if (selectedAction && *selectedAction >= found->second.actions.size()) return false;
	found->second.state = state;
	found->second.selectedAction = selectedAction;
	return true;
}
