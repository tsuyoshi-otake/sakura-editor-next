/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/account/AccountDiscovery.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <utility>

namespace workbench::account {
namespace {

AccountDiscoveryRequest Request()
{
	AccountDiscoveryRequest request;
	request.workingDirectory = L"C:\\repo";
	return request;
}

scm::GitExecutionResult GitSuccess(std::string_view output)
{
	scm::GitExecutionResult result;
	result.status = scm::EGitExecutionStatus::Succeeded;
	result.exitCode = 0;
	result.standardOutput.assign(output.begin(), output.end());
	return result;
}

scm::GitExecutionResult GitMissing()
{
	scm::GitExecutionResult result;
	result.status = scm::EGitExecutionStatus::Failed;
	result.exitCode = 1;
	return result;
}

scm::GitExecutionResult GitCancelled()
{
	scm::GitExecutionResult result;
	result.status = scm::EGitExecutionStatus::Cancelled;
	return result;
}

GhAuthStatusResult GhSuccess(std::string output)
{
	GhAuthStatusResult result;
	result.status = EAccountCommandStatus::Succeeded;
	result.exitCode = 0;
	const auto parsed = ParseGitHubAuthStatus(output);
	result.parseStatus = parsed.status;
	result.accounts = parsed.accounts;
	return result;
}

GhAuthStatusResult GhUnavailable()
{
	GhAuthStatusResult result;
	result.status = EAccountCommandStatus::Unavailable;
	return result;
}

GhAuthStatusResult GhFailedWithJson(std::string output)
{
	auto result = GhSuccess(std::move(output));
	result.status = EAccountCommandStatus::Failed;
	result.exitCode = 1;
	return result;
}

constexpr std::string_view kAuthJson = R"json({
  "hosts": {
    "github.com": [{
      "state": "success",
      "active": true,
      "host": "github.com",
      "login": "alice",
      "tokenSource": "keyring",
      "scopes": "repo, workflow",
      "oauthToken": "must-not-escape",
      "gitProtocol": "https"
    }]
  }
})json";

TEST(AccountDiscovery, ParsesAccountsWithoutRetainingSecretLikeMembers)
{
	const auto parsed = ParseGitHubAuthStatus(kAuthJson);
	ASSERT_EQ(EGitHubAuthParseStatus::Ready, parsed.status);
	ASSERT_EQ(1U, parsed.accounts.size());
	EXPECT_EQ(L"github.com", parsed.accounts[0].host);
	EXPECT_EQ(L"alice", parsed.accounts[0].login);
	EXPECT_EQ(L"https", parsed.accounts[0].gitProtocol);
	EXPECT_TRUE(parsed.accounts[0].active);
	EXPECT_EQ(EGitHubAccountState::Success, parsed.accounts[0].state);
	EXPECT_EQ((std::vector<std::wstring>{ L"auth", L"status", L"--json", L"hosts" }),
		BuildGhAuthStatusArguments());
	EXPECT_FALSE(std::ranges::any_of(BuildGhAuthStatusArguments(), [](const auto& argument) {
		return argument.find(L"token") != std::wstring::npos
			|| argument.find(L"scope") != std::wstring::npos;
	}));
}

TEST(AccountDiscovery, ParsesMultipleHostsAndAccountsInStableHostOrder)
{
	const auto parsed = ParseGitHubAuthStatus(R"json({
    "hosts": {
      "ghe.example": [
        {"active": false, "state": "failure", "host": "ghe.example", "login": "bob", "gitProtocol": "ssh"},
        {"active": true, "state": "success", "host": "ghe.example", "login": "carol", "gitProtocol": "https"}
      ],
      "github.com": [
        {"active": true, "state": "success", "host": "github.com", "login": "alice", "gitProtocol": "https"}
      ]
    }
  })json");
	ASSERT_EQ(EGitHubAuthParseStatus::Ready, parsed.status);
	ASSERT_EQ(3U, parsed.accounts.size());
	EXPECT_EQ(L"github.com", parsed.accounts[0].host);
	EXPECT_EQ(L"alice", parsed.accounts[0].login);
	EXPECT_TRUE(parsed.accounts[0].active);
	EXPECT_EQ(EGitHubAccountState::Success, parsed.accounts[0].state);
	EXPECT_EQ(L"ghe.example", parsed.accounts[1].host);
	EXPECT_FALSE(parsed.accounts[1].active);
	EXPECT_EQ(EGitHubAccountState::Failure, parsed.accounts[1].state);
	EXPECT_EQ(L"carol", parsed.accounts[2].login);
}

TEST(AccountDiscovery, KeepsUnknownAndEmptyGhStatesExplicit)
{
	const auto parsed = ParseGitHubAuthStatus(R"json({
    "hosts": {
      "github.com": [
        {"active": true, "host": "github.com", "login": "missing", "gitProtocol": "https"},
        {"active": false, "state": "", "host": "github.com", "login": "empty", "gitProtocol": "https"},
        {"active": false, "state": "future-state", "host": "github.com", "login": "future", "gitProtocol": "https"}
      ]
    }
  })json");
	ASSERT_EQ(EGitHubAuthParseStatus::Ready, parsed.status);
	ASSERT_EQ(3U, parsed.accounts.size());
	EXPECT_EQ(EGitHubAccountState::Unknown, parsed.accounts[0].state);
	EXPECT_EQ(EGitHubAccountState::Unknown, parsed.accounts[1].state);
	EXPECT_EQ(EGitHubAccountState::Unknown, parsed.accounts[2].state);
}

TEST(AccountDiscovery, RejectsInvalidAndOversizedJsonBeforeBuildingAccounts)
{
	EXPECT_EQ(EGitHubAuthParseStatus::Invalid,
		ParseGitHubAuthStatus(R"json({"hosts":[)json").status);
	AccountDiscoveryLimits limits;
	limits.maximumJsonBytes = 8;
	const auto oversized = ParseGitHubAuthStatus(R"json({"hosts":{}})json", limits);
	EXPECT_EQ(EGitHubAuthParseStatus::Oversized, oversized.status);
	EXPECT_TRUE(oversized.accounts.empty());
}

TEST(AccountDiscovery, ReportsReadyOnlyWhenBothSourcesAreComplete)
{
	std::vector<std::vector<std::wstring>> gitArguments;
	AccountDiscoveryRunners runners;
	runners.runGit = [&gitArguments](const scm::GitExecutionRequest& request, HANDLE) {
		gitArguments.push_back(request.arguments);
		return request.arguments.back() == L"user.name"
			? GitSuccess("Alice\n") : GitSuccess("alice@example.test\n");
	};
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		return GhSuccess(std::string(kAuthJson));
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Ready, snapshot.state);
	EXPECT_EQ(EAccountSourceState::Ready, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Ready, snapshot.githubState);
	ASSERT_TRUE(snapshot.gitIdentity.has_value());
	EXPECT_EQ(L"Alice", snapshot.gitIdentity->userName);
	EXPECT_EQ(L"alice@example.test", snapshot.gitIdentity->userEmail);
	ASSERT_EQ(1U, snapshot.githubAccounts.size());
	ASSERT_EQ(2U, gitArguments.size());
	EXPECT_EQ(L"config", gitArguments[0][0]);
	EXPECT_EQ(L"--get", gitArguments[0][1]);
}

TEST(AccountDiscovery, ConvertsInjectedCancellationToStoppedTerminalState)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitCancelled(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		ADD_FAILURE() << "Git cancellation must prevent the second source from running";
		return GhAuthStatusResult{};
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Stopped, snapshot.state);
}

TEST(AccountDiscovery, DistinguishesUnconfiguredGitFromUnavailableTools)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitMissing(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) { return GhUnavailable(); };
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Unavailable, snapshot.state);
	EXPECT_EQ(EAccountSourceState::Unconfigured, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Unavailable, snapshot.githubState);
	EXPECT_FALSE(snapshot.gitIdentity.has_value());
	EXPECT_TRUE(snapshot.githubAccounts.empty());
}

TEST(AccountDiscovery, KeepsUsableGitDataAndMarksTheAggregatePartialWhenGhFails)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest& request, HANDLE) {
		return request.arguments.back() == L"user.name" ? GitSuccess("Alice\n") : GitMissing();
	};
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) { return GhUnavailable(); };
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Partial, snapshot.state);
	EXPECT_EQ(EAccountSourceState::Partial, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Unavailable, snapshot.githubState);
	ASSERT_TRUE(snapshot.gitIdentity.has_value());
	EXPECT_EQ(L"Alice", snapshot.gitIdentity->userName);
	EXPECT_TRUE(snapshot.gitIdentity->userEmail.empty());
}

TEST(AccountDiscovery, RetainsValidGhAccountsWhenAnotherRecordIsMalformed)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitMissing(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		return GhSuccess(R"json({
      "hosts": {
        "github.com": [
          {"active": true, "state": "success", "host": "github.com", "login": "alice", "gitProtocol": "https"},
          {"active": true, "state": "success", "host": "github.com", "login": 7, "gitProtocol": "https"}
        ]
      }
    })json");
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Partial, snapshot.state);
	EXPECT_EQ(EAccountSourceState::Unconfigured, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Partial, snapshot.githubState);
	ASSERT_EQ(1U, snapshot.githubAccounts.size());
	EXPECT_EQ(L"alice", snapshot.githubAccounts[0].login);
}

TEST(AccountDiscovery, AValidGhResponseWithNoAccountsIsReadyAndDistinctFromUnavailable)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitMissing(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		return GhSuccess(R"({"hosts":{}})");
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Partial, snapshot.state);
	EXPECT_EQ(EAccountSourceState::Unconfigured, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Ready, snapshot.githubState);
	EXPECT_TRUE(snapshot.githubAccounts.empty());
}

TEST(AccountDiscovery, KeepsValidEmptyGhResponseReadyWhenCliReportsUnauthenticated)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitMissing(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		return GhFailedWithJson(R"json({"hosts":{}})json");
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountSourceState::Ready, snapshot.githubState);
	EXPECT_EQ(EAccountDiscoveryState::Partial, snapshot.state);
	EXPECT_TRUE(snapshot.githubAccounts.empty());
}

TEST(AccountDiscovery, RetainsAccountsFromFailedGhStatusAsPartial)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitMissing(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		return GhFailedWithJson(R"json({
      "hosts": {
        "github.com": [{"active": true, "state": "failure", "host": "github.com", "login": "alice", "gitProtocol": "https"}]
      }
    })json");
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountSourceState::Partial, snapshot.githubState);
	EXPECT_EQ(EAccountDiscoveryState::Partial, snapshot.state);
	ASSERT_EQ(1U, snapshot.githubAccounts.size());
	EXPECT_EQ(EGitHubAccountState::Failure, snapshot.githubAccounts[0].state);
}

TEST(AccountDiscovery, ServiceDeduplicatesInFlightWorkAndStopJoinsIt)
{
	const HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(nullptr, entered);
	std::atomic<int> calls = 0;
	AccountDiscoveryService service(
		[&](const AccountDiscoveryRequest&, HANDLE stop) {
			++calls;
			::SetEvent(entered);
			while (::WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) ::Sleep(1);
			AccountDiscoverySnapshot result;
			result.state = EAccountDiscoveryState::Stopped;
			return result;
		});

	EXPECT_EQ(EAccountRefreshResult::Started, service.RequestRefresh(L"C:\\repo"));
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(entered, 1000));
	EXPECT_EQ(EAccountRefreshResult::AlreadyInFlight, service.RequestRefresh(L"C:\\repo"));
	service.Stop();
	EXPECT_EQ(1, calls.load());
	EXPECT_EQ(EAccountDiscoveryState::Stopped, service.Snapshot().state);
	EXPECT_EQ(EAccountRefreshResult::RejectedStopped, service.RequestRefresh(L"C:\\repo"));
	::CloseHandle(entered);
}

} // namespace
} // namespace workbench::account
