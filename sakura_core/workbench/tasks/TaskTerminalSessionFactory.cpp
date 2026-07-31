/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/TaskTerminalSessionFactory.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace workbench::tasks {
namespace {

constexpr std::size_t kMaximumCommandCharacters = 4'096;
constexpr std::size_t kMaximumArgumentCharacters = 4'096;
constexpr std::size_t kMaximumWorkingDirectoryCharacters = 4'096;
constexpr std::size_t kMaximumArguments = 64;
constexpr std::size_t kMaximumSerializedCommandCharacters = 64U * 1024U;
constexpr std::uint16_t kMaximumTerminalColumns = 1'000;
constexpr std::uint16_t kMaximumTerminalRows = 1'000;
constexpr std::uint32_t kErrorInvalidParameter = 87;
constexpr std::uint32_t kErrorFileNotFound = 2;
constexpr std::uint32_t kErrorOperationAborted = 995;
constexpr std::size_t kMaximumOutputDrainsPerNotification = 96;

bool HasNul(const std::wstring_view value) noexcept
{
	return value.find(L'\0') != std::wstring_view::npos;
}

bool IsBounded(const std::wstring_view value, const std::size_t maximum) noexcept
{
	return value.size() <= maximum && !HasNul(value);
}

bool IsValidSize(const terminal::TerminalSize size) noexcept
{
	return size.columns != 0 && size.rows != 0 && size.columns <= kMaximumTerminalColumns && size.rows <= kMaximumTerminalRows;
}

bool IsValidShellRequest(const std::wstring_view command, const std::span<const std::wstring> arguments,
	const std::wstring_view workingDirectory, const terminal::TerminalSize size) noexcept
{
	if (command.empty() || !IsBounded(command, kMaximumCommandCharacters) || arguments.size() > kMaximumArguments
		|| !IsBounded(workingDirectory, kMaximumWorkingDirectoryCharacters) || !IsValidSize(size)) return false;
	std::size_t total = command.size();
	for (const auto& argument : arguments) {
		if (!IsBounded(argument, kMaximumArgumentCharacters) || argument.size() > kMaximumSerializedCommandCharacters - total) return false;
		total += argument.size();
	}
	return true;
}

bool IsValidProcessRequest(const TaskTerminalLaunchRequest& request) noexcept
{
	const auto& options = request.terminalLaunchOptions;
	if (options.executablePath.empty() || !IsBounded(options.executablePath, kMaximumCommandCharacters)
		|| options.arguments.size() > kMaximumArguments || !IsBounded(options.workingDirectory, kMaximumWorkingDirectoryCharacters)
		|| !IsValidSize(options.initialSize)) return false;
	std::size_t total = options.executablePath.size() + options.workingDirectory.size();
	for (const auto& argument : options.arguments) {
		if (!IsBounded(argument, kMaximumArgumentCharacters) || argument.size() > kMaximumSerializedCommandCharacters - total) return false;
		total += argument.size();
	}
	return true;
}

class CTaskTerminalExecutionSession final : public ITaskExecutionSession {
public:
	CTaskTerminalExecutionSession(std::shared_ptr<ITaskShellLaunchPolicy> shellPolicy, TerminalSessionCreator createSession,
		TaskTerminalOutputSink outputSink, TaskExecutionSessionCallbacks callbacks)
		: m_shared(std::make_shared<Shared>(std::move(shellPolicy), std::move(createSession), std::move(outputSink), std::move(callbacks)))
	{
	}

	TaskExecutionSessionStartResult Start(const TaskTerminalLaunchRequest& request) override
	{
		auto shared = m_shared;
		if (!shared) return TaskExecutionSessionStartResult::Failure(kErrorOperationAborted, L"Task terminal session is unavailable.");
		{
			const std::lock_guard lock(shared->mutex);
			if (shared->started || shared->starting || shared->closed)
				return TaskExecutionSessionStartResult::Failure(kErrorInvalidParameter, L"Task terminal session was already started or closed.");
			shared->starting = true;
		}
		auto fail = [&shared](const std::uint32_t errorCode, std::wstring diagnostic) {
			const std::lock_guard lock(shared->mutex);
			shared->starting = false;
			return TaskExecutionSessionStartResult::Failure(errorCode, std::move(diagnostic));
		};

		terminal::TerminalLaunchOptions options;
		if (request.executionKind == ETaskExecutionKind::Process) {
			if (!request.shellCommand.empty() || !request.shellArguments.empty() || !IsValidProcessRequest(request))
				return fail(kErrorInvalidParameter, L"Process task contains shell fields or invalid launch data.");
			options = request.terminalLaunchOptions;
		} else if (request.executionKind == ETaskExecutionKind::Shell) {
			if (!request.terminalLaunchOptions.executablePath.empty() || !request.terminalLaunchOptions.arguments.empty()
				|| !IsValidShellRequest(request.shellCommand, request.shellArguments, request.terminalLaunchOptions.workingDirectory,
					request.terminalLaunchOptions.initialSize))
				return fail(kErrorInvalidParameter, L"Shell task contains process fields or invalid launch data.");
			if (!shared->shellPolicy) return fail(kErrorFileNotFound, L"No task shell launch policy is available.");
			auto resolved = shared->shellPolicy->Resolve(request.shellCommand, request.shellArguments,
				request.terminalLaunchOptions.workingDirectory, request.terminalLaunchOptions.initialSize);
			if (!resolved.succeeded) return fail(resolved.errorCode, std::move(resolved.diagnostic));
			options = std::move(resolved.options);
		} else {
			return fail(kErrorInvalidParameter, L"Unsupported task execution kind.");
		}

		if (!shared->createSession) return fail(kErrorFileNotFound, L"No terminal session creator is available.");
		terminal::TerminalSessionCallbacks terminalCallbacks;
		std::weak_ptr<Shared> weak = shared;
		terminalCallbacks.outputAvailable = [weak] {
			if (const auto current = weak.lock()) current->DrainAvailableOutput();
		};
		terminalCallbacks.completed = [weak](const terminal::TerminalSessionCompletionResult completion) {
			if (const auto current = weak.lock()) current->Complete(completion);
		};

		std::unique_ptr<terminal::CTerminalSession> created;
		try {
			created = shared->createSession(std::move(terminalCallbacks));
		} catch (...) {
			return fail(kErrorOperationAborted, L"Terminal session creation threw an exception.");
		}
		if (!created) return fail(kErrorFileNotFound, L"Terminal session creator returned no session.");
		const auto createdShared = std::shared_ptr<terminal::CTerminalSession>(std::move(created));
		bool closeAfterStart = false;
		{
			const std::lock_guard lock(shared->mutex);
			shared->terminal = createdShared;
			closeAfterStart = shared->closed;
		}
		const auto start = createdShared->Start(options);
		if (!start.succeeded) {
			const std::lock_guard lock(shared->mutex);
			shared->terminal.reset();
			shared->starting = false;
			return TaskExecutionSessionStartResult::Failure(start.errorCode, start.diagnostic);
		}
		if (closeAfterStart) createdShared->BeginClose();
		{
			const std::lock_guard lock(shared->mutex);
			shared->starting = false;
			shared->started = true;
		}
		return TaskExecutionSessionStartResult::Success();
	}

	void RequestCancel() noexcept override
	{
		const auto shared = m_shared;
		if (!shared) return;
		std::shared_ptr<terminal::CTerminalSession> terminal;
		{
			const std::lock_guard lock(shared->mutex);
			shared->cancelRequested = true;
			terminal = shared->terminal;
		}
		if (terminal) terminal->BeginClose();
	}

	void BeginClose() noexcept override
	{
		const auto shared = m_shared;
		if (!shared) return;
		std::shared_ptr<terminal::CTerminalSession> terminal;
		{
			const std::lock_guard lock(shared->mutex);
			shared->closed = true;
			terminal = shared->terminal;
		}
		if (terminal) terminal->BeginClose();
	}

	TaskExecutionSessionCloseResult WaitForClose(const std::chrono::steady_clock::time_point deadline) noexcept override
	{
		const auto shared = m_shared;
		if (!shared) return TaskExecutionSessionCloseResult::Closed();
		std::shared_ptr<terminal::CTerminalSession> terminal;
		{
			const std::lock_guard lock(shared->mutex);
			shared->closed = true;
			terminal = shared->terminal;
		}
		if (!terminal) return TaskExecutionSessionCloseResult::Closed();
		terminal->BeginClose();
		const auto close = terminal->WaitForClose(deadline);
		if (close.status == terminal::TerminalSessionCloseWaitStatus::InProgress)
			return TaskExecutionSessionCloseResult::HostFailure(kErrorOperationAborted, L"Task session close is running on its own callback worker.");
		if (close.status == terminal::TerminalSessionCloseWaitStatus::DeadlineExceeded)
			return TaskExecutionSessionCloseResult::TimedOut(L"Task terminal close exceeded its deadline after quiescing.");
		return TaskExecutionSessionCloseResult::Closed();
	}

private:
	struct Shared final {
		Shared(std::shared_ptr<ITaskShellLaunchPolicy> shellPolicyValue, TerminalSessionCreator createSessionValue,
			TaskTerminalOutputSink outputSinkValue, TaskExecutionSessionCallbacks callbacksValue)
			: shellPolicy(std::move(shellPolicyValue)), createSession(std::move(createSessionValue)), outputSink(std::move(outputSinkValue)), callbacks(std::move(callbacksValue)) {}

		void DrainAvailableOutput() noexcept
		{
			bool expected = false;
			if (!draining.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				drainRequested.store(true, std::memory_order_release);
				return;
			}
			// One pass can drain more than the terminal queue's high-water mark.
			// A second pass solely closes the hand-off race with a nested
			// outputAvailable callback that observed the gate as busy.
			for (std::size_t pass = 0; pass < 2; ++pass) {
				for (std::size_t count = 0; count < kMaximumOutputDrainsPerNotification; ++count) {
					std::shared_ptr<terminal::CTerminalSession> current;
					{
						const std::lock_guard lock(mutex);
						current = terminal;
					}
					if (!current) break;
					const auto bytes = current->DrainOutput();
					if (!bytes.empty() && outputSink) {
						try { outputSink(bytes); } catch (...) {}
					}
					const bool requested = drainRequested.exchange(false, std::memory_order_acq_rel);
					if (current->GetQueuedOutputBytes() == 0 && !requested) break;
				}
				draining.store(false, std::memory_order_release);
				if (!drainRequested.exchange(false, std::memory_order_acq_rel)) return;
				expected = false;
				if (!draining.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
					// The callback that acquired the gate owns the next drain.
					drainRequested.store(true, std::memory_order_release);
					return;
				}
			}
			draining.store(false, std::memory_order_release);
		}

		void Complete(const terminal::TerminalSessionCompletionResult completion) noexcept
		{
			TaskExecutionSessionCallbacks callbackCopy;
			TaskSessionExit exit;
			{
				const std::lock_guard lock(mutex);
				if (completionDelivered) return;
				// Close/Stop owns its terminal Task state after WaitForClose proves
				// quiescence. Do not race that explicit operation with a synthetic
				// Exited result merely because the terminal observed a root code
				// during requested shutdown. Cancellation still needs an async
				// result because Cancel does not wait for the session.
				if (completion.kind == terminal::TerminalSessionCompletionKind::Closed && !cancelRequested) {
					completionDelivered = true;
					return;
				}
				completionDelivered = true;
				if (cancelRequested) exit.kind = ETaskSessionExitKind::Cancelled;
				else if (completion.kind == terminal::TerminalSessionCompletionKind::Failed) exit.kind = ETaskSessionExitKind::Failed;
				else exit.kind = ETaskSessionExitKind::Exited;
				exit.exitCode = completion.exitCode;
				if (completion.kind == terminal::TerminalSessionCompletionKind::Failed) {
					exit.diagnostic = L"Terminal session failed after startup (error " + std::to_wstring(completion.errorCode) + L").";
				}
				callbackCopy = callbacks;
			}
			if (callbackCopy.exited) {
				try { callbackCopy.exited(std::move(exit)); } catch (...) {}
			}
		}

		std::mutex mutex;
		std::shared_ptr<ITaskShellLaunchPolicy> shellPolicy;
		TerminalSessionCreator createSession;
		TaskTerminalOutputSink outputSink;
		TaskExecutionSessionCallbacks callbacks;
		std::shared_ptr<terminal::CTerminalSession> terminal;
		std::atomic_bool draining{};
		std::atomic_bool drainRequested{};
		bool started{};
		bool starting{};
		bool closed{};
		bool cancelRequested{};
		bool completionDelivered{};
	};

	std::shared_ptr<Shared> m_shared;
};

TerminalSessionCreator DefaultSessionCreator()
{
	return [](terminal::TerminalSessionCallbacks callbacks) {
		return std::make_unique<terminal::CTerminalSession>(terminal::CreateConPtyTerminalBackend(), std::move(callbacks));
	};
}

class CDefaultTaskTerminalSessionFactory final : public ITaskExecutionSessionFactory {
public:
	explicit CDefaultTaskTerminalSessionFactory(TaskTerminalOutputSink outputSink)
		: m_profiles(m_locatorProvider)
		, m_shellPolicy(std::make_shared<PowerShellTaskShellLaunchPolicy>(m_profiles))
		, m_factory(m_shellPolicy, {}, std::move(outputSink))
	{
	}

	std::unique_ptr<ITaskExecutionSession> Create(const TaskExecutionSessionCallbacks& callbacks) override
	{
		return m_factory.Create(callbacks);
	}

private:
	// Declaration order is the lifetime contract: policy borrows profiles, and
	// profiles borrow the native locator provider.
	terminal::NativePowerShellLocatorProvider m_locatorProvider;
	terminal::PowerShellLocator m_profiles;
	std::shared_ptr<PowerShellTaskShellLaunchPolicy> m_shellPolicy;
	CTaskTerminalSessionFactory m_factory;
};

} // namespace

PowerShellTaskShellLaunchPolicy::PowerShellTaskShellLaunchPolicy(terminal::ITerminalProfileProvider& profiles)
	: m_profiles(profiles)
{
}

TaskShellLaunchResult PowerShellTaskShellLaunchPolicy::Resolve(const std::wstring_view command, const std::span<const std::wstring> arguments,
	const std::wstring_view workingDirectory, const terminal::TerminalSize initialSize)
{
	if (!IsValidShellRequest(command, arguments, workingDirectory, initialSize))
		return TaskShellLaunchResult::Failure(kErrorInvalidParameter, L"Shell task contains invalid command, argument, working-directory, or terminal-size data.");

	std::optional<terminal::TerminalProfile> profile;
	try {
		profile = m_profiles.DiscoverProfiles().defaultCandidate;
	} catch (...) {
		return TaskShellLaunchResult::Failure(kErrorFileNotFound, L"Terminal profile discovery failed.");
	}
	if (!profile || profile->path.empty() || !IsBounded(profile->path, kMaximumCommandCharacters))
		return TaskShellLaunchResult::Failure(kErrorFileNotFound, L"No valid default PowerShell terminal profile is available.");

	std::wstring script(command);
	for (const auto& argument : arguments) {
		if (script.size() >= kMaximumSerializedCommandCharacters - 1) return TaskShellLaunchResult::Failure(kErrorInvalidParameter, L"Serialized shell command is too large.");
		script.push_back(L' ');
		const auto literal = QuoteArgument(argument);
		if (literal.size() > kMaximumSerializedCommandCharacters - script.size())
			return TaskShellLaunchResult::Failure(kErrorInvalidParameter, L"Serialized shell command is too large.");
		script += literal;
	}

	terminal::TerminalLaunchOptions options;
	options.executablePath = std::move(profile->path);
	options.arguments = { L"-NoLogo", L"-Command", std::move(script) };
	options.workingDirectory.assign(workingDirectory);
	options.initialSize = initialSize;
	return TaskShellLaunchResult::Success(std::move(options));
}

std::wstring PowerShellTaskShellLaunchPolicy::QuoteArgument(const std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size() + 2);
	result.push_back(L'\'');
	for (const auto character : value) {
		result.push_back(character);
		if (character == L'\'') result.push_back(L'\'');
	}
	result.push_back(L'\'');
	return result;
}

CTaskTerminalSessionFactory::CTaskTerminalSessionFactory(std::shared_ptr<ITaskShellLaunchPolicy> shellPolicy,
	TerminalSessionCreator createSession, TaskTerminalOutputSink outputSink)
	: m_shellPolicy(std::move(shellPolicy)), m_createSession(createSession ? std::move(createSession) : DefaultSessionCreator()), m_outputSink(std::move(outputSink))
{
}

std::unique_ptr<ITaskExecutionSession> CTaskTerminalSessionFactory::Create(const TaskExecutionSessionCallbacks& callbacks)
{
	return std::make_unique<CTaskTerminalExecutionSession>(m_shellPolicy, m_createSession, m_outputSink, callbacks);
}

std::shared_ptr<ITaskExecutionSessionFactory> CreateDefaultTaskTerminalSessionFactory(TaskTerminalOutputSink outputSink)
{
	return std::make_shared<CDefaultTaskTerminalSessionFactory>(std::move(outputSink));
}

} // namespace workbench::tasks
