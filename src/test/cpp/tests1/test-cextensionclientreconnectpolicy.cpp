/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionClientReconnectPolicy.h"

#include <chrono>

namespace {

using namespace std::chrono_literals;
using Policy = CExtensionClientReconnectPolicy;
using TimePoint = Policy::TimePoint;

SExtensionClientReconnectConfig FastConfig()
{
	SExtensionClientReconnectConfig config;
	config.initialBackoff = 100ms;
	config.maximumBackoff = 400ms;
	config.retryWindow = 1s;
	config.cooldown = 500ms;
	config.helloTimeout = 200ms;
	config.maximumRetryCount = 2;
	return config;
}

std::uint64_t StartAttempt(Policy& policy, TimePoint now)
{
	EXPECT_TRUE(policy.RequestReconnect(now));
	const auto attempt = policy.TakeDueReconnect(now);
	EXPECT_TRUE(attempt.has_value());
	return attempt.value_or(0);
}

} // namespace

TEST(CExtensionClientReconnectPolicy, DeduplicatesAndUsesCappedExponentialBackoff)
{
	Policy policy(FastConfig());
	const TimePoint start{};
	const auto first = StartAttempt(policy, start);
	EXPECT_FALSE(policy.RequestReconnect(start));
	EXPECT_TRUE(policy.OnConnectFailure(first, start, 0.5));
	EXPECT_EQ(1u, policy.GetRetryCount());
	EXPECT_FALSE(policy.TakeDueReconnect(start + 99ms).has_value());
	const auto second = policy.TakeDueReconnect(start + 100ms);
	ASSERT_TRUE(second.has_value());
	EXPECT_TRUE(policy.OnConnectFailure(*second, start + 100ms, 1.0));
	EXPECT_EQ(2u, policy.GetRetryCount());
	EXPECT_FALSE(policy.TakeDueReconnect(start + 349ms).has_value());
	EXPECT_TRUE(policy.TakeDueReconnect(start + 350ms).has_value());
}

TEST(CExtensionClientReconnectPolicy, RejectsStaleTokenAndGeneration)
{
	Policy policy(FastConfig());
	const TimePoint start{};
	const auto first = StartAttempt(policy, start);
	ASSERT_TRUE(policy.BeginHello(first, 11, start));
	EXPECT_FALSE(policy.OnHello(first + 1, 11));
	EXPECT_FALSE(policy.OnHello(first, 12));
	EXPECT_FALSE(policy.OnFailure(first + 1, 11, start + 1ms, 0.5));
	EXPECT_FALSE(policy.OnFailure(first, 12, start + 1ms, 0.5));
	EXPECT_TRUE(policy.OnHello(first, 11));
	EXPECT_EQ(Policy::State::Connected, policy.GetState());
	EXPECT_TRUE(policy.OnFailure(first, 11, start + 2ms, 0.5));
	EXPECT_EQ(Policy::State::Scheduled, policy.GetState());
	EXPECT_FALSE(policy.OnFailure(first, 11, start + 3ms, 0.5));
}

TEST(CExtensionClientReconnectPolicy, ExhaustionNeedsCooldownBeforeExternalRestart)
{
	Policy policy(FastConfig());
	const TimePoint start{};
	auto attempt = StartAttempt(policy, start);
	ASSERT_TRUE(policy.OnConnectFailure(attempt, start, 0.5));
	attempt = *policy.TakeDueReconnect(start + 100ms);
	ASSERT_TRUE(policy.OnConnectFailure(attempt, start + 100ms, 0.5));
	attempt = *policy.TakeDueReconnect(start + 300ms);
	EXPECT_TRUE(policy.OnConnectFailure(attempt, start + 300ms, 0.5));
	EXPECT_EQ(Policy::State::Exhausted, policy.GetState());
	EXPECT_FALSE(policy.RequestReconnect(start + 799ms));
	EXPECT_TRUE(policy.RequestReconnect(start + 800ms));
	EXPECT_EQ(Policy::State::Scheduled, policy.GetState());
}

TEST(CExtensionClientReconnectPolicy, HelloTimeoutSuccessAndShutdownHaveExplicitTerminals)
{
	Policy policy(FastConfig());
	const TimePoint start{};
	const auto first = StartAttempt(policy, start);
	ASSERT_TRUE(policy.BeginHello(first, 41, start));
	EXPECT_FALSE(policy.IsHelloTimedOut(start + 199ms));
	EXPECT_TRUE(policy.IsHelloTimedOut(start + 200ms));
	EXPECT_TRUE(policy.OnFailure(first, 41, start + 200ms, 0.5));
	const auto second = policy.TakeDueReconnect(start + 300ms);
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(policy.BeginHello(*second, 42, start + 300ms));
	EXPECT_TRUE(policy.OnHello(*second, 42));
	EXPECT_EQ(Policy::State::Connected, policy.GetState());
	EXPECT_EQ(0u, policy.GetRetryCount());
	policy.Shutdown();
	EXPECT_EQ(Policy::State::Shutdown, policy.GetState());
	EXPECT_FALSE(policy.RequestReconnect(start + 1s));
	EXPECT_FALSE(policy.NextDeadline().has_value());
}

TEST(CExtensionClientReconnectPolicy, JitterNeverExceedsConfiguredMaximumBackoff)
{
	auto config = FastConfig();
	config.maximumRetryCount = 10;
	Policy policy(config);
	const TimePoint start{};
	auto attempt = StartAttempt(policy, start);
	ASSERT_TRUE(policy.OnConnectFailure(attempt, start, 1.0));
	attempt = *policy.TakeDueReconnect(start + 125ms);
	ASSERT_TRUE(policy.OnConnectFailure(attempt, start + 125ms, 1.0));
	attempt = *policy.TakeDueReconnect(start + 375ms);
	ASSERT_TRUE(policy.OnConnectFailure(attempt, start + 375ms, 1.0));

	EXPECT_FALSE(policy.TakeDueReconnect(start + 774ms).has_value());
	EXPECT_TRUE(policy.TakeDueReconnect(start + 775ms).has_value());
}

TEST(CExtensionClientReconnectPolicy, NormalizesInvalidDurationsBeforeScheduling)
{
	SExtensionClientReconnectConfig config;
	config.initialBackoff = -10ms;
	config.maximumBackoff = -20ms;
	config.retryWindow = -1ms;
	config.cooldown = -1ms;
	config.helloTimeout = -1ms;
	config.maximumRetryCount = 1;
	Policy policy(config);
	const TimePoint start{};
	const auto first = StartAttempt(policy, start);
	ASSERT_TRUE(policy.OnConnectFailure(first, start, 0.0));
	EXPECT_FALSE(policy.TakeDueReconnect(start).has_value());
	const auto second = policy.TakeDueReconnect(start + 1ms);
	ASSERT_TRUE(second.has_value());
	ASSERT_TRUE(policy.BeginHello(*second, 5, start + 1ms));
	EXPECT_FALSE(policy.IsHelloTimedOut(start + 1ms));
	EXPECT_TRUE(policy.IsHelloTimedOut(start + 2ms));
	EXPECT_TRUE(policy.OnFailure(*second, 5, start + 2ms, 0.0));
	EXPECT_EQ(Policy::State::Exhausted, policy.GetState());
	EXPECT_TRUE(policy.RequestReconnect(start + 2ms));
}
