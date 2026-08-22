/*! @file
 * @brief The Source Control view's context menus, as VS Code contributes them.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitScmPublisher.h"
#include "workbench/scm/GitStageCommands.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief One entry of a Source Control context menu.
//!
//! `title` is upstream's **bare** `package.nls.json` string — `Stage Changes`,
//! not `Git: Stage Changes`. The category prefix belongs to the Command
//! Palette, and `WorkbenchCommandDescriptor::title` carries the prefixed form
//! for exactly that surface, so a menu must not reuse it.
//!
struct GitMenuItem final {
	//! Upstream's own command id. Empty only for a separator.
	std::string commandId;
	//! Upstream's bare title. Empty only for a separator.
	std::wstring title;
	//! True when this row is the rule between two of upstream's menu groups.
	bool separator{};
	//! JSON payload passed to the command route. Context menus use `[]`; the
	//! commit action button uses this to carry upstream's post-commit command.
	std::string argumentsJson{ "[]" };

	[[nodiscard]] bool operator==(const GitMenuItem&) const = default;
};

//!
//! @brief `scm/resourceState/context` for one resource row.
//!
//! Reproduces upstream's contribution for that row's group, ordered the way
//! upstream's `MenuInfo._compareMenuItems` orders it: `navigation` first, then
//! the remaining groups alphabetically, and within a group by `order` and then
//! by title. `Discard Changes` therefore precedes `Stage Changes`, because both
//! sit in `1_modification` at the default order and upstream sorts them by
//! title.
//!
//! Every upstream entry whose command has no route here is **absent**, not
//! rendered disabled and not approximated by a different command. The omitted
//! set and its reason are recorded in this directory's CLAUDE.md.
//!
[[nodiscard]] std::vector<GitMenuItem> BuildGitResourceContextMenu(EGitResourceGroup group);

//!
//! @brief `scm/resourceGroup/context` for one group header row.
//!
//! `untrackedChanges` is upstream's `git.untrackedChanges`, whose default
//! `mixed` is what this product publishes. It selects between upstream's
//! `stageAll`/`cleanAll` pair and its `*Tracked`/`*Untracked` variants exactly
//! as the contribution's `when` clauses do.
//!
//! Returns an empty menu for a group whose entire upstream contribution is
//! unroutable here. A caller must then show no menu at all rather than an empty
//! popup, because an empty menu claims the row has actions that are merely
//! unavailable.
//!
[[nodiscard]] std::vector<GitMenuItem> BuildGitResourceGroupContextMenu(
	EGitResourceGroup group, EUntrackedChangesPolicy untrackedChanges);

//!
//! @brief `ISCMProvider.actionButton`, as the built-in Git extension builds it.
//!
//! Upstream's `ActionButtonCommand` publishes one primary command plus grouped
//! secondary commands, which `SCMViewPane` renders as a split button under the
//! commit box. `hasChanges` is upstream's gate for the commit button: with no
//! resource in any group there is nothing to commit and the button is absent.
//!
//! `enabled` follows the input box, because upstream disables the whole button
//! while a repository operation is running and that is the same condition that
//! disables the box.
//!
struct GitActionButton final {
	//! Upstream's `$(check) Commit`, kept in `renderLabelWithIcons` syntax so the
	//! native renderer draws the same Codicon it does.
	std::wstring title;
	std::string commandId;
	bool enabled{ true };
	//! Upstream's `secondaryCommands`, already flattened with separators between
	//! its groups, so a renderer appends rows without knowing the grouping.
	std::vector<GitMenuItem> secondaryCommands;

	[[nodiscard]] bool operator==(const GitActionButton&) const = default;
};

//!
//! @brief One primary (always-visible) action of the repository row's toolbar.
//!
//! Upstream's `RepositoryRenderer` builds that toolbar from the provider's
//! `statusBarCommands` followed by `scm/title`'s `navigation` group, and every
//! `navigation` entry renders icon-only with its title as the hover text. The
//! icon is kept in `renderLabelWithIcons` syntax, exactly as the action button's
//! title is, so the same native renderer draws the same Codicon.
//!
struct GitToolbarAction final {
	//! `$(check)`, `$(refresh)`, ... Never empty: a toolbar action upstream draws
	//! as an icon must not fall back to a text button here.
	std::wstring icon;
	//! Upstream's bare title, which is the tooltip on an icon-only action.
	std::wstring tooltip;
	std::string commandId;

	[[nodiscard]] bool operator==(const GitToolbarAction&) const = default;
};

//!
//! @brief `scm/title`'s `navigation` group: the repository row's primary actions.
//!
//! Ordered as upstream contributes it - `git.commit`, then `git.refresh` - since
//! neither entry carries an explicit order and `MenuInfo._compareMenuItems`
//! keeps contribution order within one group at the same order value.
//!
//! These follow, never replace, the provider's `statusBarCommands`, which the
//! row already renders.
//!
[[nodiscard]] std::vector<GitToolbarAction> BuildGitScmTitleToolbarActions();

//!
//! @brief `scm/title`'s remaining groups: what the row's `...` overflow shows.
//!
//! Upstream's secondary actions in group order: `1_header` (Pull, Push, Clone,
//! Checkout to..., Fetch) by explicit order, then `2_main`'s submenus, then
//! `3_footer` (Show Git Output). The `2_main` submenus have no route here and
//! are absent rather than rendered as dead rows; the omission is recorded in
//! this directory's CLAUDE.md.
//!
[[nodiscard]] std::vector<GitMenuItem> BuildGitScmTitleOverflowMenu();
//!
//! @brief `scm/resourceGroup/context`'s `inline` group for one group header row.
//!
//! Upstream renders this group as an always-visible action bar on the group's
//! own row, which is why the `Changes` row carries a discard and a stage button
//! rather than only a context menu. The order is upstream's: `inline@1` first,
//! then `inline@2` in contribution order.
//!
//! `git.viewChanges` / `git.viewStagedChanges` / `git.viewUntrackedChanges`
//! (`inline@1`) are **absent**: they open a multi-file diff editor that has no
//! route here. `git.stageAllMerge` is absent for the same reason, which leaves
//! the merge group with no inline action at all. The omissions are recorded in
//! this directory's CLAUDE.md; nothing is approximated with a different command.
//!
[[nodiscard]] std::vector<GitToolbarAction> BuildGitResourceGroupInlineActions(
	EGitResourceGroup group, EUntrackedChangesPolicy untrackedChanges);

//!
//! @brief `MenuId.SCMHistoryTitle`'s `navigation` group: the Graph header's toolbar.
//!
//! Upstream's own order, by the contributed `order` values: the repository and
//! history-item-reference pickers (0, 1), `Go to Current History Item` (2),
//! `git.fetchAll` (900), `git.pullRef` (901), `git.pushRef` / `git.publish`
//! (902 / 903), and the view's own `Refresh` (1000).
//!
//! What ships here is `git.fetchAll`, pull, push, and refresh. The two pickers,
//! `Go to Current History Item`, the `pushRef`/`publish` switch, and the `...`
//! overflow's `View as List` / `View as Tree` are absent rather than drawn
//! inert, because each needs view state this Graph does not keep. The reasons
//! are recorded in this directory's CLAUDE.md.
//!
[[nodiscard]] std::vector<GitToolbarAction> BuildGitScmHistoryTitleToolbarActions();


//!
//! @brief `scm/historyItem/context` for one commit in the Graph view.
//!
//! Upstream's groups in order: `1_checkout` (Checkout, Checkout (Detached)),
//! `2_branch` (Create Branch..., Delete Branch), `3_tag` (Create Tag...,
//! Delete Tag), `4_modify` (Cherry Pick), `5_compare` (Compare with Remote,
//! Compare with Merge Base, Compare with...), and `9_copy` (Copy Commit Hash,
//! Copy Commit Message).
//!
//! Only `9_copy` has a route here. Every `git.graph.*` command operates on the
//! clicked commit, and this product registers no command that does - its
//! `git.checkout` is the Quick Pick "Checkout to...", which would check out
//! something other than the row the user right-clicked. Offering it here would
//! be a different command wearing upstream's label, so those entries are absent
//! rather than approximated, and the omission is recorded in this directory's
//! CLAUDE.md.
//!
[[nodiscard]] std::vector<GitMenuItem> BuildGitHistoryItemContextMenu();

//! Nothing when upstream contributes no button for this state.
[[nodiscard]] std::optional<GitActionButton> BuildGitCommitActionButton(bool hasChanges, bool enabled);

//! Map a published group id (`merge`, `index`, `workingTree`, `untracked`) back
//! to its group. Nothing for an id no built-in Git group publishes.
[[nodiscard]] std::optional<EGitResourceGroup> ParseGitResourceGroupId(std::string_view groupId) noexcept;

} // namespace workbench::scm
