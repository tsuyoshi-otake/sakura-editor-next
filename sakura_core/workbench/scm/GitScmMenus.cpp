/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/GitScmMenus.h"

namespace workbench::scm {
namespace {

//! Upstream's bare titles, from `extensions/git/package.nls.json`.
constexpr std::wstring_view kOpenChangesTitle = L"Open Changes";
constexpr std::wstring_view kOpenFileTitle = L"Open File";
constexpr std::wstring_view kStageTitle = L"Stage Changes";
constexpr std::wstring_view kUnstageTitle = L"Unstage Changes";
constexpr std::wstring_view kDiscardTitle = L"Discard Changes";
constexpr std::wstring_view kStageAllTitle = L"Stage All Changes";
constexpr std::wstring_view kUnstageAllTitle = L"Unstage All Changes";
constexpr std::wstring_view kDiscardAllTitle = L"Discard All Changes";

GitMenuItem Item(std::string_view commandId, std::wstring_view title)
{
	return GitMenuItem{ std::string(commandId), std::wstring(title), false };
}

GitMenuItem Separator()
{
	return GitMenuItem{ {}, {}, true };
}

} // namespace

std::vector<GitMenuItem> BuildGitResourceContextMenu(EGitResourceGroup group)
{
	// `navigation` first, then `1_modification`. Upstream's `2_view` reveal
	// entries and its `worktree_diff` comparison have no route here and are
	// therefore absent; so is the `inline@1` action, which under the default
	// `git.openDiffOnClick` is `git.openFile2` rather than `git.openChange`.
	std::vector<GitMenuItem> items;
	// Every navigation entry upstream contributes carries no explicit order, so
	// they sort by title: `Open Changes` < `Open File` < `Open File (HEAD)`.
	// `git.openHEADFile` has no route here and is absent. Merge Changes is the
	// one group upstream gives no `git.openChange` at all — a conflicted file
	// is opened, not compared — so its navigation stays a single entry.
	if (group != EGitResourceGroup::Merge) {
		items.push_back(Item("git.openChange", kOpenChangesTitle));
	}
	items.push_back(Item("git.openFile", kOpenFileTitle));
	items.push_back(Separator());
	switch (group) {
	case EGitResourceGroup::Merge:
		// Upstream stages a conflicted file to mark it resolved. Ours refuses
		// with a typed `UnsupportedMergeConflict` and says so, which is a
		// reported boundary rather than a silent no-op, so the entry stays.
		items.push_back(Item("git.stage", kStageTitle));
		break;
	case EGitResourceGroup::Index:
		items.push_back(Item("git.unstage", kUnstageTitle));
		break;
	case EGitResourceGroup::WorkingTree:
	case EGitResourceGroup::Untracked:
		// Title order, exactly as upstream sorts one menu group at equal order.
		// `git.ignore` carries an explicit `order` of 3 and would follow these
		// two; it has no route here and is absent.
		items.push_back(Item("git.clean", kDiscardTitle));
		items.push_back(Item("git.stage", kStageTitle));
		break;
	}
	return items;
}

std::vector<GitMenuItem> BuildGitResourceGroupContextMenu(
	EGitResourceGroup group, EUntrackedChangesPolicy untrackedChanges)
{
	std::vector<GitMenuItem> items;
	switch (group) {
	case EGitResourceGroup::Merge:
		// Upstream contributes `git.stageAllMerge` alone here. It is not
		// registered, so this header has no menu rather than a stage-all that
		// would silently do nothing.
		break;
	case EGitResourceGroup::Index:
		items.push_back(Item("git.unstageAll", kUnstageAllTitle));
		break;
	case EGitResourceGroup::WorkingTree:
		// Upstream's `when` clause: the plain pair under `mixed`, the `*Tracked`
		// variants otherwise. Those variants are not registered, so the header
		// carries no menu under a policy this product does not publish anyway.
		if (untrackedChanges == EUntrackedChangesPolicy::Mixed) {
			items.push_back(Item("git.cleanAll", kDiscardAllTitle));
			items.push_back(Item("git.stageAll", kStageAllTitle));
		}
		break;
	case EGitResourceGroup::Untracked:
		// `git.cleanAllUntracked` / `git.stageAllUntracked` are not registered.
		break;
	}
	return items;
}

std::optional<EGitResourceGroup> ParseGitResourceGroupId(std::string_view groupId) noexcept
{
	if (groupId == kGitStageGroupTokenMerge) return EGitResourceGroup::Merge;
	if (groupId == kGitStageGroupTokenIndex) return EGitResourceGroup::Index;
	if (groupId == kGitStageGroupTokenWorkingTree) return EGitResourceGroup::WorkingTree;
	if (groupId == kGitStageGroupTokenUntracked) return EGitResourceGroup::Untracked;
	// An extension-contributed provider's group is not a built-in Git group, and
	// giving it Git's menu would offer to stage something Git does not own.
	return std::nullopt;
}

std::vector<GitToolbarAction> BuildGitScmTitleToolbarActions()
{
	// `git.commit`'s `$(check)` and `git.refresh`'s `$(refresh)` are the icons the
	// contribution itself declares, so the row shows the same two glyphs upstream
	// shows rather than a locally chosen pair.
	return {
		GitToolbarAction{ L"$(check)", L"Commit", "git.commit" },
		GitToolbarAction{ L"$(refresh)", L"Refresh", "git.refresh" },
	};
}

std::vector<GitMenuItem> BuildGitScmTitleOverflowMenu()
{
	// `1_header`, in its contributed order. `git.clone` and `git.checkout` are
	// upstream's `Clone` and `Checkout to...`, which this product also registers,
	// so the whole group is routable.
	std::vector<GitMenuItem> items{
		Item("git.pull", L"Pull"),
		Item("git.push", L"Push"),
		Item("git.clone", L"Clone"),
		Item("git.checkout", L"Checkout to..."),
		Item("git.fetch", L"Fetch"),
	};
	// `2_main`'s eight submenus are skipped entirely: a submenu whose every entry
	// is unroutable would be an empty popup claiming actions exist.
	items.push_back(Separator());
	items.push_back(Item("git.showOutput", L"Show Git Output"));
	return items;
}

std::vector<GitToolbarAction> BuildGitResourceGroupInlineActions(
	EGitResourceGroup group, EUntrackedChangesPolicy untrackedChanges)
{
	// The icons are the ones the contribution itself declares, so the row shows
	// upstream's own glyphs rather than a locally chosen pair.
	switch (group) {
	case EGitResourceGroup::Merge:
		// `git.stageAllMerge` alone, and it has no route here.
		return {};
	case EGitResourceGroup::Index:
		return { GitToolbarAction{ L"$(remove)", std::wstring{ kUnstageAllTitle }, "git.unstageAll" } };
	case EGitResourceGroup::WorkingTree:
		if (untrackedChanges == EUntrackedChangesPolicy::Mixed) {
			return {
				GitToolbarAction{ L"$(discard)", std::wstring{ kDiscardAllTitle }, "git.cleanAll" },
				GitToolbarAction{ L"$(add)", std::wstring{ kStageAllTitle }, "git.stageAll" },
			};
		}
		// `git.cleanAllTracked` / `git.stageAllTracked` are not registered.
		return {};
	case EGitResourceGroup::Untracked:
		// `git.cleanAllUntracked` / `git.stageAllUntracked` are not registered.
		return {};
	}
	return {};
}

std::vector<GitToolbarAction> BuildGitScmHistoryTitleToolbarActions()
{
	// `git.fetchAll` is upstream's own id and is registered here with a real
	// executor. Pull and Push keep upstream's `$(repo-pull)` / `$(repo-push)`
	// icons but route to `git.pull` / `git.push` rather than to upstream's
	// ref-scoped `git.pullRef` / `git.pushRef`: with no history-item reference
	// filter, the Graph's current reference is always HEAD, so the ref-scoped
	// command and the plain one are the same operation. Refresh routes to
	// `git.refresh`, which refreshes this Graph along with the rest of the view.
	return {
		GitToolbarAction{ L"$(git-fetch)", L"Fetch From All Remotes", "git.fetchAll" },
		GitToolbarAction{ L"$(repo-pull)", L"Pull", "git.pull" },
		GitToolbarAction{ L"$(repo-push)", L"Push", "git.push" },
		GitToolbarAction{ L"$(refresh)", L"Refresh", "git.refresh" },
	};
}

std::vector<GitMenuItem> BuildGitHistoryItemContextMenu()
{
	// `9_copy`, in its contributed order. Upstream's `!listMultiSelection` guard
	// is satisfied by construction: this Graph selects one row at a time.
	return {
		Item("git.copyCommitId", L"Copy Commit Hash"),
		Item("git.copyCommitMessage", L"Copy Commit Message"),
	};
}

std::optional<GitActionButton> BuildGitCommitActionButton(bool hasChanges, bool enabled)
{
	if (!hasChanges) return std::nullopt;
	GitActionButton button;
	button.title = L"$(check) Commit";
	button.commandId = "git.commit";
	button.enabled = enabled;
	// Upstream's two secondary groups are the commit variants and the
	// post-commit-command variants. Only the first group is routable here; the
	// omission of Commit & Push / Commit & Sync is recorded in this directory's
	// CLAUDE.md rather than approximated with a plain commit.
	button.secondaryCommands = {
		Item("git.commit", L"Commit"),
		Item("git.commitAmend", L"Commit (Amend)"),
	};
	return button;
}

} // namespace workbench::scm
