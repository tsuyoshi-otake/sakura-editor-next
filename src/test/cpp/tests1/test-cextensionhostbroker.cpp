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

template<typename T>
constexpr bool HasExtensionHostSessionMember = requires(T value) {
	value.extensionHostSessionId;
};

class FakeProcess final : public IExtensionHostProcess {
public:
	void QueueStartResult(SExtensionHostProcessStartResult result)
	{
		m_startResults.push_back(std::move(result));
	}
	const std::vector<SExtensionHostLaunchOptions>& StartRequests() const noexcept { return m_starts; }
	std::uint32_t ProcessId() const noexcept { return m_processId; }
	void SetExitCode(std::uint32_t value) noexcept { m_exitCode = value; }

	SExtensionHostProcessStartResult Start(const SExtensionHostLaunchOptions& options) override
	{
		m_starts.push_back(options);
		if (!m_startResults.empty()) {
			auto result = std::move(m_startResults.front());
			m_startResults.pop_front();
			if (result.success) {
				m_processId = result.processId;
			}
			return result;
		}
		m_processId = m_nextProcessId++;
		return { true, m_processId, 0, {} };
	}
	std::optional<std::uint32_t> PollExitCode() const noexcept override { return m_exitCode; }
	bool WaitForExit(std::chrono::milliseconds) noexcept override { return m_exitCode.has_value(); }
	void Terminate(std::uint32_t code) noexcept override
	{
		m_terminations.push_back(code);
		m_processId = 0;
		m_exitCode.reset();
	}
	std::uint32_t GetProcessId() const noexcept override { return m_processId; }

private:
	std::deque<SExtensionHostProcessStartResult> m_startResults;
	std::vector<SExtensionHostLaunchOptions> m_starts;
	std::vector<std::uint32_t> m_terminations;
	std::optional<std::uint32_t> m_exitCode;
	std::uint32_t m_processId = 0;
	std::uint32_t m_nextProcessId = 7001;
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
private:
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
	ASSERT_EQ(1u, processView->StartRequests().size());
	const auto& launch = processView->StartRequests().front();
	EXPECT_EQ(1u, launch.generation);
	EXPECT_EQ(6001u, launch.brokerProcessId);
	EXPECT_EQ(L"0123456789abcdef0123456789abcdef", launch.bootId);
	EXPECT_EQ(L"\\\\.\\pipe\\sakura-exthost-" + launch.profileHash + L"-" + launch.bootId, launch.pipeName);
	const auto snapshot = broker.GetSnapshot();
	EXPECT_EQ(32u, snapshot.extensionHostSessionId.size());
	EXPECT_EQ(snapshot.extensionHostSessionId, broker.GetSnapshot().extensionHostSessionId);
	EXPECT_EQ(std::wstring::npos, launch.profileHash.find(snapshot.extensionHostSessionId));
	EXPECT_EQ(std::wstring::npos, launch.bootId.find(snapshot.extensionHostSessionId));
	EXPECT_EQ(std::wstring::npos, launch.pipeName.find(snapshot.extensionHostSessionId));
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::StartHost));
	EXPECT_EQ(2u, broker.GetSnapshot().leaseOwnerCount);
}

TEST(CExtensionHostBroker, NativeSessionIdIsNotALaunchOption)
{
	static_assert(!HasExtensionHostSessionMember<SExtensionHostLaunchOptions>);
	EXPECT_FALSE((HasExtensionHostSessionMember<SExtensionHostLaunchOptions>));
}

TEST(CExtensionHostBroker, HandshakeRequiresCurrentGenerationBootIdAndActualServerPid)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	const auto pid = processView->ProcessId();

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
	process->QueueStartResult({ false, 0, ERROR_FILE_NOT_FOUND, L"node missing" });
	process->QueueStartResult({ true, 7100, 0, {} });
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};

	broker.AcquireLease(101, start, 0.5);
	EXPECT_EQ(EExtensionHostState::Failed, broker.GetSnapshot().state);
	EXPECT_EQ(1u, processView->StartRequests().size());
	broker.Tick(start + 99ms, 0.5);
	EXPECT_EQ(1u, processView->StartRequests().size());
	broker.Tick(start + 100ms, 0.5);
	ASSERT_EQ(2u, processView->StartRequests().size());
	EXPECT_EQ(2u, processView->StartRequests().back().generation);
	EXPECT_EQ(EExtensionHostState::Starting, broker.GetSnapshot().state);
}

TEST(CExtensionHostBroker, RetryRotatesNativeSessionWithoutExposingItToHostLaunch)
{
	auto config = TestConfig();
	config.bootIdOverride.clear();
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(std::move(config), std::move(process), &observer);
	const TimePoint start{};

	broker.AcquireLease(101, start, 0.5);
	const auto firstSession = broker.GetSnapshot().extensionHostSessionId;
	ASSERT_EQ(32u, firstSession.size());
	broker.NotifyHostLost(1, EExtensionHostLossKind::HostCrash, "test crash", start, 0.5);
	broker.Tick(start + 100ms, 0.5);
	ASSERT_EQ(2u, processView->StartRequests().size());
	EXPECT_NE(processView->StartRequests()[0].bootId, processView->StartRequests()[1].bootId);
	EXPECT_NE(processView->StartRequests()[0].pipeName, processView->StartRequests()[1].pipeName);
	EXPECT_NE(firstSession, broker.GetSnapshot().extensionHostSessionId);
	EXPECT_EQ(std::wstring::npos, processView->StartRequests()[1].profileHash.find(firstSession));
	EXPECT_EQ(std::wstring::npos, processView->StartRequests()[1].bootId.find(firstSession));
	EXPECT_EQ(std::wstring::npos, processView->StartRequests()[1].pipeName.find(firstSession));
	EXPECT_EQ(processView->StartRequests()[1].bootId, broker.GetSnapshot().bootId);
}

TEST(CExtensionHostBroker, CrashRejectsPendingAndRestartsAfterBoundedBackoff)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->ProcessId(), broker.GetSnapshot().bootId, start));

	processView->SetExitCode(9);
	broker.Tick(start + 1s, 0.5);
	EXPECT_EQ(EExtensionHostState::Failed, broker.GetSnapshot().state);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::RejectPendingHostLost));
	broker.Tick(start + 1100ms, 0.5);
	ASSERT_EQ(2u, processView->StartRequests().size());
	EXPECT_EQ(2u, processView->StartRequests().back().generation);
}

TEST(CExtensionHostBroker, KeepAliveExitCompletesQuiesceWithoutCrashRetry)
{
	auto process = std::make_unique<FakeProcess>();
	auto* processView = process.get();
	RecordingObserver observer;
	CExtensionHostBroker broker(TestConfig(), std::move(process), &observer);
	const TimePoint start{};
	broker.AcquireLease(101, start);
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->ProcessId(), broker.GetSnapshot().bootId, start));
	broker.ReleaseLease(101, start);

	broker.Tick(start + 60s);
	EXPECT_EQ(EExtensionHostState::Quiescing, broker.GetSnapshot().state);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::BeginQuiesce));
	processView->SetExitCode(0);
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
	ASSERT_TRUE(broker.AcceptHandshake(1, processView->ProcessId(), broker.GetSnapshot().bootId, start));

	broker.Shutdown(start);
	EXPECT_EQ(EExtensionHostState::Quiescing, broker.GetSnapshot().state);
	EXPECT_TRUE(broker.GetSnapshot().extensionHostSessionId.empty());
	broker.NotifyQuiesceCompleted(1, start + 1s);
	EXPECT_EQ(EExtensionHostState::Stopped, broker.GetSnapshot().state);
	EXPECT_TRUE(broker.GetSnapshot().extensionHostSessionId.empty());
	broker.AcquireLease(202, start + 2s);
	EXPECT_EQ(1u, observer.Count(EExtensionHostLifecycleActionKind::LeaseRejected));
}

TEST(CExtensionHostBroker, ProfileHashAndRandomBootIdArePipeSafe)
{
	const auto hashA = CExtensionHostBroker::ComputeProfileHash(L"C:\\profiles\\unit");
	const auto hashB = CExtensionHostBroker::ComputeProfileHash(L"c:\\PROFILES\\unit\\.");
	const auto hashWithTrailingSeparator = CExtensionHostBroker::ComputeProfileHash(L"C:\\profiles\\unit\\");
	EXPECT_EQ(hashA, hashB);
	EXPECT_EQ(hashA, hashWithTrailingSeparator);
	EXPECT_EQ(32u, hashA.size());
	const auto bootA = CExtensionHostBroker::GenerateBootId();
	const auto bootB = CExtensionHostBroker::GenerateBootId();
	const auto sessionA = CExtensionHostBroker::GenerateExtensionHostSessionId();
	const auto sessionB = CExtensionHostBroker::GenerateExtensionHostSessionId();
	EXPECT_EQ(32u, bootA.size());
	EXPECT_EQ(32u, bootB.size());
	EXPECT_NE(bootA, bootB);
	EXPECT_EQ(32u, sessionA.size());
	EXPECT_EQ(32u, sessionB.size());
	EXPECT_NE(sessionA, sessionB);
}
