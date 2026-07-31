/*! @file
 * @brief Production task-to-terminal session composition and PowerShell shell policy.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "terminal/PowerShellLocator.h"
#include "terminal/session/TerminalSession.h"
#include "workbench/tasks/TaskExecutionService.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace workbench::tasks {

//! Result of resolving a shell task without leaking command-line serialization to the catalog.
struct TaskShellLaunchResult final {
	bool succeeded{};
	terminal::TerminalLaunchOptions options;
	std::uint32_t errorCode{};
	std::wstring diagnostic;

	[[nodiscard]] static TaskShellLaunchResult Success(terminal::TerminalLaunchOptions value)
	{
		return { true, std::move(value), 0, {} };
	}
	[[nodiscard]] static TaskShellLaunchResult Failure(std::uint32_t errorCode, std::wstring diagnostic)
	{
		return { false, {}, errorCode, std::move(diagnostic) };
	}
};

/*! 
	@brief OS/shell-specific task launch boundary.

	The input command and argument vector are semantic task fields, not a command
	line. A policy is the only component permitted to serialize them for a shell.
*/
class ITaskShellLaunchPolicy {
public:
	virtual ~ITaskShellLaunchPolicy() = default;
	virtual TaskShellLaunchResult Resolve(std::wstring_view command, std::span<const std::wstring> arguments,
		std::wstring_view workingDirectory, terminal::TerminalSize initialSize) = 0;
};

/*! 
	@brief PowerShell task policy backed by the product's terminal-profile provider.

	The configured command fragment is retained verbatim. Each separate task
	argument is encoded as a PowerShell single-quoted literal; embedded apostrophes
	are doubled. This deliberately does not add `-NoProfile`: normal profile
	behavior remains owned by the selected terminal profile/product policy.
*/
class PowerShellTaskShellLaunchPolicy final : public ITaskShellLaunchPolicy {
public:
	explicit PowerShellTaskShellLaunchPolicy(terminal::ITerminalProfileProvider& profiles);
	TaskShellLaunchResult Resolve(std::wstring_view command, std::span<const std::wstring> arguments,
		std::wstring_view workingDirectory, terminal::TerminalSize initialSize) override;

	[[nodiscard]] static std::wstring QuoteArgument(std::wstring_view value);

private:
	terminal::ITerminalProfileProvider& m_profiles;
};

//! Optional observer for raw terminal bytes. Invoked outside adapter locks.
using TaskTerminalOutputSink = std::function<void(std::span<const std::uint8_t>)>;

//! Injected to keep the factory testable and to retain ConPTY behind the terminal boundary.
using TerminalSessionCreator = std::function<std::unique_ptr<terminal::CTerminalSession>(terminal::TerminalSessionCallbacks)>;

/*! 
	@brief Concrete `ITaskExecutionSessionFactory` for native terminal sessions.

	It owns neither profiles nor task service. Every returned adapter owns exactly
	one terminal session and publishes a task exit only from TerminalSession's
	post-quiescence completion callback.
*/
class CTaskTerminalSessionFactory final : public ITaskExecutionSessionFactory {
public:
	explicit CTaskTerminalSessionFactory(std::shared_ptr<ITaskShellLaunchPolicy> shellPolicy,
		TerminalSessionCreator createSession = {}, TaskTerminalOutputSink outputSink = {});

	std::unique_ptr<ITaskExecutionSession> Create(const TaskExecutionSessionCallbacks& callbacks) override;

private:
	std::shared_ptr<ITaskShellLaunchPolicy> m_shellPolicy;
	TerminalSessionCreator m_createSession;
	TaskTerminalOutputSink m_outputSink;
};

//! Creates the native Windows composition while keeping the locator, profile
//! provider, shell policy, and ConPTY session factory under one shared lifetime.
[[nodiscard]] std::shared_ptr<ITaskExecutionSessionFactory> CreateDefaultTaskTerminalSessionFactory(
	TaskTerminalOutputSink outputSink = {});

} // namespace workbench::tasks
