/*! @file
	@brief Editor extension-client reconnect ownership and bounded retry policy
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

struct SExtensionClientReconnectConfig {
	std::chrono::milliseconds initialBackoff = std::chrono::milliseconds(100);
	std::chrono::milliseconds maximumBackoff = std::chrono::seconds(5);
	std::chrono::milliseconds retryWindow = std::chrono::minutes(1);
	std::chrono::milliseconds cooldown = std::chrono::minutes(1);
	std::chrono::milliseconds helloTimeout = std::chrono::seconds(10);
	std::uint32_t maximumRetryCount = 6;
};

/*! @brief One worker-owned reconnect state machine. */
class CExtensionClientReconnectPolicy final {
public:
	using Clock = std::chrono::steady_clock;
	using TimePoint = Clock::time_point;
	enum class State { Idle, Scheduled, Connecting, AwaitingHello, Connected, Exhausted, Shutdown };

	explicit CExtensionClientReconnectPolicy(SExtensionClientReconnectConfig config = {})
		: m_config(NormalizeConfig(config))
	{
	}

	//! A user/initialization request starts one window and cannot reset a live one.
	bool RequestReconnect(TimePoint now)
	{
		if (m_state == State::Shutdown || m_state == State::Scheduled || m_state == State::Connecting ||
			m_state == State::AwaitingHello || m_state == State::Connected) return false;
		if (m_state == State::Exhausted && now < m_cooldownUntil) return false;
		m_retryCount = 0;
		m_windowStarted = now;
		m_windowActive = true;
		m_cooldownUntil = {};
		Schedule(now);
		return true;
	}

	std::optional<std::uint64_t> TakeDueReconnect(TimePoint now)
	{
		if (m_state != State::Scheduled || now < m_deadline) return std::nullopt;
		m_state = State::Connecting;
		m_activeToken = m_scheduledToken;
		return m_activeToken;
	}

	bool BeginHello(std::uint64_t token, std::uint64_t generation, TimePoint now)
	{
		if (m_state != State::Connecting || token == 0 || token != m_activeToken || generation == 0) return false;
		m_activeGeneration = generation;
		m_state = State::AwaitingHello;
		m_deadline = now + m_config.helloTimeout;
		return true;
	}

	//! Connect did not create a pipe, so no broker generation can yet be trusted.
	bool OnConnectFailure(std::uint64_t token, TimePoint now, double jitterUnit)
	{
		if (m_state != State::Connecting || token == 0 || token != m_activeToken) return false;
		m_activeToken = 0;
		return ScheduleFailure(now, jitterUnit);
	}

	bool OnHello(std::uint64_t token, std::uint64_t generation)
	{
		if (m_state != State::AwaitingHello || token != m_activeToken || generation != m_activeGeneration) return false;
		m_state = State::Connected;
		m_retryCount = 0;
		m_windowActive = false;
		m_deadline = {};
		return true;
	}

	bool IsHelloTimedOut(TimePoint now) const noexcept
	{
		return m_state == State::AwaitingHello && now >= m_deadline;
	}

	//! Returns false when a stale timer/pipe callback cannot own this session.
	bool OnFailure(std::uint64_t token, std::uint64_t generation, TimePoint now, double jitterUnit)
	{
		if (!AcceptsPipeEvent(token, generation)) return false;
		m_activeGeneration = 0;
		m_activeToken = 0;
		return ScheduleFailure(now, jitterUnit);
	}

	bool AcceptsPipeEvent(std::uint64_t token, std::uint64_t generation) const noexcept
	{
		return token != 0 && token == m_activeToken && generation != 0 && generation == m_activeGeneration &&
			(m_state == State::Connecting || m_state == State::AwaitingHello || m_state == State::Connected);
	}

	std::optional<TimePoint> NextDeadline() const noexcept
	{
		return (m_state == State::Scheduled || m_state == State::AwaitingHello) ? std::optional(m_deadline) : std::nullopt;
	}
	State GetState() const noexcept { return m_state; }
	std::uint32_t GetRetryCount() const noexcept { return m_retryCount; }
	std::uint64_t GetActiveToken() const noexcept { return m_activeToken; }
	std::uint64_t GetActiveGeneration() const noexcept { return m_activeGeneration; }
	TimePoint GetCooldownUntil() const noexcept { return m_cooldownUntil; }
	void Shutdown() noexcept { m_state = State::Shutdown; m_activeToken = 0; m_activeGeneration = 0; m_deadline = {}; }

private:
	static SExtensionClientReconnectConfig NormalizeConfig(SExtensionClientReconnectConfig config) noexcept
	{
		constexpr auto minimumDelay = std::chrono::milliseconds(1);
		config.maximumBackoff = std::max(config.maximumBackoff, minimumDelay);
		config.initialBackoff = std::clamp(config.initialBackoff, minimumDelay, config.maximumBackoff);
		config.retryWindow = std::max(config.retryWindow, minimumDelay);
		config.cooldown = std::max(config.cooldown, std::chrono::milliseconds::zero());
		config.helloTimeout = std::max(config.helloTimeout, minimumDelay);
		return config;
	}

	bool ScheduleFailure(TimePoint now, double jitterUnit)
	{
		if (!m_windowActive) {
			m_windowStarted = now;
			m_windowActive = true;
		}
		if (now - m_windowStarted >= m_config.retryWindow ||
			m_retryCount >= m_config.maximumRetryCount) {
			m_state = State::Exhausted;
			m_deadline = {};
			m_cooldownUntil = now + m_config.cooldown;
			return true;
		}
		++m_retryCount;
		Schedule(now + Backoff(m_retryCount, jitterUnit));
		return true;
	}
	std::chrono::milliseconds Backoff(std::uint32_t retry, double jitterUnit) const noexcept
	{
		const auto jitter = std::clamp(jitterUnit, 0.0, 1.0);
		auto delay = m_config.initialBackoff;
		for (std::uint32_t index = 1; index < retry && delay < m_config.maximumBackoff; ++index) {
			delay = delay > m_config.maximumBackoff / 2
				? m_config.maximumBackoff
				: std::min(delay * 2, m_config.maximumBackoff);
		}
		const auto scaled = static_cast<long double>(delay.count()) * (0.75L + 0.5L * jitter);
		const auto bounded = std::clamp(
			scaled,
			1.0L,
			static_cast<long double>(m_config.maximumBackoff.count()));
		return std::chrono::milliseconds(static_cast<std::int64_t>(bounded));
	}
	void Schedule(TimePoint deadline) noexcept
	{
		m_state = State::Scheduled;
		m_deadline = deadline;
		m_scheduledToken = ++m_nextToken;
	}

	SExtensionClientReconnectConfig m_config;
	State m_state = State::Idle;
	std::uint32_t m_retryCount = 0;
	std::uint64_t m_nextToken = 0;
	std::uint64_t m_scheduledToken = 0;
	std::uint64_t m_activeToken = 0;
	std::uint64_t m_activeGeneration = 0;
	TimePoint m_windowStarted{};
	bool m_windowActive = false;
	TimePoint m_deadline{};
	TimePoint m_cooldownUntil{};
};
