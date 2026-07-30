/*! @file
	@brief 拡張通知と応答の明示的 terminal-state モデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class EExtensionNotificationSeverity : std::uint8_t {
	Information,
	Warning,
	Error,
};
enum class EExtensionNotificationState : std::uint8_t {
	Pending,
	Resolved,
	Dismissed,
	Cancelled,
	HostLost,
};

struct SExtensionNotification {
	std::uint64_t id = 0;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	EExtensionNotificationSeverity severity = EExtensionNotificationSeverity::Information;
	EExtensionNotificationState state = EExtensionNotificationState::Pending;
	std::wstring message;
	std::wstring detail;
	std::vector<std::wstring> actions;
	std::optional<std::size_t> selectedAction;
	bool modal = false;
};

class CExtensionNotificationCenter final {
public:
	explicit CExtensionNotificationCenter(std::size_t maximumPending = 100);

	[[nodiscard]] std::optional<std::uint64_t> Show(SExtensionNotification notification);
	bool Resolve(std::uint64_t id, std::optional<std::size_t> selectedAction);
	bool Cancel(std::uint64_t id);
	void NotifyHostLost(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();

	[[nodiscard]] std::vector<SExtensionNotification> Pending() const;
	//! Removes and returns a completed notification. Pending notifications are not consumed.
	[[nodiscard]] std::optional<SExtensionNotification> TakeCompletion(std::uint64_t id);

private:
	bool FinishLocked(std::uint64_t id, EExtensionNotificationState state, std::optional<std::size_t> selectedAction);

	mutable std::shared_mutex m_mutex;
	std::unordered_map<std::uint64_t, SExtensionNotification> m_notifications;
	std::vector<std::uint64_t> m_order;
	std::size_t m_maximumPending = 100;
	std::uint64_t m_nextId = 1;
};
