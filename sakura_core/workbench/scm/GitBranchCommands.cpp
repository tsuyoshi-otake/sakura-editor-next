/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitBranchCommands.h"

#include "workbench/scm/GitFailureText.h"

#include <algorithm>
#include <utility>

namespace workbench::scm {

namespace {

//! Upstream's input box strings, from `promptForBranchName`.
constexpr std::wstring_view kBranchNamePrompt = L"Please provide a new branch name";
constexpr std::wstring_view kBranchNamePlaceholder = L"Branch name";
//! Upstream's `git.branchFrom` ref picker placeholder.
constexpr std::wstring_view kBranchFromPlaceholder = L"Select a ref to create the branch from";

[[nodiscard]] std::wstring ResolveText(const GitBranchCommandContext& context, std::string_view key,
	std::wstring_view fallback, std::wstring_view argument = {})
{
	std::wstring result;
	if (context.text) {
		result = context.text(key, argument);
	}
	if (result.empty()) result = fallback;
	std::size_t position = 0;
	while (!argument.empty() && (position = result.find(L"{0}", position)) != std::wstring::npos) {
		result.replace(position, 3, argument);
		position += argument.size();
	}
	return result;
}

[[nodiscard]] GitBranchCommandResult Cancelled()
{
	return { EGitBranchCommandStatus::Cancelled, {} };
}

[[nodiscard]] GitBranchCommandResult Succeeded()
{
	return { EGitBranchCommandStatus::Succeeded, {} };
}

[[nodiscard]] GitBranchCommandResult Failed(std::wstring message)
{
	return { EGitBranchCommandStatus::Failed, std::move(message) };
}

//! `DescribeGitFailure` under the name this file's call sites already use.
[[nodiscard]] std::wstring DescribeFailure(const GitExecutionResult& result)
{
	return DescribeGitFailure(result);
}

[[nodiscard]] bool HasPresenters(const GitBranchCommandContext& context) noexcept
{
	return static_cast<bool>(context.run) && static_cast<bool>(context.quickPick)
		&& static_cast<bool>(context.inputBox);
}

void Notify(const GitBranchCommandContext& context, std::wstring_view message)
{
	if (context.message) {
		context.message(message);
	}
}

//! Reads every ref once. Both commands need the full listing: `git.checkout` to
//! populate its Quick Pick, `git.branch` to reject a name that already exists.
[[nodiscard]] bool ListRefs(
	const GitBranchCommandContext& context, std::vector<GitRef>& refs, GitBranchCommandResult& failure)
{
	const auto result = context.run(BuildForEachRefArguments());
	if (!result.Succeeded() || result.exitCode != 0) {
		failure = Failed(DescribeFailure(result));
		return false;
	}
	const std::string_view bytes(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size());
	refs = ParseForEachRef(bytes);
	return true;
}

//! `git rev-parse HEAD`, for the description upstream's `HEADItem` shows. A
//! repository with no commit yet has no HEAD, and that is not an error here:
//! the row is still offered, just without its object name.
[[nodiscard]] std::wstring ReadHeadCommit(const GitBranchCommandContext& context)
{
	const auto result = context.run({ L"rev-parse", L"HEAD" });
	if (!result.Succeeded() || result.exitCode != 0) {
		return {};
	}
	const std::string_view bytes(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size());
	auto commit = DecodeGitOutput(bytes);
	while (!commit.empty() && (commit.back() == L'\n' || commit.back() == L'\r')) {
		commit.pop_back();
	}
	return commit;
}

//! Runs one git command whose only interesting outcome is success or a reason.
[[nodiscard]] GitBranchCommandResult RunAndReport(
	const GitBranchCommandContext& context, const std::vector<std::wstring>& arguments)
{
	const auto result = context.run(arguments);
	if (!result.Succeeded() || result.exitCode != 0) {
		auto message = DescribeFailure(result);
		Notify(context, message);
		return Failed(std::move(message));
	}
	return Succeeded();
}

//! Reproduces `CheckoutRemoteHeadItem.run`: a local branch that already tracks
//! this remote ref is checked out by name; otherwise git creates the tracking
//! branch. Checking the remote ref out directly would detach HEAD.
[[nodiscard]] GitBranchCommandResult CheckoutRemoteHead(
	const GitBranchCommandContext& context, std::wstring_view refName)
{
	const auto tracking = context.run(BuildTrackingBranchArguments());
	if (tracking.Succeeded() && tracking.exitCode == 0) {
		const std::string_view bytes(
			reinterpret_cast<const char*>(tracking.standardOutput.data()), tracking.standardOutput.size());
		const auto branches = ParseTrackingBranches(bytes, refName);
		if (!branches.empty()) {
			return RunAndReport(context, BuildCheckoutArguments(branches.front(), false));
		}
	}
	return RunAndReport(context, BuildCheckoutTrackingArguments(refName));
}

//! Reproduces `promptForBranchName`'s loop. Upstream validates while the user
//! types and keeps the box open on a collision; the native input box has no
//! live validation callback, so the equivalent is to report the collision and
//! ask again with the rejected text still in the field.
[[nodiscard]] bool PromptForBranchName(
	const GitBranchCommandContext& context, const std::vector<GitRef>& refs, std::wstring& branchName)
{
	std::wstring value;
	std::wstring prompt = ResolveText(context, "GitBranchNamePrompt", kBranchNamePrompt);
	while (true) {
		auto typed = context.inputBox(prompt,
			ResolveText(context, "GitBranchNamePlaceholder", kBranchNamePlaceholder), value);
		if (!typed.has_value()) {
			return false;
		}
		const auto validation = ValidateBranchName(*typed, refs, kGitBranchWhitespaceChar, context.text);
		switch (validation.state) {
		case EGitBranchNameValidation::Empty:
			// Upstream's OK button is simply disabled for an empty name, so an
			// empty accept is a cancel rather than an error.
			return false;
		case EGitBranchNameValidation::AlreadyExists:
			// The collision replaces the prompt rather than going to a separate
			// surface, which is the closest reachable equivalent of upstream
			// rendering it inside the still-open input box.
			prompt = validation.message;
			value = std::move(*typed);
			continue;
		case EGitBranchNameValidation::Sanitized:
			// Upstream shows this as an informational notice and still accepts
			// the sanitized name, so it is not a re-prompt.
			Notify(context, validation.message);
			branchName = validation.sanitizedName;
			return true;
		case EGitBranchNameValidation::Valid:
		default:
			branchName = validation.sanitizedName;
			return true;
		}
	}
}

//! `git.branch`'s tail, shared with the `Create new branch...` rows of
//! `git.checkout`'s own Quick Pick.
[[nodiscard]] GitBranchCommandResult CreateBranch(
	const GitBranchCommandContext& context, const std::vector<GitRef>& refs, bool from)
{
	std::wstring target = L"HEAD";
	if (from) {
		const auto items = BuildBranchFromItems(refs, ReadHeadCommit(context), context.text);
		const auto chosen = context.quickPick(items,
			ResolveText(context, "GitBranchFromPlaceholder", kBranchFromPlaceholder));
		if (!chosen.has_value() || *chosen >= items.size()) {
			return Cancelled();
		}
		const auto& item = items[*chosen];
		if (item.kind == EGitCheckoutItemKind::Separator) {
			return Cancelled();
		}
		if (!item.refName.empty()) {
			target = item.refName;
		}
	}
	std::wstring branchName;
	if (!PromptForBranchName(context, refs, branchName)) {
		return Cancelled();
	}
	return RunAndReport(context, BuildCreateBranchArguments(branchName, target));
}

} // namespace

std::vector<std::wstring> BuildTrackingBranchArguments()
{
	return { L"for-each-ref", L"--format", std::wstring(kGitTrackingRefsFormat), L"refs/heads" };
}

std::vector<std::wstring> ParseTrackingBranches(std::string_view bytes, std::wstring_view remoteRefName)
{
	std::vector<std::wstring> branches;
	if (remoteRefName.empty()) {
		return branches;
	}
	const std::wstring text = DecodeGitOutput(bytes);
	std::size_t begin = 0;
	while (begin <= text.size()) {
		std::size_t end = text.find(L'\n', begin);
		if (end == std::wstring::npos) {
			end = text.size();
		}
		std::wstring_view line(text.data() + begin, end - begin);
		begin = end + 1;
		while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' || line.back() == L'\t')) {
			line.remove_suffix(1);
		}
		while (!line.empty() && (line.front() == L' ' || line.front() == L'\t')) {
			line.remove_prefix(1);
		}
		const std::size_t nul = line.find(L'\0');
		if (nul == std::wstring_view::npos) {
			continue;
		}
		const auto name = line.substr(0, nul);
		const auto upstream = line.substr(nul + 1);
		if (name.empty() || upstream != remoteRefName) {
			continue;
		}
		branches.emplace_back(name);
	}
	return branches;
}

GitBranchCommandResult RunGitCheckout(const GitBranchCommandContext& context, bool detached)
{
	if (!HasPresenters(context)) {
		return Failed(ResolveText(context, "GitCheckoutNoPresenter", L"The checkout command has no presenter."));
	}
	std::vector<GitRef> refs;
	GitBranchCommandResult failure;
	if (!ListRefs(context, refs, failure)) {
		Notify(context, failure.message);
		return failure;
	}

	// The native Quick Pick has no filter box, so the filter is always empty and
	// upstream's command-rows-first ordering is the one that applies.
	const auto items = BuildCheckoutItems(refs, detached, true, context.text);
	const auto chosen = context.quickPick(items, CheckoutPlaceholder(detached, context.text));
	if (!chosen.has_value() || *chosen >= items.size()) {
		return Cancelled();
	}
	const auto& item = items[*chosen];
	switch (item.kind) {
	case EGitCheckoutItemKind::CreateBranch:
		return CreateBranch(context, refs, false);
	case EGitCheckoutItemKind::CreateBranchFrom:
		return CreateBranch(context, refs, true);
	case EGitCheckoutItemKind::CheckoutDetached:
		// Upstream's row runs `git.checkoutDetached`, which re-opens the picker
		// with the tag group and the command rows removed.
		return RunGitCheckout(context, true);
	case EGitCheckoutItemKind::Separator:
		return Cancelled();
	case EGitCheckoutItemKind::Ref:
	default:
		break;
	}
	if (item.refName.empty()) {
		return Cancelled();
	}
	if (item.refKind == EGitRefKind::RemoteHead && !detached) {
		return CheckoutRemoteHead(context, item.refName);
	}
	return RunAndReport(context, BuildCheckoutArguments(item.refName, detached));
}

GitBranchCommandResult RunGitCreateBranch(const GitBranchCommandContext& context, bool from)
{
	if (!HasPresenters(context)) {
		return Failed(ResolveText(context, "GitBranchNoPresenter", L"The branch command has no presenter."));
	}
	std::vector<GitRef> refs;
	GitBranchCommandResult failure;
	if (!ListRefs(context, refs, failure)) {
		Notify(context, failure.message);
		return failure;
	}
	return CreateBranch(context, refs, from);
}

} // namespace workbench::scm
