/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostStateMachine.h"

#include <chrono>

namespace {

using namespace std::chrono_literals;
using TimePoint = CExtensionHostStateMachine::TimePoint;

SExtensionHostLifecycleConfig FastConfig()
{
	SExtensionHostLifecycleConfig config;
	config.keepAlive = 60s;
	config.startTimeout = 2s;
	config.quiesceTimeout = 3s;
	config.initialBackoff = 100ms;
	config.maximumBackoff = 800ms;
	config.maximumRetryCount = 3;
	config.jitterRatio = 0.20;
	return config;
}

void ExpectSingleAction(
	const CExtensionHostStateMachine::Actions& actions,
	EExtensionHostLifecycleActionKind kind,
	std::uint64_t generation)
{
	ASSERT_EQ(1u, actions.size());
	EXPECT_EQ(kind, actions[0].kind);
	EXPECT_EQ(generation, actions[0].generation);
}

} // namespace

TEST(CExtensionHostStateMachine, LeaseStartIsDeduplicatedAndReferenceCounted)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};

	ExpectSingleAction(machine.AcquireLease(101, start), EExtensionHostLifecycleActionKind::StartHost, 1);
	EXPECT_TRUE(machine.AcquireLease(101, start).empty());
	EXPECT_TRUE(machine.AcquireLease(202, start).empty());
	EXPECT_EQ(3u, machine.GetLeaseCount());
	EXPECT_EQ(2u, machine.GetLeaseOwnerCount());
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());

	EXPECT_TRUE(machine.OnHostReady(1, start + 10ms).empty());
	EXPECT_EQ(EExtensionHostState::Ready, machine.GetState());
	EXPECT_TRUE(machine.ReleaseLease(101, start + 20ms).empty());
	EXPECT_TRUE(machine.ReleaseLease(101, start + 20ms).empty());
	EXPECT_TRUE(machine.ReleaseLease(202, start + 20ms).empty());
	EXPECT_EQ(EExtensionHostState::KeepAlive, machine.GetState());
	EXPECT_EQ(start + 20ms + 60s, machine.GetDeadline());

	EXPECT_TRUE(machine.AcquireLease(303, start + 30ms).empty());
	EXPECT_EQ(EExtensionHostState::Ready, machine.GetState());
	EXPECT_EQ(1u, machine.GetGeneration());
}

TEST(CExtensionHostStateMachine, KeepAliveQuiescesThenStartsANewGeneration)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);
	machine.OnHostReady(1, start);
	machine.ReleaseLease(101, start);

	ExpectSingleAction(machine.Tick(start + 60s), EExtensionHostLifecycleActionKind::BeginQuiesce, 1);
	EXPECT_EQ(EExtensionHostState::Quiescing, machine.GetState());
	ExpectSingleAction(machine.OnQuiesceCompleted(1, start + 61s), EExtensionHostLifecycleActionKind::Stopped, 1);
	EXPECT_EQ(EExtensionHostState::Stopped, machine.GetState());

	ExpectSingleAction(machine.AcquireLease(202, start + 62s), EExtensionHostLifecycleActionKind::StartHost, 2);
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());
}

TEST(CExtensionHostStateMachine, FailureUsesBoundedExponentialBackoffAndStopsAtLimit)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);

	auto actions = machine.OnHostStartFailed(1, start, "first", 0.5);
	ExpectSingleAction(actions, EExtensionHostLifecycleActionKind::ScheduleRetry, 1);
	EXPECT_EQ(100ms, actions[0].delay);
	EXPECT_EQ(1u, machine.GetRetryCount());
	ExpectSingleAction(machine.Tick(start + 100ms), EExtensionHostLifecycleActionKind::StartHost, 2);

	actions = machine.OnHostStartFailed(2, start + 100ms, "second", 1.0);
	ExpectSingleAction(actions, EExtensionHostLifecycleActionKind::ScheduleRetry, 2);
	EXPECT_EQ(240ms, actions[0].delay);
	ExpectSingleAction(machine.Tick(start + 340ms), EExtensionHostLifecycleActionKind::StartHost, 3);

	actions = machine.OnHostStartFailed(3, start + 340ms, "third", 0.0);
	ExpectSingleAction(actions, EExtensionHostLifecycleActionKind::ScheduleRetry, 3);
	EXPECT_EQ(320ms, actions[0].delay);
	ExpectSingleAction(machine.Tick(start + 660ms), EExtensionHostLifecycleActionKind::StartHost, 4);

	actions = machine.OnHostStartFailed(4, start + 660ms, "last", 0.5);
	ExpectSingleAction(actions, EExtensionHostLifecycleActionKind::Stopped, 4);
	EXPECT_EQ(EExtensionHostState::Stopped, machine.GetState());
}

TEST(CExtensionHostStateMachine, StartTimeoutForcesTerminationAndSchedulesRetry)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);

	const auto actions = machine.Tick(start + 2s, 0.5);
	ASSERT_EQ(2u, actions.size());
	EXPECT_EQ(EExtensionHostLifecycleActionKind::ForceTerminate, actions[0].kind);
	EXPECT_EQ(EExtensionHostLifecycleActionKind::ScheduleRetry, actions[1].kind);
	EXPECT_EQ(100ms, actions[1].delay);
	EXPECT_EQ(EExtensionHostState::Failed, machine.GetState());
}

TEST(CExtensionHostStateMachine, CrashRejectsPendingThenRetriesButBrokerLossReturnsAbsent)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);
	machine.OnHostReady(1, start);

	auto actions = machine.OnHostLost(1, start + 1s, EExtensionHostLossKind::HostCrash, "crash", 0.5);
	ASSERT_EQ(2u, actions.size());
	EXPECT_EQ(EExtensionHostLifecycleActionKind::RejectPendingHostLost, actions[0].kind);
	EXPECT_EQ(EExtensionHostLifecycleActionKind::ScheduleRetry, actions[1].kind);
	EXPECT_EQ(EExtensionHostState::Failed, machine.GetState());
	ExpectSingleAction(machine.Tick(start + 1100ms), EExtensionHostLifecycleActionKind::StartHost, 2);
	machine.OnHostReady(2, start + 1100ms);

	actions = machine.OnHostLost(2, start + 2s, EExtensionHostLossKind::BrokerLost, "broker lost");
	ExpectSingleAction(actions, EExtensionHostLifecycleActionKind::RejectPendingHostLost, 2);
	EXPECT_EQ(EExtensionHostState::Absent, machine.GetState());
	ExpectSingleAction(machine.AcquireLease(101, start + 3s), EExtensionHostLifecycleActionKind::StartHost, 3);
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());
}

TEST(CExtensionHostStateMachine, LeaseDuringQuiesceRestartsOnlyAfterOldHostTerminates)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);
	machine.OnHostReady(1, start);
	machine.ReleaseLease(101, start);
	machine.Tick(start + 60s);

	EXPECT_TRUE(machine.AcquireLease(202, start + 61s).empty());
	EXPECT_EQ(EExtensionHostState::Quiescing, machine.GetState());
	ExpectSingleAction(machine.OnQuiesceCompleted(1, start + 62s), EExtensionHostLifecycleActionKind::StartHost, 2);
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());
}

TEST(CExtensionHostStateMachine, QuiesceTimeoutHasExplicitTerminationAndRestartOwner)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);
	machine.OnHostReady(1, start);
	machine.ReleaseLease(101, start);
	machine.Tick(start + 60s);
	machine.AcquireLease(202, start + 61s);

	const auto actions = machine.Tick(start + 63s);
	ASSERT_EQ(2u, actions.size());
	EXPECT_EQ(EExtensionHostLifecycleActionKind::ForceTerminate, actions[0].kind);
	EXPECT_EQ(EExtensionHostLifecycleActionKind::StartHost, actions[1].kind);
	EXPECT_EQ(2u, actions[1].generation);
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());
}

TEST(CExtensionHostStateMachine, StaleGenerationCannotChangeCurrentState)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);

	EXPECT_TRUE(machine.OnHostReady(99, start).empty());
	EXPECT_TRUE(machine.OnHostStartFailed(99, start, "stale").empty());
	EXPECT_TRUE(machine.OnHostLost(99, start, EExtensionHostLossKind::HostCrash, "stale").empty());
	EXPECT_EQ(EExtensionHostState::Starting, machine.GetState());
	EXPECT_EQ(1u, machine.GetGeneration());
}

TEST(CExtensionHostStateMachine, ShutdownHasATerminalOutcomeAndRejectsNewLeases)
{
	CExtensionHostStateMachine machine(FastConfig());
	const TimePoint start{};
	machine.AcquireLease(101, start);
	machine.OnHostReady(1, start);

	ExpectSingleAction(machine.Shutdown(start), EExtensionHostLifecycleActionKind::BeginQuiesce, 1);
	EXPECT_TRUE(machine.IsShutdownRequested());
	ExpectSingleAction(machine.OnQuiesceCompleted(1, start + 1s), EExtensionHostLifecycleActionKind::Stopped, 1);
	EXPECT_EQ(EExtensionHostState::Stopped, machine.GetState());
	ExpectSingleAction(machine.AcquireLease(202, start + 2s), EExtensionHostLifecycleActionKind::LeaseRejected, 1);
	EXPECT_EQ(0u, machine.GetLeaseCount());
}
