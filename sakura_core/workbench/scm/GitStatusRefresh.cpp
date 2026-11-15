/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/scm/GitStatusRefresh.h"

#include <utility>

namespace workbench::scm {
namespace {

constexpr std::size_t kMaximumStatusBytes = 4u * 1024u * 1024u;
//! How many commits the Graph reads. Upstream pages its history; this reads one
//! bounded page, because an unbounded `git log` on the refresh timer is not.
constexpr std::size_t kGraphHistoryCount = 50;

} // namespace

GitExecutionRequest MakeGitStatusRefreshRequest(const std::wstring& root)
{
	GitExecutionRequest request;
	request.workingDirectory = root;
	request.arguments = { L"status", L"--porcelain=v2", L"--branch", L"-z", L"--untracked-files=normal" };
	request.policy = EGitRequestPolicy::PassiveRepositoryRead;
	request.timeoutMilliseconds = 3000;
	request.maximumOutputBytes = kMaximumStatusBytes;
	return request;
}

GitExecutionRequest MakeGitHistoryRefreshRequest(const std::wstring& root)
{
	GitExecutionRequest request;
	request.workingDirectory = root;
	request.arguments = MakeGitHistoryArguments(kGraphHistoryCount);
	request.policy = EGitRequestPolicy::PassiveRepositoryRead;
	request.timeoutMilliseconds = 3000;
	request.maximumOutputBytes = kMaximumStatusBytes;
	return request;
}

GitStatusRefreshOutcome RunGitStatusRefresh(
	const std::wstring& root,
	HANDLE stop,
	const GitOutputSink& sink,
	const GitRunner& run)
{
	GitStatusRefreshOutcome outcome;
	const auto execution = RunGitLogged(MakeGitStatusRefreshRequest(root), stop, sink, run);
	if (execution.status == EGitExecutionStatus::Cancelled) {
		outcome.cancelled = true;
		return outcome;
	}
	outcome.execution = execution.status;
	outcome.failureReason = execution.standardError;
	if (execution.Succeeded()) {
		outcome.state = ParsePorcelainV2({ reinterpret_cast<const char*>(execution.standardOutput.data()),
			execution.standardOutput.size() });
	}
	// Only a repository has a history to read, and a repository with no commit
	// at all makes `git log` fail; that failure is reported as "unavailable"
	// rather than as an empty history.
	if (execution.Succeeded() && outcome.state.repository) {
		const auto history = RunGitLogged(MakeGitHistoryRefreshRequest(root), stop, sink, run);
		if (history.status == EGitExecutionStatus::Cancelled) {
			outcome.cancelled = true;
			return outcome;
		}
		if (history.Succeeded()) {
			outcome.historyRead = true;
			outcome.history = ParseGitHistory({ reinterpret_cast<const char*>(history.standardOutput.data()),
				history.standardOutput.size() });
		}
	}
	return outcome;
}

} // namespace workbench::scm
