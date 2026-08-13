/*! @file
 * @brief HWND-free row model for the Explorer context menu, in VS Code's order.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/commands/ExplorerCommandIds.h"

#include <string_view>
#include <vector>

namespace workbench::explorer {

//! What the right-clicked Explorer row is.  A workspace root is a folder in
//! upstream's `ExplorerFolderContext`, but its own `ExplorerRootContext` negates
//! the rename/delete items, so the two folder cases are distinct inputs here.
enum class EExplorerResourceKind : unsigned char {
	File,
	Folder,
	WorkspaceRoot,
};

enum class EExplorerContextMenuRowKind : unsigned char {
	Command,
	Separator,
};

//! One rendered menu row.  It carries only the stable command ID; titles and
//! keybinding hints are the command registry's facts and are resolved by the
//! projection so this model never becomes a second title authority.
class ExplorerContextMenuRow final {
public:
	[[nodiscard]] static ExplorerContextMenuRow Command(std::string_view commandId) noexcept
	{
		return ExplorerContextMenuRow{ EExplorerContextMenuRowKind::Command, commandId };
	}

	[[nodiscard]] static ExplorerContextMenuRow Separator() noexcept
	{
		return ExplorerContextMenuRow{ EExplorerContextMenuRowKind::Separator, {} };
	}

	[[nodiscard]] EExplorerContextMenuRowKind Kind() const noexcept { return m_kind; }

	//! Empty exactly when the row is a separator.
	[[nodiscard]] std::string_view CommandId() const noexcept { return m_commandId; }

	[[nodiscard]] bool operator==(const ExplorerContextMenuRow&) const = default;

private:
	ExplorerContextMenuRow(EExplorerContextMenuRowKind kind, std::string_view commandId) noexcept
		: m_kind(kind)
		, m_commandId(commandId)
	{
	}

	EExplorerContextMenuRowKind m_kind;
	std::string_view m_commandId;
};

//!
//! @brief Builds the Explorer context menu rows exactly as upstream VS Code
//! orders the commands this registry implements.
//!
//! Upstream's `MenuId.ExplorerContext` sorts `navigation` first and the
//! remaining groups lexicographically, with one separator between adjacent
//! non-empty groups.  Restricted to the eight registered commands, the groups
//! and orders are (fileActions.contribution.ts, fileCommands contribution):
//!
//! - `navigation`: `explorer.newFile` (4, folders only), `explorer.newFolder`
//!   (6, folders only), `revealFileInOS` (20, every filesystem resource).
//! - `6_copypath`: `copyFilePath` (10), `copyRelativeFilePath` (20).
//! - `7_modification`: `renameFile` (10), then `moveFileToTrash` (20) when the
//!   resource can reach the trash or `deleteFile` (20) when it cannot -
//!   upstream registers the two deletions as separate items on
//!   `ExplorerResourceMoveableToTrash`, never one item reading a flag.  Both
//!   items carry `ExplorerRootContext.toNegated()`, so a workspace root gets
//!   no `7_modification` group at all.
//!
//! `moveableToTrash` is ignored for a workspace root because no deletion row
//! exists there.  Commands upstream shows in this menu but this product has
//! not implemented (Open to Side, Open With, cut/copy/paste, compare,
//! workspace-root management) are omitted entirely rather than rendered
//! disabled; the divergence record lives in this directory's `CLAUDE.md`.
//!
[[nodiscard]] inline std::vector<ExplorerContextMenuRow> BuildExplorerContextMenuRows(
	EExplorerResourceKind resourceKind, bool moveableToTrash)
{
	namespace commands = workbench::commands;

	std::vector<ExplorerContextMenuRow> rows;
	rows.reserve(9);

	if (resourceKind != EExplorerResourceKind::File) {
		rows.push_back(ExplorerContextMenuRow::Command(commands::kExplorerNewFileCommandId));
		rows.push_back(ExplorerContextMenuRow::Command(commands::kExplorerNewFolderCommandId));
	}
	rows.push_back(ExplorerContextMenuRow::Command(commands::kRevealFileInOSCommandId));

	rows.push_back(ExplorerContextMenuRow::Separator());
	rows.push_back(ExplorerContextMenuRow::Command(commands::kCopyFilePathCommandId));
	rows.push_back(ExplorerContextMenuRow::Command(commands::kCopyRelativeFilePathCommandId));

	if (resourceKind != EExplorerResourceKind::WorkspaceRoot) {
		rows.push_back(ExplorerContextMenuRow::Separator());
		rows.push_back(ExplorerContextMenuRow::Command(commands::kRenameFileCommandId));
		rows.push_back(ExplorerContextMenuRow::Command(moveableToTrash
			? commands::kMoveFileToTrashCommandId
			: commands::kDeleteFileCommandId));
	}

	return rows;
}

} // namespace workbench::explorer
