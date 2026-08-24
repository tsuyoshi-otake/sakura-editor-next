/*! @file
 * @brief Bounded, cancellable execution of one git command.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! Why a git invocation ended. Every branch is a separate terminal state, so a
//! caller never has to infer "git is missing" from an empty output buffer.
enum class EGitExecutionStatus : std::uint8_t {
	//! The process exited with code 0.
	Succeeded,
	//! The process ran to completion and exited with a non-zero code.
	Failed,
	//! The request itself was rejected before any process was created.
	InvalidRequest,
	//! `git.exe` could not be resolved from the search path.
	GitUnavailable,
	//! `CreateProcessW` failed.
	LaunchFailed,
	//! The deadline elapsed; the process was terminated.
	TimedOut,
	//! The caller's stop event was signalled; the process was terminated.
	Cancelled,
	//! Standard output exceeded the caller's budget; the process was terminated.
	OutputLimitExceeded,
};

//! Policy for the repository boundary a request is allowed to use. Ordinary
//! commands preserve Git's normal configuration behavior; passive reads opt in
//! to the runner's repository-native safety overrides.
enum class EGitRequestPolicy : std::uint8_t {
	//! Preserve Git's ordinary configuration and hook behavior.
	Ordinary,
	//! Read repository state without allowing a repository-local fsmonitor hook
	//! or command to execute.
	PassiveRepositoryRead,
};

struct GitExecutionRequest final {
	//! Working directory. Also passed as `-C <dir>` so git resolves the right repository.
	std::wstring workingDirectory;
	//! Arguments after the program name. Quoting is applied by the runner.
	std::vector<std::wstring> arguments;
	//! The runner enforces this policy while composing Git's effective arguments.
	//! Callers must not reproduce the safety override in individual argument lists.
	EGitRequestPolicy policy{ EGitRequestPolicy::Ordinary };
	//! Bytes written to the child's standard input, which is then closed.
	std::string standardInput;
	std::uint32_t timeoutMilliseconds{ 15000 };
	std::size_t maximumOutputBytes{ 4u * 1024u * 1024u };
};

struct GitExecutionResult final {
	EGitExecutionStatus status{ EGitExecutionStatus::InvalidRequest };
	//! Meaningful only when the process actually exited.
	int exitCode{ -1 };
	std::vector<std::uint8_t> standardOutput;
	//! UTF-8, bounded. git writes its human-readable failure reason here.
	std::string standardError;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitExecutionStatus::Succeeded; }
};

//! Upper bounds that keep an untrusted or runaway request from becoming unbounded work.
inline constexpr std::size_t kMaximumGitArguments = 64;
inline constexpr std::size_t kMaximumGitArgumentLength = 32768;
inline constexpr std::size_t kMaximumGitCommandLineLength = 32000;
inline constexpr std::size_t kMaximumGitStandardInputBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaximumGitStandardErrorBytes = 64u * 1024u;

//!
//! @brief Quote one argument for the `CommandLineToArgvW` convention.
//!
//! An argument with no separator and no quote is emitted verbatim; an empty
//! argument becomes `""`. Otherwise the value is wrapped in quotes, backslashes
//! that precede a quote (or the closing quote) are doubled, and an embedded
//! quote is escaped. Branch names, paths, and commit messages all reach git
//! through this, so it must never let a value split into two arguments.
//!
[[nodiscard]] std::wstring QuoteGitArgument(std::wstring_view value);

//! Build the full command line, including the quoted executable as argv[0].
[[nodiscard]] std::wstring BuildGitCommandLine(std::wstring_view executable, const std::vector<std::wstring>& arguments);

//!
//! @brief Prepend `-C <workingDirectory>` to the caller's arguments.
//!
//! The working directory alone is not enough: a caller could be handed a
//! directory that is inside a different repository's worktree, and `-C` is what
//! makes git resolve the repository this request names. Kept public and pure so
//! the composed argument vector can be asserted without creating a process.
//!
[[nodiscard]] std::vector<std::wstring> BuildEffectiveGitArguments(const GitExecutionRequest& request);

//! Resolve `git.exe` from the search path. Empty when git is not installed.
[[nodiscard]] std::wstring ResolveGitExecutable();

//! True when the request is structurally executable. Pure; no process is created.
[[nodiscard]] bool IsExecutableGitRequest(const GitExecutionRequest& request) noexcept;

//!
//! @brief Run one git command to completion.
//!
//! Standard output and standard error are captured through separate pipes so a
//! failure reason is never interleaved into parsed output. `stop` may be null;
//! when signalled, the child is terminated and `Cancelled` is returned. The call
//! blocks, so it belongs on a worker thread, never on the UI thread.
//!
[[nodiscard]] GitExecutionResult RunGit(const GitExecutionRequest& request, HANDLE stop);

} // namespace workbench::scm
