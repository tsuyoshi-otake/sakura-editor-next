/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/tmux/TmuxWaitChannelService.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace terminal::tmux {
namespace {

[[nodiscard]] bool IsSafeChannel(std::string_view channel, const TmuxWaitChannelLimits& limits) noexcept
{
	if (channel.empty() || channel.size() > limits.maximumNameBytes) return false;
	for (const auto ch : channel) {
		const auto byte = static_cast<unsigned char>(ch);
		if (byte == 0 || byte < 0x20 || byte == 0x7f) return false;
	}
	return true;
}

[[nodiscard]] TmuxWaitResult Result(const TmuxWaitCode code) noexcept
{
	return TmuxWaitResult{ code };
}

} // namespace

struct TmuxWaitChannelService::State final {
	struct Channel final {
		bool locked{};
		bool woken{};
		std::uint64_t signalGeneration{};
		std::uint64_t nextLockerTicket{ 1 };
		std::deque<std::uint64_t> grantedLockerTickets;
		std::deque<std::uint64_t> lockers;
		std::size_t waiters{};
	};

	explicit State(TmuxWaitChannelLimits limitsValue)
		: limits(std::move(limitsValue))
	{
		limits.maximumChannels = (std::max)(std::size_t{ 1 }, limits.maximumChannels);
		limits.maximumWaiters = (std::max)(std::size_t{ 1 }, limits.maximumWaiters);
		limits.maximumNameBytes = (std::max)(std::size_t{ 1 }, limits.maximumNameBytes);
		if (limits.maximumWait <= std::chrono::seconds::zero()) limits.maximumWait = std::chrono::seconds{ 1 };
	}

	TmuxWaitChannelLimits limits;
	mutable std::mutex mutex;
	std::condition_variable condition;
	std::unordered_map<std::string, std::shared_ptr<Channel>> channels;
	std::size_t waiterCount{};
	bool stopping{};

	[[nodiscard]] std::shared_ptr<Channel> FindOrCreateLocked(std::string_view name)
	{
		const auto found = channels.find(std::string(name));
		if (found != channels.end()) return found->second;
		if (channels.size() >= limits.maximumChannels) return nullptr;
		const auto channel = std::make_shared<Channel>();
		channels.emplace(std::string(name), channel);
		return channel;
	}

	void CleanupLocked(std::string_view name, const std::shared_ptr<Channel>& channel)
	{
		if (channel->locked || channel->woken || channel->waiters != 0
			|| !channel->lockers.empty() || !channel->grantedLockerTickets.empty()) return;
		const auto found = channels.find(std::string(name));
		if (found != channels.end() && found->second == channel) channels.erase(found);
	}
};

TmuxWaitChannelService::TmuxWaitChannelService(TmuxWaitChannelLimits limits)
	: m_state(std::make_shared<State>(std::move(limits)))
{
}

TmuxWaitChannelService::~TmuxWaitChannelService()
{
	BeginShutdown();
}

TmuxWaitResult TmuxWaitChannelService::Wait(
	const std::string_view channelName,
	const std::chrono::steady_clock::time_point absoluteDeadline)
{
	const auto state = m_state;
	if (!state || !IsSafeChannel(channelName, state->limits)) return Result(TmuxWaitCode::InvalidRequest);
	const auto now = std::chrono::steady_clock::now();
	const auto cap = now + state->limits.maximumWait;
	const auto deadline = absoluteDeadline == std::chrono::steady_clock::time_point{}
		? cap : (absoluteDeadline < cap ? absoluteDeadline : cap);
	if (now >= deadline) {
		return Result(TmuxWaitCode::DeadlineExceeded);
	}
	std::unique_lock lock(state->mutex);
	if (state->stopping) return Result(TmuxWaitCode::Stopped);
	const auto channel = state->FindOrCreateLocked(channelName);
	if (!channel) return Result(TmuxWaitCode::ResourceExhausted);
	if (channel->woken) {
		channel->woken = false;
		state->CleanupLocked(channelName, channel);
		return Result(TmuxWaitCode::Succeeded);
	}
	const auto observedGeneration = channel->signalGeneration;
	++channel->waiters;
	++state->waiterCount;
	TmuxWaitCode code = TmuxWaitCode::Succeeded;
	for (;;) {
		if (state->stopping) {
			code = TmuxWaitCode::Stopped;
			break;
		}
		if (channel->signalGeneration != observedGeneration) {
			code = TmuxWaitCode::Succeeded;
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			code = TmuxWaitCode::DeadlineExceeded;
			break;
		}
		if (state->condition.wait_until(lock, deadline) == std::cv_status::timeout) {
			if (channel->signalGeneration == observedGeneration && !state->stopping) code = TmuxWaitCode::DeadlineExceeded;
		}
	}
	--channel->waiters;
	--state->waiterCount;
	state->CleanupLocked(channelName, channel);
	return Result(code);
}

TmuxWaitResult TmuxWaitChannelService::Signal(const std::string_view channelName)
{
	const auto state = m_state;
	if (!state || !IsSafeChannel(channelName, state->limits)) return Result(TmuxWaitCode::InvalidRequest);
	std::lock_guard lock(state->mutex);
	if (state->stopping) return Result(TmuxWaitCode::Stopped);
	const auto channel = state->FindOrCreateLocked(channelName);
	if (!channel) return Result(TmuxWaitCode::ResourceExhausted);
	if (channel->waiters != 0) ++channel->signalGeneration;
	else channel->woken = true;
	state->condition.notify_all();
	return Result(TmuxWaitCode::Succeeded);
}

TmuxWaitResult TmuxWaitChannelService::Lock(
	const std::string_view channelName,
	const std::chrono::steady_clock::time_point absoluteDeadline)
{
	const auto state = m_state;
	if (!state || !IsSafeChannel(channelName, state->limits)) return Result(TmuxWaitCode::InvalidRequest);
	const auto now = std::chrono::steady_clock::now();
	const auto cap = now + state->limits.maximumWait;
	const auto deadline = absoluteDeadline == std::chrono::steady_clock::time_point{}
		? cap : (absoluteDeadline < cap ? absoluteDeadline : cap);
	if (now >= deadline) {
		return Result(TmuxWaitCode::DeadlineExceeded);
	}
	std::unique_lock lock(state->mutex);
	if (state->stopping) return Result(TmuxWaitCode::Stopped);
	const auto channel = state->FindOrCreateLocked(channelName);
	if (!channel) return Result(TmuxWaitCode::ResourceExhausted);
	if (!channel->locked && channel->lockers.empty()) {
		channel->locked = true;
		return Result(TmuxWaitCode::Succeeded);
	}
	if (state->waiterCount >= state->limits.maximumWaiters) return Result(TmuxWaitCode::ResourceExhausted);
	if (channel->nextLockerTicket == 0) return Result(TmuxWaitCode::ResourceExhausted);
	const auto ticket = channel->nextLockerTicket++;
	channel->lockers.push_back(ticket);
	++state->waiterCount;
	TmuxWaitCode code = TmuxWaitCode::Succeeded;
	for (;;) {
		if (state->stopping) {
			code = TmuxWaitCode::Stopped;
			break;
		}
		const auto granted = std::find(channel->grantedLockerTickets.begin(),
			channel->grantedLockerTickets.end(), ticket);
		if (granted != channel->grantedLockerTickets.end()) {
			channel->grantedLockerTickets.erase(granted);
			code = TmuxWaitCode::Succeeded;
			break;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			code = TmuxWaitCode::DeadlineExceeded;
			break;
		}
		if (state->condition.wait_until(lock, deadline) == std::cv_status::timeout) {
			if (std::find(channel->grantedLockerTickets.begin(),
				channel->grantedLockerTickets.end(), ticket) == channel->grantedLockerTickets.end() && !state->stopping) {
				code = TmuxWaitCode::DeadlineExceeded;
			}
		}
	}
	if (code != TmuxWaitCode::Succeeded) {
		const auto found = std::find(channel->lockers.begin(), channel->lockers.end(), ticket);
		if (found != channel->lockers.end()) channel->lockers.erase(found);
	}
	--state->waiterCount;
	state->CleanupLocked(channelName, channel);
	return Result(code);
}

TmuxWaitResult TmuxWaitChannelService::Unlock(const std::string_view channelName)
{
	const auto state = m_state;
	if (!state || !IsSafeChannel(channelName, state->limits)) return Result(TmuxWaitCode::InvalidRequest);
	std::lock_guard lock(state->mutex);
	if (state->stopping) return Result(TmuxWaitCode::Stopped);
	const auto found = state->channels.find(std::string(channelName));
	if (found == state->channels.end() || !found->second->locked) return Result(TmuxWaitCode::InvalidRequest);
	const auto& channel = found->second;
	if (!channel->lockers.empty()) {
		channel->grantedLockerTickets.push_back(channel->lockers.front());
		channel->lockers.pop_front();
	} else {
		channel->locked = false;
	}
	state->condition.notify_all();
	state->CleanupLocked(channelName, channel);
	return Result(TmuxWaitCode::Succeeded);
}

void TmuxWaitChannelService::BeginShutdown() noexcept
{
	const auto state = m_state;
	if (!state) return;
	{
		std::lock_guard lock(state->mutex);
		state->stopping = true;
	}
	state->condition.notify_all();
}

bool TmuxWaitChannelService::IsStopping() const noexcept
{
	const auto state = m_state;
	if (!state) return true;
	std::lock_guard lock(state->mutex);
	return state->stopping;
}

} // namespace terminal::tmux
