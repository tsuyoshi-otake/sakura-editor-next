/*! @file
 * @brief Bounded, cancellable, read-only Git worktree discovery source.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/worktree/GitWorktreeDiscoverySource.h"

#include "workbench/scm/GitCommandRunner.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace workbench::worktree {
namespace {

EGitWorktreeCommandStatus MapCommandStatus(scm::EGitExecutionStatus status) noexcept
{
	switch (status) {
	case scm::EGitExecutionStatus::Succeeded: return EGitWorktreeCommandStatus::Succeeded;
	case scm::EGitExecutionStatus::Failed: return EGitWorktreeCommandStatus::Failed;
	case scm::EGitExecutionStatus::InvalidRequest: return EGitWorktreeCommandStatus::InvalidRequest;
	case scm::EGitExecutionStatus::GitUnavailable: return EGitWorktreeCommandStatus::GitUnavailable;
	case scm::EGitExecutionStatus::LaunchFailed: return EGitWorktreeCommandStatus::LaunchFailed;
	case scm::EGitExecutionStatus::TimedOut: return EGitWorktreeCommandStatus::TimedOut;
	case scm::EGitExecutionStatus::Cancelled: return EGitWorktreeCommandStatus::Cancelled;
	case scm::EGitExecutionStatus::OutputLimitExceeded: return EGitWorktreeCommandStatus::OutputLimitExceeded;
	}
	return EGitWorktreeCommandStatus::InvalidRequest;
}

bool IsRetryable(EGitWorktreeCommandStatus status) noexcept
{
	return status == EGitWorktreeCommandStatus::LaunchFailed
		|| status == EGitWorktreeCommandStatus::Failed
		|| status == EGitWorktreeCommandStatus::TimedOut;
}

EGitWorktreeDiscoveryOutcome MapOutcome(EGitWorktreeCommandStatus status) noexcept
{
	switch (status) {
	case EGitWorktreeCommandStatus::Succeeded: return EGitWorktreeDiscoveryOutcome::Succeeded;
	case EGitWorktreeCommandStatus::InvalidRequest: return EGitWorktreeDiscoveryOutcome::InvalidRequest;
	case EGitWorktreeCommandStatus::GitUnavailable: return EGitWorktreeDiscoveryOutcome::GitUnavailable;
	case EGitWorktreeCommandStatus::LaunchFailed: return EGitWorktreeDiscoveryOutcome::LaunchFailed;
	case EGitWorktreeCommandStatus::Failed: return EGitWorktreeDiscoveryOutcome::CommandFailed;
	case EGitWorktreeCommandStatus::TimedOut: return EGitWorktreeDiscoveryOutcome::TimedOut;
	case EGitWorktreeCommandStatus::Cancelled: return EGitWorktreeDiscoveryOutcome::Cancelled;
	case EGitWorktreeCommandStatus::OutputLimitExceeded: return EGitWorktreeDiscoveryOutcome::OutputLimitExceeded;
	}
	return EGitWorktreeDiscoveryOutcome::InternalFailure;
}

std::string BoundedDiagnostic(std::string value, std::size_t maximum)
{
	if (value.size() > maximum) value.resize(maximum);
	return value;
}

GitWorktreeDiscoveryResult Terminal(EGitWorktreeDiscoveryOutcome outcome,
	std::uint64_t operationId, std::string diagnostic = {})
{
	GitWorktreeDiscoveryResult result;
	result.outcome = outcome;
	result.operationId = operationId;
	result.diagnostic = std::move(diagnostic);
	return result;
}

//! Build the fallback used by noexcept paths after an allocation failure.  Do
//! not add a diagnostic here: even constructing a long std::string can allocate
//! while handling std::bad_alloc.
GitWorktreeDiscoveryResult InternalFailureNoAlloc(std::uint64_t operationId,
	std::uint32_t attempts = 0) noexcept
{
	GitWorktreeDiscoveryResult result;
	result.outcome = EGitWorktreeDiscoveryOutcome::InternalFailure;
	result.operationId = operationId;
	result.attempts = attempts;
	return result;
}

GitWorktreeDiscoveryResult InternalFailureNoAlloc(std::uint64_t operationId,
	std::uint32_t attempts, std::vector<std::chrono::milliseconds>&& retryDelays) noexcept
{
	auto result = InternalFailureNoAlloc(operationId, attempts);
	result.retryDelays = std::move(retryDelays);
	return result;
}

static_assert(std::is_nothrow_move_constructible_v<GitWorktreeDiscoveryResult>);
static_assert(std::is_nothrow_move_assignable_v<GitWorktreeDiscoveryResult>);

class SystemGitWorktreeDiscoveryRuntime final : public IGitWorktreeDiscoveryRuntime {
public:
	bool FailStagingAt(EGitWorktreeDiscoveryStagingPoint) noexcept override
	{
		return false;
	}

	HANDLE CreateManualResetEvent() noexcept override
	{
		return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	std::thread StartWorker(std::function<void()> worker) override
	{
		return std::thread(std::move(worker));
	}

	void JoinWorker(std::thread& worker) noexcept override
	{
		if (worker.joinable()) worker.join();
	}
};

} // namespace

bool IsValidGitWorktreeListRequest(const GitWorktreeListRequest& request) noexcept
{
	return !request.repositoryPath.empty()
		&& request.repositoryPath.size() <= kGitWorktreeMaximumRepositoryPathCharacters
		&& request.timeoutMilliseconds != 0
		&& request.timeoutMilliseconds <= kGitWorktreeMaximumCommandTimeoutMilliseconds
		&& request.maximumOutputBytes != 0
		&& request.maximumOutputBytes <= kGitWorktreeMaximumOutputBytes;
}

GitWorktreeCommandResult GitWorktreeListRunner::RunListPorcelainZ(
	const GitWorktreeListRequest& request, HANDLE cancellation)
{
	if (!IsValidGitWorktreeListRequest(request)) {
		return { EGitWorktreeCommandStatus::InvalidRequest };
	}
	scm::GitExecutionRequest git;
	git.workingDirectory = request.repositoryPath;
	git.arguments = { L"worktree", L"list", L"--porcelain", L"-z" };
	git.policy = scm::EGitRequestPolicy::PassiveRepositoryRead;
	git.timeoutMilliseconds = request.timeoutMilliseconds;
	git.maximumOutputBytes = request.maximumOutputBytes;
	auto executed = scm::RunGit(git, cancellation);
	GitWorktreeCommandResult result;
	result.status = MapCommandStatus(executed.status);
	result.exitCode = executed.exitCode;
	result.standardOutput = std::move(executed.standardOutput);
	result.standardError = std::move(executed.standardError);
	return result;
}

SystemGitWorktreeRetryJitterSource::SystemGitWorktreeRetryJitterSource() noexcept
	: m_state((::GetTickCount64() << 17) ^ static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)))
{
	if (m_state.load(std::memory_order_relaxed) == 0) m_state.store(0x9e3779b97f4a7c15ULL);
}

std::uint32_t SystemGitWorktreeRetryJitterSource::Next(std::uint32_t maximumInclusive) noexcept
{
	auto current = m_state.load(std::memory_order_relaxed);
	for (;;) {
		auto next = current;
		next ^= next << 13;
		next ^= next >> 7;
		next ^= next << 17;
		if (next == 0) next = 0x9e3779b97f4a7c15ULL;
		if (m_state.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
			if (maximumInclusive == (std::numeric_limits<std::uint32_t>::max)()) {
				return static_cast<std::uint32_t>(next);
			}
			return static_cast<std::uint32_t>(next % (static_cast<std::uint64_t>(maximumInclusive) + 1));
		}
	}
}

bool IsValidGitWorktreeDiscoveryLimits(const GitWorktreeDiscoveryLimits& limits) noexcept
{
	return limits.commandTimeoutMilliseconds != 0
		&& limits.commandTimeoutMilliseconds <= kGitWorktreeMaximumCommandTimeoutMilliseconds
		&& limits.maximumOutputBytes != 0
		&& limits.maximumOutputBytes <= kGitWorktreeMaximumOutputBytes
		&& limits.maximumOutputBytes <= limits.parser.maximumInputBytes
		&& limits.maximumDiagnosticBytes != 0
		&& limits.maximumDiagnosticBytes <= kGitWorktreeMaximumDiagnosticBytes
		&& limits.parser.maximumInputBytes != 0
		&& limits.parser.maximumInputBytes <= kGitWorktreeMaximumOutputBytes
		&& limits.parser.maximumRecordBytes != 0
		&& limits.parser.maximumRecordBytes <= kGitWorktreeMaximumParserRecordBytes
		&& limits.parser.maximumRecordBytes <= limits.parser.maximumInputBytes
		&& limits.parser.maximumFieldBytes != 0
		&& limits.parser.maximumFieldBytes <= kGitWorktreeMaximumParserFieldBytes
		&& limits.parser.maximumFieldBytes <= limits.parser.maximumRecordBytes
		&& limits.parser.maximumFieldsPerRecord != 0
		&& limits.parser.maximumFieldsPerRecord <= kGitWorktreeMaximumParserFieldsPerRecord
		&& limits.parser.maximumRecords != 0
		&& limits.parser.maximumRecords <= kGitWorktreeMaximumParserRecords
		&& limits.retry.maximumAttempts >= 1
		&& limits.retry.maximumAttempts <= kGitWorktreeMaximumRetryAttempts
		&& limits.retry.initialDelay >= std::chrono::milliseconds::zero()
		&& limits.retry.maximumDelay >= limits.retry.initialDelay
		&& limits.retry.maximumDelay <= kGitWorktreeMaximumRetryDelay
		&& limits.retry.maximumJitter >= std::chrono::milliseconds::zero()
		&& limits.retry.maximumJitter <= limits.retry.maximumDelay
		&& limits.retry.maximumJitter <= kGitWorktreeMaximumRetryDelay;
}

std::chrono::milliseconds ComputeGitWorktreeRetryDelay(std::uint32_t failedAttempt,
	const GitWorktreeRetryPolicy& policy, IGitWorktreeRetryJitterSource& jitter) noexcept
{
	std::uint64_t multiplier = 1;
	for (std::uint32_t index = 1; index < failedAttempt && multiplier < (1ULL << 31); ++index) {
		multiplier <<= 1;
	}
	const auto initial = static_cast<std::uint64_t>((std::max)(std::int64_t{ 0 }, policy.initialDelay.count()));
	const auto maximum = static_cast<std::uint64_t>((std::max)(std::int64_t{ 0 }, policy.maximumDelay.count()));
	const auto jitterMaximum = static_cast<std::uint64_t>((std::max)(std::int64_t{ 0 }, policy.maximumJitter.count()));
	const auto base = (initial == 0 || multiplier <= maximum / initial)
		? (std::min)(initial * multiplier, maximum)
		: maximum;
	const auto remaining = maximum - base;
	const auto allowedJitter = (std::min)(jitterMaximum, remaining);
	const auto injected = allowedJitter == 0 ? 0u
		: jitter.Next(static_cast<std::uint32_t>(allowedJitter));
	return std::chrono::milliseconds(base + injected);
}

GitWorktreeDiscoverySource::GitWorktreeDiscoverySource(
	std::shared_ptr<IGitWorktreeListRunner> runner,
	std::shared_ptr<IGitWorktreeRetryJitterSource> jitter,
	GitWorktreeDiscoveryLimits limits,
	std::shared_ptr<IGitWorktreeDiscoveryRuntime> runtime)
	: m_runner(std::move(runner)), m_jitter(std::move(jitter)),
	  m_runtime(runtime ? std::move(runtime) : std::make_shared<SystemGitWorktreeDiscoveryRuntime>()),
	  m_limits(std::move(limits)),
	  m_internalFailureCompletion(Ready(Terminal(
		EGitWorktreeDiscoveryOutcome::InternalFailure, 0,
		"failed to stage discovery request state")))
{
}

GitWorktreeDiscoverySource::~GitWorktreeDiscoverySource()
{
	(void)Stop();
}

std::shared_future<GitWorktreeDiscoveryResult> GitWorktreeDiscoverySource::Ready(
	GitWorktreeDiscoveryResult result)
{
	std::promise<GitWorktreeDiscoveryResult> promise;
	promise.set_value(std::move(result));
	return promise.get_future().share();
}

GitWorktreeRefresh GitWorktreeDiscoverySource::Refresh(std::wstring_view repositoryPath)
{
	{
		std::lock_guard entryLock(m_mutex);
		if (m_stopRequested) {
			auto result = Terminal(EGitWorktreeDiscoveryOutcome::Stopped, 0, "source is stopped");
			return { EGitWorktreeRefreshAdmission::RejectedStopped, 0, Ready(std::move(result)) };
		}
		++m_refreshCallsInProgress;
	}
	auto releaseRefresh = [this](void*) noexcept {
		std::lock_guard releaseLock(m_mutex);
		--m_refreshCallsInProgress;
		// Keep the notification inside the lock.  Stop can destroy the source
		// immediately after observing the zero count; there must be no member
		// access after that quiescence fence.
		m_stateCondition.notify_all();
	};
	std::unique_ptr<void, decltype(releaseRefresh)> refreshGuard(
		static_cast<void*>(this), releaseRefresh);

	if (repositoryPath.empty() || repositoryPath.size() > kGitWorktreeMaximumRepositoryPathCharacters
		|| !m_runner || !m_jitter || !m_runtime || !IsValidGitWorktreeDiscoveryLimits(m_limits)) {
		auto result = Terminal(EGitWorktreeDiscoveryOutcome::InvalidRequest, 0,
			"discovery request or limits are invalid");
		return { EGitWorktreeRefreshAdmission::RejectedInvalidRequest, 0, Ready(std::move(result)) };
	}

	std::wstring stagedRepositoryPath;
	std::wstring stagedIdentity;
	std::shared_ptr<std::promise<GitWorktreeDiscoveryResult>> stagedPromise;
	std::shared_future<GitWorktreeDiscoveryResult> stagedCompletion;
	try {
		auto normalizedRepository = NormalizeWindowsWorktreePath(repositoryPath);
		if (!normalizedRepository) {
			auto result = Terminal(EGitWorktreeDiscoveryOutcome::InvalidRequest, 0,
				"repository path is not an unambiguous absolute Windows path");
			return { EGitWorktreeRefreshAdmission::RejectedInvalidRequest, 0, Ready(std::move(result)) };
		}
		if (m_runtime->FailStagingAt(EGitWorktreeDiscoveryStagingPoint::RepositoryState)) {
			throw std::bad_alloc();
		}
		stagedRepositoryPath.assign(repositoryPath);
		stagedIdentity = std::move(normalizedRepository->second);
		if (m_runtime->FailStagingAt(EGitWorktreeDiscoveryStagingPoint::CompletionPromise)) {
			throw std::bad_alloc();
		}
		stagedPromise = std::make_shared<std::promise<GitWorktreeDiscoveryResult>>();
		if (m_runtime->FailStagingAt(EGitWorktreeDiscoveryStagingPoint::CompletionFuture)) {
			throw std::bad_alloc();
		}
		stagedCompletion = stagedPromise->get_future().share();
	} catch (...) {
		return { EGitWorktreeRefreshAdmission::RejectedInternalFailure, 0,
			m_internalFailureCompletion };
	}

	auto completeStaged = [&](EGitWorktreeRefreshAdmission admission,
		EGitWorktreeDiscoveryOutcome outcome, std::string diagnostic) -> GitWorktreeRefresh {
		auto result = Terminal(outcome, 0, std::move(diagnostic));
		stagedPromise->set_value(std::move(result));
		return { admission, 0, stagedCompletion };
	};

	std::unique_lock lock(m_mutex);
	for (;;) {
		if (m_stopRequested) {
			return completeStaged(EGitWorktreeRefreshAdmission::RejectedStopped,
				EGitWorktreeDiscoveryOutcome::Stopped, "source is stopped");
		}
		if (m_reapInProgress) {
			m_stateCondition.wait(lock, [this] { return m_stopRequested || !m_reapInProgress; });
			continue;
		}
		if (m_inFlight) {
			if (stagedIdentity == m_inFlightRepositoryIdentity) {
				return { EGitWorktreeRefreshAdmission::JoinedInFlight,
					m_inFlightOperationId, m_inFlightCompletion };
			}
			return completeStaged(EGitWorktreeRefreshAdmission::RejectedDifferentRequest,
				EGitWorktreeDiscoveryOutcome::BusyDifferentRequest,
				"another repository discovery is already in flight");
		}
		if (m_worker.joinable() || m_cancellation != nullptr) {
			m_reapInProgress = true;
			std::thread completedWorker = std::move(m_worker);
			HANDLE completedCancellation = std::exchange(m_cancellation, nullptr);
			lock.unlock();
			m_runtime->JoinWorker(completedWorker);
			if (completedCancellation != nullptr) ::CloseHandle(completedCancellation);
			lock.lock();
			m_reapInProgress = false;
			m_stateCondition.notify_all();
			continue;
		}
		break;
	}

	if (m_nextOperationId == 0) {
		return completeStaged(EGitWorktreeRefreshAdmission::RejectedOperationIdExhausted,
			EGitWorktreeDiscoveryOutcome::OperationIdExhausted,
			"discovery operation identity is exhausted");
	}

	HANDLE stagedCancellation = m_runtime->CreateManualResetEvent();
	if (stagedCancellation == nullptr) {
		// Resource failure must not construct a diagnostic while recovering from
		// the failed allocation.  The prebuilt ready future preserves typed
		// admission/result parity and leaves operation identity unconsumed.
		return { EGitWorktreeRefreshAdmission::RejectedInternalFailure, 0,
			m_internalFailureCompletion };
	}
	const std::uint64_t operationId = m_nextOperationId;
	std::thread stagedWorker;
	try {
		std::function<void()> worker = [this, promise = stagedPromise, operationId,
			repository = std::move(stagedRepositoryPath), cancellation = stagedCancellation]() mutable {
			// Refresh holds m_mutex across worker construction and publication.
			// Taking this first lock is the non-fallible publication barrier: the
			// worker cannot observe a partially published operation and no startup
			// event or SetEvent result is needed.
			{
				std::lock_guard startLock(m_mutex);
				m_workerThreadId = std::this_thread::get_id();
			}
			auto result = Execute(operationId, repository, cancellation);
			{
				std::lock_guard finishLock(m_mutex);
				if (m_stopRequested || m_cancelRequested) {
					result.outcome = m_stopRequested ? EGitWorktreeDiscoveryOutcome::Stopped
						: EGitWorktreeDiscoveryOutcome::Cancelled;
					result.records.clear();
					result.parseStatus.reset();
					result.exitCode = -1;
					result.diagnostic.clear();
				}
				// Promise/future state was staged before publication and this worker
				// is its sole completion owner. GitWorktreeDiscoveryResult has a
				// statically verified no-throw move, so terminal publication performs
				// no allocation and set_value is reached exactly once.
				promise->set_value(std::move(result));
				m_inFlight = false;
				m_inFlightOperationId = 0;
				m_inFlightRepositoryIdentity.clear();
				m_workerThreadId = {};
			}
			m_stateCondition.notify_all();
		};
		stagedWorker = m_runtime->StartWorker(std::move(worker));
	} catch (const std::bad_alloc&) {
		::CloseHandle(stagedCancellation);
		return { EGitWorktreeRefreshAdmission::RejectedInternalFailure, 0,
			m_internalFailureCompletion };
	} catch (...) {
		::CloseHandle(stagedCancellation);
		// Thread startup exceptions are handled through the same prebuilt result
		// as allocation failures.  Constructing a diagnostic here would make the
		// failure path itself fallible and could escape without admission/result
		// parity after the cancellation handle has been released.
		return { EGitWorktreeRefreshAdmission::RejectedInternalFailure, 0,
			m_internalFailureCompletion };
	}

	// All potentially throwing construction is complete.  Publication below uses
	// only no-throw moves/writes while the worker is held behind m_mutex; its first
	// lock above prevents it from entering Execute before this state is visible.
	m_inFlightCompletion = std::move(stagedCompletion);
	m_inFlightRepositoryIdentity = std::move(stagedIdentity);
	m_worker = std::move(stagedWorker);
	m_cancellation = stagedCancellation;
	m_inFlightOperationId = operationId;
	m_cancelRequested = false;
	m_inFlight = true;
	m_nextOperationId = operationId == (std::numeric_limits<std::uint64_t>::max)() ? 0 : operationId + 1;
	return { EGitWorktreeRefreshAdmission::Started, operationId, m_inFlightCompletion };
}

EGitWorktreeCancelStatus GitWorktreeDiscoverySource::CancelCurrent() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopRequested) return EGitWorktreeCancelStatus::Stopped;
	if (!m_inFlight) return EGitWorktreeCancelStatus::NoInFlight;
	m_cancelRequested = true;
	if (m_cancellation != nullptr) (void)::SetEvent(m_cancellation);
	return EGitWorktreeCancelStatus::CancellationRequested;
}

EGitWorktreeStopStatus GitWorktreeDiscoverySource::Stop() noexcept
{
	std::thread worker;
	HANDLE cancellation = nullptr;
	bool ownsExternalCleanup = false;
	{
		std::unique_lock lock(m_mutex);
		if (m_stopCompleted) return EGitWorktreeStopStatus::AlreadyStopped;
		m_stopRequested = true;
		if (m_cancellation != nullptr) (void)::SetEvent(m_cancellation);
		if (m_workerThreadId == std::this_thread::get_id()) {
			// A worker may request cancellation, but it cannot join itself or
			// close the handle owned by the external cleanup path.  This is a
			// request-only, nonterminal result; a later external Stop owns the
			// terminal transition.
			return EGitWorktreeStopStatus::StopRequestedFromWorker;
		}
		if (m_stopInProgress) {
			m_stateCondition.wait(lock, [this] { return m_stopCompleted; });
			return EGitWorktreeStopStatus::AlreadyStopped;
		}
		// Reserve the sole external cleanup owner before waiting for active
		// Refresh calls or an existing reaper.  Other external callers wait for
		// this owner to publish the terminal state and return AlreadyStopped.
		m_stopInProgress = true;
		ownsExternalCleanup = true;
		while (m_reapInProgress || m_refreshCallsInProgress != 0) {
			m_stateCondition.wait(lock, [this] {
				return m_stopCompleted || (!m_reapInProgress && m_refreshCallsInProgress == 0);
			});
			if (m_stopCompleted) return EGitWorktreeStopStatus::AlreadyStopped;
		}
		if (m_worker.joinable()) worker = std::move(m_worker);
		cancellation = std::exchange(m_cancellation, nullptr);
	}
	m_runtime->JoinWorker(worker);
	if (cancellation != nullptr) ::CloseHandle(cancellation);
	{
		std::lock_guard lock(m_mutex);
		m_stopInProgress = false;
		m_stopCompleted = true;
		m_stateCondition.notify_all();
	}
	return ownsExternalCleanup ? EGitWorktreeStopStatus::Stopped
		: EGitWorktreeStopStatus::AlreadyStopped;
}

bool GitWorktreeDiscoverySource::IsStopped() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_stopRequested;
}

bool GitWorktreeDiscoverySource::IsStopRequested() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_stopRequested;
}

bool GitWorktreeDiscoverySource::IsCancelRequested() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_cancelRequested;
}

GitWorktreeDiscoveryResult GitWorktreeDiscoverySource::CancellationResultNoAlloc(
	std::uint64_t operationId, std::uint32_t attempts,
	std::vector<std::chrono::milliseconds> retryDelays) const noexcept
{
	auto result = InternalFailureNoAlloc(operationId, attempts, std::move(retryDelays));
	result.outcome = IsStopRequested() ? EGitWorktreeDiscoveryOutcome::Stopped
		: EGitWorktreeDiscoveryOutcome::Cancelled;
	return result;
}

GitWorktreeDiscoveryResult GitWorktreeDiscoverySource::Execute(std::uint64_t operationId,
	const std::wstring& repositoryPath, HANDLE cancellation) noexcept
{
	std::vector<std::chrono::milliseconds> retryDelays;
	std::uint32_t currentAttempt = 0;
	try {
		// reserve is fallible even though Execute is noexcept.  Keep it inside
		// the typed catch so an injected or real allocation failure returns a
		// terminal result instead of terminating the worker process.
		retryDelays.reserve(m_limits.retry.maximumAttempts - 1);
		for (std::uint32_t attempt = 1; attempt <= m_limits.retry.maximumAttempts; ++attempt) {
			currentAttempt = attempt;
			if (IsStopRequested() || IsCancelRequested()) {
				return CancellationResultNoAlloc(operationId, attempt - 1, std::move(retryDelays));
			}
			GitWorktreeListRequest request;
			request.repositoryPath = repositoryPath;
			request.timeoutMilliseconds = m_limits.commandTimeoutMilliseconds;
			request.maximumOutputBytes = m_limits.maximumOutputBytes;
			auto command = m_runner->RunListPorcelainZ(request, cancellation);
			if (IsStopRequested() || IsCancelRequested()
				|| command.status == EGitWorktreeCommandStatus::Cancelled) {
				return CancellationResultNoAlloc(operationId, attempt, std::move(retryDelays));
			}
			if (command.standardOutput.size() > m_limits.maximumOutputBytes) {
				auto result = Terminal(EGitWorktreeDiscoveryOutcome::OutputLimitExceeded,
					operationId, "command returned stdout beyond the discovery budget");
				result.attempts = attempt;
				result.retryDelays = std::move(retryDelays);
				return result;
			}
			if (command.status == EGitWorktreeCommandStatus::Succeeded) {
				auto parsed = ParseGitWorktreePorcelainZ(
					std::span<const std::uint8_t>(command.standardOutput), m_limits.parser);
				GitWorktreeDiscoveryResult result;
				result.operationId = operationId;
				result.attempts = attempt;
				result.retryDelays = std::move(retryDelays);
				if (parsed.Succeeded()) {
					result.outcome = EGitWorktreeDiscoveryOutcome::Succeeded;
					result.records = std::move(parsed.records);
				} else {
					result.outcome = EGitWorktreeDiscoveryOutcome::ParseFailed;
					result.parseStatus = parsed.status;
					result.diagnostic = BoundedDiagnostic(std::move(parsed.diagnostic),
						m_limits.maximumDiagnosticBytes);
				}
				return result;
			}
			if (IsRetryable(command.status) && attempt < m_limits.retry.maximumAttempts) {
				const auto delay = ComputeGitWorktreeRetryDelay(attempt, m_limits.retry, *m_jitter);
				retryDelays.push_back(delay);
				if (delay > std::chrono::milliseconds::zero()
					&& ::WaitForSingleObject(cancellation, static_cast<DWORD>(delay.count())) == WAIT_OBJECT_0) {
					return CancellationResultNoAlloc(operationId, attempt, std::move(retryDelays));
				}
				continue;
			}

			GitWorktreeDiscoveryResult result;
			result.outcome = MapOutcome(command.status);
			result.operationId = operationId;
			result.attempts = attempt;
			result.exitCode = command.exitCode;
			result.diagnostic = BoundedDiagnostic(std::move(command.standardError),
				m_limits.maximumDiagnosticBytes);
			result.retryDelays = std::move(retryDelays);
			return result;
		}
	} catch (const std::bad_alloc&) {
		return InternalFailureNoAlloc(operationId, currentAttempt, std::move(retryDelays));
	} catch (...) {
		return InternalFailureNoAlloc(operationId, currentAttempt, std::move(retryDelays));
	}
	return InternalFailureNoAlloc(operationId, currentAttempt, std::move(retryDelays));
}

} // namespace workbench::worktree
