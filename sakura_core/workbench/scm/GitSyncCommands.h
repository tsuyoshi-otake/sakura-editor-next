/*! @file
 * @brief `git.fetch`, `git.pull`, `git.push`, `git.sync`, and `git.publish`.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitCommandRunner.h"
#include "workbench/scm/GitPrompt.h"
#include "workbench/scm/GitScmModel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief One configured remote, as `Git.getRemotes` reports it.
//!
//! `isReadOnly` is not a separate fact a caller may set: upstream derives it in
//! `getRemotes` from the push URL alone, and every parser here does the same, so
//! a remote can never be described as writable merely because nobody looked.
//!
struct GitRemote final {
	std::wstring name;
	std::wstring fetchUrl;
	std::wstring pushUrl;
	//! `pushUrl === undefined || pushUrl === 'no_push'`, exactly as upstream
	//! computes it for https://github.com/microsoft/vscode/issues/45271.
	bool isReadOnly{ false };

	[[nodiscard]] bool operator==(const GitRemote&) const = default;
};

//! `git remote --verbose`, which is upstream's `getRemotesGit`.
[[nodiscard]] std::vector<std::wstring> BuildGitRemoteArguments();

//!
//! @brief Reproduces `getRemotesGit`, including its read-only derivation.
//!
//! Upstream prefers parsing `.git/config` and falls back to this command; the
//! command is the authority here because it is the one answer git itself gives
//! for a worktree, a submodule, and an `includeIf` config alike. A line is split
//! on whitespace into name, URL, and type, and a type matching `fetch` or `push`
//! selects which URL is being reported; anything else sets both, which is what
//! upstream does for a config shape it does not recognize.
//!
[[nodiscard]] std::vector<GitRemote> ParseGitRemotes(std::string_view bytes);

//! Finds a remote by name. Null when the repository has no such remote, which is
//! a normal state — `_sync` reads it precisely to ask whether the push half applies.
[[nodiscard]] const GitRemote* FindGitRemote(const std::vector<GitRemote>& remotes, std::wstring_view name);

//!
//! @brief The `git.*` settings the sync path reads, with upstream's defaults.
//!
//! Taken from `extensions/git/package.json`'s own `configuration` block rather
//! than from recollection. There is no configuration reader on this path yet, so
//! the defaults are stated here as data: the value that will later come from
//! Settings has exactly one place to arrive, and the divergence is one struct
//! rather than a dozen literals buried in branches.
//!
struct GitSyncConfiguration final {
	//! `git.confirmSync`. True upstream, which is why syncing asks first.
	bool confirmSync{ true };
	//! `git.rebaseWhenSync`. False upstream; `git.syncRebase` is the explicit route.
	bool rebaseWhenSync{ false };
	//! `git.followTagsWhenSync`. False upstream, so the sync push omits `--follow-tags`.
	bool followTagsWhenSync{ false };
	//! `git.pullTags`. True upstream, so the pull carries `--tags`.
	bool pullTags{ true };
	//! `git.fetchOnPull`. False upstream; true fetches every remote before pulling.
	bool fetchOnPull{ false };
	//! `git.pruneOnFetch`. False upstream. `_fetch` applies it only when the caller
	//! did not already ask for a prune.
	bool pruneOnFetch{ false };
	//! `git.ignoreRebaseWarning`. False upstream, which is what makes the
	//! "might have been rebased" question appear at all.
	bool ignoreRebaseWarning{ false };
	//!
	//! @brief `git.autoStash`. False upstream.
	//!
	//! Kept as data even though nothing reads it yet: `maybeAutoStash` is not
	//! implemented here, and recording the setting's real default is how that
	//! absence stays visible instead of looking like a decision nobody made.
	//!
	bool autoStash{ false };
	//! `git.allowForcePush`. False upstream, so a force push is refused outright.
	bool allowForcePush{ false };
	//! `git.useForcePushWithLease`. True upstream.
	bool useForcePushWithLease{ true };
	//! `git.useForcePushIfIncludes`. True upstream.
	bool useForcePushIfIncludes{ true };
	//! `git.confirmForcePush`. True upstream.
	bool confirmForcePush{ true };
	//!
	//! @brief `git.supportCancellation`. False upstream.
	//!
	//! True upstream wraps the pull half of a sync in a cancellable progress
	//! notification. There is no progress/cancellation surface on this path, so
	//! the default is the only supported value; see this subsystem's `CLAUDE.md`.
	//!
	bool supportCancellation{ false };

	[[nodiscard]] bool operator==(const GitSyncConfiguration&) const = default;
};

//! Upstream's `ForcePushMode`.
enum class EGitForcePushMode : std::uint8_t {
	None,
	Force,
	ForceWithLease,
	ForceWithLeaseIfIncludes,
};

//! `Git.fetch`'s options, in the shape it reads them.
struct GitFetchOptions final {
	//! Empty means "no remote argument", which is `git fetch` against the default.
	std::wstring remote;
	//! Only meaningful with a remote, exactly as upstream nests it.
	std::wstring ref;
	//! `--all`. Ignored when a remote was named, as upstream's `else if` does.
	bool all{ false };
	bool prune{ false };

	[[nodiscard]] bool operator==(const GitFetchOptions&) const = default;
};

//! `Git.pull`'s options.
struct GitPullOptions final {
	bool tags{ false };
	bool unshallow{ false };
	//! `--autostash`. Upstream also gates this on git >= 2.27.0; that version probe
	//! does not exist here, so nothing sets it — see `GitSyncConfiguration::autoStash`.
	bool autoStash{ false };

	[[nodiscard]] bool operator==(const GitPullOptions&) const = default;
};

//! `Git.push`'s parameters, as one value.
struct GitPushOptions final {
	std::wstring remote;
	//! A refspec such as `main:main`, or a plain branch name for a publish.
	std::wstring name;
	bool setUpstream{ false };
	bool followTags{ false };
	EGitForcePushMode forcePushMode{ EGitForcePushMode::None };
	bool tags{ false };

	[[nodiscard]] bool operator==(const GitPushOptions&) const = default;
};

[[nodiscard]] std::vector<std::wstring> BuildGitFetchArguments(const GitFetchOptions& options);
[[nodiscard]] std::vector<std::wstring> BuildGitPullArguments(
	bool rebase, std::wstring_view remote, std::wstring_view branch, const GitPullOptions& options);
//!
//! @brief `Git.push`'s argument construction, reproduced in its exact order.
//!
//! `--force-if-includes` is emitted alongside `--force-with-lease` for the
//! if-includes mode; upstream additionally requires git >= 2.30, and there is no
//! version probe here. Nothing currently produces a non-`None` mode, because
//! `git.allowForcePush` defaults false and no command surfaces a force push, but
//! the builder stays faithful so the flag order is asserted rather than assumed.
//!
[[nodiscard]] std::vector<std::wstring> BuildGitPushArguments(const GitPushOptions& options);

//! `log --oneline --cherry <b>...<b>@{upstream} --`, upstream's rebase probe.
[[nodiscard]] std::vector<std::wstring> BuildMaybeRebasedArguments(std::wstring_view branch);
//! True when the probe's output begins with `=`, which is upstream's exact test.
//! A failed probe is not evidence of a rebase, so it answers false.
[[nodiscard]] bool ParseMaybeRebased(const GitExecutionResult& result);

//!
//! @brief Why a fetch, pull, or push did not succeed.
//!
//! One enumerator per `GitErrorCodes` value the sync path's own `catch` blocks
//! assign, plus the generic classifier's remote-facing codes. This is the typed
//! answer to "was that an authentication problem or a rejected push", which a
//! caller must be able to ask without pattern-matching a sentence.
//!
enum class EGitSyncFailureReason : std::uint8_t {
	//! The command succeeded; no failure to describe.
	None,
	//! `Authentication failed`.
	AuthenticationFailed,
	//! `Permission denied`, from `Git.push`.
	PermissionDenied,
	//! `Could not read from remote repository`.
	RemoteConnectionError,
	//! `Repository not found`.
	RepositoryNotFound,
	//! `unable to access`.
	CantAccessRemote,
	//! `Couldn't find remote ref`.
	NoRemoteReference,
	//! `No remote repository specified.`, from `Git.fetch`.
	NoRemoteRepositorySpecified,
	//! Another git process holds the lock.
	RepositoryIsLocked,
	//! `fatal: The current branch ... has no upstream branch`.
	NoUpstreamBranch,
	//! `error: failed to push some refs to`.
	PushRejected,
	ForcePushWithLeaseRejected,
	ForcePushWithLeaseIfIncludesRejected,
	//! `CONFLICT (...)` on **stdout**, which is where git writes it for a pull.
	Conflict,
	//! Local changes block the operation.
	DirtyWorkTree,
	//! `Please tell me who you are.`
	NoUserNameConfigured,
	//! `cannot lock ref` / `unable to update local ref`.
	CantLockRef,
	//! `cannot rebase onto multiple branches`.
	CantRebaseMultipleBranches,
	//! `! [rejected] ... (would clobber existing tag)`.
	TagConflict,
	//! `! [rejected] ... (non-fast-forward)`, from `Git.fetch`.
	BranchFastForwardRejected,
	//! `Not a git repository`.
	NotAGitRepository,
	//! `detected dubious ownership in repository at`.
	NotASafeGitRepository,
	//! Recognized as a failure, but not as one of the causes above.
	Other,
};

//!
//! @brief What, if anything, would make retrying the same command work.
//!
//! Separate from the reason because two different causes can call for the same
//! recovery, and because "can I just try again" is the question a caller has to
//! answer to decide whether to keep the command's operands alive.
//!
enum class EGitSyncRecovery : std::uint8_t {
	//! Nothing here says the identical command would ever succeed.
	None,
	//! Retrying can succeed once credentials are supplied or corrected.
	Authenticate,
	//! The remote moved ahead. Pull, then retry.
	PullThenRetry,
	//! Merge conflicts are in the working tree. Resolve them, then retry.
	ResolveConflicts,
	//! Local changes are in the way. Commit or stash them, then retry.
	CleanWorkingTree,
	//! `user.name` / `user.email` are not configured.
	ConfigureIdentity,
	//! The branch has no upstream, so publishing is the operation that applies.
	PublishBranch,
	//! Transient: a lock or a network path that the identical command may clear.
	RetryLater,
};

//! A failed git operation, described once for every caller of it.
struct GitSyncFailure final {
	EGitSyncFailureReason reason{ EGitSyncFailureReason::None };
	EGitSyncRecovery recovery{ EGitSyncRecovery::None };
	//! Upstream's own sentence where its error handler has one, and git's stderr
	//! through `DescribeGitFailure` where it does not.
	std::wstring message;

	[[nodiscard]] bool operator==(const GitSyncFailure&) const = default;
};

//! `Git.fetch`'s own `catch`, then the generic `getGitErrorCode` classifier.
[[nodiscard]] EGitSyncFailureReason ClassifyGitFetchFailure(const GitExecutionResult& result);
//! `Git.pull`'s `catch`. Conflict is detected on standard **output**, not stderr.
[[nodiscard]] EGitSyncFailureReason ClassifyGitPullFailure(const GitExecutionResult& result);
//! `Git.push`'s `catch`. The force-push mode decides which rejection applies.
[[nodiscard]] EGitSyncFailureReason ClassifyGitPushFailure(
	const GitExecutionResult& result, EGitForcePushMode forcePushMode);
//! Which recovery a reason calls for.
[[nodiscard]] EGitSyncRecovery RecoveryForGitSyncFailure(EGitSyncFailureReason reason) noexcept;
//!
//! @brief The full typed failure, message included.
//!
//! Reproduces `CommandCenter`'s error switch for the codes this path can produce,
//! including its `Authentication failed for '(.*)'` capture, and falls back to
//! its default branch — the first stderr line, or the last stdout line — before
//! finally deferring to the shared `DescribeGitFailure`, which is what turns a
//! timeout or a missing git into a sentence rather than an empty string.
//!
[[nodiscard]] GitSyncFailure DescribeGitSyncFailure(
	EGitSyncFailureReason reason, const GitExecutionResult& result);

//!
//! @brief What the sync commands need to know about the repository.
//!
//! Upstream reads these off the live `Repository` and its `HEAD`. They are a
//! value here so the orchestration can be exercised without one.
//!
struct GitSyncRepositoryState final {
	//! `HEAD.name`. Empty in a detached HEAD, where upstream leaves it undefined
	//! and every command below declines rather than guessing a branch.
	std::wstring headName;
	//! `HEAD.upstream.remote` / `HEAD.upstream.name`. Both empty when the branch
	//! has no upstream, which is the state `git.publish` exists for.
	std::wstring upstreamRemote;
	std::wstring upstreamName;
	int ahead{};
	int behind{};
	std::vector<GitRemote> remotes;

	//! True when HEAD is a named branch that tracks something.
	[[nodiscard]] bool HasUpstream() const noexcept { return !headName.empty() && !upstreamRemote.empty(); }
	[[nodiscard]] bool operator==(const GitSyncRepositoryState&) const = default;
};

//! Splits `origin/main` at its **first** slash, as upstream's `getBranch` does.
//! False when the value carries no slash, and then neither output is written.
[[nodiscard]] bool SplitGitUpstream(std::wstring_view upstream, std::wstring& remote, std::wstring& name);

//! Derives the state above from the published SCM state and the parsed remotes.
[[nodiscard]] GitSyncRepositoryState BuildGitSyncRepositoryState(
	const GitScmState& state, std::vector<GitRemote> remotes);

//! Which row a remote pick is offering.
enum class EGitRemotePickKind : std::uint8_t {
	Remote,
	//! Upstream's `FetchAllRemotesItem`, offered only by the fetch pick.
	AllRemotes,
};

struct GitRemotePickItem final {
	EGitRemotePickKind kind{ EGitRemotePickKind::Remote };
	std::wstring label;
	std::wstring description;
	//! Index into the state's remotes for a `Remote` row; unused otherwise.
	std::size_t remoteIndex{};

	[[nodiscard]] bool operator==(const GitRemotePickItem&) const = default;
};

//!
//! @brief The fetch pick, in upstream's order.
//!
//! The remote that HEAD's upstream names is spliced to the front, then the
//! `Fetch all remotes` row follows. Upstream renders a separator before that row;
//! there is no separator kind here because the native list has no group headers,
//! the same reason the checkout pick drops upstream's `RefItemSeparator`.
//!
[[nodiscard]] std::vector<GitRemotePickItem> BuildFetchRemotePickItems(const GitSyncRepositoryState& state);
//!
//! @brief `git.publish`'s pick: every remote, described by its push URL.
//!
//! Upstream's publish pick deliberately does **not** filter by push URL — only
//! `_push`'s explicit push-to variant does — so a read-only remote is offered
//! and git reports the refusal, rather than the row disappearing with no reason
//! given. Upstream also appends `Add a new remote...`; `git.addRemote` is not
//! registered here, so that row is absent rather than present and inert.
//!
[[nodiscard]] std::vector<GitRemotePickItem> BuildPublishRemotePickItems(const GitSyncRepositoryState& state);

//! Presents a remote pick and returns the chosen row, or nothing on dismissal.
using GitSyncRemotePicker = std::function<std::optional<std::size_t>(
	const std::vector<GitRemotePickItem>& items, std::wstring_view placeholder)>;

//! Runs one git command in the repository the context names.
using GitSyncInvoker = std::function<GitExecutionResult(const std::vector<std::wstring>& arguments)>;

//! Shows one human-readable message: upstream's warnings and failure reasons.
using GitSyncMessagePresenter = std::function<void(std::wstring_view message)>;

/*!
	@brief Everything the sync commands need, injected. HWND-free by design.
*/
struct GitSyncCommandContext final {
	GitSyncInvoker run;
	GitSyncRemotePicker pickRemote;
	GitPromptPresenter confirm;
	GitSyncMessagePresenter message;
	GitSyncConfiguration configuration;
};

enum class EGitSyncCommandStatus : std::uint8_t {
	Succeeded,
	//! A gate upstream returns early from: no remotes, no branch checked out,
	//! a read-only remote, or nothing ahead to push. Never an error.
	NotApplicable,
	//! The user dismissed a pick or a confirmation. Not a failure.
	Cancelled,
	//! A git invocation did not succeed, or a required presenter was missing.
	Failed,
};

struct GitSyncCommandResult final {
	EGitSyncCommandStatus status{ EGitSyncCommandStatus::Failed };
	//! Meaningful only when `status` is `Failed`.
	GitSyncFailure failure;
	//! Why the command stopped. Populated for every non-`Succeeded` status, so a
	//! caller reporting "nothing happened" can say which gate it was.
	std::wstring message;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EGitSyncCommandStatus::Succeeded; }
	[[nodiscard]] bool operator==(const GitSyncCommandResult&) const = default;
};

//! `'This action will pull and push commits from and to "{0}/{1}".'`
//! Upstream's second button, `OK, Don't Show Again`, writes `git.confirmSync` to
//! Settings; there is no configuration writer here, so it is absent rather than
//! present and inert — the same policy the commit path already documents.
[[nodiscard]] GitPrompt BuildSyncConfirmationPrompt(std::wstring_view remote, std::wstring_view branch);

//! `'The branch "{0}" has no remote branch. Would you like to publish this branch?'`
//! Upstream's `OK, Don't Ask Again` writes the `confirmBranchPublish` memento,
//! and there is no memento store here, so it is absent for the same reason.
[[nodiscard]] GitPrompt BuildPublishBranchPrompt(std::wstring_view branch);

//! `'It looks like the current branch "{0}" might have been rebased. ...'`
//! Upstream offers `Always Pull` / `Pull` / `Don't Pull`; `Always Pull` writes
//! `git.ignoreRebaseWarning`, so only the latter two appear here.
[[nodiscard]] GitPrompt BuildMaybeRebasedPrompt(std::wstring_view branch);

//! Which fetch upstream is performing.
enum class EGitFetchScope : std::uint8_t {
	//! `git.fetch`: the default remote, or a pick when there is more than one.
	Default,
	//! `git.fetchPrune`.
	Prune,
	//! `git.fetchAll`, and the fetch pick's `Fetch all remotes` row.
	All,
};

//!
//! @brief `git.fetch`, `git.fetchPrune`, and `git.fetchAll`.
//!
//! A repository with no remotes shows upstream's own warning and stops; with one
//! remote the default scope fetches without naming it, and with more than one it
//! asks which. Upstream ships each scope as its own command ID, and all three are
//! registered; the `All` scope is additionally reachable from the default scope's
//! pick through its `Fetch all remotes` row, exactly as upstream's own pick does.
//!
[[nodiscard]] GitSyncCommandResult RunGitFetch(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, EGitFetchScope scope);

//! `git.pull` and `git.pullRebase`. Declines a detached or untracked HEAD the way
//! upstream's `Repository.pull` does: with no upstream there is no remote to name,
//! so it runs a plain `git pull` and lets git report the outcome.
[[nodiscard]] GitSyncCommandResult RunGitPull(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, bool rebase);

//!
//! @brief `git.push`.
//!
//! Reproduces `_push`'s `PushType.Push` branch, including its recovery: a push
//! that fails with `NoUpstreamBranch` offers to publish the branch instead, which
//! is the one place where a failed command legitimately becomes a different one.
//!
[[nodiscard]] GitSyncCommandResult RunGitPush(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state);

//!
//! @brief `git.sync` and `git.syncRebase`.
//!
//! With no upstream this degrades to `git.push`, exactly as `_sync` does. The
//! push half is skipped entirely for a read-only remote, and skipped when HEAD is
//! not ahead — both are upstream's own early returns, not shortcuts taken here.
//!
[[nodiscard]] GitSyncCommandResult RunGitSync(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state, bool rebase);

//!
//! @brief `git.publish`.
//!
//! Pushes the current branch with `-u` to the single remote, or to the one the
//! user picks. Upstream falls back to registered `RemoteSourcePublisher`s when
//! there are no remotes at all; there are none here, which lands on upstream's
//! own zero-publisher warning rather than on an invented one.
//!
[[nodiscard]] GitSyncCommandResult RunGitPublish(
	const GitSyncCommandContext& context, const GitSyncRepositoryState& state);

} // namespace workbench::scm
