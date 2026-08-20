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

//! Nothing when upstream contributes no button for this state.
[[nodiscard]] std::optional<GitActionButton> BuildGitCommitActionButton(bool hasChanges, bool enabled);

//! Map a published group id (`merge`, `index`, `workingTree`, `untracked`) back
//! to its group. Nothing for an id no built-in Git group publishes.
[[nodiscard]] std::optional<EGitResourceGroup> ParseGitResourceGroupId(std::string_view groupId) noexcept;

} // namespace workbench::scm
