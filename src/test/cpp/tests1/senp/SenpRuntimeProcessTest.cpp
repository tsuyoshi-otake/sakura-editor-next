/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpEffectRuntime.h"
#include "senp/SenpContributionOwners.h"
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
		// _wdupenv_s is an MSVC-only secure variant with no MinGW-w64 import
		// library entry, and the three other readers of this variable already
		// use _wgetenv. One idiom across the four, and nothing here to free.
		if (const auto value = _wgetenv(L"SAKURA_SENP_RUNTIME_FIXTURES"); value && *value) fixtures = value;
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

TEST_F(SenpRuntimeProcess, RealGithubIssuesComponentPublishesTheFilteredPageAndNextCursor)
{
	auto launch = Launch();
	launch.hostExecutable = (fixtures / L"sakura-senp-host.exe").native();
	launch.modulePath = (fixtures / L"github-pull-requests-extension.wasm").native();
	std::ifstream digest(fixtures / L"github-pull-requests-extension.sha256");
	std::string hash;
	digest >> hash;
	ASSERT_EQ(hash.size(), 64U);
	launch.moduleSha256.assign(hash.begin(), hash.end());
	launch.extensionId = L"sakura-github-pull-requests";
	CSenpEffectRuntime runtime(std::move(launch));
	ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
	auto activation = WaitResult(runtime);
	ASSERT_TRUE(activation);
	ASSERT_EQ(activation->status, InvocationStatus::EffectsReady);
	ASSERT_EQ(activation->effects.size(), 2U);
	EXPECT_EQ(std::get<InvalidateTree>(activation->effects[0]).viewId, L"issues:github");

	ASSERT_EQ(runtime.Submit(Context(), TreeRequest{ L"issues:github", L"", L"" }, Clock::now() + 2s).status,
		AdmissionStatus::Accepted);
	auto requested = WaitResult(runtime);
	ASSERT_TRUE(requested);
	ASSERT_EQ(requested->status, InvocationStatus::EffectsReady);
	ASSERT_EQ(requested->effects.size(), 1U);
	const auto& read = std::get<StartToolRead>(requested->effects[0]);
	EXPECT_EQ(read.readId, L"issues:open:1");
	EXPECT_EQ(read.toolId, L"github");
	EXPECT_EQ(read.operation, L"repositoryRead");

	const std::wstring response = LR"({"body":[{"id":11,"number":7,"title":"Visible issue","state":"open","user":{"login":"octocat"},"labels":[{"name":"bug"}],"html_url":"https://github.com/o/r/issues/7","comments":2},{"id":12,"number":8,"title":"Filtered PR","state":"open","user":{"login":"hubot"},"labels":[],"html_url":"https://github.com/o/r/pull/8","comments":0,"pull_request":{}}],"nextPage":2})";
	ASSERT_EQ(runtime.Submit(Context(), ToolCompleted{ read.readId, CompletionStatus::Succeeded,
		response, L"" }, Clock::now() + 2s).status, AdmissionStatus::Accepted);
	auto displayed = WaitResult(runtime);
	ASSERT_TRUE(displayed);
	ASSERT_EQ(displayed->status, InvocationStatus::EffectsReady);
	ASSERT_EQ(displayed->effects.size(), 1U);
	const auto& page = std::get<PublishTreePage>(displayed->effects[0]);
	EXPECT_EQ(page.viewId, L"issues:github");
	EXPECT_EQ(page.items.size(), 1U);
	EXPECT_EQ(page.items[0].id, L"issue:11:7");
	EXPECT_EQ(page.items[0].label, L"#7 Visible issue");
	EXPECT_EQ(page.nextCursor, L"issues:open:2");
	EXPECT_EQ(page.status, PageStatus::Partial);
	runtime.Stop(StopReason::Shutdown);
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

TEST_F(SenpRuntimeProcess, ContributionOwnerUpdateFailureAndShutdownKeepPhysicalCleanup)
{
	struct Publication final : ISenpOwnerPublication {
		std::size_t& applied;
		explicit Publication(std::size_t& count) : applied(count) {}
		bool Validate(const InvocationResult& result) const noexcept override { return result.effects.size() <= 1; }
		bool Commit() noexcept override { return true; }
		bool Apply(InvocationResult) noexcept override { ++applied; return true; }
		void Revoke(StopReason) noexcept override {}
	};
	auto launch = Launch();
	launch.hostExecutable = (fixtures / L"sakura-senp-host.exe").native();
	launch.modulePath = (fixtures / L"extension.wasm").native();
	std::ifstream digest(fixtures / L"extension.sha256");
	std::string hash;
	digest >> hash;
	ASSERT_EQ(hash.size(), 64U);
	launch.moduleSha256.assign(hash.begin(), hash.end());
	std::size_t applied{};
	CSenpEffectRuntime* latest{}; // Borrow only while the just-activated owner is live.
	CSenpContributionOwners owners([&](EffectRuntimeLaunch value) {
		auto runtime = std::make_unique<CSenpEffectRuntime>(std::move(value));
		latest = runtime.get();
		return runtime;
	});
	const auto prepare = [&](EffectRuntimeLaunch value) {
		return owners.Prepare(std::move(value), std::wstring(64, L'a'),
			[&](const auto&, const auto*) { return std::make_unique<Publication>(applied); }, Clock::now());
	};
	const auto awaitTransition = [&]() -> std::optional<OwnerChangeResult> {
		const auto deadline = Clock::now() + 12s;
		while (Clock::now() < deadline) {
			owners.Poll(Clock::now());
			if (auto terminal = owners.TakeTransition()) return terminal;
			::Sleep(5);
		}
		return {};
	};
	const auto first = prepare(launch);
	ASSERT_EQ(first.status, OwnerChangeStatus::Accepted);
	const auto started = awaitTransition();
	ASSERT_TRUE(started);
	ASSERT_EQ(started->status, OwnerChangeStatus::Activated);
	ASSERT_TRUE(owners.IsCurrent(first.owner));
	const auto firstPid = latest->Snapshot().processId;
	ASSERT_NE(firstPid, 0U);
	auto bad = launch;
	bad.moduleSha256 = std::wstring(64, L'0');
	ASSERT_EQ(prepare(std::move(bad)).status, OwnerChangeStatus::Accepted);
	const auto failed = awaitTransition();
	ASSERT_TRUE(failed);
	EXPECT_EQ(failed->status, OwnerChangeStatus::Failed);
	EXPECT_TRUE(owners.IsCurrent(first.owner));
	const auto updated = prepare(launch);
	ASSERT_EQ(updated.status, OwnerChangeStatus::Accepted);
	const auto activated = awaitTransition();
	ASSERT_TRUE(activated);
	ASSERT_EQ(activated->status, OwnerChangeStatus::Activated);
	EXPECT_FALSE(owners.IsCurrent(first.owner));
	EXPECT_TRUE(owners.IsCurrent(updated.owner));
	const auto secondPid = latest->Snapshot().processId;
	EXPECT_NE(firstPid, secondPid);
	EXPECT_EQ(applied, 2U);
	ASSERT_TRUE(owners.Revoke(launch.extensionId, StopReason::Disabled));
	EXPECT_FALSE(owners.IsCurrent(updated.owner));
	EXPECT_TRUE(owners.Close());
	EXPECT_EQ(owners.Snapshot().retiring, 0U);
	EXPECT_EQ(owners.Snapshot().cleanupFailed, 0U);
	for (auto pid : { firstPid, secondPid }) {
		const auto process = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (process) { EXPECT_EQ(::WaitForSingleObject(process, 0), WAIT_OBJECT_0); ::CloseHandle(process); }
		else EXPECT_EQ(::GetLastError(), ERROR_INVALID_PARAMETER);
	}
}

} // namespace
