/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostBroker.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using TimePoint = CExtensionHostBroker::TimePoint;

class FakeProcess final : public IExtensionHostProcess {
public:
	SExtensionHostProcessStartResult Start(const SExtensionHostLaunchOptions& options) override
	{
		starts.push_back(options);
		if (!startResults.empty()) {
			auto result = std::move(startResults.front());
			startResults.pop_front();
			if (result.success) {
				processId = result.processId;
			}
			return result;
		}
		processId = nextProcessId++;
		return { true, processId, 0, {} };
	}
	std::optional<std::uint32_t> PollExitCode() const noexcept override { return exitCode; }
	bool WaitForExit(std::chrono::milliseconds) noexcept override { return exitCode.has_value(); }
	void Terminate(std::uint32_t code) noexcept override
	{
		terminations.push_back(code);
		processId = 0;
		exitCode.reset();
	}
	std::uint32_t GetProcessId() const noexcept override { return processId; }

	std::deque<SExtensionHostProcessStartResult> startResults;
	std::vector<SExtensionHostLaunchOptions> starts;
	std::vector<std::uint32_t> terminations;
	mutable std::optional<std::uint32_t> exitCode;
	std::uint32_t processId = 0;
	std::uint32_t nextProcessId = 7001;
};

class RecordingObserver final : public IExtensionHostBrokerObserver {
public:
	void OnExtensionHostLifecycleAction(
		const SExtensionHostLifecycleAction& action,
		const SExtensionHostBrokerSnapshot& snapshot) noexcept override
	{
		actions.push_back(action);
		snapshots.push_back(snapshot);
	}
	std::size_t Count(EExtensionHostLifecycleActionKind kind) const
	{
		return static_cast<std::size_t>(std::count_if(actions.begin(), actions.end(), [kind](const auto& action) {
			return action.kind == kind;
		}));
	}
	std::vector<SExtensionHostLifecycleAction> actions;
	std::vector<SExtensionHostBrokerSnapshot> snapshots;
};

SExtensionHostBrokerConfig TestConfig()
{
	SExtensionHostBrokerConfig config;
	config.nodeExecutable = L"node.exe";
	config.hostBundle = L"extension-host.js";
	config.profileDirectory = L"C:\\profiles\\unit";
	config.bootIdOverride = L"0123456789abcdef0123456789abcdef";
	config.brokerProcessId = 6001;
	config.lifecycle.keepAlive = 60s;
	config.lifecycle.startTimeout = 2s;
	config.lifecycle.quiesceTimeout = 3s;
	config.lifecycle.initialBackoff = 100ms;
	config.lifecycle.maximumBackoff = 800ms;
	config.lifecycle.maximumRetryCount = 3;
	config.lifecycle.jitterRatio = 0.20;
	return config;
}

} // namespace

TEST(CExtensionHostBroker, LeaseStartsOneGenerationWithBoundIdentity)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};

	broker.AcquireLease(101, start);
	broker.AcquireLease(202, start);
	ASSERT_EQ(1u, processView->starts.size());
	const auto& launch = processView->starts.front();
	EXPECT_EQ(1u, launch.generation);
	EXPECT_EQ(6001u, launch.brokerProcessId);
	EXPECT_EQ(L"0123456789abcdef0123456789abcdef", launch.bootId);
	EXPECT_EQ(L"\\\\.\\pipe\\sakura-exthost-" + launch.profileHash + L"-" + launch.bootId, launch.pipeName);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::StartHost));
	EXPECT_EQ(2u, broker.GetSnapshot().leaseOwnerCount);
}

TEST(CExtensionHostBroker, HandshakeRequiresCurrentGenerationBootIdAndActualServerPid)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	const auto pid = processView->processId;

	EXPECT_FALSE(broker.AcceptHandshake(99, pid, broker.GetSnapshot().bootId, start));
	EXPECT_EQ(EExtensionHostState::Starting, broker.GetSnapshot().state);
	EXPECT_FALSE(broker.AcceptHandshake(1, pid + 1, broker.GetSnapshot().bootId, start));
	EXPECT_EQ(EExtensionHostState::Failed, broker.GetSnapshot().state);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::RejectPendingHostLost));
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::ScheduleRetry));
}

TEST(CExtensionHostBroker, StartFailureRetriesWithANewGenerationAtDeadline)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	process->startResults.push_back({ false, 0, ERROR_FILE_NOT_FOUND, L"node missing" });
	process->startResults.push_back({ true, 7100, 0, {} });
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};

	broker.AcquireLease(101, start, 0.5);
	EXPECT_EQ(EExtensionHostState::Failed, broker.GetSnapshot().state);
	EXPECT_EQ(1u, processView->starts.size());
	broker.Tick(start + 99ms, 0.5);
	EXPECT_EQ(1u, processView->starts.size());
	broker.Tick(start + 100ms, 0.5);
	ASSERT_EQ(2u, processView->starts.size());
	EXPECT_EQ(2u, processView->starts.back().generation);
	EXPECT_EQ(EExtensionHostState::Starting, broker.GetSnapshot().state);
}

TEST(CExtensionHostBroker, RetryUsesANewUnpredictableBootIdentityAndPipe)
{
	auto config = TestConfig();
	config.bootIdOverride.clear();
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	process->startResults.push_back({ false, 0, ERROR_FILE_NOT_FOUND, L"node missing" });
	process->startResults.push_back({ true, 7100, 0, {} });
	CExtensionHostBroker broker(std::move(config), std::move(process));
	const TimePoint start{};

	broker.AcquireLease(101, start, 0.5);
	broker.Tick(start + 100ms, 0.5);
	ASSERT_EQ(2u, processView->starts.size());
	EXPECT_NE(processView->starts[0].bootId, processView->starts[1].bootId);
	EXPECT_NE(processView->starts[0].pipeName, processView->starts[1].pipeName);
	EXPECT_EQ(processView->starts[1].bootId, broker.GetSnapshot().bootId);
}

TEST(CExtensionHostBroker, CrashRejectsPendingAndRestartsAfterBoundedBackoff)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->processId, broker.GetSnapshot().bootId, start));

	processView->exitCode = 9;
	broker.Tick(start + 1s, 0.5);
	EXPECT_EQ(EExtensionHostState::Failed, broker.GetSnapshot().state);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::RejectPendingHostLost));
	broker.Tick(start + 1100ms, 0.5);
	ASSERT_EQ(2u, processView->starts.size());
	EXPECT_EQ(2u, processView->starts.back().generation);
}

TEST(CExtensionHostBroker, KeepAliveExitCompletesQuiesceWithoutCrashRetry)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->processId, broker.GetSnapshot().bootId, start));
	broker.ReleaseLease(101, start);

	broker.Tick(start + 60s);
	EXPECT_EQ(EExtensionHostState::Quiescing, broker.GetSnapshot().state);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::BeginQuiesce));
	processView->exitCode = 0;
	broker.Tick(start + 61s);
	EXPECT_EQ(EExtensionHostState::Stopped, broker.GetSnapshot().state);
	EXPECT_EQ(0u, observer.Count(EExtensionHostLifecycleActionKind::ScheduleRetry));
}

TEST(CExtensionHostBroker, ShutdownOwnsTerminalStateAndRejectsLaterLease)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->processId, broker.GetSnapshot().bootId, start));

	broker.Shutdown(start);
	EXPECT_EQ(EExtensionHostState::Quiescing, broker.GetSnapshot().state);
	broker.NotifyQuiesceCompleted(1, start + 1s);
	EXPECT_EQ(EExtensionHostState::Stopped, broker.GetSnapshot().state);
	broker.AcquireLease(202, start + 2s);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::LeaseRejected));
}

TEST(CExtensionHostBroker, ProfileHashAndRandomBootIdArePipeSafe)
{
	const auto hashA = CExtensionHostBroker::ComputeProfileHash(L"C:\\profiles\\unit");
	const auto hashB = CExtensionHostBroker::ComputeProfileHash(L"c:\\PROFILES\\unit\\.");
	EXPECT_EQ(hashA, hashB);
	EXPECT_EQ(32u, hashA.size());
	const auto bootA = CExtensionHostBroker::GenerateBootId();
	const auto bootB = CExtensionHostBroker::GenerateBootId();
	EXPECT_EQ(32u, bootA.size());
	EXPECT_EQ(32u, bootB.size());
	EXPECT_NE(bootA, bootB);
}
