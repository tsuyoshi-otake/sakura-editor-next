/*! @file
 * @brief Bounded task-to-terminal orchestration independent of Win32 and ConPTY.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "terminal/session/TerminalSession.h"
#include "workbench/tasks/TaskConfigurationCatalog.h"

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::tasks {

//! Caller identity used to make a task start/cancel/close exactly replayable.
struct TaskExecutionOperation final {
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;
};

/*! 
	@brief A launch description passed to the host-owned terminal policy.

	Process tasks map directly to `terminalLaunchOptions.executablePath` and its
	argument vector. Shell tasks deliberately leave that executable path empty:
	the factory chooses the shell/interpreter and its quoting policy, and receives
	the command and argument tokens separately. This service never concatenates a
	command line or assumes PowerShell/cmd.exe.
*/
struct TaskTerminalLaunchRequest final {
	ETaskExecutionKind executionKind{ ETaskExecutionKind::Shell };
	terminal::TerminalLaunchOptions terminalLaunchOptions;
	std::wstring shellCommand;
	std::vector<std::wstring> shellArguments;
};

enum class ETaskSessionExitKind : std::uint8_t {
	Exited,
	Cancelled,
	Failed,
};

struct TaskSessionExit final {
	ETaskSessionExitKind kind{ ETaskSessionExitKind::Exited };
	std::uint32_t exitCode{};
	std::wstring diagnostic;
};

struct TaskExecutionSessionCallbacks final {
	//! May arrive synchronously from Start or asynchronously from a host worker,
	//! but only after the host process and its worker callbacks are quiescent.
	std::function<void(TaskSessionExit)> exited;
};

struct TaskExecutionSessionStartResult final {
	bool succeeded{};
	std::uint32_t errorCode{};
	std::wstring diagnostic;

	[[nodiscard]] static TaskExecutionSessionStartResult Success() noexcept { return { true, 0, {} }; }
	[[nodiscard]] static TaskExecutionSessionStartResult Failure(std::uint32_t errorCode, std::wstring diagnostic)
	{
		return { false, errorCode, std::move(diagnostic) };
	}
};

/*! 
	@brief Result of a complete terminal-session close.

	Every value means the host process and workers are quiescent. `TimedOut` means
	the host exhausted graceful shutdown, forced cleanup, and joined before it
	returned; it never permits the caller to release a still-running session.
*/
enum class ETaskExecutionSessionCloseKind : std::uint8_t {
	Closed,
	TimedOut,
	HostFailure,
};

struct TaskExecutionSessionCloseResult final {
	ETaskExecutionSessionCloseKind kind{ ETaskExecutionSessionCloseKind::Closed };
	std::uint32_t errorCode{};
	std::wstring diagnostic;

	[[nodiscard]] static TaskExecutionSessionCloseResult Closed() noexcept { return {}; }
	[[nodiscard]] static TaskExecutionSessionCloseResult TimedOut(std::wstring diagnostic)
	{
		return { ETaskExecutionSessionCloseKind::TimedOut, 0, std::move(diagnostic) };
	}
	[[nodiscard]] static TaskExecutionSessionCloseResult HostFailure(std::uint32_t errorCode, std::wstring diagnostic)
	{
		return { ETaskExecutionSessionCloseKind::HostFailure, errorCode, std::move(diagnostic) };
	}
};

//! The execution service owns the returned session until it reaches a terminal state.
class ITaskExecutionSession {
public:
	virtual ~ITaskExecutionSession() = default;
	virtual TaskExecutionSessionStartResult Start(const TaskTerminalLaunchRequest& request) = 0;
	virtual void RequestCancel() noexcept = 0;
	//! Initiates closing without waiting, so service shutdown can fan out first. Idempotent, thread-safe, and non-throwing.
	virtual void BeginClose() noexcept = 0;
	//! Waits until the session is joined or the shared absolute deadline expires. Thread-safe with BeginClose.
	//! Every result, including TimedOut, guarantees that the host is quiescent.
	virtual TaskExecutionSessionCloseResult WaitForClose(std::chrono::steady_clock::time_point deadline) noexcept = 0;
};

/*! 
	@brief Host boundary for shell policy and terminal creation.

	The production factory may select a shell, create a `CTerminalSession`, and
	bridge its exit callback. Tests provide a fake factory without starting a
	process. Factory calls are never made under the service lock.
*/
class ITaskExecutionSessionFactory {
public:
	virtual ~ITaskExecutionSessionFactory() = default;
	virtual std::unique_ptr<ITaskExecutionSession> Create(const TaskExecutionSessionCallbacks& callbacks) = 0;
};

enum class ETaskExecutionRunState : std::uint8_t {
	Starting,
	Running,
	Cancelling,
	Closing,
	Exited,
	Cancelled,
	Failed,
	Closed,
};

[[nodiscard]] constexpr bool IsTerminalTaskRunState(const ETaskExecutionRunState state) noexcept
{
	return state == ETaskExecutionRunState::Exited || state == ETaskExecutionRunState::Cancelled
		|| state == ETaskExecutionRunState::Failed || state == ETaskExecutionRunState::Closed;
}

enum class ETaskExecutionOperationStatus : std::uint8_t {
	Started,
	Succeeded,
	Replayed,
	Rejected,
	Conflict,
	StaleRevision,
	Stopped,
	Deferred,
};

enum class ETaskExecutionOperationReason : std::uint8_t {
	None,
	InvalidOperationId,
	InvalidTask,
	UnsupportedTask,
	MaximumActiveRuns,
	FactoryUnavailable,
	SessionStartFailed,
	SessionClosePending,
	SessionCloseTimedOut,
	SessionCloseHostFailure,
	RunNotFound,
	RunAlreadyTerminal,
	RunClosing,
	OperationIdConflict,
	ExpectedRevisionMismatch,
	StopInProgress,
};

struct TaskExecutionOperationResult final {
	ETaskExecutionOperationStatus status{ ETaskExecutionOperationStatus::Rejected };
	ETaskExecutionOperationReason reason{ ETaskExecutionOperationReason::None };
	std::uint64_t revision{};
	std::optional<std::uint64_t> runId;
	std::uint32_t errorCode{};
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ETaskExecutionOperationStatus::Started || status == ETaskExecutionOperationStatus::Succeeded
			|| (status == ETaskExecutionOperationStatus::Replayed && reason == ETaskExecutionOperationReason::None);
	}
};

struct TaskExecutionStartRequest final {
	TaskExecutionOperation operation;
	TaskConfigurationDefinition definition;
	terminal::TerminalSize initialSize{};
};

struct TaskExecutionRunMutationRequest final {
	TaskExecutionOperation operation;
	std::uint64_t runId{};
};

struct TaskExecutionRunSnapshot final {
	std::uint64_t runId{};
	std::wstring label;
	ETaskExecutionKind executionKind{ ETaskExecutionKind::Shell };
	ETaskExecutionRunState state{ ETaskExecutionRunState::Starting };
	std::uint32_t exitCode{};
	std::wstring diagnostic;
};

struct TaskExecutionServiceSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	//! Advisory notifications are bounded; snapshots remain authoritative when this is nonzero.
	std::uint64_t droppedNotificationCount{};
	std::vector<TaskExecutionRunSnapshot> runs;
};

enum class ETaskExecutionChangeKind : std::uint8_t {
	RunStarted,
	RunRunning,
	RunCancelling,
	RunClosing,
	RunExited,
	RunCancelled,
	RunFailed,
	RunClosed,
};

struct TaskExecutionServiceChange final {
	std::uint64_t revision{};
	std::uint64_t runId{};
	ETaskExecutionChangeKind kind{ ETaskExecutionChangeKind::RunStarted };
	ETaskExecutionRunState state{ ETaskExecutionRunState::Starting };
};

using TaskExecutionSubscriptionId = std::uint64_t;
using TaskExecutionListener = std::function<void(const TaskExecutionServiceChange&)>;

struct TaskExecutionServiceLimits final {
	std::size_t maximumActiveRuns{ 16 };
	std::size_t maximumRetainedRuns{ 256 };
	std::size_t maximumSubscriptions{ 128 };
	std::size_t maximumRememberedOperations{ 512 };
	std::size_t maximumPendingNotifications{ 512 };
	//! One bounded budget shared by every session during a Stop fan-out.
	std::chrono::milliseconds sessionCloseTimeout{ std::chrono::seconds(2) };
};

/*! 
	@brief Thread-safe, bounded owner of task execution sessions.

	`Start` reserves a run before calling a host factory, so an operation replay
	cannot create a second process even when factory startup is synchronous or
	reentrant. Notifications and session calls happen outside the model lock.
*/
class TaskExecutionService final {
public:
	explicit TaskExecutionService(std::shared_ptr<ITaskExecutionSessionFactory> factory, TaskExecutionServiceLimits limits = {});
	~TaskExecutionService();

	TaskExecutionService(const TaskExecutionService&) = delete;
	TaskExecutionService& operator=(const TaskExecutionService&) = delete;

	[[nodiscard]] TaskExecutionOperationResult Start(const TaskExecutionStartRequest& request);
	[[nodiscard]] TaskExecutionOperationResult Cancel(const TaskExecutionRunMutationRequest& request);
	[[nodiscard]] TaskExecutionOperationResult Close(const TaskExecutionRunMutationRequest& request);
	//! A terminal state: rejects later work and closes every still-owned session.
	[[nodiscard]] TaskExecutionOperationResult Stop();

	[[nodiscard]] TaskExecutionServiceSnapshot Snapshot() const;
	[[nodiscard]] std::optional<TaskExecutionSubscriptionId> Subscribe(TaskExecutionListener listener);
	void Unsubscribe(TaskExecutionSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidOperationId(std::string_view value) noexcept;

private:
	struct Impl;
	void ProcessDeferredStop();
	std::shared_ptr<Impl> m_impl;
};

} // namespace workbench::tasks
