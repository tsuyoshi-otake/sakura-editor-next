/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpEffectRuntime.h"
#include "senp/SenpContributionOwners.h"
#include "senp/SenpRuntimeService.h"
#include "senp/SenpManagementService.h"
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <thread>
#include <vector>
#include <tlhelp32.h>

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
		const auto admission = runtime.Submit(Context(), large, Clock::now() + 2s);
		ASSERT_EQ(admission.status, AdmissionStatus::Accepted);
		auto result = WaitResult(runtime);
		ASSERT_TRUE(result);
		EXPECT_EQ(result->context.operationId, admission.operationId);
		// The exchange now waits until the invocation's own deadline, so a
		// blocked peer ends the invocation there - TimedOut when the drain sees
		// the deadline first, HostUnavailable when the pipe failure lands first.
		EXPECT_TRUE(result->status == InvocationStatus::TimedOut
			|| result->status == InvocationStatus::HostUnavailable);
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

TEST_F(SenpRuntimeProcess, LaunchedHostRunsInsideTheSharedChildJob)
{
	CSenpEffectRuntime runtime(Launch());
	Activate(runtime);
	const auto pid = runtime.Snapshot().processId;
	ASSERT_NE(pid, 0U);
	const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	ASSERT_NE(process, nullptr);
	BOOL inJob = FALSE;
	ASSERT_TRUE(::IsProcessInJob(process, nullptr, &inJob));
	EXPECT_TRUE(inJob);
	::CloseHandle(process);
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, WarmExchangeIsBoundByTheInvocationDeadlineNotAFixedSecond)
{
	// The peer answers after 1.5 s. The exchange bound is the invocation's own
	// deadline (<= 10 s), so a healthy-but-slow reply still completes (#345).
	CSenpEffectRuntime runtime(Launch(L"delayed-reply"));
	Activate(runtime);
	const auto admission = runtime.Submit(Context(), TreeRequest{ L"issues", L"", L"" }, Clock::now() + 10s);
	ASSERT_EQ(admission.status, AdmissionStatus::Accepted);
	const auto result = WaitResult(runtime);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->context.operationId, admission.operationId);
	EXPECT_EQ(result->status, InvocationStatus::EffectsReady);
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, UnansweredInvocationEndsAtItsDeadlineAndStopsTheHost)
{
	// A peer that never replies fails closed at the invocation deadline; the
	// exchange fails there too, which ends the transport (HostUnavailable for
	// the owner, an owner terminal).
	CSenpEffectRuntime runtime(Launch(L"blocked-read"));
	Activate(runtime);
	const auto started = Clock::now();
	ASSERT_EQ(runtime.Submit(Context(), TreeRequest{ L"issues", L"", L"" }, Clock::now() + 2s).status,
		AdmissionStatus::Accepted);
	const auto result = WaitResult(runtime);
	ASSERT_TRUE(result);
	EXPECT_TRUE(result->status == InvocationStatus::TimedOut
		|| result->status == InvocationStatus::HostUnavailable);
	EXPECT_GE(Clock::now() - started, 1500ms);
	EXPECT_LT(Clock::now() - started, 4s);
	// TakeCompleted expires the invocation on this thread, which can run a few
	// milliseconds before the worker's own wait reaches the same deadline. The
	// worker must then stop the host by itself - before any Stop or Join here,
	// which would otherwise produce Stopped on their own.
	const auto stopBy = Clock::now() + 1s;
	while (runtime.Snapshot().phase != RuntimePhase::Stopped && Clock::now() < stopBy) ::Sleep(5);
	EXPECT_EQ(runtime.Snapshot().phase, RuntimePhase::Stopped);
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, ColdStartHonoursItsOwnBudgetAcrossWaitSlices)
{
	// Hello lands at 1 s while each wait slice is only 200 ms: the exchange
	// must keep waiting until its absolute deadline instead of failing at the
	// first slice.
	auto launch = Launch(L"delayed-hello");
	launch.waitSlice = 200ms;
	CSenpEffectRuntime runtime(std::move(launch));
	const auto started = Clock::now();
	ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
	const auto activation = WaitResult(runtime);
	ASSERT_TRUE(activation);
	EXPECT_EQ(activation->status, InvocationStatus::EffectsReady);
	EXPECT_GE(Clock::now() - started, 900ms);
	VerifyExit(runtime);
}

TEST_F(SenpRuntimeProcess, UnansweredHelloFailsAtTheColdStartBudgetNotEarlierNotLater)
{
	auto launch = Launch(L"blocked-hello");
	launch.coldStartBudget = 2s;
	launch.waitSlice = 200ms;
	CSenpEffectRuntime runtime(std::move(launch));
	const auto started = Clock::now();
	ASSERT_EQ(runtime.Start().status, AdmissionStatus::Accepted);
	const auto activation = WaitResult(runtime);
	ASSERT_TRUE(activation);
	EXPECT_NE(activation->status, InvocationStatus::EffectsReady);
	const auto elapsed = Clock::now() - started;
	EXPECT_GE(elapsed, 1500ms);
	EXPECT_LT(elapsed, 3s);
	VerifyExit(runtime);
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

// Management authority for the v1 decoration worker's fixture tests: a
// Ready catalog the worker polls through Revision()/Snapshot().
class V1FakeManagement final : public ISenpManagementService {
public:
	void SetExtensions(std::vector<ExtensionDescriptor> extensions)
	{
		std::lock_guard lock(m_mutex);
		m_snapshot.state = EManagementState::Ready;
		m_snapshot.extensions = std::move(extensions);
		++m_snapshot.revision;
	}

	// Holds the worker inside its next Revision() poll until Release(), so
	// requests offered meanwhile drain as one batch.
	void Hold() { m_hold.store(true); }
	void Release() { m_hold.store(false); }

	[[nodiscard]] ManagementOperationResult Start() override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult InstallDeveloperPackage(
		std::wstring_view, bool) override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult InstallBuiltInPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult UninstallBuiltInPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult SetExtensionEnabled(
		std::wstring_view, bool) override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult UninstallPackage(
		std::wstring_view) override { return Unavailable(); }
	[[nodiscard]] ManagementOperationResult Refresh() override { return Unavailable(); }
	void Stop() noexcept override {}
	[[nodiscard]] ManagementSnapshot Snapshot() const override
	{
		std::lock_guard lock(m_mutex);
		return m_snapshot;
	}
	[[nodiscard]] std::uint64_t Revision() const override
	{
		const auto deadline = Clock::now() + 30s;
		while (m_hold.load() && Clock::now() < deadline) ::Sleep(1);
		std::lock_guard lock(m_mutex);
		return m_snapshot.revision;
	}

private:
	[[nodiscard]] ManagementOperationResult Unavailable() const
	{
		return { EManagementOperationStatus::Unavailable, {} };
	}

	mutable std::mutex m_mutex;
	ManagementSnapshot m_snapshot;
	std::atomic<bool> m_hold{ false };
};

// The live processes carrying a fixture executable name. The v1 tests
// require that none survive their host terminal, and compare PIDs across a
// revision change.
std::set<DWORD> FixtureProcessIds(const wchar_t* name)
{
	const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return {};
	std::set<DWORD> ids;
	PROCESSENTRY32W entry{ sizeof(entry) };
	for (BOOL next = ::Process32FirstW(snapshot, &entry); next; next = ::Process32NextW(snapshot, &entry)) {
		if (_wcsicmp(entry.szExeFile, name) == 0) ids.insert(entry.th32ProcessID);
	}
	::CloseHandle(snapshot);
	return ids;
}

// Polls a v1 decoration request until the worker stored its result; the
// diagnostic is published before the batch's results are cached.
std::optional<std::vector<IndentDecoration>> WaitDecorations(
	ISenpRuntimeService& service, std::wstring_view line)
{
	const auto deadline = Clock::now() + 12s;
	for (;;) {
		auto result = service.RequestIndentDecorations(line, 4, 0);
		if (result || Clock::now() >= deadline) return result;
		::Sleep(5);
	}
}

TEST_F(SenpRuntimeProcess, V1HostOverJobMemoryCapIsRetiredWithDiagnostic)
{
	// The fixture allocates ~768 MiB on every `decorate`; the shared 512 MiB
	// job cap kills it mid-exchange. A job kill is a crash like any other
	// under VS Code's policy (#364): the first two restart the host at the
	// next boundary, the third inside five minutes retires the identity with
	// the diagnostic. Unlike the v2 memory-limit oracle in
	// FaultyPeersCannotLeaveBlockedIoOrPendingInvocations, a v1 host restarts.
	V1FakeManagement management;
	ExtensionDescriptor extension;
	extension.id = L"ext.memory";
	extension.enabled = true;
	extension.contributesIndentDecorations = true;
	extension.modulePath = (fixtures / L"memory-limit.wasm").native();
	extension.moduleSha256 = std::wstring(64, L'0');
	management.SetExtensions({ extension });

	detail::SenpRuntimeHooks hooks;
	hooks.hostExecutable = (fixtures / L"senp-host-fixture.exe").native();
	const auto service = senp::CreateWin32SenpRuntimeService(management, hooks);
	ASSERT_TRUE(service->Start());
	// One line per crash, each sent only after the previous crash restarted
	// the host: two lines in one batch would be one crash, not two. A
	// restart is the first live host published at least two revisions after
	// the request - the first request also opens the host, so a live host
	// one revision later is that open, not the restart.
	for (const auto line : { L"\tfirst", L"\tsecond" }) {
		const auto requested = service->Snapshot().revision;
		(void)service->RequestIndentDecorations(line, 4, 0);
		const auto restartDeadline = Clock::now() + 12s;
		for (;;) {
			const auto snapshot = service->Snapshot();
			if ((snapshot.revision >= requested + 2 && snapshot.activeHosts == 1U)
				|| snapshot.state == ERuntimeState::ReadyWithDiagnostics
				|| Clock::now() >= restartDeadline) break;
			::Sleep(5);
		}
		const auto restarted = service->Snapshot();
		ASSERT_EQ(restarted.state, ERuntimeState::Ready);
		ASSERT_EQ(restarted.activeHosts, 1U);
	}
	(void)service->RequestIndentDecorations(L"\tthird", 4, 0);
	const auto deadline = Clock::now() + 12s;
	while (service->Snapshot().state != ERuntimeState::ReadyWithDiagnostics
		&& Clock::now() < deadline) ::Sleep(5);
	const auto snapshot = service->Snapshot();
	ASSERT_EQ(snapshot.state, ERuntimeState::ReadyWithDiagnostics);
	EXPECT_NE(snapshot.diagnostic.find(L"ext.memory"), std::wstring::npos);
	EXPECT_EQ(snapshot.activeHosts, 0U);
	// The 12 s bound above is well inside the fixture's own 30 s watchdog.
	// The line completes from the remaining (zero) hosts and is cached.
	const auto cached = WaitDecorations(*service, L"\tvalue");
	ASSERT_TRUE(cached.has_value());
	EXPECT_TRUE(cached->empty());
	service->Stop();
	EXPECT_TRUE(FixtureProcessIds(L"senp-host-fixture.exe").empty());
}

TEST_F(SenpRuntimeProcess, NonV1HelloPeerIsRefusedAtOpenAndNeverDecorated)
{
	// The peer answers the hello with protocol 2: Open must refuse it before
	// any `decorate` frame exists (the hello precedes the exchange in code),
	// and the refused identity is retired like any other failure.
	V1FakeManagement management;
	ExtensionDescriptor extension;
	extension.id = L"ext.hello";
	extension.enabled = true;
	extension.contributesIndentDecorations = true;
	extension.modulePath = (fixtures / L"wrong-hello.wasm").native();
	extension.moduleSha256 = std::wstring(64, L'0');
	management.SetExtensions({ extension });

	detail::SenpRuntimeHooks hooks;
	hooks.hostExecutable = (fixtures / L"senp-host-fixture.exe").native();
	const auto service = senp::CreateWin32SenpRuntimeService(management, hooks);
	ASSERT_TRUE(service->Start());
	(void)service->RequestIndentDecorations(L"\tvalue", 4, 0);
	const auto deadline = Clock::now() + 12s;
	while (service->Snapshot().state != ERuntimeState::ReadyWithDiagnostics
		&& Clock::now() < deadline) ::Sleep(5);
	const auto snapshot = service->Snapshot();
	ASSERT_EQ(snapshot.state, ERuntimeState::ReadyWithDiagnostics);
	EXPECT_NE(snapshot.diagnostic.find(L"ext.hello"), std::wstring::npos);
	EXPECT_EQ(snapshot.activeHosts, 0U);
	// A later revision carrying the same identity leaves it retired: the
	// next line drives a reconcile pass and still completes with no host.
	management.SetExtensions({ extension });
	ASSERT_TRUE(WaitDecorations(*service, L"\tafter").has_value());
	EXPECT_EQ(service->Snapshot().activeHosts, 0U);
	EXPECT_NE(service->Snapshot().diagnostic.find(L"ext.hello"), std::wstring::npos);
	service->Stop();
	EXPECT_TRUE(FixtureProcessIds(L"senp-host-fixture.exe").empty());
}

// A v1 extension backed by the fixture peer in its echo mode.
ExtensionDescriptor V1EchoExtension(const std::filesystem::path& fixtures, std::wstring id)
{
	ExtensionDescriptor extension;
	extension.id = std::move(id);
	extension.enabled = true;
	extension.contributesIndentDecorations = true;
	extension.modulePath = (fixtures / L"echo.wasm").native();
	extension.moduleSha256 = std::wstring(64, L'0');
	return extension;
}

TEST_F(SenpRuntimeProcess, V1BatchedFrameRoundTripsThroughTheRealPipe)
{
	// #348: four lines queued while the worker is held drain as one
	// `decorate` frame. The peer decodes that frame strictly and answers one
	// slot per line (depth = the line's index in the frame, length = its
	// UTF-16 length + 1), so each cached result proves both the native frame
	// encoding - quotes, backslashes, control characters, non-ASCII - and
	// the slot-to-line mapping.
	V1FakeManagement management;
	management.SetExtensions({ V1EchoExtension(fixtures, L"ext.echo") });
	detail::SenpRuntimeHooks hooks;
	hooks.hostExecutable = (fixtures / L"senp-host-fixture.exe").native();
	const auto service = senp::CreateWin32SenpRuntimeService(management, hooks);
	management.Hold();
	ASSERT_TRUE(service->Start());
	const std::vector<std::wstring> lines{
		L"\tvalue", L"a\"b\\c", std::wstring(L"x") + wchar_t{ 1 } + L"y",
		std::wstring{ wchar_t{ 0x65e5 }, wchar_t{ 0x672c } } + L"\U0001F600" };
	for (const auto& line : lines) (void)service->RequestIndentDecorations(line, 4, 0);
	management.Release();
	for (std::size_t index = 0; index < lines.size(); ++index) {
		const auto result = WaitDecorations(*service, lines[index]);
		ASSERT_TRUE(result.has_value()) << index;
		ASSERT_EQ(result->size(), 1U) << index;
		EXPECT_EQ(result->front().depth, index) << index;
		EXPECT_EQ(result->front().visualLength, lines[index].size() + 1) << index;
	}
	EXPECT_EQ(service->Snapshot().state, ERuntimeState::Ready);
	service->Stop();
	EXPECT_TRUE(FixtureProcessIds(L"senp-host-fixture.exe").empty());
}

TEST_F(SenpRuntimeProcess, V1RevisionChangeKeepsTheUnchangedHostProcess)
{
	// #350: removing A closes only A's process; B's process was already
	// running before the revision change and still serves after it.
	V1FakeManagement management;
	const auto a = V1EchoExtension(fixtures, L"ext.a");
	const auto b = V1EchoExtension(fixtures, L"ext.b");
	management.SetExtensions({ a, b });
	detail::SenpRuntimeHooks hooks;
	hooks.hostExecutable = (fixtures / L"senp-host-fixture.exe").native();
	const auto service = senp::CreateWin32SenpRuntimeService(management, hooks);
	ASSERT_TRUE(service->Start());
	const auto first = WaitDecorations(*service, L"\tfirst");
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->size(), 2U);
	const auto before = FixtureProcessIds(L"senp-host-fixture.exe");
	ASSERT_EQ(before.size(), 2U);

	management.SetExtensions({ b });
	const auto second = WaitDecorations(*service, L"\tsecond");
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(second->size(), 1U);
	const auto after = FixtureProcessIds(L"senp-host-fixture.exe");
	ASSERT_EQ(after.size(), 1U);
	EXPECT_TRUE(before.contains(*after.begin()));
	EXPECT_EQ(service->Snapshot().activeHosts, 1U);
	service->Stop();
	EXPECT_TRUE(FixtureProcessIds(L"senp-host-fixture.exe").empty());
}

} // namespace
