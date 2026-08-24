/*! @file
 * @brief Bounded local Git and GitHub CLI account discovery.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "workbench/scm/GitCommandRunner.h"

namespace workbench::account {

//! A discovery snapshot is always in one of these explicit states.
enum class EAccountDiscoveryState : std::uint8_t {
	//! A worker has been requested but has not reached a terminal result.
	Loading,
	//! Both local sources were read and parsed without a source failure.
	Ready,
	//! At least one source produced usable data, while another source failed or was incomplete.
	Partial,
	//! No source produced usable account data.
	Unavailable,
	//! The owning service has been stopped and cannot be refreshed again.
	Stopped,
};

//! Per-source state keeps an unconfigured Git identity distinct from an
//! unavailable executable or a failed/invalid response. A valid GitHub query
//! with zero accounts is Ready plus an empty account vector.
enum class EAccountSourceState : std::uint8_t {
	Loading,
	Ready,
	Partial,
	Unconfigured,
	Unavailable,
	Failed,
};

[[nodiscard]] constexpr bool IsTerminalAccountDiscoveryState(
	const EAccountDiscoveryState state) noexcept
{
	return state != EAccountDiscoveryState::Loading;
}

//! The process boundary deliberately has no token, scope, or token-source fields.
enum class EAccountCommandStatus : std::uint8_t {
	Succeeded,
	Failed,
	Unavailable,
	LaunchFailed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
	InvalidRequest,
};

//! Bounds applied before process creation and again by the JSON parser.
struct AccountDiscoveryLimits final {
	std::size_t maximumJsonBytes{256u * 1024u};
	std::size_t maximumHosts{64};
	std::size_t maximumAccounts{256};
	std::size_t maximumStringBytes{1024};
	std::size_t maximumNestingDepth{32};

	[[nodiscard]] bool IsValid() const noexcept;
};

inline constexpr std::uint32_t kDefaultAccountCommandTimeoutMilliseconds = 5000;
inline constexpr std::size_t kDefaultAccountCommandOutputBytes = 256u * 1024u;

struct AccountDiscoveryRequest final {
	std::wstring workingDirectory;
	std::uint32_t timeoutMilliseconds{kDefaultAccountCommandTimeoutMilliseconds};
	std::size_t maximumOutputBytes{kDefaultAccountCommandOutputBytes};
	AccountDiscoveryLimits limits{};

	[[nodiscard]] bool IsValid() const noexcept;
};

struct AccountGitIdentity final {
	std::wstring userName;
	std::wstring userEmail;

	[[nodiscard]] bool operator==(const AccountGitIdentity&) const noexcept = default;
};

//! gh reports the authentication state separately from the active marker.
//! Unknown includes an omitted, empty, or newer state value and is deliberately
//! retained as an explicit value instead of being mistaken for success.
enum class EGitHubAccountState : std::uint8_t {
	Success,
	Failure,
	Unknown,
};

struct GitHubCliAccount final {
	std::wstring host;
	std::wstring login;
	std::wstring gitProtocol;
	bool active{};
	EGitHubAccountState state{EGitHubAccountState::Unknown};

	[[nodiscard]] bool operator==(const GitHubCliAccount&) const noexcept = default;
};

struct AccountDiscoverySnapshot final {
	EAccountDiscoveryState state{EAccountDiscoveryState::Stopped};
	EAccountSourceState gitState{EAccountSourceState::Unavailable};
	EAccountSourceState githubState{EAccountSourceState::Unavailable};
	std::optional<AccountGitIdentity> gitIdentity;
	std::vector<GitHubCliAccount> githubAccounts;
	std::uint64_t revision{};

	[[nodiscard]] bool operator==(const AccountDiscoverySnapshot&) const noexcept = default;
};

//! Parser result intentionally contains only display-safe account data.
enum class EGitHubAuthParseStatus : std::uint8_t {
	Ready,
	Partial,
	Invalid,
	Oversized,
};

struct GitHubAuthParseResult final {
	EGitHubAuthParseStatus status{EGitHubAuthParseStatus::Invalid};
	std::vector<GitHubCliAccount> accounts;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EGitHubAuthParseStatus::Ready
			|| status == EGitHubAuthParseStatus::Partial;
	}
};

//! The safe gh runner accepts no caller-supplied command arguments. The
//! implementation always invokes exactly `auth status --json hosts`.
struct GhAuthStatusRequest final {
	std::wstring workingDirectory;
	std::uint32_t timeoutMilliseconds{kDefaultAccountCommandTimeoutMilliseconds};
	std::size_t maximumOutputBytes{kDefaultAccountCommandOutputBytes};
	AccountDiscoveryLimits limits{};

	[[nodiscard]] bool IsValid() const noexcept;
};

struct GhAuthStatusResult final {
	EAccountCommandStatus status{EAccountCommandStatus::InvalidRequest};
	int exitCode{-1};
	//! Only parser status and display-safe records cross this process boundary.
	//! Raw stdout is consumed and destroyed inside RunGhAuthStatus, so token,
	//! scope, and token-source members cannot escape through this result.
	EGitHubAuthParseStatus parseStatus{EGitHubAuthParseStatus::Invalid};
	std::vector<GitHubCliAccount> accounts;
};

using GhAuthStatusRunner = std::function<GhAuthStatusResult(
	const GhAuthStatusRequest&, HANDLE stop)>;

using GitAccountRunner = std::function<scm::GitExecutionResult(
	const scm::GitExecutionRequest&, HANDLE stop)>;

struct AccountDiscoveryRunners final {
	GitAccountRunner runGit;
	GhAuthStatusRunner runGhAuthStatus;
};

//! Parse the `hosts` object returned by `gh auth status --json hosts`.
//! Unknown members, including token/scopes/tokenSource-like members, are
//! ignored and never copied into the result.
[[nodiscard]] GitHubAuthParseResult ParseGitHubAuthStatus(
	std::string_view json,
	AccountDiscoveryLimits limits = {});

//! Alias kept descriptive for callers that do not need to mention the CLI.
[[nodiscard]] inline GitHubAuthParseResult ParseGitHubCliAccounts(
	std::string_view json,
	AccountDiscoveryLimits limits = {})
{
	return ParseGitHubAuthStatus(json, limits);
}

//! The fixed command vector is public so process-boundary tests can assert that
//! no token-display flag can be supplied by a caller.
[[nodiscard]] std::vector<std::wstring> BuildGhAuthStatusArguments();

//! Bounded, cancellable execution of the fixed gh authentication query.
[[nodiscard]] GhAuthStatusResult RunGhAuthStatus(
	const GhAuthStatusRequest& request, HANDLE stop);

//! Run both local sources synchronously through the injected boundaries. The
//! default runners are workbench::scm::RunGit and RunGhAuthStatus.
[[nodiscard]] AccountDiscoverySnapshot DiscoverAccounts(
	const AccountDiscoveryRequest& request,
	const AccountDiscoveryRunners& runners = {},
	HANDLE stop = nullptr);

//! A status returned by the asynchronous wrapper's request gate.
enum class EAccountRefreshResult : std::uint8_t {
	Started,
	AlreadyInFlight,
	RejectedStopped,
	InvalidRequest,
	Unavailable,
};

using AccountDiscoveryFunction = std::function<AccountDiscoverySnapshot(
	const AccountDiscoveryRequest&, HANDLE stop)>;

/*! @brief Thread-safe, one-worker account discovery owner.

No detached work or retry occurs. A second request while the first worker is
running is explicitly deduplicated. Stop signals the shared event, waits for
the worker, and publishes Stopped before returning.
*/
class AccountDiscoveryService final {
public:
	explicit AccountDiscoveryService(
		AccountDiscoveryFunction discover = {},
		AccountDiscoveryRequest defaults = {});
	~AccountDiscoveryService();

	AccountDiscoveryService(const AccountDiscoveryService&) = delete;
	AccountDiscoveryService& operator=(const AccountDiscoveryService&) = delete;

	[[nodiscard]] EAccountRefreshResult RequestRefresh(std::wstring workingDirectory);
	[[nodiscard]] AccountDiscoverySnapshot Snapshot() const;
	void Stop() noexcept;

private:
	[[nodiscard]] std::uint64_t NextRevisionLocked() noexcept;
	void PublishWorkerResult(AccountDiscoverySnapshot result) noexcept;

	const AccountDiscoveryFunction m_discover;
	const AccountDiscoveryRequest m_defaults;
	mutable std::mutex m_mutex;
	std::condition_variable m_condition;
	std::thread m_worker;
	std::thread::id m_workerId{};
	HANDLE m_stopEvent{};
	AccountDiscoverySnapshot m_snapshot;
	bool m_workerRunning{};
	bool m_stopJoining{};
	bool m_stopping{};
	std::uint64_t m_revision{};
};

} // namespace workbench::account
