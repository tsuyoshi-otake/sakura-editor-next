/*! @file
 * @brief Bounded, cancellable, read-only Git worktree discovery source.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/worktree/GitWorktreePorcelainParser.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace workbench::worktree {

inline constexpr std::uint32_t kGitWorktreeMaximumCommandTimeoutMilliseconds = 60000;
inline constexpr std::size_t kGitWorktreeMaximumRepositoryPathCharacters = 32766;
inline constexpr std::size_t kGitWorktreeMaximumOutputBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kGitWorktreeMaximumDiagnosticBytes = 256u * 1024u;
inline constexpr std::size_t kGitWorktreeMaximumParserRecordBytes = 1024u * 1024u;
inline constexpr std::size_t kGitWorktreeMaximumParserFieldBytes = 256u * 1024u;
inline constexpr std::size_t kGitWorktreeMaximumParserFieldsPerRecord = 64;
inline constexpr std::size_t kGitWorktreeMaximumParserRecords = 4096;
inline constexpr std::uint32_t kGitWorktreeMaximumRetryAttempts = 8;
inline constexpr std::chrono::milliseconds kGitWorktreeMaximumRetryDelay{ 60000 };

//! This request deliberately contains no argv surface. Implementations may only
//! perform the fixed `worktree list --porcelain -z` passive read.
struct GitWorktreeListRequest final {
	std::wstring repositoryPath;
	std::uint32_t timeoutMilliseconds = 15000;
	std::size_t maximumOutputBytes = 4u * 1024u * 1024u;
};

enum class EGitWorktreeCommandStatus : std::uint8_t {
	Succeeded,
	InvalidRequest,
	GitUnavailable,
	LaunchFailed,
	Failed,
	TimedOut,
	Cancelled,
	OutputLimitExceeded,
};

struct GitWorktreeCommandResult final {
	EGitWorktreeCommandStatus status{ EGitWorktreeCommandStatus::InvalidRequest };
	int exitCode = -1;
	std::vector<std::uint8_t> standardOutput;
	std::string standardError;
};

[[nodiscard]] bool IsValidGitWorktreeListRequest(
	const GitWorktreeListRequest& request) noexcept;

class IGitWorktreeListRunner {
public:
	virtual ~IGitWorktreeListRunner() = default;
	[[nodiscard]] virtual GitWorktreeCommandResult RunListPorcelainZ(
		const GitWorktreeListRequest& request, HANDLE cancellation) = 0;
};

//! Production adapter over the existing hardened Git process authority.
class GitWorktreeListRunner final : public IGitWorktreeListRunner {
public:
	[[nodiscard]] GitWorktreeCommandResult RunListPorcelainZ(
		const GitWorktreeListRequest& request, HANDLE cancellation) override;
};

enum class EGitWorktreeDiscoveryStagingPoint : std::uint8_t {
	RepositoryState,
	//! Compatibility name for the completion-promise allocation boundary.
	CompletionState,
	CompletionFuture,
	//! More precise spelling for callers that inject the promise allocation.
	CompletionPromise = CompletionState,
};

//! Narrow owner for the fallible OS/thread primitives used during admission and reaping.
class IGitWorktreeDiscoveryRuntime {
public:
	virtual ~IGitWorktreeDiscoveryRuntime() = default;
	[[nodiscard]] virtual bool FailStagingAt(EGitWorktreeDiscoveryStagingPoint point) noexcept = 0;
	[[nodiscard]] virtual HANDLE CreateManualResetEvent() noexcept = 0;
	[[nodiscard]] virtual std::thread StartWorker(std::function<void()> worker) = 0;
	virtual void JoinWorker(std::thread& worker) noexcept = 0;
};

class IGitWorktreeRetryJitterSource {
public:
	virtual ~IGitWorktreeRetryJitterSource() = default;
	//! Return an inclusive value in [0, maximumInclusive].
	[[nodiscard]] virtual std::uint32_t Next(std::uint32_t maximumInclusive) noexcept = 0;
};

class SystemGitWorktreeRetryJitterSource final : public IGitWorktreeRetryJitterSource {
public:
	SystemGitWorktreeRetryJitterSource() noexcept;
	[[nodiscard]] std::uint32_t Next(std::uint32_t maximumInclusive) noexcept override;

private:
	std::atomic<std::uint64_t> m_state;
};

struct GitWorktreeRetryPolicy final {
	std::uint32_t maximumAttempts = 3;
	std::chrono::milliseconds initialDelay{ 100 };
	std::chrono::milliseconds maximumDelay{ 5000 };
	std::chrono::milliseconds maximumJitter{ 250 };
};

struct GitWorktreeDiscoveryLimits final {
	std::uint32_t commandTimeoutMilliseconds = 15000;
	std::size_t maximumOutputBytes = 4u * 1024u * 1024u;
	std::size_t maximumDiagnosticBytes = 64u * 1024u;
	GitWorktreeParserLimits parser;
	GitWorktreeRetryPolicy retry;
};

enum class EGitWorktreeDiscoveryOutcome : std::uint8_t {
	Succeeded,
	InvalidRequest,
	OperationIdExhausted,
	GitUnavailable,
	LaunchFailed,
	CommandFailed,
	TimedOut,
	Cancelled,
	Stopped,
	OutputLimitExceeded,
	ParseFailed,
	BusyDifferentRequest,
	InternalFailure,
};

struct GitWorktreeDiscoveryResult final {
	EGitWorktreeDiscoveryOutcome outcome{ EGitWorktreeDiscoveryOutcome::InternalFailure };
	std::vector<GitWorktreeRecord> records;
	std::optional<EGitWorktreeParseStatus> parseStatus;
	std::uint64_t operationId = 0;
	std::uint32_t attempts = 0;
	int exitCode = -1;
	std::string diagnostic;
	std::vector<std::chrono::milliseconds> retryDelays;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return outcome == EGitWorktreeDiscoveryOutcome::Succeeded;
	}
};

enum class EGitWorktreeRefreshAdmission : std::uint8_t {
	Started,
	JoinedInFlight,
	RejectedStopped,
	RejectedInvalidRequest,
	RejectedOperationIdExhausted,
	RejectedDifferentRequest,
	//! OS event or worker construction failed before the operation could start.
	//! These primitives are owned here; the injectable runner seam begins after startup.
	RejectedInternalFailure,
};

struct GitWorktreeRefresh final {
	EGitWorktreeRefreshAdmission admission{ EGitWorktreeRefreshAdmission::RejectedInvalidRequest };
	std::uint64_t operationId = 0;
	std::shared_future<GitWorktreeDiscoveryResult> completion;
};

enum class EGitWorktreeCancelStatus : std::uint8_t {
	CancellationRequested,
	NoInFlight,
	Stopped,
};

enum class EGitWorktreeStopStatus : std::uint8_t {
	Stopped,
	AlreadyStopped,
	//! Cancellation was requested from the owned worker; an external caller must join.
	StopRequestedFromWorker,
};

[[nodiscard]] bool IsValidGitWorktreeDiscoveryLimits(
	const GitWorktreeDiscoveryLimits& limits) noexcept;

[[nodiscard]] std::chrono::milliseconds ComputeGitWorktreeRetryDelay(
	std::uint32_t failedAttempt,
	const GitWorktreeRetryPolicy& policy,
	IGitWorktreeRetryJitterSource& jitter) noexcept;

//! One source owns at most one Git process and one retry sequence at a time.
//! Concurrent equivalent-repository Refresh calls share the exact same future;
//! other repositories fail busy. External Stop calls all wait for terminal join.
class GitWorktreeDiscoverySource final {
public:
	GitWorktreeDiscoverySource(std::shared_ptr<IGitWorktreeListRunner> runner,
		std::shared_ptr<IGitWorktreeRetryJitterSource> jitter,
		GitWorktreeDiscoveryLimits limits = {},
		std::shared_ptr<IGitWorktreeDiscoveryRuntime> runtime = {});
	~GitWorktreeDiscoverySource();

	GitWorktreeDiscoverySource(const GitWorktreeDiscoverySource&) = delete;
	GitWorktreeDiscoverySource& operator=(const GitWorktreeDiscoverySource&) = delete;

	[[nodiscard]] GitWorktreeRefresh Refresh(std::wstring_view repositoryPath);
	[[nodiscard]] EGitWorktreeCancelStatus CancelCurrent() noexcept;
	[[nodiscard]] EGitWorktreeStopStatus Stop() noexcept;
	[[nodiscard]] bool IsStopped() const noexcept;

private:
	[[nodiscard]] GitWorktreeDiscoveryResult Execute(std::uint64_t operationId,
		const std::wstring& repositoryPath, HANDLE cancellation) noexcept;
	[[nodiscard]] bool IsStopRequested() const noexcept;
	[[nodiscard]] bool IsCancelRequested() const noexcept;
	[[nodiscard]] GitWorktreeDiscoveryResult CancellationResultNoAlloc(std::uint64_t operationId,
		std::uint32_t attempts,
		std::vector<std::chrono::milliseconds> retryDelays) const noexcept;
	static std::shared_future<GitWorktreeDiscoveryResult> Ready(
		GitWorktreeDiscoveryResult result);

	std::shared_ptr<IGitWorktreeListRunner> m_runner;
	std::shared_ptr<IGitWorktreeRetryJitterSource> m_jitter;
	std::shared_ptr<IGitWorktreeDiscoveryRuntime> m_runtime;
	GitWorktreeDiscoveryLimits m_limits;
	//! A ready result allocated during source construction for no-allocation
	//! admission-failure paths.
	std::shared_future<GitWorktreeDiscoveryResult> m_internalFailureCompletion;
	mutable std::mutex m_mutex;
	std::condition_variable m_stateCondition;
	std::thread m_worker;
	std::thread::id m_workerThreadId;
	HANDLE m_cancellation = nullptr;
	std::shared_future<GitWorktreeDiscoveryResult> m_inFlightCompletion;
	std::wstring m_inFlightRepositoryIdentity;
	std::uint64_t m_inFlightOperationId = 0;
	std::uint64_t m_nextOperationId = 1;
	std::size_t m_refreshCallsInProgress = 0;
	bool m_inFlight = false;
	bool m_cancelRequested = false;
	bool m_stopRequested = false;
	bool m_reapInProgress = false;
	bool m_stopInProgress = false;
	bool m_stopCompleted = false;
};

} // namespace workbench::worktree
