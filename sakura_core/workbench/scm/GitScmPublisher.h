/*! @file
 * @brief Publishes the built-in Git repository into the VS Code SCM model.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitDiffModel.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/GitStageCommands.h"
#include "workbench/scm/SourceControlService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! VS Code's built-in Git extension identity. Ours must be the same string so a
//! consumer that already knows VS Code's SCM model finds the provider it expects
//! instead of a parallel one we invented.
inline constexpr std::string_view kGitExtensionId = "vscode.git";
inline constexpr std::string_view kGitProviderId = "git";
inline constexpr std::string_view kGitProviderLabel = "Git";

//! Upstream `createResourceGroup` ids and labels, in upstream declaration order.
inline constexpr std::string_view kGitMergeGroupId = "merge";
inline constexpr std::string_view kGitMergeGroupLabel = "Merge Changes";
inline constexpr std::string_view kGitIndexGroupId = "index";
inline constexpr std::string_view kGitIndexGroupLabel = "Staged Changes";
inline constexpr std::string_view kGitWorkingTreeGroupId = "workingTree";
inline constexpr std::string_view kGitWorkingTreeGroupLabel = "Changes";
inline constexpr std::string_view kGitUntrackedGroupId = "untracked";
inline constexpr std::string_view kGitUntrackedGroupLabel = "Untracked Changes";

//! VS Code's `git.untrackedChanges`. `Mixed` is upstream's default: an untracked
//! file is listed in Changes, not in a group of its own.
enum class EUntrackedChangesPolicy : std::uint8_t {
	Mixed,
	Separate,
	Hidden,
};

//!
//! @brief Which groups one change belongs to.
//!
//! Not an enum: a file with both staged and unstaged edits genuinely appears in
//! Staged Changes *and* Changes in VS Code, because those are two different
//! comparisons (HEAD to index, index to worktree) of the same path.
//!
struct GitResourceGroupSet final {
	bool merge{};
	bool index{};
	bool workingTree{};
	bool untracked{};

	[[nodiscard]] bool Any() const noexcept { return merge || index || workingTree || untracked; }
	[[nodiscard]] bool operator==(const GitResourceGroupSet&) const noexcept = default;
};

//! File the change the way upstream's model state update does.
[[nodiscard]] GitResourceGroupSet ClassifyChange(const GitChange& change, EUntrackedChangesPolicy policy) noexcept;

//!
//! @brief The status one group's row carries, or none when that group has no row.
//!
//! A group *is* an area. Staged Changes reads upstream's `raw.x` switch and every
//! other group reads the `raw.y` one, with conflicted and untracked paths decided
//! before either switch runs. Returning no status is upstream falling out of a
//! `switch` with no matching `case` — an index `T` and a working-tree `C` are
//! real porcelain codes upstream lists nowhere — and the caller must then publish
//! no row for that group rather than inventing one.
//!
[[nodiscard]] std::optional<EGitFileStatus> GitGroupStatus(
	const GitChange& change, EGitResourceGroup group) noexcept;

//!
//! @brief The status the file's single badge is derived from.
//!
//! Upstream fills one URI-keyed decoration map in the order index, untracked,
//! workingTree, merge, so a later group overwrites an earlier one and the
//! effective precedence is merge > workingTree > untracked > index. A staged add
//! that was edited again therefore shows `M`, not `A`. A group that publishes no
//! row is skipped, because upstream had no resource there to overwrite with.
//!
[[nodiscard]] std::optional<EGitFileStatus> GitDecorationStatus(
	const GitChange& change, const GitResourceGroupSet& groups) noexcept;

//!
//! @brief The codicon id upstream's CheckoutStatusBar picks for the current state.
//!
//! Upstream's `getIcon` also has a `$(lock)` case for a protected branch. That
//! depends on `git.branchProtection`, which is not read here, so the case is
//! absent rather than approximated by an unrelated condition.
//!
[[nodiscard]] std::string_view GitCheckoutStatusIcon(const GitScmState& state, EUntrackedChangesPolicy policy) noexcept;

//!
//! @brief Upstream's `headShortName`: the branch name, else the short object name.
//!
//! Empty only when HEAD names nothing at all, i.e. a repository with no commit
//! and a detached HEAD. Upstream returns `undefined` there and its callers take
//! their no-branch branch.
//!
[[nodiscard]] std::string GitHeadShortName(const GitScmState& state);

//!
//! @brief Upstream's `headLabel`: `headShortName` plus its dirty-state markers.
//!
//! `*` for working-tree or untracked changes, `+` for staged changes, `!` for a
//! merge or rebase in progress, appended in that order. The icon carries the
//! same information for the state it can express; the markers are not a
//! substitute for it, because upstream renders both.
//!
[[nodiscard]] std::string GitHeadLabel(const GitScmState& state, EUntrackedChangesPolicy policy);

//! The `git.checkout` status-bar command, titled `$(icon) headLabel` exactly as upstream.
[[nodiscard]] ScmCommand BuildCheckoutStatusBarCommand(const GitScmState& state, EUntrackedChangesPolicy policy);

//!
//! @brief The sync status-bar command.
//!
//! With no upstream branch this is `git.publish` under `$(cloud-upload)`; with an
//! upstream it is `git.sync` under `$(sync)`, labelled `N\x2193 M\x2191` only when
//! the branch has actually diverged.
//!
[[nodiscard]] ScmCommand BuildSyncStatusBarCommand(const GitScmState& state);

//!
//! @brief One resource's status badge.
//!
//! Upstream publishes this through a FileDecorationProvider, which is a
//! different service from SCM: `SourceControlResourceState` has no letter. Ours
//! is a side table for the same reason — so the SCM model stays free of a
//! concept it does not own, and an extension-contributed provider simply has no
//! entry rather than an invented one.
//!
struct GitResourceDecoration final {
	//! `Uri::ToString()` of the resource, which is the join key.
	std::wstring resourceUri;
	wchar_t letter{ L'M' };
};

//!
//! @brief One published row as a stage/discard command operand.
//!
//! A side table for the same reason the decorations above are one: upstream's
//! `SourceControlResourceState` carries no such value, and its command handlers
//! receive the resource object itself. A native menu has only the rendered row,
//! so it needs the row's operand keyed by something the row knows — the URI it
//! renders and the group it sits in. Both halves are required: the same path
//! legitimately occupies a row in Staged Changes and one in Changes, and those
//! two rows stage and unstage different things.
//!
struct GitResourceOperand final {
	//! `Uri::ToString()` of the resource, which is half the join key.
	std::wstring resourceUri;
	//! The operand itself; `resource.group` is the other half of the key.
	GitStageResource resource;
};

//! One fully built provider snapshot, ready to be applied to the service.
struct GitPublication final {
	ScmProviderState provider;
	//! One entry per published resource URI, in group order.
	std::vector<GitResourceDecoration> decorations;
	//! One entry per published *row*, in the change walk's order rather than in
	//! group order — the groups are four separate lists, and a consumer joins on
	//! `(resourceUri, resource.group)` instead of on position. Derived by the same
	//! walk that builds the groups, so a row the view renders and the operand a
	//! menu names cannot describe different things.
	std::vector<GitResourceOperand> operands;
	//! Paths that could not be expressed as a file URI. Reported rather than
	//! dropped, so an unrepresentable path is never mistaken for a clean tree.
	std::vector<std::wstring> rejectedPaths;
};

//!
//! @brief Build the provider snapshot for one repository. Pure; touches no service.
//!
//! `repositoryRoot` must be an absolute Windows path. The provider `label` is
//! the source-control system's own name, `Git`; the repository's directory name
//! is the provider `name`, which is what the repository row renders.
//!
[[nodiscard]] GitPublication BuildGitPublication(
	const ScmOwner& owner,
	std::wstring_view repositoryRoot,
	const GitScmState& state,
	EUntrackedChangesPolicy policy = EUntrackedChangesPolicy::Mixed);

//!
//! @brief Every row of the publication above, as a stage/discard command operand.
//!
//! Upstream's `stageAll`, `unstageAll`, and `cleanAll` take only the repository
//! and read `repository.workingTreeGroup.resourceStates` and friends directly,
//! because in VS Code the resource groups *are* the operand list. Ours are built
//! by `BuildGitPublication` from the same `GitScmState` through the same
//! `ClassifyChange`, so this walks that identical classification rather than
//! introducing a second one — a row this returns is exactly a row the view
//! renders, and the two cannot drift.
//!
//! A path that occupies two groups yields two resources, one per group, which is
//! what makes "stage all changes" and "unstage all changes" name different sets
//! for the same file. Filtering to the groups a command accepts is left to
//! `SelectStageableResources` / `SelectDiscardableResources`, exactly as upstream
//! leaves it to the handler.
//!
[[nodiscard]] std::vector<GitStageResource> CollectGitStageResources(
	const GitScmState& state, EUntrackedChangesPolicy policy = EUntrackedChangesPolicy::Mixed);

//!
//! @brief One published row, as the diff resolver needs to see it.
//!
//! `GitDiffRow` carries no group because upstream's `Resource` does not: in VS
//! Code the handler receives the resource object itself, which already belongs
//! to exactly one group. A native menu has only the rendered row, so the group
//! travels beside the row here for the same reason it travels beside a stage
//! operand — the same path legitimately occupies a row in Staged Changes and a
//! row in Changes, and those two rows compare different pairs of texts.
//!
struct GitDiffRowEntry final {
	//! Half the join key; `row.path` is the other half.
	EGitResourceGroup group{ EGitResourceGroup::WorkingTree };
	GitDiffRow row;

	[[nodiscard]] bool operator==(const GitDiffRowEntry&) const = default;
};

//!
//! @brief Every row of the publication above, as a diff resolver input.
//!
//! Upstream's `git.openChange` receives the resource objects themselves and
//! calls `resource.openChange()`, so the diff it opens is always the *live*
//! row's. A native menu can only name a row by `(path, group)`, so this
//! re-derives the row from the current state through the same single change
//! walk `BuildGitPublication` and `CollectGitStageResources` use. Re-deriving
//! is what keeps the diff a command opens from being the one a stale snapshot
//! described.
//!
//! `GitDiffRow::stagedInIndex` is decided per change rather than per row,
//! because upstream's `sanitizeRef('~')` looks the path up in the index group
//! regardless of which group's row was clicked.
//!
[[nodiscard]] std::vector<GitDiffRowEntry> CollectGitDiffRows(
	const GitScmState& state, EUntrackedChangesPolicy policy = EUntrackedChangesPolicy::Mixed);

//!
//! @brief Applies successive publications for one repository to the SCM service.
//!
//! Owns exactly one provider handle. The service is borrowed, never owned; the
//! publisher retracts its provider on destruction so a torn-down repository can
//! never leave a stale provider behind.
//!
class GitScmPublisher final {
public:
	GitScmPublisher(SourceControlService* service, ScmOwner owner);
	~GitScmPublisher();

	GitScmPublisher(const GitScmPublisher&) = delete;
	GitScmPublisher& operator=(const GitScmPublisher&) = delete;

	//! Create or refresh the provider. `NotApplicable` when there is no service.
	[[nodiscard]] EScmOperationStatus Publish(std::wstring_view repositoryRoot, const GitScmState& state,
		EUntrackedChangesPolicy policy = EUntrackedChangesPolicy::Mixed);

	//! Remove the provider. Idempotent, and safe when nothing was ever published.
	[[nodiscard]] EScmOperationStatus Retract();

	//!
	//! @brief Record the commit message the user is currently typing.
	//!
	//! `BuildGitPublication` is pure and clears `inputBox.value` on every build,
	//! and `Publish` replaces the whole provider whenever the repository state
	//! changes. Without an owner for the value outside that build, the periodic
	//! refresh would delete a half-written commit message. The publisher owns it
	//! and reapplies it, so the SCM service stays the single authority for what
	//! the box contains while the view stays the authority for what was typed.
	//!
	//! Returns `NotApplicable` before anything has been published; the value is
	//! still retained and carried by the next `Publish`.
	//!
	[[nodiscard]] EScmOperationStatus SetInputBoxValue(std::string value);

	//! The last recorded commit message. Empty before anything was typed.
	[[nodiscard]] const std::string& InputBoxValue() const noexcept { return m_inputBox.value; }
	//! The last published box, whose placeholder names the branch being committed to.
	[[nodiscard]] const ScmInputBoxState& InputBox() const noexcept { return m_inputBox; }

	[[nodiscard]] const std::string& ProviderHandle() const noexcept { return m_handle; }
	[[nodiscard]] bool HasProvider() const noexcept { return m_created; }
	//! Badges for the resources of the last successful publication.
	[[nodiscard]] const std::vector<GitResourceDecoration>& Decorations() const noexcept { return m_decorations; }
	//! Command operands for the rows of the last successful publication.
	[[nodiscard]] const std::vector<GitResourceOperand>& Operands() const noexcept { return m_operands; }

private:
	SourceControlService* m_service{};
	ScmOwner m_owner;
	std::string m_handle;
	bool m_created{};
	//! The last applied input, so an unchanged refresh costs no service revision.
	std::wstring m_lastRoot;
	GitScmState m_lastState;
	EUntrackedChangesPolicy m_lastPolicy{ EUntrackedChangesPolicy::Mixed };
	//! The last published box. Its `value` is the typed commit message, which
	//! outlives any one published provider; the rest mirrors what the service
	//! holds, so a value-only update cannot silently reset the placeholder.
	ScmInputBoxState m_inputBox;
	std::vector<GitResourceDecoration> m_decorations;
	std::vector<GitResourceOperand> m_operands;
};

} // namespace workbench::scm
