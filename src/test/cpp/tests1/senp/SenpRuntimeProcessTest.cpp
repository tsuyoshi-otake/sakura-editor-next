/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpEffectRuntime.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace {
using namespace senp;
using namespace senp::effect;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class SenpRuntimeProcess : public ::testing::Test {
protected:
	std::filesystem::path fixtures;
	void SetUp() override
	{
		wchar_t* value{};
		std::size_t size{};
		_wdupenv_s(&value, &size, L"SAKURA_SENP_RUNTIME_FIXTURES");
		if (value) fixtures = value;
		std::free(value);
		if (fixtures.empty()) GTEST_SKIP() << "Run tools/verify-senp-runtime.py to build and verify process fixtures";
		ASSERT_TRUE(std::filesystem::is_regular_file(fixtures / "senp-host-fixture.exe"));
	}
	OperationContext Context() const { return { L"", 4, 2, 0, 1 }; }
	EffectRuntimeLaunch Launch(std::wstring_view scenario = L"echo") const
	{
		return { (fixtures / L"senp-host-fixture.exe").native(),
			(fixtures / (std::wstring(scenario) + L".wasm")).native(), std::wstring(64, L'0'),
			L"test.extension", Context(), 9 };
	}
	std::optional<InvocationResult> WaitResult(CSenpEffectRuntime& runtime) const
	{
		const auto deadline = Clock::now() + 12s;
		while (Clock::now() < deadline) {
			if (auto result = runtime.TakeCompleted()) return result;
			if (runtime.Snapshot().workerExited) return std::nullopt;
			::Sleep(5);
		}
		return std::nullopt;
	}
	void Activate(CSenpEffectRuntime& runtime) const
	{
		ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
		const auto result = WaitResult(runtime);
		ASSERT_TRUE(result);
		ASSERT_EQ(result->status, InvocationStatus::EffectsReady);
		ASSERT_EQ(runtime.Snapshot().phase, RuntimePhase::Active);
	}
	void VerifyExit(CSenpEffectRuntime& runtime) const
	{
		runtime.Join();
		const auto snapshot = runtime.Snapshot();
		EXPECT_EQ(snapshot.phase, RuntimePhase::Stopped);
		EXPECT_TRUE(snapshot.workerExited);
		EXPECT_TRUE(snapshot.processExitConfirmed);
		EXPECT_EQ(snapshot.pending, 0U);
		if (snapshot.processId == 0) return;
		const auto process = ::OpenProcess(SYNCHRONIZE, FALSE, snapshot.processId);
		if (process) {
			EXPECT_EQ(::WaitForSingleObject(process, 0), WAIT_OBJECT_0);
			::CloseHandle(process);
		} else {
			EXPECT_EQ(::GetLastError(), ERROR_INVALID_PARAMETER);
		}
	}
};

TEST_F(SenpRuntimeProcess, RealWasmV2ActivationEventAndFuelTrapUseTheNativeSession)
{
	auto launch = Launch();
	launch.hostExecutable = (fixtures / L"sakura-senp-host.exe").native();
	launch.modulePath = (fixtures / L"extension.wasm").native();
	std::ifstream digest(fixtures / L"extension.sha256");
	std::string hash;
	digest >> hash;
	ASSERT_EQ(hash.size(), 64U);
	launch.moduleSha256.assign(hash.begin(), hash.end());
	CSenpEffectRuntime runtime(std::move(launch));
	ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
	auto activation = WaitResult(runtime);
	ASSERT_TRUE(activation);
	ASSERT_EQ(activation->status, InvocationStatus::EffectsReady);
	ASSERT_EQ(activation->effects.size(), 1U);
	EXPECT_EQ(std::get<InvalidateTree>(activation->effects.front()).viewId, L"test.issues");
	ASSERT_EQ(runtime.Submit(Context(), TreeRequest{ L"test.issues", L"", L"" }, Clock::now() + 2s).status, AdmissionStatus::Accepted);
	auto event = WaitResult(runtime);
	ASSERT_TRUE(event);
	ASSERT_EQ(event->status, InvocationStatus::EffectsReady);
	ASSERT_EQ(event->effects.size(), 1U);
	EXPECT_EQ(std::get<OpenDocument>(event->effects.front()).resourceId, L"test:issue/1");
	ASSERT_EQ(runtime.Submit(Context(), CommandInvoked{ L"test.spin", {} }, Clock::now() + 2s).status, AdmissionStatus::Accepted);
	auto trap = WaitResult(runtime);
	ASSERT_TRUE(trap);
	EXPECT_EQ(trap->status, InvocationStatus::HostUnavailable);
	EXPECT_TRUE(trap->effects.empty());
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, FaultyPeersCannotLeaveBlockedIoOrPendingInvocations)
{
	for (const auto scenario : { L"blocked-read", L"blocked-write", L"partial", L"oversized", L"crash", L"memory-limit" }) {
		SCOPED_TRACE(scenario);
		CSenpEffectRuntime runtime(Launch(scenario));
		Activate(runtime);
		const auto started = Clock::now();
		ToolCompleted large{ L"read:1", CompletionStatus::Succeeded, std::wstring(65536, L'x'), L"" };
		const auto admission = runtime.Submit(Context(), large, Clock::now() + 5s);
		ASSERT_EQ(admission.status, AdmissionStatus::Accepted);
		auto result = WaitResult(runtime);
		ASSERT_TRUE(result);
		EXPECT_EQ(result->context.operationId, admission.operationId);
		EXPECT_EQ(result->status, InvocationStatus::HostUnavailable);
		EXPECT_TRUE(result->effects.empty());
		VerifyExit(runtime);
		EXPECT_LT(Clock::now() - started, 4s);
		EXPECT_FALSE(runtime.TakeCompleted());
	}
}

TEST_F(SenpRuntimeProcess, DeadlineOwnsItsResultAndCrashFinalizesTheOtherAcceptedRequests)
{
	CSenpEffectRuntime runtime(Launch(L"blocked-read"));
	Activate(runtime);
	std::set<std::wstring> accepted;
	for (int i = 0; i < 4; ++i) {
		const auto admission = runtime.Submit(Context(), TreeRequest{ L"issues", L"", L"" }, Clock::now() + (i == 0 ? 80ms : 2000ms));
		ASSERT_EQ(admission.status, AdmissionStatus::Accepted);
		accepted.insert(admission.operationId);
	}
	std::size_t timedOut{};
	for (int i = 0; i < 4; ++i) {
		const auto result = WaitResult(runtime);
		ASSERT_TRUE(result);
		EXPECT_EQ(accepted.erase(result->context.operationId), 1U);
		timedOut += result->status == InvocationStatus::TimedOut;
		EXPECT_TRUE(result->status == InvocationStatus::TimedOut || result->status == InvocationStatus::HostUnavailable);
	}
	EXPECT_EQ(timedOut, 1U);
	EXPECT_TRUE(accepted.empty());
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, ConcurrentStopsAndJoinsHaveOneWorkerCleanupOwner)
{
	CSenpEffectRuntime runtime(Launch(L"blocked-read"));
	Activate(runtime);
	ASSERT_EQ(runtime.Submit(Context(), TreeRequest{ L"issues", L"", L"" }, Clock::now() + 5s).status, AdmissionStatus::Accepted);
	::Sleep(30);
	const auto started = Clock::now();
	std::thread first([&] { runtime.Stop(StopReason::Disabled); runtime.Join(); });
	std::thread second([&] { runtime.Stop(StopReason::Updated); runtime.Join(); });
	first.join();
	second.join();
	VerifyExit(runtime);
	EXPECT_LT(Clock::now() - started, 3s);
	const auto result = runtime.TakeCompleted();
	ASSERT_TRUE(result);
	EXPECT_EQ(result->status, InvocationStatus::Cancelled);
	EXPECT_FALSE(runtime.TakeCompleted());
	EXPECT_EQ(runtime.Start().status, AdmissionStatus::Unavailable);
}

TEST_F(SenpRuntimeProcess, FailedLaunchAndImmediateStopStillProduceOneTerminalOutcome)
{
	for (bool missing : { false, true }) {
		auto launch = Launch();
		if (missing) launch.hostExecutable = (fixtures / L"absent.exe").native();
		CSenpEffectRuntime runtime(std::move(launch));
		ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
		if (!missing) runtime.Stop(StopReason::Disabled);
		const auto result = WaitResult(runtime);
		ASSERT_TRUE(result);
		EXPECT_EQ(result->status, missing ? InvocationStatus::HostUnavailable : InvocationStatus::Cancelled);
		VerifyExit(runtime);
		EXPECT_FALSE(runtime.TakeCompleted());
	}
}

} // namespace
