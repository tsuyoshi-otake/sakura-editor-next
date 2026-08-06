/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitCommitCommands.h"

#include "workbench/scm/GitFailureText.h"
#include "workbench/scm/GitRefModel.h"

#include <algorithm>
#include <utility>

namespace workbench::scm {

namespace {

[[nodiscard]] GitCommitCommandResult Succeeded(std::optional<std::wstring> inputBoxValue = std::nullopt)
{
	return { EGitCommitCommandStatus::Succeeded, {}, std::move(inputBoxValue) };
}

[[nodiscard]] GitCommitCommandResult NotApplicable()
{
	return { EGitCommitCommandStatus::NotApplicable, {}, std::nullopt };
}

[[nodiscard]] GitCommitCommandResult Cancelled()
{
	return { EGitCommitCommandStatus::Cancelled, {}, std::nullopt };
}

[[nodiscard]] GitCommitCommandResult Failed(std::wstring message)
{
	return { EGitCommitCommandStatus::Failed, std::move(message), std::nullopt };
}

void Notify(const GitCommitCommandContext& context, std::wstring_view message)
{
	if (context.message) {
		context.message(message);
	}
}

//! `path.basename`, matching the stage commands: a confirmation names the file.
[[nodiscard]] std::wstring BaseName(std::wstring_view path)
{
	const auto separator = path.find_last_of(L"/\\");
	return std::wstring(separator == std::wstring_view::npos ? path : path.substr(separator + 1));
}

//! `pathEquals`, which is case-insensitive on Windows and separator-agnostic.
[[nodiscard]] bool PathEquals(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t i = 0; i < left.size(); ++i) {
		auto a = left[i];
		auto b = right[i];
		if (a == L'/') a = L'\\';
		if (b == L'/') b = L'\\';
		if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
		if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
		if (a != b) return false;
	}
	return true;
}

[[nodiscard]] bool IsStagedPath(const GitCommitRepositoryState& state, std::wstring_view path)
{
	return std::any_of(state.stagedPaths.begin(), state.stagedPaths.end(),
		[path](const std::wstring& staged) { return PathEquals(staged, path); });
}

[[nodiscard]] bool PathExists(const std::wstring& path)
{
	return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

//!
//! @brief The metadata directory, asked of git rather than assembled by hand.
//!
//! Upstream joins `<root>/.git/<name>` literally, which names the wrong
//! directory in a linked worktree, where `.git` is a file pointing elsewhere and
//! the in-progress state is per-worktree. One `--absolute-git-dir` answers that
//! correctly and costs one invocation instead of one per probed name.
//!
[[nodiscard]] std::wstring AbsoluteGitDirectory(const GitCommitCommandContext& context)
{
	const auto result = context.run({ L"rev-parse", L"--absolute-git-dir" }, {});
	if (!result.Succeeded() || result.exitCode != 0) return {};
	auto directory = DecodeGitOutput(std::string_view(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size()));
	while (!directory.empty() && (directory.back() == L'\n' || directory.back() == L'\r')) {
		directory.pop_back();
	}
	if (!directory.empty() && directory.back() != L'/' && directory.back() != L'\\') {
		directory += L'/';
	}
	return directory;
}

//! The two in-progress states upstream reads off the filesystem.
struct GitInProgressState final {
	bool merge{};
	bool rebase{};
};

[[nodiscard]] GitInProgressState ReadInProgressState(const GitCommitCommandContext& context)
{
	GitInProgressState state;
	const auto directory = AbsoluteGitDirectory(context);
	if (directory.empty()) return state;
	// `isMergeInProgress`.
	state.merge = PathExists(directory + L"MERGE_HEAD");
	// `getRebaseCommit` requires one of the rebase state directories before it
	// trusts `REBASE_HEAD`, because that file can outlive the rebase that wrote
	// it and a stale one would refuse a perfectly ordinary commit.
	state.rebase = (PathExists(directory + L"rebase-apply") || PathExists(directory + L"rebase-merge"))
		&& PathExists(directory + L"REBASE_HEAD");
	return state;
}

//! Run one chunked pathspec invocation set, stopping at the first failure.
[[nodiscard]] GitCommitCommandResult RunPaths(const GitCommitCommandContext& context,
	const std::vector<std::wstring>& prefix, const std::vector<std::wstring>& paths)
{
	const auto limits = GitPathChunkLimits::ForRepository(context.repositoryRoot);
	for (const auto& arguments : BuildGitPathChunks(prefix, paths, limits)) {
		const auto result = context.run(arguments, {});
		if (!result.Succeeded() || result.exitCode != 0) {
			auto message = DescribeGitFailure(result);
			Notify(context, message);
			return Failed(std::move(message));
		}
	}
	return Succeeded();
}

//!
//! @brief `handleCommitError`, reproduced including its two configuration probes.
//!
//! Upstream matches git's own stderr first and only then asks git whether an
//! identity is configured, so a failure that merely mentions a name cannot be
//! reported as a missing identity. The probes run afterwards precisely because
//! they cost two extra invocations that most failures do not need.
//!
[[nodiscard]] std::wstring DescribeCommitFailure(
	const GitCommitCommandContext& context, const GitExecutionResult& result)
{
	const auto& stderrText = result.standardError;
	const auto contains = [&stderrText](std::string_view needle) {
		return stderrText.find(needle) != std::string::npos;
	};
	if (contains("not possible because you have unmerged files")) {
		// Upstream maps this to `UnmergedChanges`, whose user-facing handler is the
		// generic error display; the resolution path it would point at does not
		// exist here, so git's own sentence is what the user gets.
		return DescribeGitFailure(result);
	}
	if (contains("Aborting commit due to empty commit message")) {
		return L"Commit operation was cancelled due to empty commit message.";
	}
	const auto configured = [&context](std::wstring_view key) {
		const auto probe = context.run({ L"config", L"--get-all", std::wstring(key) }, {});
		return probe.Succeeded() && probe.exitCode == 0 && !probe.standardOutput.empty();
	};
	if (!configured(L"user.name") || !configured(L"user.email")) {
		// Upstream's `NoUserNameConfigured` / `NoUserEmailConfigured` sentence,
		// minus its `Learn More` button: there is no browser route from here, and
		// a button that does nothing is worse than no button.
		return L"Make sure you configure your \"user.name\" and \"user.email\" in git.";
	}
	return DescribeGitFailure(result);
}

//! `git rev-list --parents -n 1 HEAD` yields `<commit> <parent>...`.
[[nodiscard]] std::optional<std::size_t> HeadParentCount(const GitCommitCommandContext& context)
{
	const auto result = context.run({ L"rev-list", L"--parents", L"-n", L"1", L"HEAD" }, {});
	if (!result.Succeeded() || result.exitCode != 0) return std::nullopt;
	std::size_t tokens = 0;
	bool inToken = false;
	for (const auto byte : result.standardOutput) {
		const bool space = byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
		if (space) {
			inToken = false;
		} else if (!inToken) {
			inToken = true;
			++tokens;
		}
	}
	if (tokens == 0) return std::nullopt;
	return tokens - 1;
}

[[nodiscard]] std::string ToUtf8(std::wstring_view text)
{
	if (text.empty()) {
		return {};
	}
	const int required = ::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0) {
		return {};
	}
	std::string result(static_cast<std::size_t>(required), '\0');
	::WideCharToMultiByte(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
	return result;
}

//! `git log -1 --format=%B`, which is the undone commit's full message. Decoded
//! through the shared `DecodeGitOutput`, so a message read here and a ref name
//! read elsewhere cannot disagree by a character.
[[nodiscard]] std::optional<std::wstring> HeadCommitMessage(const GitCommitCommandContext& context)
{
	const auto result = context.run({ L"log", L"-1", L"--format=%B" }, {});
	if (!result.Succeeded() || result.exitCode != 0) return std::nullopt;
	auto text = DecodeGitOutput(std::string_view(
		reinterpret_cast<const char*>(result.standardOutput.data()), result.standardOutput.size()));
	// `%B` emits a trailing newline that is not part of the message.
	while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
		text.pop_back();
	}
	return text;
}

} // namespace

GitCommitInvocation BuildGitCommitInvocation(std::wstring_view message, const GitCommitOptions& options)
{
	GitCommitInvocation invocation;
	auto& args = invocation.arguments;
	args = { L"commit", L"--quiet" };

	if (!message.empty()) {
		invocation.standardInput = ToUtf8(message);
		invocation.writesStandardInput = true;
		args.push_back(L"--allow-empty-message");
		args.push_back(L"--file");
		args.push_back(L"-");
	}
	if (options.verbose) args.push_back(L"--verbose");
	// `--all` is deliberately absent even when `options.all` is set: upstream's
	// `Repository.commit` runs `git add` for it and deletes the field before
	// `Git.commit` ever sees it, so that branch is unreachable on this path.
	if (options.amend) args.push_back(L"--amend");
	if (!options.useEditor) {
		if (message.empty()) {
			if (options.amend) {
				args.push_back(L"--no-edit");
			} else {
				invocation.standardInput.clear();
				invocation.writesStandardInput = true;
				args.push_back(L"--file");
				args.push_back(L"-");
			}
		}
		// Upstream appends this a second time when a message was given. Emitting
		// it once would be tidier and would also be a different command line.
		args.push_back(L"--allow-empty-message");
	}
	if (options.signoff) args.push_back(L"--signoff");
	if (options.signCommit.has_value()) args.push_back(*options.signCommit ? L"-S" : L"--no-gpg-sign");
	if (options.empty) args.push_back(L"--allow-empty");
	if (options.noVerify) args.push_back(L"--no-verify");
	if (options.requireUserConfig) {
		args.insert(args.begin(), { L"-c", L"user.useConfigOnly=true" });
	}
	return invocation;
}

GitCommitRepositoryState BuildGitCommitRepositoryState(std::wstring_view repositoryRoot,
	const std::vector<GitStageResource>& resources, std::wstring_view headShortName, bool headHasCommit)
{
	GitCommitRepositoryState state;
	state.headHasCommit = headHasCommit;
	state.headShortName = headShortName;
	bool anyTracked = false;
	for (const auto& resource : resources) {
		switch (resource.group) {
		case EGitResourceGroup::Index:
			++state.stagedCount;
			state.stagedPaths.push_back(JoinRepositoryPath(repositoryRoot, resource.path));
			break;
		case EGitResourceGroup::WorkingTree:
			++state.workingTreeCount;
			if (!resource.untracked) anyTracked = true;
			break;
		case EGitResourceGroup::Merge:
		case EGitResourceGroup::Untracked:
			break;
		}
	}
	state.workingTreeAllUntracked = state.workingTreeCount > 0 && !anyTracked;
	return state;
}

GitCommitPrompt BuildUnsavedDocumentsPrompt(const std::vector<std::wstring>& documents)
{
	GitCommitPrompt prompt;
	prompt.message = documents.size() == 1
		? L"The following file has unsaved changes which won't be included in the commit if you proceed: "
			+ BaseName(documents.front()) + L".\n\nWould you like to save it before committing?"
		: L"There are " + std::to_wstring(documents.size())
			+ L" unsaved files.\n\nWould you like to save them before committing?";
	prompt.choices = { L"Save All & Commit Changes", L"Commit Changes" };
	return prompt;
}

GitCommitPrompt BuildNoStagedChangesPrompt()
{
	GitCommitPrompt prompt;
	prompt.message = L"There are no staged changes to commit.\n\n"
		L"Would you like to stage all your changes and commit them directly?";
	prompt.choices = { L"Yes" };
	return prompt;
}

GitCommitPrompt BuildNoChangesPrompt()
{
	GitCommitPrompt prompt;
	prompt.message = L"There are no changes to commit.";
	prompt.choices = { L"Create Empty Commit" };
	prompt.warning = false;
	prompt.modal = false;
	return prompt;
}

GitCommitPrompt BuildNoVerifyCommitPrompt()
{
	GitCommitPrompt prompt;
	prompt.message = L"You are about to commit your changes without verification, "
		L"this skips pre-commit hooks and can be undesirable.\n\nAre you sure to continue?";
	prompt.choices = { L"OK" };
	return prompt;
}

GitCommitPrompt BuildUndoMergeCommitPrompt()
{
	GitCommitPrompt prompt;
	prompt.message = L"The last commit was a merge commit. Are you sure you want to undo it?";
	prompt.choices = { L"Undo merge commit" };
	return prompt;
}

GitCommitCommandResult RunGitCommit(const GitCommitCommandContext& context,
	const GitCommitRepositoryState& state, std::wstring_view inputBoxValue, GitCommitOptions options)
{
	if (!context.run) {
		return Failed(L"The commit command has no git invoker.");
	}
	const auto& config = context.configuration;
	options.requireUserConfig = config.requireUserConfig;

	// Upstream's `Repository.commit` never reaches `git commit` during a rebase;
	// it continues the rebase instead. Read once, before any prompt, so the user
	// is not walked through a confirmation for something that cannot happen.
	const auto inProgress = ReadInProgressState(context);
	if (inProgress.rebase) {
		auto message = std::wstring(
			L"A rebase is in progress. Continuing a rebase is not supported here; "
			L"use git on the command line to finish or abort it.");
		Notify(context, message);
		return { EGitCommitCommandStatus::UnsupportedRebaseInProgress, std::move(message), std::nullopt };
	}

	bool enableSmartCommit = config.enableSmartCommit;
	const bool noStagedChanges = state.stagedCount == 0;
	const bool noUnstagedChanges = state.workingTreeCount == 0;

	if (!options.empty) {
		if (config.promptToSaveFilesBeforeCommit) {
			auto documents = context.dirtyDocuments ? context.dirtyDocuments() : std::vector<std::wstring>{};
			if (config.promptToSaveStagedFilesOnly || state.stagedCount > 0) {
				std::erase_if(documents, [&state](const std::wstring& document) {
					return !IsStagedPath(state, document);
				});
			}
			if (!documents.empty()) {
				if (!context.confirm) {
					return Failed(L"The commit command has no confirmation presenter.");
				}
				const auto pick = context.confirm(BuildUnsavedDocumentsPrompt(documents));
				if (!pick) return Cancelled();
				if (*pick == 0) {
					if (!context.saveDocuments || !context.saveDocuments()) {
						auto message = std::wstring(L"The unsaved files could not be saved before committing.");
						Notify(context, message);
						return Failed(std::move(message));
					}
					// Upstream re-adds the saved documents that belong to the index
					// group, because saving them made the worktree differ from what
					// was staged and the newly written bytes must be what is
					// committed. It then recomputes both counts; that recomputation
					// cannot change any gate below, because reaching this point
					// required a non-empty index group, and every remaining gate is
					// guarded by `noStagedChanges`. Re-running `git status` here to
					// reproduce a recomputation with no observable effect would cost
					// an invocation and prove nothing.
					std::erase_if(documents, [&state](const std::wstring& document) {
						return !IsStagedPath(state, document);
					});
					if (!documents.empty()) {
						auto staged = RunPaths(context, BuildStagePrefix(false), documents);
						if (!staged.Succeeded()) return staged;
					}
				} else if (*pick != 1) {
					return Cancelled();
				}
			}
		}

		if (!noUnstagedChanges && noStagedChanges && !enableSmartCommit
			&& options.all == EGitCommitAll::None && !options.amend) {
			if (!config.suggestSmartCommit) return NotApplicable();
			if (!context.confirm) {
				return Failed(L"The commit command has no confirmation presenter.");
			}
			const auto pick = context.confirm(BuildNoStagedChangesPrompt());
			if (!pick) return Cancelled();
			enableSmartCommit = true;
		}

		if (enableSmartCommit && options.all == EGitCommitAll::None) {
			options.all = noStagedChanges ? EGitCommitAll::All : EGitCommitAll::None;
		}
	}

	options.signCommit = config.enableCommitSigning ? std::optional<bool>(true) : std::nullopt;
	if (config.alwaysSignOff) options.signoff = true;
	if (config.useEditorAsCommitInput) {
		options.useEditor = true;
		if (config.verboseCommit) options.verbose = true;
	}

	if (((noStagedChanges && noUnstagedChanges)
			|| (options.all == EGitCommitAll::None && noStagedChanges)
			|| (noStagedChanges && config.smartCommitChangesTrackedOnly && state.workingTreeAllUntracked))
		&& !options.amend && !options.empty && !inProgress.merge) {
		if (!context.confirm) {
			return Failed(L"The commit command has no confirmation presenter.");
		}
		const auto pick = context.confirm(BuildNoChangesPrompt());
		if (!pick) return NotApplicable();
		options.empty = true;
	}

	if (options.noVerify) {
		if (!config.allowNoVerifyCommit) {
			auto message = std::wstring(L"Commits without verification are not allowed, "
				L"please enable them with the \"git.allowNoVerifyCommit\" setting.");
			Notify(context, message);
			return Failed(std::move(message));
		}
		if (config.confirmNoVerifyCommit) {
			if (!context.confirm) {
				return Failed(L"The commit command has no confirmation presenter.");
			}
			const auto pick = context.confirm(BuildNoVerifyCommitPrompt());
			if (!pick) return Cancelled();
		}
	}

	// `getCommitMessage`. The box wins; an empty box goes to the prompt, except
	// for an amend over an existing commit, where upstream deliberately yields no
	// message so `--amend --no-edit` keeps the previous one.
	std::wstring message(inputBoxValue);
	if (message.empty() && !config.useEditorAsCommitInput && !(options.amend && state.headHasCommit)) {
		if (!context.promptForMessage) {
			return Failed(L"The commit command has no commit-message prompt.");
		}
		const auto placeholder = state.headShortName.empty()
			? std::wstring(L"Commit message")
			: L"Message (commit on \"" + state.headShortName + L"\")";
		const auto typed = context.promptForMessage(placeholder, L"Please provide a commit message");
		if (!typed) return Cancelled();
		message = *typed;
	}
	if (message.empty() && !options.amend && !options.useEditor) {
		return NotApplicable();
	}

	if (options.all != EGitCommitAll::None
		&& (config.smartCommitChangesTrackedOnly || config.untrackedChangesSeparated)) {
		options.all = EGitCommitAll::Tracked;
	}

	// `Repository.commit`'s staging step. `add([], …)` is `add -A -- .` upstream,
	// so the whole worktree is staged rather than a listing of known rows; a file
	// changed since the last refresh is therefore included, exactly as upstream.
	if (options.all != EGitCommitAll::None) {
		auto staged = RunPaths(context, BuildStagePrefix(options.all == EGitCommitAll::Tracked), { L"." });
		if (!staged.Succeeded()) return staged;
	}

	const auto invocation = BuildGitCommitInvocation(message, options);
	const auto result = context.run(invocation.arguments, invocation.standardInput);
	if (!result.Succeeded() || result.exitCode != 0) {
		auto failure = DescribeCommitFailure(context, result);
		Notify(context, failure);
		return Failed(std::move(failure));
	}
	// `commitOperationCleanup` resets the box to `getInputTemplate()`. Reading
	// `commit.template` is not implemented, so the reset is to empty; the
	// divergence is recorded in this subsystem's `CLAUDE.md`.
	return Succeeded(std::wstring{});
}

GitCommitCommandResult RunGitUndoCommit(
	const GitCommitCommandContext& context, const GitCommitRepositoryState& state)
{
	if (!context.run) {
		return Failed(L"The undo command has no git invoker.");
	}
	if (!state.headHasCommit) {
		auto message = std::wstring(L"Can't undo because HEAD doesn't point to any commit.");
		Notify(context, message);
		return NotApplicable();
	}

	const auto parents = HeadParentCount(context);
	if (!parents) {
		auto message = std::wstring(L"The commit to undo could not be read.");
		Notify(context, message);
		return Failed(std::move(message));
	}
	// Read the message before the reset, because afterwards HEAD names a
	// different commit and the text the box must be restored to is gone.
	const auto restored = HeadCommitMessage(context);

	if (*parents > 1) {
		if (!context.confirm) {
			return Failed(L"The undo command has no confirmation presenter.");
		}
		const auto pick = context.confirm(BuildUndoMergeCommitPrompt());
		if (!pick) return Cancelled();
	}

	if (*parents > 0) {
		// `repository.reset('HEAD~')` is `reset --soft`, which keeps the undone
		// commit's content staged. A mixed or hard reset would unstage or destroy
		// it, which is a materially different operation from undoing the commit.
		const auto result = context.run({ L"reset", L"--soft", L"HEAD~" }, {});
		if (!result.Succeeded() || result.exitCode != 0) {
			auto failure = DescribeGitFailure(result);
			Notify(context, failure);
			return Failed(std::move(failure));
		}
	} else {
		// The first commit has no parent to reset onto, so upstream deletes the ref
		// and then unstages everything.
		const auto deleted = context.run({ L"update-ref", L"-d", L"HEAD" }, {});
		if (!deleted.Succeeded() || deleted.exitCode != 0) {
			auto failure = DescribeGitFailure(deleted);
			Notify(context, failure);
			return Failed(std::move(failure));
		}
		auto unstaged = RunPaths(context, BuildUnstagePrefix(false), { L"." });
		if (!unstaged.Succeeded()) return unstaged;
	}

	return Succeeded(restored.value_or(std::wstring{}));
}

} // namespace workbench::scm
