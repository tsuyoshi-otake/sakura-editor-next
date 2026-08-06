/*! @file
 * @brief One rendering of "why did that git command not work".
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitRefModel.h"

#include <string>

namespace workbench::scm {

//!
//! @brief Turns a non-`Succeeded` git invocation into a reason a person can act on.
//!
//! `RunGit` already separates its terminal states, so the message names the
//! actual cause instead of collapsing everything into "git failed". For a
//! non-zero exit git's own stderr is the most accurate reason available —
//! "pathspec did not match", "local changes would be overwritten" — so it is
//! preferred over any sentence written here.
//!
//! Shared by every command family rather than copied into each: a user who has
//! to decide whether to retry must not get two different sentences for the same
//! failure depending on which button produced it.
//!
[[nodiscard]] inline std::wstring DescribeGitFailure(const GitExecutionResult& result)
{
	switch (result.status) {
	case EGitExecutionStatus::GitUnavailable:
		return L"Git was not found on PATH.";
	case EGitExecutionStatus::LaunchFailed:
		return L"Git could not be started.";
	case EGitExecutionStatus::TimedOut:
		return L"The git command timed out.";
	case EGitExecutionStatus::Cancelled:
		return L"The git command was cancelled.";
	case EGitExecutionStatus::OutputLimitExceeded:
		return L"The git command produced too much output.";
	case EGitExecutionStatus::InvalidRequest:
		return L"The git command was not a valid request.";
	case EGitExecutionStatus::Succeeded:
	case EGitExecutionStatus::Failed:
	default:
		break;
	}
	auto text = DecodeGitOutput(result.standardError);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
		text.pop_back();
	}
	if (!text.empty()) {
		return text;
	}
	return L"The git command failed.";
}

} // namespace workbench::scm
