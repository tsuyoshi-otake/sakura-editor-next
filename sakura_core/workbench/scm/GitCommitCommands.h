/*! @file
 * @brief `git.commit`, `git.commitAmend`, and `git.undoCommit`.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitPrompt.h"
#include "workbench/scm/GitRefModel.h"
#include "workbench/scm/GitStageCommands.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief The `git.*` settings the commit path reads, with upstream's defaults.
//!
//! Upstream reads each of these from `workspace.getConfiguration('git')` at the
//! moment of the command. There is no configuration reader on this path yet, so
//! the defaults are stated here as data rather than as scattered literals: the
//! value that will later come from Settings has exactly one place to arrive.
//!
struct GitCommitConfiguration final {
	//! `git.enableSmartCommit`. False upstream: with nothing staged, committing
	//! asks first instead of silently staging everything.
	bool enableSmartCommit{ false };
	//! `git.suggestSmartCommit`. True upstream, and what makes that question appear.
	bool suggestSmartCommit{ true };
	//! `git.smartCommitChanges`. `all` upstream; `tracked` excludes untracked files.
	bool smartCommitChangesTrackedOnly{ false };
	//! `git.untrackedChanges` being other than `mixed`, which forces `add -u`.
	bool untrackedChangesSeparated{ false };
	//! `git.enableCommitSigning`. False upstream, which emits `--no-gpg-sign`.
	bool enableCommitSigning{ false };
	//! `git.alwaysSignOff`. False upstream.
	bool alwaysSignOff{ false };
	//! `git.verboseCommit`. False upstream, and only meaningful with an editor.
	bool verboseCommit{ false };
	//! `git.requireGitUserConfig`. True upstream, which splices
	//! `-c user.useConfigOnly=true` so an unconfigured identity fails loudly
	//! instead of recording a guessed `user@hostname` author.
	bool requireUserConfig{ true };
	//!
	//! @brief `git.useEditorAsCommitInput`.
	//!
	//! Upstream's default is **true**, and this is the one deliberate divergence:
	//! that path hands `GIT_EDITOR` to git and hosts the resulting `COMMIT_EDITMSG`
	//! buffer as an editor tab whose save/close completes the commit. That editor
	//! hosting is not implemented here, so enabling it would leave a `git commit`
	//! waiting on an editor that never opens. Setting it false selects upstream's
	//! own `!useEditorAsCommitInput` branch — a real upstream code path, not an
	//! invented one — where the message comes from the input box or a prompt.
	//!
	bool useEditorAsCommitInput{ false };
	//! `git.allowNoVerifyCommit`. False upstream, which refuses `--no-verify`
	//! outright rather than quietly skipping the repository's pre-commit hooks.
	bool allowNoVerifyCommit{ false };
	//! `git.confirmNoVerifyCommit`. True upstream, and only reachable once the
	//! setting above has been turned on.
	bool confirmNoVerifyCommit{ true };
	//! `git.promptToSaveFilesBeforeCommit`. `always` upstream; `staged` narrows the
	//! prompt to documents that are actually staged, `never` suppresses it.
	bool promptToSaveFilesBeforeCommit{ true };
	//! `git.promptToSaveFilesBeforeCommit === 'staged'`.
	bool promptToSaveStagedFilesOnly{ false };

	[[nodiscard]] bool operator==(const GitCommitConfiguration&) const = default;
};

//! Upstream's `CommitOptions.all`, which is `boolean | 'tracked'`.
enum class EGitCommitAll : std::uint8_t {
	None,
	All,
	Tracked,
};

//!
//! @brief Upstream's `CommitOptions`.
//!
//! `all` never reaches the git argument builder: `Repository.commit` runs
//! `git add` for it and then deletes the field before calling `Git.commit`. It
//! is carried here for the same reason upstream carries it — the staging step
//! and the smart-commit gates both read it.
//!
struct GitCommitOptions final {
	EGitCommitAll all{ EGitCommitAll::None };
	bool amend{};
	bool signoff{};
	bool empty{};
	bool noVerify{};
	bool verbose{};
	bool useEditor{};
	//! `undefined` upstream means "say nothing about signing"; `false` emits
	//! `--no-gpg-sign`, which is not the same thing as saying nothing.
	std::optional<bool> signCommit;
	bool requireUserConfig{ true };

	[[nodiscard]] bool operator==(const GitCommitOptions&) const = default;
};

//! One `git commit` invocation: its arguments and the bytes fed to its stdin.
struct GitCommitInvocation final {
	std::vector<std::wstring> arguments;
	//! The commit message, which upstream always passes through `--file -` rather
	//! than through an argument, so no message content can be reread as an option
	//! and no command-line length limit can truncate it.
	std::string standardInput;
	//! True when `--file -` is present, i.e. when stdin must be written and closed.
	bool writesStandardInput{};

	[[nodiscard]] bool operator==(const GitCommitInvocation&) const = default;
};

//!
//! @brief `Git.commit`'s argument construction, reproduced exactly.
//!
//! Including its oddities: with a message, `--allow-empty-message` is appended
//! once for the message and a second time on the `!useEditor` path, and upstream
//! emits both. `requireUserConfig` splices `-c user.useConfigOnly=true` ahead of
//! `commit`, which this runner then prefixes with `-C <root>`.
//!
[[nodiscard]] GitCommitInvocation BuildGitCommitInvocation(
	std::wstring_view message, const GitCommitOptions& options);

//!
//! @brief What the commit gates need to know about the repository.
//!
//! Upstream reads these off the live `Repository` object. They are a value here
//! so the orchestration below can be exercised without one.
//!
struct GitCommitRepositoryState final {
	//! `repository.indexGroup.resourceStates.length`.
	std::size_t stagedCount{};
	//! `repository.workingTreeGroup.resourceStates.length`.
	std::size_t workingTreeCount{};
	//! Every working-tree row is untracked, which is upstream's
	//! `smartCommitChanges === 'tracked'` no-op condition.
	bool workingTreeAllUntracked{};
	//! `repository.HEAD?.commit !== undefined`.
	bool headHasCommit{};
	//! `repository.headShortName`, which names the branch in the message prompt.
	std::wstring headShortName;
	//! Absolute paths of the Staged Changes rows. Upstream compares its dirty
	//! documents against `indexGroup.resourceStates` by path, so the identity of
	//! the staged set — not merely its size — is part of this value.
	std::vector<std::wstring> stagedPaths;

	[[nodiscard]] bool operator==(const GitCommitRepositoryState&) const = default;
};

//! Derive the facts above from the rows the SCM view published.
[[nodiscard]] GitCommitRepositoryState BuildGitCommitRepositoryState(
	std::wstring_view repositoryRoot,
	const std::vector<GitStageResource>& resources,
	std::wstring_view headShortName,
	bool headHasCommit);

//! The confirmation shape is not commit-specific, so it lives in `GitPrompt.h`
//! and the sync path uses the same one. The names here are kept because they say
//! which family a value belongs to at a call site.
using GitCommitPrompt = GitPrompt;
using GitCommitConfirmationPresenter = GitPromptPresenter;

//!
//! @brief `window.showInputBox`, for the case where the SCM box was left empty.
//!
//! Returns nothing when the user dismissed the box. An empty string is a
//! different answer from a dismissal and upstream treats it as one: it becomes
//! an empty commit message, which `--allow-empty-message` permits.
//!
using GitCommitMessagePrompt = std::function<std::optional<std::wstring>(
	std::wstring_view placeholder, std::wstring_view prompt)>;

//! Runs one git command, writing `standardInput` to the child and closing it.
using GitCommitInvoker = std::function<GitExecutionResult(
	const std::vector<std::wstring>& arguments, std::string_view standardInput)>;

//!
//! @brief Unsaved documents this process holds under the repository root.
//!
//! Upstream enumerates `workspace.textDocuments` and filters to dirty documents
//! inside the repository, optionally narrowing to the staged set. One editor
//! process owns one document here, so this can only ever report that document —
//! a divergence recorded in this subsystem's `CLAUDE.md` rather than papered
//! over by pretending the enumeration is complete.
//!
using GitCommitDirtyDocumentEnumerator = std::function<std::vector<std::wstring>()>;

//! Saves every document the enumerator above would report. False when any failed.
using GitCommitDocumentSaver = std::function<bool()>;

/*!
	@brief Everything the commit commands need, injected. HWND-free by design.
*/
struct GitCommitCommandContext final {
	//! Absolute repository root.
	std::wstring repositoryRoot;
	GitCommitInvoker run;
	GitCommitConfirmationPresenter confirm;
	GitStageMessagePresenter message;
	GitCommitMessagePrompt promptForMessage;
	GitCommitDirtyDocumentEnumerator dirtyDocuments;
	GitCommitDocumentSaver saveDocuments;
	GitCommitConfiguration configuration;
	//! Optional localization callback for commit prompts and status messages.
	GitRefTextResolver text;
};

enum class EGitCommitCommandStatus : std::uint8_t {
	Succeeded,
	//! A gate upstream returns early from without saying anything: nothing to
	//! commit and the empty commit declined, or a suggestion the user turned down.
	NotApplicable,
	//! The user dismissed a confirmation or the message prompt. Not a failure.
	Cancelled,
	//! A rebase is in progress. Upstream's `Repository.commit` does not commit at
	//! all in that state — it runs `git rebase --continue`, whose conflict
	//! handling and stopped-commit model do not exist here. Committing anyway
	//! would write a commit onto a detached rebase HEAD, so this fails closed.
	UnsupportedRebaseInProgress,
	//! A git invocation did not succeed, or a required presenter was missing.
	Failed,
};

struct GitCommitCommandResult final {
	EGitCommitCommandStatus status{ EGitCommitCommandStatus::Failed };
	//! Empty unless `status` is `Failed` or `UnsupportedRebaseInProgress`.
	std::wstring message;
	//!
	//! @brief What the SCM input box must contain after this command.
	//!
	//! Present only when the command actually changes it: a successful commit
	//! clears it, and `git.undoCommit` restores the undone commit's message so the
	//! text is not lost. Absent means "leave the box alone", which is what keeps a
	//! failed commit from discarding the message that failed.
	//!
	std::optional<std::wstring> inputBoxValue;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitCommitCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitCommitCommandResult&) const = default;
};

//! `'The following file has unsaved changes ...'` / `'There are {0} unsaved files.'`
[[nodiscard]] GitCommitPrompt BuildUnsavedDocumentsPrompt(const std::vector<std::wstring>& documents,
	const GitRefTextResolver& text = {});

//!
//! @brief `'There are no staged changes to commit.'`
//!
//! Upstream offers `Yes` / `Always` / `Never`, where the latter two write
//! `git.enableSmartCommit` / `git.suggestSmartCommit` to the user's Settings.
//! There is no configuration writer on this path, so those two buttons are
//! absent rather than present and inert — the same policy the SCM menus use for
//! an unroutable entry.
//!
[[nodiscard]] GitCommitPrompt BuildNoStagedChangesPrompt(const GitRefTextResolver& text = {});

//! `'There are no changes to commit.'` with `Create Empty Commit`. Informational
//! and non-modal upstream, which is why both flags are carried on the prompt.
[[nodiscard]] GitCommitPrompt BuildNoChangesPrompt(const GitRefTextResolver& text = {});

//!
//! @brief `'You are about to commit your changes without verification...'`
//!
//! Upstream offers `OK` and `OK, Don't Ask Again`; the second writes
//! `git.confirmNoVerifyCommit` to Settings, so it is absent here for the same
//! reason `Always` / `Never` are absent above.
//!
[[nodiscard]] GitCommitPrompt BuildNoVerifyCommitPrompt(const GitRefTextResolver& text = {});

//! `'The last commit was a merge commit. ...'`, shown by `git.undoCommit`.
[[nodiscard]] GitCommitPrompt BuildUndoMergeCommitPrompt(const GitRefTextResolver& text = {});

//!
//! @brief `git.commit` and `git.commitAmend`.
//!
//! Reproduces `smartCommit`'s gate order: save unsaved documents, offer to stage
//! everything when nothing is staged, resolve the message, refuse or offer an
//! empty commit, then stage and commit. `inputBoxValue` is what the SCM box
//! currently holds; an empty one sends the command to upstream's prompt path.
//!
[[nodiscard]] GitCommitCommandResult RunGitCommit(
	const GitCommitCommandContext& context,
	const GitCommitRepositoryState& state,
	std::wstring_view inputBoxValue,
	GitCommitOptions options);

//! `git.undoCommit`: `reset HEAD~`, or a `HEAD` deletion plus a full unstage when
//! the undone commit was the first one and has no parent to reset onto.
[[nodiscard]] GitCommitCommandResult RunGitUndoCommit(
	const GitCommitCommandContext& context, const GitCommitRepositoryState& state);

} // namespace workbench::scm
