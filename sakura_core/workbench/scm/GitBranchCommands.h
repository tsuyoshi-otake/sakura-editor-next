/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitRefModel.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! Presents a Quick Pick and returns the chosen row, or nothing when the user
//! cancelled. A `Separator` row is never a legal answer.
using GitQuickPickPresenter = std::function<std::optional<std::size_t>(
	const std::vector<GitCheckoutItem>& items, std::wstring_view placeholder)>;

//! Presents a single-line input box and returns the typed text, or nothing when
//! the user cancelled. An accepted empty string is upstream's cancel path too.
using GitInputBoxPresenter = std::function<std::optional<std::wstring>(
	std::wstring_view prompt, std::wstring_view placeholder, std::wstring_view value)>;

//! Runs one git command in the repository the context names.
using GitCommandInvoker = std::function<GitExecutionResult(const std::vector<std::wstring>& arguments)>;

//! Shows one human-readable message. Used for the validation notices upstream
//! renders inside its input box and for a failed git invocation's reason.
using GitMessagePresenter = std::function<void(std::wstring_view message)>;

/*!
	@brief Everything `git.checkout` and `git.branch` need, injected.

	Deliberately HWND-free: the orchestration below is the behavior worth
	testing, and it must not require a window to exercise.
*/
struct GitBranchCommandContext {
	GitCommandInvoker run;
	GitQuickPickPresenter quickPick;
	GitInputBoxPresenter inputBox;
	GitMessagePresenter message;
};

enum class EGitBranchCommandStatus {
	Succeeded,
	//! The user dismissed a Quick Pick or input box. Not a failure.
	Cancelled,
	//! A git invocation did not succeed, or a presenter was not supplied.
	Failed,
};

struct GitBranchCommandResult {
	EGitBranchCommandStatus status = EGitBranchCommandStatus::Failed;
	//! Empty unless `status` is `Failed`.
	std::wstring message;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitBranchCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitBranchCommandResult&) const = default;
};

//! Upstream's `Git.findTrackingBranches` format, and its `refs/heads` scope.
inline constexpr std::wstring_view kGitTrackingRefsFormat = L"%(refname:short)%00%(upstream:short)";

[[nodiscard]] std::vector<std::wstring> BuildTrackingBranchArguments();

//! Reproduces `findTrackingBranches`: local heads whose upstream is exactly
//! `remoteRefName`, in `for-each-ref` order. Checking out `origin/main` while a
//! local `main` already tracks it must switch to `main`, not start a second
//! branch, which is why this lookup exists at all.
[[nodiscard]] std::vector<std::wstring> ParseTrackingBranches(
	std::string_view bytes, std::wstring_view remoteRefName);

//! `git.checkout` / `git.checkoutDetached`.
[[nodiscard]] GitBranchCommandResult RunGitCheckout(const GitBranchCommandContext& context, bool detached);

//! `git.branch` / `git.branchFrom`.
[[nodiscard]] GitBranchCommandResult RunGitCreateBranch(const GitBranchCommandContext& context, bool from);

} // namespace workbench::scm
