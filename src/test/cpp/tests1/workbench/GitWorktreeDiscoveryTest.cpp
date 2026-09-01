/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/worktree/GitWorktreeDiscoverySource.h"
#include "workbench/worktree/GitWorktreePorcelainParser.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::worktree {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kHeadA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kHeadB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void AddField(std::string& output, std::string_view field)
{
	output.append(field);
	output.push_back('\0');
}

void EndRecord(std::string& output)
{
	output.push_back('\0');
}

void AddBranchRecord(std::string& output, std::string_view path,
	std::string_view head, std::string_view branch,
	std::initializer_list<std::string_view> extra = {})
{
	AddField(output, std::string("worktree ") + std::string(path));
	AddField(output, std::string("HEAD ") + std::string(head));
	AddField(output, std::string("branch refs/heads/") + std::string(branch));
	for (const auto field : extra) AddField(output, field);
	EndRecord(output);
}

void AddDetachedRecord(std::string& output, std::string_view path,
	std::string_view head, std::initializer_list<std::string_view> extra = {})
{
	AddField(output, std::string("worktree ") + std::string(path));
	AddField(output, std::string("HEAD ") + std::string(head));
	AddField(output, "detached");
	for (const auto field : extra) AddField(output, field);
	EndRecord(output);
}

void AddBareRecord(std::string& output, std::string_view path,
	std::initializer_list<std::string_view> extra = {})
{
	AddField(output, std::string("worktree ") + std::string(path));
	AddField(output, "bare");
	for (const auto field : extra) AddField(output, field);
	EndRecord(output);
}

GitWorktreeParseResult Parse(const std::string& output,
	const GitWorktreeParserLimits& limits = {})
{
	return ParseGitWorktreePorcelainZ(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(output.data()), output.size()), limits);
}

GitWorktreeCommandResult Success(std::string output)
{
	GitWorktreeCommandResult result;
	result.status = EGitWorktreeCommandStatus::Succeeded;
	result.exitCode = 0;
	result.standardOutput.assign(output.begin(), output.end());
	return result;
}

GitWorktreeCommandResult Command(EGitWorktreeCommandStatus status,
	int exitCode = -1, std::string error = {})
{
	GitWorktreeCommandResult result;
	result.status = status;
	result.exitCode = exitCode;
	result.standardError = std::move(error);
	return result;
}

std::string OneMainRecord()
{
	std::string output;
	AddBranchRecord(output, "C:/repo", kHeadA, "main");
	return output;
}

class FakeJitter final : public IGitWorktreeRetryJitterSource {
public:
	explicit FakeJitter(std::vector<std::uint32_t> values = {}) : m_values(std::move(values)) {}

	std::uint32_t Next(std::uint32_t maximumInclusive) noexcept override
	{
		maximums.push_back(maximumInclusive);
		if (m_index >= m_values.size()) return 0;
		return (std::min)(m_values[m_index++], maximumInclusive);
	}

	std::vector<std::uint32_t> maximums;

private:
	std::vector<std::uint32_t> m_values;
	std::size_t m_index = 0;
};

class FakeRunner final : public IGitWorktreeListRunner {
public:
	struct Action final {
		GitWorktreeCommandResult result;
		bool waitForCancellation = false;
		bool holdAfterCancellation = false;
		bool throwException = false;
		bool throwBadAlloc = false;
	};

	explicit FakeRunner(std::vector<Action> actions) : m_actions(std::move(actions))
	{
		entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		cancellationObserved = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		completed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	~FakeRunner() override
	{
		if (entered != nullptr) ::CloseHandle(entered);
		if (cancellationObserved != nullptr) ::CloseHandle(cancellationObserved);
		if (release != nullptr) ::CloseHandle(release);
		if (completed != nullptr) ::CloseHandle(completed);
	}

	GitWorktreeCommandResult RunListPorcelainZ(const GitWorktreeListRequest& request,
		HANDLE cancellation) override
	{
		const int active = ++activeCalls;
		int observed = maximumActive.load();
		while (active > observed && !maximumActive.compare_exchange_weak(observed, active)) {}
		const std::size_t index = calls.fetch_add(1);
		Action action;
		{
			std::lock_guard lock(m_mutex);
			requests.push_back(request);
			action = index < m_actions.size()
				? m_actions[index]
				: Action{ Command(EGitWorktreeCommandStatus::InvalidRequest), false };
		}
		(void)::SetEvent(entered);
		if (onRun) onRun();
		if (action.throwException) {
			--activeCalls;
			throw std::runtime_error("injected runner failure");
		}
		if (action.waitForCancellation) {
			const DWORD wait = ::WaitForSingleObject(cancellation, 2000);
			if (wait == WAIT_OBJECT_0) {
				(void)::SetEvent(cancellationObserved);
				if (action.holdAfterCancellation) (void)::WaitForSingleObject(release, 2000);
			}
			action.result = wait == WAIT_OBJECT_0
				? Command(EGitWorktreeCommandStatus::Cancelled)
				: Command(EGitWorktreeCommandStatus::TimedOut);
		}
		if (action.throwBadAlloc) {
			--activeCalls;
			throw std::bad_alloc();
		}
		--activeCalls;
		++completions;
		(void)::SetEvent(completed);
		return action.result;
	}

	HANDLE entered = nullptr;
	HANDLE cancellationObserved = nullptr;
	HANDLE release = nullptr;
	HANDLE completed = nullptr;
	std::function<void()> onRun;
	std::atomic<std::size_t> calls{ 0 };
	std::atomic<std::size_t> completions{ 0 };
	std::atomic<int> activeCalls{ 0 };
	std::atomic<int> maximumActive{ 0 };
	std::vector<GitWorktreeListRequest> requests;

private:
	std::mutex m_mutex;
	std::vector<Action> m_actions;
};

class FakeDiscoveryRuntime final : public IGitWorktreeDiscoveryRuntime {
public:
	FakeDiscoveryRuntime()
	{
		joinEntered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		releaseJoin = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	bool FailStagingAt(EGitWorktreeDiscoveryStagingPoint point) noexcept override
	{
		const int expected = static_cast<int>(point) + 1;
		if (failStagingPoint.load() != expected) return false;
		failStagingPoint.store(0);
		return true;
	}

	~FakeDiscoveryRuntime() override
	{
		if (joinEntered != nullptr) ::CloseHandle(joinEntered);
		if (releaseJoin != nullptr) ::CloseHandle(releaseJoin);
	}

	HANDLE CreateManualResetEvent() noexcept override
	{
		const int call = ++createCalls;
		if (failCreateCall.load() == call) {
			failCreateCall.store(0);
			return nullptr;
		}
		return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	std::thread StartWorker(std::function<void()> worker) override
	{
		++startCalls;
		if (failNextStart.exchange(false)) throw std::runtime_error("injected worker start failure");
		return std::thread(std::move(worker));
	}

	void JoinWorker(std::thread& worker) noexcept override
	{
		if (!worker.joinable()) return;
		++joinCalls;
		if (blockNextJoin.exchange(false)) {
			(void)::SetEvent(joinEntered);
			(void)::WaitForSingleObject(releaseJoin, 2000);
		}
		worker.join();
	}

	HANDLE joinEntered = nullptr;
	HANDLE releaseJoin = nullptr;
	std::atomic<int> createCalls{ 0 };
	std::atomic<int> startCalls{ 0 };
	std::atomic<int> joinCalls{ 0 };
	std::atomic<int> failCreateCall{ 0 };
	std::atomic<int> failStagingPoint{ 0 };
	std::atomic<bool> failNextStart{ false };
	std::atomic<bool> blockNextJoin{ false };
};

GitWorktreeDiscoveryLimits OneAttemptLimits()
{
	GitWorktreeDiscoveryLimits limits;
	limits.retry.maximumAttempts = 1;
	limits.retry.initialDelay = 0ms;
	limits.retry.maximumDelay = 0ms;
	limits.retry.maximumJitter = 0ms;
	return limits;
}

TEST(GitWorktreePorcelainParser, ParsesMainAndLinkedWorktrees)
{
	std::string output;
	AddBranchRecord(output, "C:/repo", kHeadA, "main");
	AddBranchRecord(output, "C:/repo-linked", kHeadB, "feature/linked");
	const auto parsed = Parse(output);
	ASSERT_TRUE(parsed.Succeeded()) << parsed.diagnostic;
	ASSERT_EQ(2U, parsed.records.size());
	EXPECT_EQ(L"C:\\repo", parsed.records[0].path);
	EXPECT_EQ("refs/heads/main", parsed.records[0].branch);
	EXPECT_EQ(L"C:\\repo-linked", parsed.records[1].path);
	EXPECT_EQ("refs/heads/feature/linked", parsed.records[1].branch);
}

TEST(GitWorktreePorcelainParser, RejectsMalformedTruncatedUnknownAndDuplicateFields)
{
	std::string missingHead;
	AddField(missingHead, "worktree C:/repo");
	AddField(missingHead, "branch refs/heads/main");
	EndRecord(missingHead);
	EXPECT_EQ(EGitWorktreeParseStatus::MalformedRecord, Parse(missingHead).status);

	std::string truncated = OneMainRecord();
	truncated.pop_back();
	EXPECT_EQ(EGitWorktreeParseStatus::MalformedRecord, Parse(truncated).status);

	std::string unknown;
	AddBranchRecord(unknown, "C:/repo", kHeadA, "main", { "future-field value" });
	EXPECT_EQ(EGitWorktreeParseStatus::UnknownField, Parse(unknown).status);

	std::string duplicate;
	AddField(duplicate, "worktree C:/repo");
	AddField(duplicate, std::string("HEAD ") + std::string(kHeadA));
	AddField(duplicate, std::string("HEAD ") + std::string(kHeadA));
	AddField(duplicate, "detached");
	EndRecord(duplicate);
	EXPECT_EQ(EGitWorktreeParseStatus::DuplicateField, Parse(duplicate).status);
}

TEST(GitWorktreePorcelainParser, RejectsOversizedInputFieldRecordFieldCountAndRecordCount)
{
	const auto valid = OneMainRecord();
	GitWorktreeParserLimits limits;
	limits.maximumInputBytes = valid.size() - 1;
	EXPECT_EQ(EGitWorktreeParseStatus::InputLimitExceeded, Parse(valid, limits).status);

	limits = {};
	limits.maximumFieldBytes = 10;
	EXPECT_EQ(EGitWorktreeParseStatus::FieldLimitExceeded, Parse(valid, limits).status);

	limits = {};
	limits.maximumRecordBytes = valid.size() - 2;
	EXPECT_EQ(EGitWorktreeParseStatus::RecordLimitExceeded, Parse(valid, limits).status);

	limits = {};
	limits.maximumFieldsPerRecord = 2;
	EXPECT_EQ(EGitWorktreeParseStatus::FieldCountLimitExceeded, Parse(valid, limits).status);

	std::string two;
	AddBranchRecord(two, "C:/repo", kHeadA, "main");
	AddDetachedRecord(two, "C:/linked", kHeadB);
	limits = {};
	limits.maximumRecords = 1;
	EXPECT_EQ(EGitWorktreeParseStatus::RecordLimitExceeded, Parse(two, limits).status);
}

TEST(GitWorktreePorcelainParser, ParsesDetachedLockedAndPrunableStates)
{
	std::string output;
	AddDetachedRecord(output, "C:/detached", kHeadA, { "locked reason text", "prunable stale metadata" });
	const auto parsed = Parse(output);
	ASSERT_TRUE(parsed.Succeeded()) << parsed.diagnostic;
	ASSERT_EQ(1U, parsed.records.size());
	EXPECT_TRUE(parsed.records[0].detached);
	EXPECT_FALSE(parsed.records[0].branch.has_value());
	EXPECT_TRUE(parsed.records[0].locked);
	EXPECT_EQ("reason text", parsed.records[0].lockReason);
	EXPECT_TRUE(parsed.records[0].prunable);
	EXPECT_EQ("stale metadata", parsed.records[0].pruneReason);
}

TEST(GitWorktreePorcelainParser, ParsesBareWorktreeWithoutCheckoutFields)
{
	std::string output;
	AddBareRecord(output, "C:/bare-repository", { "locked maintenance" });
	const auto parsed = Parse(output);
	ASSERT_TRUE(parsed.Succeeded()) << parsed.diagnostic;
	ASSERT_EQ(1U, parsed.records.size());
	EXPECT_TRUE(parsed.records[0].bare);
	EXPECT_TRUE(parsed.records[0].head.empty());
	EXPECT_FALSE(parsed.records[0].branch.has_value());
	EXPECT_FALSE(parsed.records[0].detached);
	EXPECT_TRUE(parsed.records[0].locked);
	EXPECT_EQ("maintenance", parsed.records[0].lockReason);
}

TEST(GitWorktreePorcelainParser, NormalizesSeparatorsDotSegmentsDriveCaseAndTrailingSeparator)
{
	std::string output;
	AddBranchRecord(output, "c:\\Root\\one\\..\\TWO\\.\\", kHeadA, "main");
	const auto parsed = Parse(output);
	ASSERT_TRUE(parsed.Succeeded()) << parsed.diagnostic;
	ASSERT_EQ(1U, parsed.records.size());
	EXPECT_EQ(L"C:\\Root\\TWO", parsed.records[0].path);
	EXPECT_EQ(L"c:\\root\\two", parsed.records[0].identity);
}

TEST(GitWorktreePorcelainParser, DeduplicatesCaseAliasedWindowsPathsInFirstSeenOrder)
{
	std::string output;
	AddBranchRecord(output, "C:/Repo/linked", kHeadA, "main");
	AddBranchRecord(output, "c:\\repo\\.\\LINKED\\", kHeadA, "main");
	const auto parsed = Parse(output);
	ASSERT_TRUE(parsed.Succeeded()) << parsed.diagnostic;
	ASSERT_EQ(1U, parsed.records.size());
	EXPECT_EQ(L"C:\\Repo\\linked", parsed.records[0].path);
}

TEST(GitWorktreePorcelainParser, ConflictingCaseAliasFailsClosed)
{
	std::string output;
	AddBranchRecord(output, "C:/Repo/linked", kHeadA, "main");
	AddBranchRecord(output, "c:/repo/LINKED", kHeadB, "main");
	EXPECT_EQ(EGitWorktreeParseStatus::AmbiguousIdentity, Parse(output).status);
}

TEST(GitWorktreePorcelainParser, RejectsInvalidUtf8DevicePathsAmbiguousComponentsAndHead)
{
	std::string invalidUtf8 = "worktree C:/repo/";
	invalidUtf8.push_back(static_cast<char>(0xff));
	invalidUtf8.push_back('\0');
	AddField(invalidUtf8, std::string("HEAD ") + std::string(kHeadA));
	AddField(invalidUtf8, "detached");
	EndRecord(invalidUtf8);
	EXPECT_EQ(EGitWorktreeParseStatus::InvalidUtf8, Parse(invalidUtf8).status);

	std::string device;
	AddDetachedRecord(device, "\\\\?\\C:\\repo", kHeadA);
	EXPECT_EQ(EGitWorktreeParseStatus::InvalidPath, Parse(device).status);

	std::string ambiguous;
	AddDetachedRecord(ambiguous, "C:/repo./linked", kHeadA);
	EXPECT_EQ(EGitWorktreeParseStatus::InvalidPath, Parse(ambiguous).status);

	std::string badHead;
	AddDetachedRecord(badHead, "C:/repo", "1234");
	EXPECT_EQ(EGitWorktreeParseStatus::InvalidHead, Parse(badHead).status);

	std::string emptyBranch;
	AddBranchRecord(emptyBranch, "C:/repo", kHeadA, "");
	EXPECT_EQ(EGitWorktreeParseStatus::MalformedRecord, Parse(emptyBranch).status);

	std::string invalidBranch;
	AddBranchRecord(invalidBranch, "C:/repo", kHeadA, "topic..name");
	EXPECT_EQ(EGitWorktreeParseStatus::MalformedRecord, Parse(invalidBranch).status);
}

TEST(GitWorktreePorcelainParser, NormalizesUncRootAndUnicodeIdentityAliases)
{
	const auto unc = NormalizeWindowsWorktreePath(L"\\\\Server\\Share\\folder\\..\\");
	ASSERT_TRUE(unc.has_value());
	EXPECT_EQ(L"\\\\Server\\Share", unc->first);
	EXPECT_EQ(L"\\\\server\\share", unc->second);

	const auto composed = NormalizeWindowsWorktreePath(L"C:\\caf\u00e9");
	const auto decomposed = NormalizeWindowsWorktreePath(L"c:/cafe\u0301/.");
	ASSERT_TRUE(composed.has_value());
	ASSERT_TRUE(decomposed.has_value());
	EXPECT_EQ(composed->second, decomposed->second);
}

TEST(GitWorktreeDiscoverySource, SuccessfulRefreshPropagatesTypedRecordsAndRequestBounds)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
	auto jitter = std::make_shared<FakeJitter>();
	GitWorktreeDiscoverySource source(runner, jitter, OneAttemptLimits());
	const auto refresh = source.Refresh(L"C:\\repo");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, refresh.admission);
	const auto result = refresh.completion.get();
	ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
	ASSERT_EQ(1U, result.records.size());
	EXPECT_EQ(1U, result.attempts);
	EXPECT_EQ(EGitWorktreeCancelStatus::NoInFlight, source.CancelCurrent());
	ASSERT_EQ(1U, runner->requests.size());
	EXPECT_EQ(L"C:\\repo", runner->requests[0].repositoryPath);
	EXPECT_EQ(OneAttemptLimits().maximumOutputBytes, runner->requests[0].maximumOutputBytes);
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, source.Stop());
}

TEST(GitWorktreeDiscoverySource, NonzeroExitAndBoundedStderrAreObservable)
{
	auto limits = OneAttemptLimits();
	limits.maximumDiagnosticBytes = 8;
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Failed, 128, "fatal: repository unavailable") } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), limits);
	const auto result = source.Refresh(L"C:\\repo").completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::CommandFailed, result.outcome);
	EXPECT_EQ(128, result.exitCode);
	EXPECT_EQ("fatal: r", result.diagnostic);
	EXPECT_EQ(1U, result.attempts);
}

TEST(GitWorktreeDiscoverySource, TimeoutIsAnExplicitTerminalOutcome)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::TimedOut) } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::TimedOut,
		source.Refresh(L"C:\\repo").completion.get().outcome);
}

TEST(GitWorktreeDiscoverySource, OversizedRunnerOutputFailsBeforeParsing)
{
	auto limits = OneAttemptLimits();
	limits.maximumOutputBytes = 32;
	limits.parser.maximumInputBytes = 32;
	limits.parser.maximumRecordBytes = 32;
	limits.parser.maximumFieldBytes = 32;
	auto oversized = Success(std::string(33, 'x'));
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ { std::move(oversized) } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), limits);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::OutputLimitExceeded,
		source.Refresh(L"C:\\repo").completion.get().outcome);
}

TEST(GitWorktreeDiscoverySource, MalformedCommandOutputMapsToTypedParseFailureWithoutRetry)
{
	std::string malformed = OneMainRecord();
	malformed.pop_back();
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ { Success(malformed) } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), {});
	const auto result = source.Refresh(L"C:\\repo").completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::ParseFailed, result.outcome);
	EXPECT_EQ(EGitWorktreeParseStatus::MalformedRecord, result.parseStatus);
	EXPECT_EQ(1U, runner->calls.load());
}

TEST(GitWorktreeDiscoverySource, CancellationCompletesOnceAndReleasesTheRunner)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto refresh = source.Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));
	EXPECT_EQ(EGitWorktreeCancelStatus::CancellationRequested, source.CancelCurrent());
	const auto result = refresh.completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, result.outcome);
	EXPECT_EQ(1U, runner->calls.load());
	EXPECT_EQ(1U, runner->completions.load());
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(EGitWorktreeCancelStatus::NoInFlight, source.CancelCurrent());
}

TEST(GitWorktreeDiscoverySource, CancellationFinalizationSurvivesInjectedAllocationFailure)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Success(OneMainRecord()), false, false, false, true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	EGitWorktreeCancelStatus callbackStatus = EGitWorktreeCancelStatus::NoInFlight;
	runner->onRun = [&] { callbackStatus = source.CancelCurrent(); };

	const auto refresh = source.Refresh(L"C:\\repo");
	auto sharedCompletion = refresh.completion;
	const auto& result = refresh.completion.get();
	EXPECT_EQ(EGitWorktreeCancelStatus::CancellationRequested, callbackStatus);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, result.outcome);
	EXPECT_EQ(refresh.operationId, result.operationId);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_TRUE(result.records.empty());
	EXPECT_FALSE(result.parseStatus.has_value());
	EXPECT_TRUE(result.diagnostic.empty());
	EXPECT_EQ(-1, result.exitCode);
	ASSERT_EQ(std::future_status::ready, sharedCompletion.wait_for(0ms));
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, sharedCompletion.get().outcome);
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(EGitWorktreeCancelStatus::NoInFlight, source.CancelCurrent());
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, source.Stop());
	EXPECT_EQ(EGitWorktreeStopStatus::AlreadyStopped, source.Stop());
}

TEST(GitWorktreeDiscoverySource, StopDuringInFlightPublishesStoppedAndJoinsBeforeReturning)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto refresh = source.Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, source.Stop());
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, refresh.completion.get().outcome);
	const auto rejected = source.Refresh(L"C:\\repo");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedStopped, rejected.admission);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, rejected.completion.get().outcome);
	EXPECT_EQ(EGitWorktreeStopStatus::AlreadyStopped, source.Stop());
}

TEST(GitWorktreeDiscoverySource, ConcurrentStopCallersWaitForJoinAndTerminalCompletion)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true, true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto refresh = source.Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));

	std::atomic<bool> firstReturned{ false };
	std::atomic<bool> firstObservedTerminal{ false };
	EGitWorktreeStopStatus firstStatus{};
	std::thread firstStop([&] {
		firstStatus = source.Stop();
		firstObservedTerminal.store(refresh.completion.wait_for(0ms) == std::future_status::ready);
		firstReturned.store(true);
	});
	EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->cancellationObserved, 1000));

	std::atomic<bool> releaseReached{ false };
	std::atomic<bool> firstReturnedBeforeRelease{ false };
	std::thread releaser([&] {
		::Sleep(50);
		firstReturnedBeforeRelease.store(firstReturned.load());
		releaseReached.store(true);
		(void)::SetEvent(runner->release);
	});
	const auto secondStatus = source.Stop();
	const bool secondObservedTerminal = refresh.completion.wait_for(0ms) == std::future_status::ready;
	releaser.join();
	firstStop.join();

	EXPECT_TRUE(releaseReached.load());
	EXPECT_FALSE(firstReturnedBeforeRelease.load());
	EXPECT_TRUE(firstObservedTerminal.load());
	EXPECT_TRUE(secondObservedTerminal);
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, firstStatus);
	EXPECT_EQ(EGitWorktreeStopStatus::AlreadyStopped, secondStatus);
	ASSERT_EQ(std::future_status::ready, refresh.completion.wait_for(0ms));
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, refresh.completion.get().outcome);
	EXPECT_EQ(0, runner->activeCalls.load());
}

TEST(GitWorktreeDiscoverySource, StopFromRunnerCallbackAvoidsSelfJoin)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Success(OneMainRecord()), false, false, false, true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	EGitWorktreeStopStatus callbackStatus{};
	runner->onRun = [&] { callbackStatus = source.Stop(); };

	const auto refresh = source.Refresh(L"C:\\repo");
	const auto& result = refresh.completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, result.outcome);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_TRUE(result.diagnostic.empty());
	EXPECT_EQ(EGitWorktreeStopStatus::StopRequestedFromWorker, callbackStatus);
	// Worker-origin Stop only requests cancellation.  The first external
	// caller owns the terminal cleanup and therefore reports Stopped.
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, source.Stop());
	EXPECT_EQ(EGitWorktreeStopStatus::AlreadyStopped, source.Stop());
}

TEST(GitWorktreeDiscoverySource, RefreshReapAndStopShareOneQuiescenceBarrier)
{
	auto runner = std::make_shared<FakeRunner>(
		std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
	auto runtime = std::make_shared<FakeDiscoveryRuntime>();
	auto source = std::make_unique<GitWorktreeDiscoverySource>(
		runner, std::make_shared<FakeJitter>(), OneAttemptLimits(), runtime);
	const auto first = source->Refresh(L"C:\\repo");
	ASSERT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, first.completion.get().outcome);
	ASSERT_EQ(EGitWorktreeCancelStatus::NoInFlight, source->CancelCurrent());

	runtime->blockNextJoin.store(true);
	std::optional<GitWorktreeRefresh> refreshDuringReap;
	std::thread reaper([&] { refreshDuringReap = source->Refresh(L"C:\\repo"); });
	EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runtime->joinEntered, 1000));

	std::atomic<bool> stopReturned{ false };
	EGitWorktreeStopStatus stopStatus{};
	std::thread stopper([&] {
		stopStatus = source->Stop();
		stopReturned.store(true);
	});
	bool stopRequestObserved = false;
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (source->IsStopped()) {
			stopRequestObserved = true;
			break;
		}
		::Sleep(1);
	}
	const bool stopReturnedBeforeRelease = stopReturned.load();
	(void)::SetEvent(runtime->releaseJoin);
	stopper.join();
	reaper.join();
	source.reset();

	EXPECT_TRUE(stopRequestObserved);
	EXPECT_FALSE(stopReturnedBeforeRelease);
	EXPECT_EQ(EGitWorktreeStopStatus::Stopped, stopStatus);
	ASSERT_TRUE(refreshDuringReap.has_value());
	EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedStopped, refreshDuringReap->admission);
	ASSERT_EQ(std::future_status::ready, refreshDuringReap->completion.wait_for(0ms));
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, refreshDuringReap->completion.get().outcome);
	EXPECT_EQ(1U, runner->calls.load());
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(1, runtime->joinCalls.load());
}

TEST(GitWorktreeDiscoverySource, ConcurrentRefreshesDeduplicateOneInFlightOperation)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	HANDLE start = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(nullptr, start);
	std::vector<GitWorktreeRefresh> refreshes(16);
	std::vector<std::thread> callers;
	for (std::size_t index = 0; index < refreshes.size(); ++index) {
		callers.emplace_back([&, index] {
			(void)::WaitForSingleObject(start, 1000);
			refreshes[index] = source.Refresh(index % 2 == 0 ? L"c:/REPO/." : L"C:\\repo\\");
		});
	}
	(void)::SetEvent(start);
	EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));
	for (auto& caller : callers) caller.join();
	::CloseHandle(start);

	const auto started = std::find_if(refreshes.begin(), refreshes.end(), [](const auto& refresh) {
		return refresh.admission == EGitWorktreeRefreshAdmission::Started;
	});
	ASSERT_NE(refreshes.end(), started);
	EXPECT_EQ(1U, std::count_if(refreshes.begin(), refreshes.end(), [](const auto& refresh) {
		return refresh.admission == EGitWorktreeRefreshAdmission::Started;
	}));
	EXPECT_EQ(15U, std::count_if(refreshes.begin(), refreshes.end(), [](const auto& refresh) {
		return refresh.admission == EGitWorktreeRefreshAdmission::JoinedInFlight;
	}));
	for (const auto& refresh : refreshes) EXPECT_EQ(started->operationId, refresh.operationId);
	EXPECT_EQ(EGitWorktreeCancelStatus::CancellationRequested, source.CancelCurrent());
	for (const auto& refresh : refreshes) {
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, refresh.completion.get().outcome);
	}
	EXPECT_EQ(1U, runner->calls.load());
	EXPECT_EQ(1, runner->maximumActive.load());
}

TEST(GitWorktreeDiscoverySource, DifferentConcurrentRepositoryIsRejectedWithoutSharingCompletion)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto first = source.Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));

	const auto different = source.Refresh(L"D:\\other");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedDifferentRequest, different.admission);
	EXPECT_EQ(0U, different.operationId);
	ASSERT_EQ(std::future_status::ready, different.completion.wait_for(0ms));
	const auto differentResult = different.completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::BusyDifferentRequest, differentResult.outcome);
	EXPECT_TRUE(differentResult.records.empty());
	EXPECT_EQ(1U, runner->calls.load());

	EXPECT_EQ(EGitWorktreeCancelStatus::CancellationRequested, source.CancelCurrent());
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, first.completion.get().outcome);
	EXPECT_EQ(1U, runner->completions.load());
	EXPECT_EQ(1, runner->maximumActive.load());
}

TEST(GitWorktreeDiscoverySource, RetriesUseBoundedExponentialBackoffAndInjectedJitter)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{
		{ Command(EGitWorktreeCommandStatus::Failed, 1, "first") },
		{ Command(EGitWorktreeCommandStatus::TimedOut) },
		{ Success(OneMainRecord()) },
	});
	auto jitter = std::make_shared<FakeJitter>(std::vector<std::uint32_t>{ 3, 7 });
	GitWorktreeDiscoveryLimits limits;
	limits.retry.maximumAttempts = 3;
	limits.retry.initialDelay = 10ms;
	limits.retry.maximumDelay = 50ms;
	limits.retry.maximumJitter = 10ms;
	GitWorktreeDiscoverySource source(runner, jitter, limits);
	const auto result = source.Refresh(L"C:\\repo").completion.get();
	ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
	EXPECT_EQ(3U, result.attempts);
	ASSERT_EQ(2U, result.retryDelays.size());
	EXPECT_EQ(13ms, result.retryDelays[0]);
	EXPECT_EQ(27ms, result.retryDelays[1]);
	EXPECT_EQ(3U, runner->calls.load());
	EXPECT_EQ((std::vector<std::uint32_t>{ 10, 10 }), jitter->maximums);
}

TEST(GitWorktreeDiscoverySource, CancellationInterruptsRetryBackoff)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{
		{ Command(EGitWorktreeCommandStatus::Failed, 1, "retry") },
		{ Success(OneMainRecord()) },
	});
	GitWorktreeDiscoveryLimits limits;
	limits.retry.maximumAttempts = 2;
	limits.retry.initialDelay = 5000ms;
	limits.retry.maximumDelay = 5000ms;
	limits.retry.maximumJitter = 0ms;
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), limits);
	const auto refresh = source.Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->completed, 1000));
	EXPECT_EQ(EGitWorktreeCancelStatus::CancellationRequested, source.CancelCurrent());
	const auto result = refresh.completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Cancelled, result.outcome);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_EQ(1U, runner->calls.load());
}

TEST(GitWorktreeDiscoverySource, RunnerExceptionPreservesAttemptObservability)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), false, false, true } });
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto result = source.Refresh(L"C:\\repo").completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InternalFailure, result.outcome);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_EQ(0, runner->activeCalls.load());
}

TEST(GitWorktreeDiscoverySource, RetryDelaySaturatesAtMaximumWithoutOverflow)
{
	FakeJitter jitter({ 20 });
	GitWorktreeRetryPolicy policy;
	policy.initialDelay = 40ms;
	policy.maximumDelay = 50ms;
	policy.maximumJitter = 20ms;
	EXPECT_EQ(50ms, ComputeGitWorktreeRetryDelay(31, policy, jitter));
	EXPECT_TRUE(jitter.maximums.empty());
}

TEST(GitWorktreeDiscoverySource, InvalidConstructionAndEmptyPathCompleteSynchronously)
{
	GitWorktreeDiscoverySource missingRunner(nullptr, std::make_shared<FakeJitter>(), {});
	const auto rejected = missingRunner.Refresh(L"C:\\repo");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInvalidRequest, rejected.admission);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InvalidRequest, rejected.completion.get().outcome);

	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{});
	GitWorktreeDiscoverySource emptyPath(runner, std::make_shared<FakeJitter>(), {});
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InvalidRequest,
		emptyPath.Refresh(L"").completion.get().outcome);
	EXPECT_EQ(0U, runner->calls.load());
}

TEST(GitWorktreeDiscoverySource, StagingFailuresRemainTypedIdleAndDoNotConsumeOperationId)
{
	for (const auto point : { EGitWorktreeDiscoveryStagingPoint::RepositoryState,
		EGitWorktreeDiscoveryStagingPoint::CompletionPromise,
		EGitWorktreeDiscoveryStagingPoint::CompletionFuture }) {
		auto runner = std::make_shared<FakeRunner>(
			std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
		auto runtime = std::make_shared<FakeDiscoveryRuntime>();
		runtime->failStagingPoint.store(static_cast<int>(point) + 1);
		GitWorktreeDiscoverySource source(
			runner, std::make_shared<FakeJitter>(), OneAttemptLimits(), runtime);

		const auto failed = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInternalFailure, failed.admission);
		EXPECT_EQ(0U, failed.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InternalFailure, failed.completion.get().outcome);
		EXPECT_EQ(EGitWorktreeCancelStatus::NoInFlight, source.CancelCurrent());
		EXPECT_EQ(0U, runner->calls.load());
		EXPECT_EQ(0, runtime->createCalls.load());

		const auto recovered = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, recovered.admission);
		EXPECT_EQ(1U, recovered.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, recovered.completion.get().outcome);
	}
}

TEST(GitWorktreeDiscoverySource, ResourceStagingFailuresRollBackBeforePublication)
{
	{
		auto runner = std::make_shared<FakeRunner>(
			std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
		auto runtime = std::make_shared<FakeDiscoveryRuntime>();
		runtime->failCreateCall.store(1);
		GitWorktreeDiscoverySource source(
			runner, std::make_shared<FakeJitter>(), OneAttemptLimits(), runtime);

		const auto failed = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInternalFailure, failed.admission);
		EXPECT_EQ(0U, failed.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InternalFailure, failed.completion.get().outcome);
		EXPECT_EQ(EGitWorktreeCancelStatus::NoInFlight, source.CancelCurrent());
		EXPECT_EQ(0U, runner->calls.load());
		EXPECT_EQ(1, runtime->createCalls.load());
		EXPECT_EQ(0, runtime->startCalls.load());

		const auto recovered = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, recovered.admission);
		EXPECT_EQ(1U, recovered.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, recovered.completion.get().outcome);
	}

	{
		auto runner = std::make_shared<FakeRunner>(
			std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
		auto runtime = std::make_shared<FakeDiscoveryRuntime>();
		runtime->failNextStart.store(true);
		GitWorktreeDiscoverySource source(
			runner, std::make_shared<FakeJitter>(), OneAttemptLimits(), runtime);

		const auto failed = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInternalFailure, failed.admission);
		EXPECT_EQ(0U, failed.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InternalFailure, failed.completion.get().outcome);
		EXPECT_EQ(0U, runner->calls.load());

		const auto recovered = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, recovered.admission);
		EXPECT_EQ(1U, recovered.operationId);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, recovered.completion.get().outcome);
	}
}

TEST(GitWorktreeDiscoverySource, StartupUsesOnlyCancellationEventAndNoSetEventGate)
{
	auto runner = std::make_shared<FakeRunner>(
		std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
	auto runtime = std::make_shared<FakeDiscoveryRuntime>();
	// A second event creation would fail.  Startup must not need a start gate;
	// the worker's first m_mutex lock is the publication barrier.
	runtime->failCreateCall.store(2);
	GitWorktreeDiscoverySource source(
		runner, std::make_shared<FakeJitter>(), OneAttemptLimits(), runtime);

	const auto refresh = source.Refresh(L"C:\\repo");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, refresh.admission);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, refresh.completion.get().outcome);
	EXPECT_EQ(1, runtime->createCalls.load());
}

TEST(GitWorktreeDiscoverySource, OverlongRepositoryPathFailsBeforeRunnerAdmission)
{
	std::wstring boundary(kGitWorktreeMaximumRepositoryPathCharacters, L'a');
	boundary[0] = L'C';
	boundary[1] = L':';
	boundary[2] = L'\\';
	GitWorktreeListRequest request;
	request.repositoryPath = boundary;
	EXPECT_TRUE(IsValidGitWorktreeListRequest(request));
	request.repositoryPath.push_back(L'a');
	EXPECT_FALSE(IsValidGitWorktreeListRequest(request));
	GitWorktreeListRunner productionRunner;
	EXPECT_EQ(EGitWorktreeCommandStatus::InvalidRequest,
		productionRunner.RunListPorcelainZ(request, nullptr).status);

	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{});
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto rejected = source.Refresh(request.repositoryPath);
	EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInvalidRequest, rejected.admission);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InvalidRequest, rejected.completion.get().outcome);
	EXPECT_EQ(0U, runner->calls.load());
}

TEST(GitWorktreeDiscoverySource, DestructorCancelsAndJoinsActiveDiscovery)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true } });
	auto source = std::make_unique<GitWorktreeDiscoverySource>(
		runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto refresh = source->Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));
	source.reset();
	ASSERT_EQ(std::future_status::ready, refresh.completion.wait_for(0ms));
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, refresh.completion.get().outcome);
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(1U, runner->completions.load());
}

TEST(GitWorktreeDiscoverySource, DestructorFinalizesAfterInjectedAllocationFailure)
{
	auto runner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{ {
		Command(EGitWorktreeCommandStatus::Succeeded), true, false, false, true } });
	auto source = std::make_unique<GitWorktreeDiscoverySource>(
		runner, std::make_shared<FakeJitter>(), OneAttemptLimits());
	const auto refresh = source->Refresh(L"C:\\repo");
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(runner->entered, 1000));
	source.reset();

	ASSERT_EQ(std::future_status::ready, refresh.completion.wait_for(0ms));
	const auto& result = refresh.completion.get();
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Stopped, result.outcome);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_TRUE(result.diagnostic.empty());
	EXPECT_EQ(0, runner->activeCalls.load());
	EXPECT_EQ(0U, runner->completions.load());
}

TEST(GitWorktreeDiscoverySource, ProductionLimitCeilingsAcceptBoundaryAndRejectEachOverflow)
{
	GitWorktreeDiscoveryLimits ceiling;
	ceiling.commandTimeoutMilliseconds = kGitWorktreeMaximumCommandTimeoutMilliseconds;
	ceiling.maximumOutputBytes = kGitWorktreeMaximumOutputBytes;
	ceiling.maximumDiagnosticBytes = kGitWorktreeMaximumDiagnosticBytes;
	ceiling.parser.maximumInputBytes = kGitWorktreeMaximumOutputBytes;
	ceiling.parser.maximumRecordBytes = kGitWorktreeMaximumParserRecordBytes;
	ceiling.parser.maximumFieldBytes = kGitWorktreeMaximumParserFieldBytes;
	ceiling.parser.maximumFieldsPerRecord = kGitWorktreeMaximumParserFieldsPerRecord;
	ceiling.parser.maximumRecords = kGitWorktreeMaximumParserRecords;
	ceiling.retry.maximumAttempts = kGitWorktreeMaximumRetryAttempts;
	ceiling.retry.initialDelay = kGitWorktreeMaximumRetryDelay;
	ceiling.retry.maximumDelay = kGitWorktreeMaximumRetryDelay;
	ceiling.retry.maximumJitter = kGitWorktreeMaximumRetryDelay;
	ASSERT_TRUE(IsValidGitWorktreeDiscoveryLimits(ceiling));
	auto exactRunner = std::make_shared<FakeRunner>(
		std::vector<FakeRunner::Action>{ { Success(OneMainRecord()) } });
	GitWorktreeDiscoverySource exactSource(exactRunner, std::make_shared<FakeJitter>(), ceiling);
	const auto exactRefresh = exactSource.Refresh(L"C:\\repo");
	EXPECT_EQ(EGitWorktreeRefreshAdmission::Started, exactRefresh.admission);
	EXPECT_EQ(EGitWorktreeDiscoveryOutcome::Succeeded, exactRefresh.completion.get().outcome);

	auto rejectedRunner = std::make_shared<FakeRunner>(std::vector<FakeRunner::Action>{});
	auto rejects = [&](auto mutate) {
		auto invalid = ceiling;
		mutate(invalid);
		EXPECT_FALSE(IsValidGitWorktreeDiscoveryLimits(invalid));
		GitWorktreeDiscoverySource source(rejectedRunner, std::make_shared<FakeJitter>(), invalid);
		const auto refresh = source.Refresh(L"C:\\repo");
		EXPECT_EQ(EGitWorktreeRefreshAdmission::RejectedInvalidRequest, refresh.admission);
		EXPECT_EQ(EGitWorktreeDiscoveryOutcome::InvalidRequest, refresh.completion.get().outcome);
	};
	rejects([](auto& value) { ++value.commandTimeoutMilliseconds; });
	rejects([](auto& value) { ++value.maximumOutputBytes; });
	rejects([](auto& value) { ++value.maximumDiagnosticBytes; });
	rejects([](auto& value) { ++value.parser.maximumInputBytes; });
	rejects([](auto& value) { ++value.parser.maximumRecordBytes; });
	rejects([](auto& value) { ++value.parser.maximumFieldBytes; });
	rejects([](auto& value) { ++value.parser.maximumFieldsPerRecord; });
	rejects([](auto& value) { ++value.parser.maximumRecords; });
	rejects([](auto& value) { ++value.retry.maximumAttempts; });
	rejects([](auto& value) { value.retry.maximumDelay += 1ms; });
	rejects([](auto& value) { value.retry.maximumJitter += 1ms; });
	EXPECT_EQ(0U, rejectedRunner->calls.load());

	GitWorktreeListRequest request;
	request.repositoryPath = L"C:\\repo";
	request.timeoutMilliseconds = kGitWorktreeMaximumCommandTimeoutMilliseconds;
	request.maximumOutputBytes = kGitWorktreeMaximumOutputBytes;
	EXPECT_TRUE(IsValidGitWorktreeListRequest(request));
	++request.timeoutMilliseconds;
	EXPECT_FALSE(IsValidGitWorktreeListRequest(request));
	request.timeoutMilliseconds = kGitWorktreeMaximumCommandTimeoutMilliseconds;
	++request.maximumOutputBytes;
	EXPECT_FALSE(IsValidGitWorktreeListRequest(request));
}

TEST(GitWorktreeListRunner, RejectsOverCeilingRequestBeforeGitExecution)
{
	GitWorktreeListRunner runner;
	GitWorktreeListRequest request;
	request.repositoryPath = L"C:\\repo";
	request.timeoutMilliseconds = kGitWorktreeMaximumCommandTimeoutMilliseconds + 1;
	request.maximumOutputBytes = kGitWorktreeMaximumOutputBytes;
	EXPECT_EQ(EGitWorktreeCommandStatus::InvalidRequest,
		runner.RunListPorcelainZ(request, nullptr).status);

	request.timeoutMilliseconds = kGitWorktreeMaximumCommandTimeoutMilliseconds;
	request.maximumOutputBytes = kGitWorktreeMaximumOutputBytes + 1;
	EXPECT_EQ(EGitWorktreeCommandStatus::InvalidRequest,
		runner.RunListPorcelainZ(request, nullptr).status);
}

TEST(GitWorktreeListRunner, RealGitSmokeUsesSupportedReadOnlyNulContract)
{
	wchar_t module[MAX_PATH]{};
	const DWORD length = ::GetModuleFileNameW(nullptr, module, MAX_PATH);
	ASSERT_GT(length, 0U);
	std::wstring directory(module, length);
	const auto separator = directory.find_last_of(L"\\/");
	ASSERT_NE(std::wstring::npos, separator);
	directory.resize(separator);

	auto runner = std::make_shared<GitWorktreeListRunner>();
	auto limits = OneAttemptLimits();
	limits.commandTimeoutMilliseconds = 5000;
	GitWorktreeDiscoverySource source(runner, std::make_shared<FakeJitter>(), limits);
	const auto result = source.Refresh(directory).completion.get();
	ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
	EXPECT_FALSE(result.records.empty());
	EXPECT_TRUE(std::all_of(result.records.begin(), result.records.end(), [](const auto& record) {
		return !record.path.empty() && !record.identity.empty();
	}));
}

} // namespace
} // namespace workbench::worktree
