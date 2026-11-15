/*! @file
 * @brief The periodic Source Control refresh query: status plus one history page.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitHistoryModel.h"
#include "workbench/scm/GitOutputChannel.h"
#include "workbench/scm/GitScmModel.h"

#include <cstddef>
#include <string>
#include <vector>

namespace workbench::scm {

//! What one refresh pass learned. `cancelled` means the caller's stop event
//! fired mid-query; the worker treats it as the thread's terminal state and
//! never publishes the partial pass.
struct GitStatusRefreshOutcome {
	//! Valid only when `execution` is `Succeeded`; a failed read leaves this
	//! default-constructed so the last published repository snapshot can remain.
	GitScmState state;
	//! Why the refresh ended. An empty change list means "clean" only when this
	//! says the command actually succeeded; otherwise it means "unknown".
	EGitExecutionStatus execution{ EGitExecutionStatus::InvalidRequest };
	//! git's own failure text, kept so a later surface can show a reason instead
	//! of an empty list. UTF-8, already bounded by the runner.
	std::string failureReason;
	//! The Graph's own page of history. `historyRead` separates "this repository
	//! has no commits" from "the history could not be read", exactly as `execution`
	//! separates a clean worktree from an unknown one.
	std::vector<GitHistoryItem> history;
	bool historyRead{};
	bool cancelled{};
};

//! The refresh query. `--branch` is what carries head/upstream/ahead/behind, and
//! `-z` is what makes a rename's two paths unambiguous even when a path contains
//! a quote or a newline.
[[nodiscard]] GitExecutionRequest MakeGitStatusRefreshRequest(const std::wstring& root);

//! The Graph's own query. It is a second invocation rather than part of the
//! status refresh because `git status` cannot report commits at all.
[[nodiscard]] GitExecutionRequest MakeGitHistoryRefreshRequest(const std::wstring& root);

//!
//! @brief Run one status refresh, plus the bounded history page for a
//! repository, mirroring every invocation into the Git Output channel.
//!
//! Each spawned process goes through `RunGitLogged`, so the periodic poll
//! appears in the "Git" channel exactly like a user command, while a null
//! `sink.service` reduces this to the same bare `run` calls the worker made
//! before logging existed. `run` is the runner seam: production passes
//! `RunGit`, tests pass an observer. The returned `cancelled` marks a stop
//! event observed mid-pass; nothing about the pass is publishable then.
//!
[[nodiscard]] GitStatusRefreshOutcome RunGitStatusRefresh(
	const std::wstring& root,
	HANDLE stop,
	const GitOutputSink& sink,
	const GitRunner& run);

} // namespace workbench::scm
