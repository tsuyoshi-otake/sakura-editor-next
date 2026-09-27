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

inline constexpr wchar_t kGitUnavailableFallback[] =
	L"Git was not found. Install Git for Windows, add git.exe to PATH, then restart Sakura Editor NEXT.";

//!
//! @brief Turns a non-`Succeeded` git invocation into a reason a person can act on.
//!
//! `RunGit` already separates its terminal states, so the message names the
//! actual cause instead of collapsing everything into "git failed". For a
//! non-zero exit git's own stderr is the most accurate reason available (for
//! example, "pathspec did not match" or "local changes would be overwritten"), so it is
//! preferred over any sentence written here.
//!
//! Shared by every command family rather than copied into each: a user who has
//! to decide whether to retry must not get two different sentences for the same
//! failure depending on which button produced it.
//!
[[nodiscard]] inline std::wstring DescribeGitFailure(
	const GitExecutionResult& result, const GitRefTextResolver& text = {})
{
	switch (result.status) {
	case EGitExecutionStatus::GitUnavailable:
		if (text) {
			auto localized = text("GitUnavailable", {});
			if (!localized.empty()) return localized;
		}
		return kGitUnavailableFallback;
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
	auto message = DecodeGitOutput(result.standardError);
	while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r')) {
		message.pop_back();
	}
	if (!message.empty()) {
		return message;
	}
	return L"The git command failed.";
}

} // namespace workbench::scm
