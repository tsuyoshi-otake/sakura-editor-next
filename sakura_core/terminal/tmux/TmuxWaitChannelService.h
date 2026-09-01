/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/tmux/TmuxCommandTypes.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

namespace terminal::tmux {

class TmuxWaitChannelServiceTestProbe;

struct TmuxWaitChannelLimits final {
	std::size_t maximumChannels{ 256 };
	std::size_t maximumWaiters{ 128 };
	std::size_t maximumNameBytes{ 128 };
	std::chrono::seconds maximumWait{ 300 };
};

enum class TmuxWaitCode : std::uint8_t {
	Succeeded,
	InvalidRequest,
	ResourceExhausted,
	DeadlineExceeded,
	Stopped,
};

struct TmuxWaitResult final {
	TmuxWaitCode code{ TmuxWaitCode::InvalidRequest };

	[[nodiscard]] bool Succeeded() const noexcept { return code == TmuxWaitCode::Succeeded; }
};

//! Bounded, in-process implementation of tmux wait-for barrier semantics.
//!
//! The service owns no thread. A caller that waits blocks only its own thread,
//! with an absolute deadline supplied by the dispatcher. BeginShutdown wakes
//! every waiter/locker and makes all future operations fail closed.
class TmuxWaitChannelService final {
public:
	explicit TmuxWaitChannelService(TmuxWaitChannelLimits limits = {});
	~TmuxWaitChannelService();

	TmuxWaitChannelService(const TmuxWaitChannelService&) = delete;
	TmuxWaitChannelService& operator=(const TmuxWaitChannelService&) = delete;

	[[nodiscard]] TmuxWaitResult Wait(
		std::string_view channel,
		std::chrono::steady_clock::time_point absoluteDeadline);
	[[nodiscard]] TmuxWaitResult Signal(std::string_view channel);
	[[nodiscard]] TmuxWaitResult Lock(
		std::string_view channel,
		std::chrono::steady_clock::time_point absoluteDeadline);
	[[nodiscard]] TmuxWaitResult Unlock(std::string_view channel);

	//! Terminalizes all blocked operations. This never detaches a worker.
	void BeginShutdown() noexcept;
	[[nodiscard]] bool IsStopping() const noexcept;

private:
	friend class TmuxWaitChannelServiceTestProbe;

	[[nodiscard]] std::size_t PendingWaiterCountForTesting(std::string_view channel) const noexcept;
	[[nodiscard]] std::size_t PendingLockerCountForTesting(std::string_view channel) const noexcept;

	struct State;
	std::shared_ptr<State> m_state;
};

} // namespace terminal::tmux
