/*! @file
 * @brief `git.stage` / `git.unstage` / `git.clean` and their "all" variants.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitRefModel.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief Upstream's `ResourceGroupType`.
//!
//! A command's first act is to filter its input by this, and the filters are not
//! the same: `git.stage` accepts Working Tree and Untracked rows, `git.unstage`
//! accepts only Index rows, and `git.clean` accepts Working Tree and Untracked.
//! Keeping the group on the resource is what makes "the same file" in Staged
//! Changes and in Changes two different operands rather than one ambiguous path.
//!
enum class EGitResourceGroup : std::uint8_t {
	Merge,
	Index,
	WorkingTree,
	Untracked,
};

//!
//! @brief One row a stage/unstage/discard command acts on.
//!
//! `path` is repository-relative, which is both what porcelain v2 reports and
//! what a git pathspec wants. The absolute form is only needed for the Recycle
//! Bin path, and it is derived from the context's repository root there.
//!
struct GitStageResource final {
	std::wstring path;
	EGitResourceGroup group{ EGitResourceGroup::WorkingTree };
	//!
	//! @brief Upstream's `Status.UNTRACKED` / `Status.IGNORED`.
	//!
	//! Deliberately **not** derived from `group`. Under the default
	//! `git.untrackedChanges: mixed` an untracked file is listed in Changes, so
	//! its group is `WorkingTree` while its status is still untracked — and
	//! upstream's discard splits `toClean` from `toCheckout` by the status, not
	//! by the group. Reading the group there would run `git checkout` against a
	//! path that has nothing in the index to restore from.
	//!
	bool untracked{};
	//! Upstream's `Status.DELETED`. It selects the "restore" wording of the
	//! discard confirmation, which is a materially different sentence from
	//! "discard changes in".
	bool deleted{};

	[[nodiscard]] bool operator==(const GitStageResource&) const = default;
};

//! Runs one git command in the repository the context names.
using GitStageCommandInvoker = std::function<GitExecutionResult(const std::vector<std::wstring>& arguments)>;

//! Shows one human-readable message, e.g. a failed invocation's reason.
using GitStageMessagePresenter = std::function<void(std::wstring_view message)>;

//!
//! @brief Moves absolute paths to the Recycle Bin.
//!
//! Returns the paths it could **not** move. Upstream trashes the first resource
//! as a probe and falls back to a permanent delete for the whole set when that
//! throws; reporting the exact failures instead means the fallback never
//! re-deletes a file that already reached the bin. A null deleter is upstream's
//! `discardUntrackedChangesToTrash === false` condition and takes the permanent
//! path with its own, louder confirmation.
//!
using GitTrashDeleter = std::function<std::vector<std::wstring>(const std::vector<std::wstring>& absolutePaths)>;

//! One button of a discard confirmation, with the exact rows it discards.
struct GitDiscardChoice final {
	std::wstring label;
	std::vector<GitStageResource> resources;

	[[nodiscard]] bool operator==(const GitDiscardChoice&) const = default;
};

//!
//! @brief A modal discard confirmation, as `window.showWarningMessage` builds it.
//!
//! `choices` is in upstream's argument order, so index 0 is the primary action.
//! The mixed tracked/untracked case is the only one with two buttons, and its
//! two buttons discard different sets — which is precisely the operational-safety
//! fact a single "are you sure" would destroy.
//!
struct GitDiscardPrompt final {
	std::wstring message;
	//! `MessageOptions.detail`. Empty when upstream passes none, including the
	//! mixed case, which folds its detail into `message` instead.
	std::wstring detail;
	std::vector<GitDiscardChoice> choices;

	[[nodiscard]] bool operator==(const GitDiscardPrompt&) const = default;
};

//! Presents a modal confirmation and returns the chosen index into `choices`,
//! or nothing when the user dismissed it. Dismissal is a cancel, never a yes.
using GitDiscardConfirmationPresenter = std::function<std::optional<std::size_t>(const GitDiscardPrompt& prompt)>;

/*!
	@brief Everything the staging commands need, injected.

	Deliberately HWND-free, exactly like `GitBranchCommandContext`: the
	orchestration below is the behavior worth testing, and it must not require a
	window to exercise.
*/
struct GitStageCommandContext final {
	//! Absolute repository root. Only the Recycle Bin path needs it, because a
	//! shell delete takes an absolute path while every git pathspec does not.
	std::wstring repositoryRoot;
	GitStageCommandInvoker run;
	GitDiscardConfirmationPresenter confirm;
	GitStageMessagePresenter message;
	GitTrashDeleter trash;
	//! Optional localization callback for discard prompts and stage messages.
	GitRefTextResolver text;
};

enum class EGitStageCommandStatus : std::uint8_t {
	Succeeded,
	//! Nothing in the selection belonged to a group this command operates on.
	//! Upstream returns early and says nothing; a separate terminal state keeps
	//! that from being reported as a success that changed something.
	NotApplicable,
	//! The user dismissed a confirmation. Not a failure.
	Cancelled,
	//! The selection contains a Merge Changes row. Staging one requires
	//! upstream's conflict categorization and its deletion-conflict picker,
	//! neither of which exists yet, so this fails closed rather than staging a
	//! file that may still hold conflict markers.
	UnsupportedMergeConflict,
	//! A git invocation did not succeed, or a required presenter was missing.
	Failed,
};

struct GitStageCommandResult final {
	EGitStageCommandStatus status{ EGitStageCommandStatus::Failed };
	//! Empty unless `status` is `Failed` or `UnsupportedMergeConflict`.
	std::wstring message;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitStageCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitStageCommandResult&) const = default;
};

//!
//! @brief Per-invocation budget for a pathspec list.
//!
//! Upstream splits on a single `MAX_CLI_LENGTH`; this runner also caps the
//! argument *count*, and `BuildEffectiveGitArguments` prepends `-C <root>` after
//! the chunk is built, so both limits are reserved against before splitting.
//!
struct GitPathChunkLimits final {
	std::size_t maximumArguments{ kMaximumGitArguments };
	std::size_t maximumCommandLineLength{ kMaximumGitCommandLineLength };

	//! Subtracts what the runner adds for this repository: the `-C` pair and an
	//! allowance for the quoted `git.exe` path that becomes argv[0].
	[[nodiscard]] static GitPathChunkLimits ForRepository(std::wstring_view repositoryRoot);
};

//!
//! @brief Split `prefix + paths` into invocations that fit the limits.
//!
//! Every prefix already ends in `--`, so a path can never be read as an option.
//! An empty `paths` yields exactly one chunk holding that bare prefix, which
//! means "no pathspec"; the commands guard against it, and the "all" forms pass
//! an explicit `.` instead. A single path too large to fit still gets its own
//! chunk: truncating it would silently change which file is operated on, so the
//! oversized request is left to fail visibly.
//!
[[nodiscard]] std::vector<std::vector<std::wstring>> BuildGitPathChunks(
	const std::vector<std::wstring>& prefix,
	const std::vector<std::wstring>& paths,
	const GitPathChunkLimits& limits);

//! `Git.add`: `add -A --`, or `add -u --` when only tracked content is wanted
//! (`git.untrackedChanges` other than `mixed`). `-A` is what makes staging a
//! deleted file record the deletion instead of doing nothing.
[[nodiscard]] std::vector<std::wstring> BuildStagePrefix(bool updateOnly);

//! `Git.revert`: `reset -q HEAD --` against a repository that has a commit, and
//! `rm --cached -r --` against an unborn branch, where there is no HEAD to reset
//! to. Upstream decides between them by whether `git branch` printed anything.
[[nodiscard]] std::vector<std::wstring> BuildUnstagePrefix(bool hasCommits);

//! `Git.checkout('', paths)`: `checkout -q --`, restoring tracked rows from the index.
[[nodiscard]] std::vector<std::wstring> BuildDiscardCheckoutPrefix();

//! `Git.clean`: `clean -f -q --`, permanently removing untracked rows.
[[nodiscard]] std::vector<std::wstring> BuildCleanPrefix();

//! Upstream's group filters, kept separate because they are genuinely different.
[[nodiscard]] std::vector<GitStageResource> SelectStageableResources(const std::vector<GitStageResource>& resources);
[[nodiscard]] std::vector<GitStageResource> SelectUnstageableResources(const std::vector<GitStageResource>& resources);
[[nodiscard]] std::vector<GitStageResource> SelectDiscardableResources(const std::vector<GitStageResource>& resources);

//! True when any row came from Merge Changes, which the stage path refuses.
[[nodiscard]] bool HasMergeResource(const std::vector<GitStageResource>& resources) noexcept;

//!
//! @brief Build the discard confirmation for a selection.
//!
//! Reproduces `_cleanAll`, `_cleanTrackedChanges`, `_cleanUntrackedChanges`, and
//! `getDiscardUntrackedChangesDialogDetails`, including the singular/plural and
//! restore/discard splits and the Recycle Bin wording. `untrackedToTrash` is
//! `git.discardUntrackedChangesToTrash`, whose default is `true`; with it false
//! the untracked wording becomes the FOREVER LOST warning, because then the
//! files really are permanently deleted.
//!
[[nodiscard]] GitDiscardPrompt BuildDiscardPrompt(
	const std::vector<GitStageResource>& resources, bool untrackedToTrash,
	const GitRefTextResolver& text = {});

//! The confirmation upstream shows when the Recycle Bin refused the delete.
[[nodiscard]] GitDiscardPrompt BuildTrashFallbackPrompt(const std::vector<GitStageResource>& resources,
	const GitRefTextResolver& text = {});

//! Join a repository-relative path onto the repository root.
[[nodiscard]] std::wstring JoinRepositoryPath(std::wstring_view repositoryRoot, std::wstring_view relativePath);

//!
//! @brief Group tokens used by the command arguments payload below.
//!
//! The same spellings as the published group IDs, which are upstream's own.
//! They are restated here rather than included from `GitScmPublisher.h`, whose
//! own include of `SourceControlService.h` would point this file's dependency at
//! the publishing layer it must stay independent of. A test that includes both
//! headers asserts the two never drift apart.
//!
inline constexpr std::string_view kGitStageGroupTokenMerge = "merge";
inline constexpr std::string_view kGitStageGroupTokenIndex = "index";
inline constexpr std::string_view kGitStageGroupTokenWorkingTree = "workingTree";
inline constexpr std::string_view kGitStageGroupTokenUntracked = "untracked";

//! Upper bound on the rows one invocation may name. A selection is a user
//! gesture over a list, so this is far above any real one; it exists so a
//! malformed or hostile payload cannot make the parser allocate without limit.
inline constexpr std::size_t kMaximumGitStageArgumentResources = 4'096;

//!
//! @brief Serialize a selection into a command's `arguments` payload.
//!
//! Upstream passes `SourceControlResourceState` objects straight to its command
//! handler, and serializes them across the provider boundary as opaque handles, so
//! there is no upstream wire form to reproduce. This shape is therefore
//! explicitly ours:
//! `[{"group":"workingTree","path":"src/a.cpp","untracked":false,"deleted":false}]`.
//!
//! A bare path would not do. The same file legitimately occupies a row in both
//! Staged Changes and Changes, so the group is half of the operand's identity;
//! and upstream's discard splits its targets by *status*, not by group, so
//! `untracked` and `deleted` have to survive the trip as well.
//!
[[nodiscard]] std::string BuildGitStageArguments(const std::vector<GitStageResource>& resources);

//!
//! @brief Parse a payload produced by `BuildGitStageArguments`.
//!
//! Fails closed: anything malformed — a bad token, an unknown key, a missing
//! `path` or `group`, more rows than the bound above, a path that is empty or
//! longer than one git argument may be — returns nothing rather than a partial
//! selection, because a partially parsed selection would stage or discard a set
//! the user never chose.
//!
//! Whitespace-only input is **not** malformed. It is the argument-less
//! invocation the two-argument `Execute` overload produces, and it yields an
//! empty selection, which the commands report as `NotApplicable`.
//!
[[nodiscard]] std::optional<std::vector<GitStageResource>> ParseGitStageArguments(std::string_view argumentsJson);

//! `git.stage` / `git.stageAll`. `updateOnly` mirrors `git.untrackedChanges`
//! being other than `mixed`, which is what makes `stageAll` skip untracked files.
[[nodiscard]] GitStageCommandResult RunGitStage(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources, bool updateOnly = false);

//! `git.unstage`.
[[nodiscard]] GitStageCommandResult RunGitUnstage(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources);

//! `git.unstageAll`, which upstream expresses as `revert([])` — a whole-index
//! reset rather than a listing of every staged path.
[[nodiscard]] GitStageCommandResult RunGitUnstageAll(const GitStageCommandContext& context);

//! `git.clean` / `git.cleanAll`.
[[nodiscard]] GitStageCommandResult RunGitDiscard(
	const GitStageCommandContext& context, const std::vector<GitStageResource>& resources);

} // namespace workbench::scm
