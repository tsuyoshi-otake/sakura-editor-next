/*! @file
 * @brief Mirrors GitCommandRunner invocations into the "Git" Output channel.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include "workbench/output/IOutputService.h"
#include "workbench/scm/GitCommandRunner.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief Stable identity for the native projection of upstream's own Git
//! Output channel.
//!
//! Verified against `microsoft/vscode`'s `extensions/git/src/main.ts`, not
//! inferred: the built-in Git extension creates its channel with
//! `window.createOutputChannel('Git', { log: true })`. `createOutputChannel`
//! takes only a display name -- there is no separate stable "channel id" VS
//! Code publishes for it -- so `kGitOutputChannelId` reuses the "git" identity
//! `GitScmPublisher::kGitProviderId` already publishes for the SCM provider,
//! which is the closest thing this codebase has to a stable upstream id for
//! "the built-in Git integration". `kGitOutputChannelLabel` is upstream's own
//! display string, verbatim.
//!
inline constexpr std::string_view kGitOutputChannelId = "git";
inline constexpr std::string_view kGitOutputChannelLabel = "Git";

//!
//! @brief Ensures the "Git" Log channel exists under `owner`.
//!
//! Idempotent by construction: a channel with `kGitOutputChannelId` already
//! owned by `owner` (same ownerId and generation) at kind `Log` is treated as
//! already-created and returns `Succeeded` without a second `CreateChannel`
//! call. This does not depend on the provider's bounded remembered-operation
//! replay cache, so it stays correct even after that cache has evicted the
//! original create operation.
//!
//! `operationId` is still required by the provider's `CreateChannel` contract for the
//! first, channel-creating call. Callers should pass a stable id that does not
//! change across repeated calls in the same owner generation (for example
//! `"workbench.scm.git.output/create"`), so that if two callers race to create
//! the channel before the snapshot check above observes it, the loser's
//! `CreateChannel` call is recognized as an exact replay (`Replayed`) instead
//! of colliding with a different id on an already-created channel (`Conflict`,
//! reason `InvalidChannelId`). A `Conflict`/`InvalidChannelId` result still
//! means the channel exists and is safe to append to; it is not a fatal error.
//!
[[nodiscard]] output::OutputOperationResult EnsureGitOutputChannel(
	output::IOutputService& service,
	const output::OutputOwner& owner,
	const std::string& operationId);

//!
//! @brief Build the Output log entries for one completed git invocation.
//!
//! Reproduces `extensions/git/src/git.ts`'s `_exec` logging, read from
//! `microsoft/vscode` rather than guessed:
//!
//!   this.log(`> git ${args.join(' ')} [${Date.now() - startExec}ms]\n`);
//!   if (bufferResult.stderr.length > 0) this.log(`${bufferResult.stderr}\n`);
//!
//! and `extensions/git/src/main.ts`'s listener, which splits each logged
//! string on `\r?\n`, drops trailing blank lines, and appends the rejoined
//! text as one `LogOutputChannel.appendLine` call (which itself resolves to an
//! Info-level entry -- `ExtHostLogOutputChannel.appendLine` delegates to
//! `append`, which delegates to `info()`).
//!
//! Two divergences from upstream's literal text, both deliberate:
//!
//!  - `arguments` here is the *effective* argument vector `RunGit` actually
//!    executes (`BuildEffectiveGitArguments`'s output), which includes the
//!    leading `-C <workingDirectory>` this runner always prepends. Upstream's
//!    own `args.join(' ')` never shows a working directory at all, because
//!    upstream passes it as a separate `cwd` spawn option instead of a command
//!    line token. Passing the same effective arguments this runner used to
//!    execute the process is the more faithful "this is the command that
//!    ran" record for a wrapper whose repository resolution is `-C`-based, and
//!    it is what makes the working directory visible without inventing a
//!    second, hand-written "Working directory: ..." line upstream does not
//!    have.
//!  - stdout is never logged. Upstream's own condition is
//!    `args.find(a => this.commandsToLog.includes(a))`, and
//!    `commandsToLog` defaults to `[]` (`git.commandsToLog`'s documented
//!    default), so a stock VS Code never logs stdout for any command either.
//!    There is no `git.commandsToLog` setting reader here, so this always
//!    takes upstream's own default branch rather than a third behavior.
//!
//! Exit code is intentionally **not** logged as a separate line: upstream's
//! `_exec` never writes one either (a non-zero exit throws a `GitError`
//! elsewhere; the Output channel only ever sees the command line and stderr).
//! Adding one would be a fabricated line no real VS Code Git Output channel
//! shows.
//!
[[nodiscard]] std::vector<output::OutputLogEntry> BuildGitOutputLogEntries(
	const std::vector<std::wstring>& arguments,
	std::chrono::milliseconds elapsed,
	const GitExecutionResult& result);

//!
//! @brief Append `entries` to the Git channel as one atomic log mutation.
//!
//! `EnsureGitOutputChannel` must have already created `kGitOutputChannelId`
//! under `owner` (or a prior generation must have, and this is a still-current
//! generation); this function does not create the channel, so a caller that
//! skips creation gets an explicit `ChannelNotFound` rather than a silently
//! dropped log line. `entries` must be non-empty; an empty vector is a caller
//! error, not "nothing to log" (`BuildGitOutputLogEntries` always returns at
//! least the command-line entry).
//!
[[nodiscard]] output::OutputOperationResult AppendGitOutputLogEntries(
	output::IOutputService& service,
	const output::OutputOwner& owner,
	const std::string& operationId,
	std::vector<output::OutputLogEntry> entries,
	std::optional<std::uint64_t> expectedRevision = std::nullopt);

//!
//! @brief What `RunGitLogged` needs to mirror one invocation into the Git
//! Output channel. A null `service` (or a default-constructed sink with no
//! `service`) makes `RunGitLogged` behave exactly like a bare `RunGit` call --
//! logging is best-effort and must never gate whether the git command itself
//! runs.
//!
struct GitOutputSink {
	output::IOutputService* service{};
	output::OutputOwner owner;
	//! A stable id reused across every `RunGitLogged` call for this owner
	//! generation; see `EnsureGitOutputChannel`'s contract above. Left empty
	//! only when `service` is null.
	std::string createOperationId;
	//! Invoked once per `RunGitLogged` call to obtain a *fresh* operation id
	//! for that call's `AppendLog` mutation -- unlike `createOperationId`,
	//! reusing the same id here would make every invocation after the first
	//! replay the first one's (now stale) log text instead of appending new
	//! text. Returning `std::nullopt` (an exhausted id budget) skips logging
	//! for that one call without failing the git command itself.
	std::function<std::optional<std::string>()> nextAppendOperationId;
};

//!
//! @brief Run one git command through `RunGit` and mirror it into the Git
//! Output channel described by `sink`, exactly as upstream's `_exec` mirrors
//! into its own "Git" channel. See `GitCommandRunner.h` for `RunGit`'s own
//! contract; this wrapper adds no new failure mode to it and always returns
//! `RunGit`'s own result, regardless of whether the Output mirroring
//! succeeded.
//!
//! Thread safety: identical to `RunGit` -- this function is stateless aside
//! from the `HANDLE`/`IOutputService`/callables it is handed, so it is safe to
//! call from the UI thread (as the branch/commit/sync/init commands already
//! call `RunGit` synchronously today) or from a background worker thread (as
//! the periodic status refresh in `CScmWorkbenchTool` already does). Every
//! call must supply its own fresh `HANDLE stop` and its own
//! `nextAppendOperationId` sequence; nothing here is safe to share
//! concurrently between two in-flight calls except the supplied output provider,
//! which must satisfy the thread-safe `IOutputService` contract.
//!
[[nodiscard]] GitExecutionResult RunGitLogged(
	const GitExecutionRequest& request,
	HANDLE stop,
	const GitOutputSink& sink);

} // namespace workbench::scm
