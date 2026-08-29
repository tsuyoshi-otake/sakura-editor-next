/*! @file @brief Task execution adapter for the process-owned terminal runtime. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/tasks/RuntimeTaskTerminalSessionFactory.h"

#include "terminal/DefaultTerminalLaunchProfileService.h"
#include "terminal/runtime/TerminalRuntimeService.h"
#include "workbench/tasks/TaskTerminalSessionFactory.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace workbench::tasks {
namespace {

constexpr std::size_t kMaximumCommandCharacters = 4096;
constexpr std::size_t kMaximumArgumentCharacters = 4096;
constexpr std::size_t kMaximumArguments = 64;
constexpr std::size_t kMaximumSerializedCommandCharacters = 64u * 1024u;
constexpr std::uint16_t kMaximumTerminalDimension = 1000;
std::atomic<std::uint64_t> g_nextTaskRuntimeOperation{ 1 };

terminal::HarnessOperationId AllocateOperationId() noexcept
{
	terminal::HarnessOperationId result;
	const auto value = g_nextTaskRuntimeOperation.fetch_add(1, std::memory_order_relaxed);
	if (value == 0 || value == (std::numeric_limits<std::uint64_t>::max)()) return {};
	for (std::size_t index = 0; index < sizeof(value); ++index) {
		result.value[index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
	return result;
}

bool Bounded(const std::wstring_view value, const std::size_t limit) noexcept
{
	return value.size() <= limit && value.find(L'\0') == std::wstring_view::npos;
}

bool ValidSize(const terminal::TerminalSize size) noexcept
{
	return size.columns != 0 && size.rows != 0
		&& size.columns <= kMaximumTerminalDimension && size.rows <= kMaximumTerminalDimension;
}

std::optional<terminal::TerminalLaunchOptions> ResolveLaunch(
	const TaskTerminalLaunchRequest& request,
	terminal::CDefaultTerminalLaunchProfileService& profiles,
	std::uint32_t& errorCode)
{
	errorCode = ERROR_INVALID_PARAMETER;
	if (!ValidSize(request.terminalLaunchOptions.initialSize)
		|| !Bounded(request.terminalLaunchOptions.workingDirectory, kMaximumCommandCharacters)) return std::nullopt;
	if (request.executionKind == ETaskExecutionKind::Process) {
		if (!request.shellCommand.empty() || !request.shellArguments.empty()
			|| request.terminalLaunchOptions.executablePath.empty()
			|| !Bounded(request.terminalLaunchOptions.executablePath, kMaximumCommandCharacters)
			|| request.terminalLaunchOptions.arguments.size() > kMaximumArguments) return std::nullopt;
		for (const auto& argument : request.terminalLaunchOptions.arguments) {
			if (!Bounded(argument, kMaximumArgumentCharacters)) return std::nullopt;
		}
		errorCode = ERROR_SUCCESS;
		return request.terminalLaunchOptions;
	}
	if (request.executionKind != ETaskExecutionKind::Shell
		|| !request.terminalLaunchOptions.executablePath.empty()
		|| !request.terminalLaunchOptions.arguments.empty()
		|| request.shellCommand.empty()
		|| !Bounded(request.shellCommand, kMaximumCommandCharacters)
		|| request.shellArguments.size() > kMaximumArguments) return std::nullopt;
	auto options = profiles.Resolve(request.terminalLaunchOptions.initialSize,
		request.terminalLaunchOptions.workingDirectory);
	if (!options || options->executablePath.empty()) {
		errorCode = ERROR_FILE_NOT_FOUND;
		return std::nullopt;
	}
	std::wstring script(request.shellCommand);
	for (const auto& argument : request.shellArguments) {
		if (!Bounded(argument, kMaximumArgumentCharacters)) return std::nullopt;
		const auto quoted = PowerShellTaskShellLaunchPolicy::QuoteArgument(argument);
		if (script.size() + 1 + quoted.size() > kMaximumSerializedCommandCharacters) return std::nullopt;
		script.push_back(L' ');
		script += quoted;
	}
	options->arguments = { L"-NoLogo", L"-Command", std::move(script) };
	errorCode = ERROR_SUCCESS;
	return options;
}

class CRuntimeTaskExecutionSession final : public ITaskExecutionSession {
public:
	CRuntimeTaskExecutionSession(std::shared_ptr<terminal::CTerminalRuntimeService> runtime,
		std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles,
		TaskExecutionSessionCallbacks callbacks)
		: m_shared(std::make_shared<Shared>(std::move(runtime), std::move(profiles), std::move(callbacks)))
	{
	}

	TaskExecutionSessionStartResult Start(const TaskTerminalLaunchRequest& request) override
	{
		const auto shared = m_shared;
		if (!shared || !shared->runtime || !shared->profiles) {
			return TaskExecutionSessionStartResult::Failure(ERROR_INVALID_STATE,
				L"The process terminal runtime is unavailable.");
		}
		bool closeAfterStart = false;
		{
			const std::lock_guard lock(shared->mutex);
			if (shared->started || shared->starting || shared->closed) {
				return TaskExecutionSessionStartResult::Failure(ERROR_INVALID_STATE,
					L"The Task terminal was already started or closed.");
			}
			shared->starting = true;
		}
		std::uint32_t errorCode{};
		auto launch = ResolveLaunch(request, *shared->profiles, errorCode);
		if (!launch) {
			const std::lock_guard lock(shared->mutex);
			shared->starting = false;
			return TaskExecutionSessionStartResult::Failure(errorCode,
				L"The Task terminal launch request is invalid or no shell profile is available.");
		}
		const auto operationId = AllocateOperationId();
		if (!operationId.IsValid()) {
			const std::lock_guard lock(shared->mutex);
			shared->starting = false;
			return TaskExecutionSessionStartResult::Failure(ERROR_NOT_ENOUGH_MEMORY,
				L"The Task operation identity space is exhausted.");
		}
		std::weak_ptr<Shared> weak = shared;
		shared->subscription = shared->runtime->Subscribe([weak](const terminal::TerminalInstanceEvent& event) {
			if (const auto current = weak.lock()) current->OnEvent(event);
		});
		terminal::TerminalCreateRequest create;
		create.operationId = operationId;
		create.origin = terminal::TerminalInstanceOrigin::Task;
		create.environmentPolicy = terminal::TerminalChildEnvironmentPolicy::TaskWithoutHarnessShim;
		create.launch = std::move(*launch);
		create.taskRunId = "task-" + std::to_string(operationId.value[0]);
		const auto created = shared->runtime->CreateInstance(create);
		{
			const std::lock_guard lock(shared->mutex);
			shared->starting = false;
			if (!created.Succeeded()) {
				shared->subscription.Reset();
				return TaskExecutionSessionStartResult::Failure(ERROR_GEN_FAILURE,
					L"The process terminal runtime rejected the Task launch.");
			}
			shared->instanceId = created.instanceId;
			shared->started = true;
			closeAfterStart = shared->closed;
		}
		if (closeAfterStart) shared->runtime->BeginCloseInstance(
			created.instanceId, terminal::TerminalInstanceCloseReason::Explicit);
		if (const auto* instance = shared->runtime->Instance(created.instanceId)) {
			const auto snapshot = instance->Snapshot();
			if (snapshot.outcome) shared->Complete(*snapshot.outcome);
		}
		return TaskExecutionSessionStartResult::Success();
	}

	void RequestCancel() noexcept override
	{
		const auto shared = m_shared;
		if (!shared) return;
		terminal::TerminalInstanceId id;
		{
			const std::lock_guard lock(shared->mutex);
			shared->cancelRequested = true;
			id = shared->instanceId;
		}
		if (shared->runtime && id.IsValid()) {
			shared->runtime->BeginCloseInstance(id, terminal::TerminalInstanceCloseReason::Cancel);
		}
	}

	void BeginClose() noexcept override
	{
		const auto shared = m_shared;
		if (!shared) return;
		terminal::TerminalInstanceId id;
		{
			const std::lock_guard lock(shared->mutex);
			shared->closed = true;
			id = shared->instanceId;
		}
		if (shared->runtime && id.IsValid()) {
			shared->runtime->BeginCloseInstance(id, terminal::TerminalInstanceCloseReason::Explicit);
		}
	}

	TaskExecutionSessionCloseResult WaitForClose(
		const std::chrono::steady_clock::time_point deadline) noexcept override
	{
		BeginClose();
		const auto shared = m_shared;
		if (!shared || !shared->runtime) return TaskExecutionSessionCloseResult::Closed();
		terminal::TerminalInstanceId id;
		{
			const std::lock_guard lock(shared->mutex);
			id = shared->instanceId;
		}
		if (!id.IsValid()) return TaskExecutionSessionCloseResult::Closed();
		const auto result = shared->runtime->WaitForInstanceClose(id, deadline);
		if (result.status == terminal::TerminalInstanceCloseWaitStatus::InProgress) {
			return TaskExecutionSessionCloseResult::HostFailure(ERROR_OPERATION_IN_PROGRESS,
				L"The Task terminal is closing on its completion callback.");
		}
		if (result.status == terminal::TerminalInstanceCloseWaitStatus::DeadlineExceeded) {
			return TaskExecutionSessionCloseResult::TimedOut(
				L"The Task terminal exceeded its bounded shutdown deadline after quiescing.");
		}
		shared->subscription.Reset();
		return result.status == terminal::TerminalInstanceCloseWaitStatus::Closed
			? TaskExecutionSessionCloseResult::Closed()
			: TaskExecutionSessionCloseResult::HostFailure(ERROR_INVALID_STATE,
				L"The Task terminal instance disappeared before close completed.");
	}

private:
	struct Shared final {
		Shared(std::shared_ptr<terminal::CTerminalRuntimeService> runtimeValue,
			std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profilesValue,
			TaskExecutionSessionCallbacks callbackValue)
			: runtime(std::move(runtimeValue)), profiles(std::move(profilesValue)),
			  callbacks(std::move(callbackValue)) {}

		void OnEvent(const terminal::TerminalInstanceEvent& event) noexcept
		{
			terminal::TerminalInstanceId current;
			{
				const std::lock_guard lock(mutex);
				current = instanceId;
			}
			if (!current.IsValid() || event.coordinate.instanceId != current) return;
			if (event.kind == terminal::TerminalInstanceEventKind::OutputAvailable && runtime) {
				static_cast<void>(runtime->DrainOutput(current));
			}
			if (event.kind == terminal::TerminalInstanceEventKind::Completed && event.outcome) {
				Complete(*event.outcome);
			}
		}

		void Complete(const terminal::TerminalInstanceOutcome& outcome) noexcept
		{
			TaskExecutionSessionCallbacks callbackCopy;
			TaskSessionExit exit;
			{
				const std::lock_guard lock(mutex);
				if (completionDelivered) return;
				if (closed && !cancelRequested) {
					completionDelivered = true;
					return;
				}
				completionDelivered = true;
				if (cancelRequested || outcome.kind == terminal::TerminalInstanceOutcomeKind::Cancelled) {
					exit.kind = ETaskSessionExitKind::Cancelled;
				} else if (outcome.kind == terminal::TerminalInstanceOutcomeKind::Exited) {
					exit.kind = ETaskSessionExitKind::Exited;
				} else {
					exit.kind = ETaskSessionExitKind::Failed;
					exit.diagnostic = L"The process terminal runtime reported a Task failure.";
				}
				exit.exitCode = outcome.processExitCode.value_or(0);
				callbackCopy = callbacks;
			}
			if (callbackCopy.exited) {
				try { callbackCopy.exited(std::move(exit)); } catch (...) {}
			}
		}

		std::mutex mutex;
		std::shared_ptr<terminal::CTerminalRuntimeService> runtime;
		std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles;
		TaskExecutionSessionCallbacks callbacks;
		terminal::TerminalSubscription subscription;
		terminal::TerminalInstanceId instanceId;
		bool started{};
		bool starting{};
		bool closed{};
		bool cancelRequested{};
		bool completionDelivered{};
	};

	std::shared_ptr<Shared> m_shared;
};

class CRuntimeTaskTerminalSessionFactory final : public ITaskExecutionSessionFactory {
public:
	CRuntimeTaskTerminalSessionFactory(std::shared_ptr<terminal::CTerminalRuntimeService> runtime,
		std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles)
		: m_runtime(std::move(runtime)), m_profiles(std::move(profiles)) {}

	std::unique_ptr<ITaskExecutionSession> Create(
		const TaskExecutionSessionCallbacks& callbacks) override
	{
		return std::make_unique<CRuntimeTaskExecutionSession>(m_runtime, m_profiles, callbacks);
	}

private:
	std::shared_ptr<terminal::CTerminalRuntimeService> m_runtime;
	std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> m_profiles;
};

} // namespace

std::shared_ptr<ITaskExecutionSessionFactory> CreateRuntimeTaskTerminalSessionFactory(
	std::shared_ptr<terminal::CTerminalRuntimeService> runtime,
	std::shared_ptr<terminal::CDefaultTerminalLaunchProfileService> profiles)
{
	if (!runtime || !profiles) return {};
	return std::make_shared<CRuntimeTaskTerminalSessionFactory>(
		std::move(runtime), std::move(profiles));
}

} // namespace workbench::tasks
