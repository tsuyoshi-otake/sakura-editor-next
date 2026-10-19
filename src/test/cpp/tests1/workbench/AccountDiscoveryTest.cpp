/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "workbench/account/AccountDiscovery.h"
#include "workbench/account/GhAuthStatusEnvironment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
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

std::wstring CurrentDirectory()
{
	std::array<wchar_t, 32768> buffer{};
	const DWORD length = ::GetCurrentDirectoryW(static_cast<DWORD>(buffer.size()),
		buffer.data());
	if (length == 0 || length >= buffer.size()) return {};
	return std::wstring(buffer.data(), length);
}

std::wstring HostnameExecutable()
{
	std::array<wchar_t, 32768> buffer{};
	const UINT length = ::GetSystemDirectoryW(buffer.data(),
		static_cast<UINT>(buffer.size()));
	if (length == 0 || length >= buffer.size()) return {};
	return std::wstring(buffer.data(), length) + L"\\hostname.exe";
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

scm::GitExecutionResult GitNotInstalled()
{
	scm::GitExecutionResult result;
	result.status = scm::EGitExecutionStatus::GitUnavailable;
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
	EXPECT_EQ(EAccountSourceState::NotInstalled, snapshot.githubState);
	EXPECT_FALSE(snapshot.gitIdentity.has_value());
	EXPECT_TRUE(snapshot.githubAccounts.empty());
}

TEST(AccountDiscovery, MissingGitAndFailedGhLaunchHaveDifferentGuidanceStates)
{
	AccountDiscoveryRunners runners;
	runners.runGit = [](const scm::GitExecutionRequest&, HANDLE) { return GitNotInstalled(); };
	runners.runGhAuthStatus = [](const GhAuthStatusRequest&, HANDLE) {
		GhAuthStatusResult result;
		result.status = EAccountCommandStatus::LaunchFailed;
		return result;
	};
	const auto snapshot = DiscoverAccounts(Request(), runners);
	EXPECT_EQ(EAccountDiscoveryState::Unavailable, snapshot.state);
	EXPECT_EQ(EAccountSourceState::NotInstalled, snapshot.gitState);
	EXPECT_EQ(EAccountSourceState::Unavailable, snapshot.githubState);
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
	EXPECT_EQ(EAccountSourceState::NotInstalled, snapshot.githubState);
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

TEST(AccountDiscovery, ServiceReportsMissingGitHubExecutableOnlyAfterDiscoverySettles)
{
	AccountDiscoveryService service([](const AccountDiscoveryRequest&, HANDLE) {
		AccountDiscoverySnapshot result;
		result.state = EAccountDiscoveryState::Unavailable;
		result.githubState = EAccountSourceState::NotInstalled;
		return result;
	});
	EXPECT_FALSE(service.GitHubCliExecutableMissing());
	EXPECT_EQ(EAccountRefreshResult::Started, service.RequestRefresh(L"C:\\repo"));
	const auto deadline = ::GetTickCount64() + 1000;
	while (service.Snapshot().state == EAccountDiscoveryState::Loading
		&& ::GetTickCount64() < deadline) ::Sleep(1);
	EXPECT_TRUE(service.GitHubCliExecutableMissing());
	service.Stop();
	EXPECT_FALSE(service.GitHubCliExecutableMissing());
}

TEST(GhCommandRunnerEnvironment, BlockDropsGhDebugForcesFixedValuesKeepsParentSortedTerminated)
{
	struct ScopedEnvironmentVariable final {
		explicit ScopedEnvironmentVariable(const wchar_t* name) : m_name(name)
		{
			std::array<wchar_t, 32768> buffer{};
			const DWORD length = ::GetEnvironmentVariableW(m_name.c_str(), buffer.data(),
				static_cast<DWORD>(buffer.size()));
			if (length != 0 && length < buffer.size()) m_original.assign(buffer.data(), length);
		}
		~ScopedEnvironmentVariable()
		{
			::SetEnvironmentVariableW(m_name.c_str(),
				m_original.empty() ? nullptr : m_original.c_str());
		}
	private:
		std::wstring m_name;
		std::wstring m_original;
	};
	ScopedEnvironmentVariable debug(L"GH_DEBUG");
	ScopedEnvironmentVariable forceTty(L"GH_FORCE_TTY");
	ScopedEnvironmentVariable kept(L"SAKURA_GH_ENV_KEEP");
	ASSERT_TRUE(::SetEnvironmentVariableW(L"GH_DEBUG", L"api"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"GH_FORCE_TTY", L"1"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"SAKURA_GH_ENV_KEEP", L"keep-value"));

	const auto block = detail::BuildGhAuthStatusEnvironmentBlock();

	// Double-NUL terminated, even on the last entry.
	ASSERT_GE(block.size(), 2u);
	EXPECT_EQ(block[block.size() - 1], L'\0');
	EXPECT_EQ(block[block.size() - 2], L'\0');

	std::vector<std::wstring> entries;
	for (const wchar_t* cursor = block.data(); *cursor != L'\0';) {
		entries.emplace_back(cursor);
		cursor += entries.back().size() + 1;
	}

	const auto nameOf = [](const std::wstring& entry) {
		const auto equals = entry.find(L'=');
		return equals == std::wstring::npos ? entry : entry.substr(0, equals);
	};
	const auto equalName = [](std::wstring_view left, std::wstring_view right) {
		return left.size() == right.size()
			&& ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
				right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
	};
	const auto findEntry = [&](std::wstring_view name) -> const std::wstring* {
		const auto found = std::find_if(entries.begin(), entries.end(),
			[&](const std::wstring& entry) { return equalName(nameOf(entry), name); });
		return found == entries.end() ? nullptr : &*found;
	};

	// Inherited GH_DEBUG is suppressed; an unrelated variable survives.
	EXPECT_EQ(findEntry(L"GH_DEBUG"), nullptr);
	const auto* keptEntry = findEntry(L"SAKURA_GH_ENV_KEEP");
	ASSERT_NE(keptEntry, nullptr);
	EXPECT_EQ(keptEntry->substr(keptEntry->find(L'=') + 1), L"keep-value");

	// The fixed non-interactive values are present exactly, and an inherited
	// GH_FORCE_TTY was replaced, not duplicated.
	const auto valueOf = [](const std::wstring* entry) {
		return entry->substr(entry->find(L'=') + 1);
	};
	EXPECT_EQ(valueOf(findEntry(L"GH_FORCE_TTY")), L"0");
	EXPECT_EQ(valueOf(findEntry(L"GH_NO_UPDATE_NOTIFIER")), L"1");
	EXPECT_EQ(valueOf(findEntry(L"GH_PAGER")), L"cat");
	EXPECT_EQ(valueOf(findEntry(L"GH_PROMPT_DISABLED")), L"1");
	EXPECT_EQ(valueOf(findEntry(L"NO_COLOR")), L"1");

	// Windows requires the block sorted case-insensitively.
	EXPECT_TRUE(std::is_sorted(entries.begin(), entries.end(),
		[](const std::wstring& left, const std::wstring& right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
				[](wchar_t a, wchar_t b) { return std::towupper(a) < std::towupper(b); });
		}));
}

// #364: the account-side runner applies the shared GitHub-publisher
// Authenticode gate through the injected detail::GhExecutableGate. Each test
// below keeps one half of the gate observable.
TEST(GhCommandRunnerTrust, UnresolvedExecutableStaysUnavailableWithoutVerifying)
{
	bool verified = false;
	const detail::GhExecutableGate gate{
		[]() -> std::optional<std::wstring> { return std::nullopt; },
		[&](std::wstring_view) { verified = true; return true; },
	};
	GhAuthStatusRequest request;
	request.workingDirectory = CurrentDirectory();
	const auto result = detail::RunGhAuthStatus(request, nullptr, gate);
	EXPECT_EQ(EAccountCommandStatus::Unavailable, result.status);
	EXPECT_FALSE(verified);
}

TEST(GhCommandRunnerTrust, UntrustedResolvedExecutableIsUnavailableAndNeverLaunched)
{
	// The verifier is consulted with the resolved path, and its refusal stops
	// the launch entirely: the nonexistent path makes a skipped gate
	// observable, since launching it would report LaunchFailed instead.
	std::optional<std::wstring> seenPath;
	const detail::GhExecutableGate gate{
		[]() -> std::optional<std::wstring> { return L"C:\\nonexistent\\gh.exe"; },
		[&](const std::wstring_view path) { seenPath.emplace(path); return false; },
	};
	GhAuthStatusRequest request;
	request.workingDirectory = CurrentDirectory();
	const auto result = detail::RunGhAuthStatus(request, nullptr, gate);
	EXPECT_EQ(EAccountCommandStatus::Unavailable, result.status);
	ASSERT_TRUE(seenPath.has_value());
	EXPECT_EQ(*seenPath, L"C:\\nonexistent\\gh.exe");
}

TEST(GhCommandRunnerTrust, ALaunchableButRefusedExecutableStillNeverRuns)
{
	// hostname.exe would exit 0 if it ran, so Unavailable - not Succeeded and
	// not LaunchFailed - proves the publisher gate stopped the launch.
	const std::wstring executable = HostnameExecutable();
	ASSERT_FALSE(executable.empty());
	const detail::GhExecutableGate gate{
		[&]() -> std::optional<std::wstring> { return executable; },
		[](std::wstring_view) { return false; },
	};
	GhAuthStatusRequest request;
	request.workingDirectory = CurrentDirectory();
	const auto result = detail::RunGhAuthStatus(request, nullptr, gate);
	EXPECT_EQ(EAccountCommandStatus::Unavailable, result.status);
}

TEST(GhCommandRunnerTrust, TrustedExecutableRunsThroughTheBoundedBoundary)
{
	// The gate passes and the bounded runner launches for real: hostname.exe
	// (always present beside cmd.exe) rejects the fixed gh arguments and
	// exits nonzero. A refused executable would return Unavailable before
	// any launch and a missing binary LaunchFailed, so Failed with the
	// child's own exit code proves the trusted binary actually ran. Its
	// output is not gh JSON, so the parse stays Invalid.
	const std::wstring executable = HostnameExecutable();
	ASSERT_FALSE(executable.empty());
	const detail::GhExecutableGate gate{
		[&]() -> std::optional<std::wstring> { return executable; },
		[](std::wstring_view) { return true; },
	};
	GhAuthStatusRequest request;
	request.workingDirectory = CurrentDirectory();
	const auto result = detail::RunGhAuthStatus(request, nullptr, gate);
	EXPECT_EQ(EAccountCommandStatus::Failed, result.status);
	EXPECT_NE(0, result.exitCode);
	EXPECT_EQ(EGitHubAuthParseStatus::Invalid, result.parseStatus);
}

TEST(GhCommandRunnerTrust, APartiallySpecifiedGateFailsClosed)
{
	GhAuthStatusRequest request;
	request.workingDirectory = CurrentDirectory();
	EXPECT_EQ(EAccountCommandStatus::InvalidRequest,
		detail::RunGhAuthStatus(request, nullptr, {}).status);
}

} // namespace
} // namespace workbench::account
