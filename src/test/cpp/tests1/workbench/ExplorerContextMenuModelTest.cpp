/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/explorer/ExplorerContextMenuModel.h"

#include <string>
#include <vector>

//!
//! @brief Row-model coverage for the Explorer context menu.
//!
//! `BuildExplorerContextMenuRows` must reproduce upstream VS Code's
//! `MenuId.ExplorerContext` ordering for the eight commands
//! `RegisterExplorerCommands` implements: `navigation` first, the remaining
//! groups lexicographically, one separator between adjacent non-empty groups,
//! creation items on folders only, and no rename/deletion on a workspace root.
//! The expected sequences below are written as literal ID lists on purpose -
//! the test is the transcription of upstream's registration facts, so it must
//! not be derived from the same constants the model uses.
//!

using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::WorkbenchCommandRegistry;
using workbench::explorer::BuildExplorerContextMenuRows;
using workbench::explorer::EExplorerContextMenuRowKind;
using workbench::explorer::EExplorerResourceKind;
using workbench::explorer::ExplorerContextMenuRow;

namespace {

//! A separator renders as this marker so an expected menu reads as one list.
constexpr const char* kSeparatorMarker = "|";

std::vector<std::string> RenderRows(const std::vector<ExplorerContextMenuRow>& rows)
{
	std::vector<std::string> rendered;
	rendered.reserve(rows.size());
	for (const auto& row : rows) {
		rendered.emplace_back(row.Kind() == EExplorerContextMenuRowKind::Separator
			? kSeparatorMarker
			: std::string(row.CommandId()));
	}
	return rendered;
}

constexpr EExplorerResourceKind kEveryKind[] = {
	EExplorerResourceKind::File,
	EExplorerResourceKind::Folder,
	EExplorerResourceKind::WorkspaceRoot,
};

} // namespace

TEST(ExplorerContextMenuModel, FileRowsMatchUpstreamOrderWhenTrashIsAvailable)
{
	const std::vector<std::string> expected = {
		"revealFileInOS",
		kSeparatorMarker,
		"copyFilePath",
		"copyRelativeFilePath",
		kSeparatorMarker,
		"renameFile",
		"moveFileToTrash",
	};
	EXPECT_EQ(expected, RenderRows(BuildExplorerContextMenuRows(EExplorerResourceKind::File, true)));
}

TEST(ExplorerContextMenuModel, FolderRowsAddTheCreationCommandsAtTheTop)
{
	const std::vector<std::string> expected = {
		"explorer.newFile",
		"explorer.newFolder",
		"revealFileInOS",
		kSeparatorMarker,
		"copyFilePath",
		"copyRelativeFilePath",
		kSeparatorMarker,
		"renameFile",
		"moveFileToTrash",
	};
	EXPECT_EQ(expected, RenderRows(BuildExplorerContextMenuRows(EExplorerResourceKind::Folder, true)));
}

TEST(ExplorerContextMenuModel, PermanentDeletionReplacesTrashDeletionAtTheSameSlot)
{
	for (const auto kind : { EExplorerResourceKind::File, EExplorerResourceKind::Folder }) {
		const auto withTrash = RenderRows(BuildExplorerContextMenuRows(kind, true));
		const auto withoutTrash = RenderRows(BuildExplorerContextMenuRows(kind, false));

		// The two menus are identical except for the one deletion row: upstream
		// registers `moveFileToTrash` and `deleteFile` as separate items on
		// `ExplorerResourceMoveableToTrash`, both at `7_modification` order 20.
		ASSERT_EQ(withTrash.size(), withoutTrash.size());
		EXPECT_EQ("moveFileToTrash", withTrash.back());
		EXPECT_EQ("deleteFile", withoutTrash.back());
		for (std::size_t index = 0; index + 1 < withTrash.size(); ++index) {
			EXPECT_EQ(withTrash[index], withoutTrash[index]) << index;
		}
		for (const auto& rendered : withTrash) EXPECT_NE("deleteFile", rendered);
		for (const auto& rendered : withoutTrash) EXPECT_NE("moveFileToTrash", rendered);
	}
}

TEST(ExplorerContextMenuModel, WorkspaceRootOffersCreationAndPathsButNoRenameOrDeletion)
{
	// Upstream's rename and both deletion items carry
	// `ExplorerRootContext.toNegated()`, so the root's `7_modification` group is
	// empty and the menu ends after `6_copypath` with no trailing separator.
	const std::vector<std::string> expected = {
		"explorer.newFile",
		"explorer.newFolder",
		"revealFileInOS",
		kSeparatorMarker,
		"copyFilePath",
		"copyRelativeFilePath",
	};
	for (const bool moveableToTrash : { true, false }) {
		EXPECT_EQ(expected,
			RenderRows(BuildExplorerContextMenuRows(EExplorerResourceKind::WorkspaceRoot, moveableToTrash)))
			<< moveableToTrash;
	}
}

TEST(ExplorerContextMenuModel, EveryCommandRowResolvesInTheRegisteredExplorerBatch)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);

	// The model's ID constants and the registry's registrations must be the
	// same eight upstream commands; a row naming an unregistered command would
	// render a menu item no dispatch can honor.
	for (const auto kind : kEveryKind) {
		for (const bool moveableToTrash : { true, false }) {
			for (const auto& row : BuildExplorerContextMenuRows(kind, moveableToTrash)) {
				if (row.Kind() != EExplorerContextMenuRowKind::Command) continue;
				const std::string commandId(row.CommandId());
				EXPECT_TRUE(registry.Find(commandId).has_value()) << commandId;
				const auto menu = registry.ResolveSurface(
					EWorkbenchCommandSurface::Menu, commandId + ".menu");
				ASSERT_TRUE(menu.has_value()) << commandId;
				EXPECT_EQ(commandId, menu->commandId) << commandId;
			}
		}
	}
}

TEST(ExplorerContextMenuModel, SeparatorsOnlySitBetweenCommandsAndOnlyCommandsCarryIds)
{
	for (const auto kind : kEveryKind) {
		for (const bool moveableToTrash : { true, false }) {
			const auto rows = BuildExplorerContextMenuRows(kind, moveableToTrash);
			ASSERT_FALSE(rows.empty());
			EXPECT_EQ(EExplorerContextMenuRowKind::Command, rows.front().Kind());
			EXPECT_EQ(EExplorerContextMenuRowKind::Command, rows.back().Kind());
			for (std::size_t index = 0; index < rows.size(); ++index) {
				const bool isSeparator = rows[index].Kind() == EExplorerContextMenuRowKind::Separator;
				EXPECT_EQ(isSeparator, rows[index].CommandId().empty()) << index;
				if (isSeparator && index + 1 < rows.size()) {
					EXPECT_NE(EExplorerContextMenuRowKind::Separator, rows[index + 1].Kind()) << index;
				}
			}
		}
	}
}
