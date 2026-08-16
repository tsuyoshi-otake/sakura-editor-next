/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/commands/ExplorerCommandArguments.h"
#include "workbench/commands/ExplorerCommandIds.h"
#include "workbench/commands/WorkbenchCommandRegistry.h"

#include <string>
#include <string_view>

//!
//! @brief Registry coverage for the Files Explorer's file-operation commands.
//!
//! `RegisterExplorerCommands` registers upstream's eight resource-scoped
//! commands (`fileActions.ts` / `fileActions.contribution.ts` /
//! `fileConstants.ts` and the electron-browser `revealFileInOS`
//! contribution) as one atomic batch. This file is a distinct suite from
//! `WorkbenchCommandRegistryTest.cpp` for the same reason
//! `GitInitCloneCommandRegistryTest.cpp` is: the batch's own contract gets a
//! home of its own. The `ExplorerResourceArguments` wire payload the batch's
//! executors receive is covered here too, because the payload contract and the
//! commands that carry it change together.
//!

using workbench::commands::BuildExplorerResourceArguments;
using workbench::commands::EWorkbenchCommandExecutionStatus;
using workbench::commands::EWorkbenchCommandRegistrationStatus;
using workbench::commands::EWorkbenchCommandSurface;
using workbench::commands::ExplorerResourceArguments;
using workbench::commands::kMaximumExplorerCommandStringLength;
using workbench::commands::kCollapseExplorerFoldersCommandId;
using workbench::commands::kCreateFileFromExplorerCommandId;
using workbench::commands::kCreateFolderFromExplorerCommandId;
using workbench::commands::kRefreshFilesExplorerCommandId;
using workbench::commands::ParseExplorerResourceArguments;
using workbench::commands::WorkbenchCommandExecutionResult;
using workbench::commands::WorkbenchCommandRegistry;
using workbench::commands::WorkbenchContextKeySnapshot;

namespace {

WorkbenchContextKeySnapshot WorkbenchReadyContext()
{
	WorkbenchContextKeySnapshot context;
	context.values.emplace("workbenchReady", true);
	return context;
}

WorkbenchCommandExecutionResult Succeeded()
{
	return { EWorkbenchCommandExecutionStatus::Succeeded, {} };
}

//! Every command the batch registers, in upstream's own stable IDs.
constexpr const char* kExplorerCommandIds[] = {
	"explorer.newFile",
	"explorer.newFolder",
	"renameFile",
	"moveFileToTrash",
	"deleteFile",
	"copyFilePath",
	"copyRelativeFilePath",
	"revealFileInOS",
};

} // namespace

TEST(ExplorerCommandRegistry, AllEightCommandsCarryUpstreamIdsTitlesAndTheWorkbenchReadyClause)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);

	// Titles are upstream's context-menu labels: `NEW_FILE_LABEL`,
	// `NEW_FOLDER_LABEL`, `TRIGGER_RENAME_LABEL`, `MOVE_FILE_TO_TRASH_LABEL`,
	// the `deleteFile` menu title, the `copyFilePath`/`copyRelativeFilePath`
	// command titles, and the Windows branch of `REVEAL_IN_OS_LABEL`.
	const std::pair<const char*, const char*> expectations[] = {
		{ "explorer.newFile", "New File..." },
		{ "explorer.newFolder", "New Folder..." },
		{ "renameFile", "Rename..." },
		{ "moveFileToTrash", "Delete" },
		{ "deleteFile", "Delete Permanently" },
		{ "copyFilePath", "Copy Path" },
		{ "copyRelativeFilePath", "Copy Relative Path" },
		{ "revealFileInOS", "Reveal in File Explorer" },
	};
	for (const auto& [commandId, title] : expectations) {
		const auto descriptor = registry.Find(commandId);
		ASSERT_TRUE(descriptor.has_value()) << commandId;
		EXPECT_EQ(title, descriptor->title) << commandId;
		// Upstream gates these on Explorer-focus context keys this native
		// provider does not publish yet, so the clause is `workbenchReady`
		// alone - the `MakeGitAlwaysAvailableDescriptor` precedent.
		EXPECT_EQ("workbenchReady", descriptor->whenClause) << commandId;
		EXPECT_EQ("workbenchReady", descriptor->enablementClause) << commandId;
	}
}

TEST(ExplorerCommandRegistry, ExplorerViewTitleActionsUseUpstreamIdsAndViewTitleSurface)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);
	const std::pair<std::string_view, std::string_view> expectations[] = {
		{ kCreateFileFromExplorerCommandId, "New File..." },
		{ kCreateFolderFromExplorerCommandId, "New Folder..." },
		{ kRefreshFilesExplorerCommandId, "Refresh Explorer" },
		{ kCollapseExplorerFoldersCommandId, "Collapse Folders in Explorer" },
	};
	for (const auto& [commandId, title] : expectations) {
		const auto descriptor = registry.Find(commandId);
		ASSERT_TRUE(descriptor.has_value()) << commandId;
		EXPECT_EQ(title, descriptor->title) << commandId;
		EXPECT_EQ("workbenchReady", descriptor->whenClause) << commandId;
		EXPECT_EQ("workbenchReady", descriptor->enablementClause) << commandId;
		const auto viewTitle = registry.ResolveSurface(
			EWorkbenchCommandSurface::ViewTitle, std::string(commandId) + ".viewTitle");
		ASSERT_TRUE(viewTitle.has_value()) << commandId;
		EXPECT_EQ(commandId, viewTitle->commandId) << commandId;
		EXPECT_FALSE(registry.ResolveSurface(
			EWorkbenchCommandSurface::Menu, std::string(commandId) + ".menu").has_value());
	}
}

TEST(ExplorerCommandRegistry, SurfaceBindingsMatchUpstreamsThreeShapes)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);

	// Every command is an Explorer context-menu entry.
	for (const auto* commandId : kExplorerCommandIds) {
		const auto menu = registry.ResolveSurface(
			EWorkbenchCommandSurface::Menu, std::string(commandId) + ".menu");
		ASSERT_TRUE(menu.has_value()) << commandId;
		EXPECT_EQ(commandId, menu->commandId) << commandId;
	}

	// `explorer.newFile`/`explorer.newFolder` have no default keybinding and no
	// Command Palette entry upstream; the other six carry a keybinding slot.
	for (const auto* commandId : { "explorer.newFile", "explorer.newFolder" }) {
		EXPECT_FALSE(registry.ResolveSurface(
			EWorkbenchCommandSurface::Keybinding, std::string(commandId) + ".key").has_value())
			<< commandId;
		EXPECT_FALSE(registry.ResolveSurface(
			EWorkbenchCommandSurface::CommandPalette, std::string(commandId) + ".palette").has_value())
			<< commandId;
	}
	for (const auto* commandId :
		{ "renameFile", "moveFileToTrash", "deleteFile", "copyFilePath", "copyRelativeFilePath",
			"revealFileInOS" }) {
		const auto key = registry.ResolveSurface(
			EWorkbenchCommandSurface::Keybinding, std::string(commandId) + ".key");
		ASSERT_TRUE(key.has_value()) << commandId;
		EXPECT_EQ(commandId, key->commandId) << commandId;
	}

	// Only the three path/reveal commands are Command Palette entries upstream;
	// a selection-scoped command with no selection has no operand to act on.
	for (const auto* commandId : { "renameFile", "moveFileToTrash", "deleteFile" }) {
		EXPECT_FALSE(registry.ResolveSurface(
			EWorkbenchCommandSurface::CommandPalette, std::string(commandId) + ".palette").has_value())
			<< commandId;
	}
	for (const auto* commandId : { "copyFilePath", "copyRelativeFilePath", "revealFileInOS" }) {
		const auto palette = registry.ResolveSurface(
			EWorkbenchCommandSurface::CommandPalette, std::string(commandId) + ".palette");
		ASSERT_TRUE(palette.has_value()) << commandId;
		EXPECT_EQ(commandId, palette->commandId) << commandId;
	}
}

TEST(ExplorerCommandRegistry, BoundExecutorReceivesTheResourcePayloadVerbatim)
{
	WorkbenchCommandRegistry registry;
	std::string received;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({
		.renameFile = [&received](std::string_view arguments) { received = arguments; return Succeeded(); },
	}).status);

	const auto payload = BuildExplorerResourceArguments({ L"file:///C:/work/a.txt" });
	EXPECT_EQ(EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute("renameFile", WorkbenchReadyContext(), payload).status);
	EXPECT_EQ(payload, received);
}

TEST(ExplorerCommandRegistry, UnboundCommandsAreUnsupportedRatherThanASilentNoOp)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);

	const auto context = WorkbenchReadyContext();
	for (const auto* commandId : kExplorerCommandIds) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::Unsupported,
			registry.Execute(commandId, context).status) << commandId;
	}
}

TEST(ExplorerCommandRegistry, EveryCommandIsNotApplicableBeforeTheWorkbenchIsReady)
{
	WorkbenchCommandRegistry registry;
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({
		.newFile = [](std::string_view) { return Succeeded(); },
		.moveFileToTrash = [](std::string_view) { return Succeeded(); },
	}).status);

	const WorkbenchContextKeySnapshot notReady;
	for (const auto* commandId : kExplorerCommandIds) {
		EXPECT_EQ(EWorkbenchCommandExecutionStatus::NotApplicable,
			registry.Execute(commandId, notReady).status) << commandId;
	}
}

TEST(ExplorerCommandRegistry, TheBatchIsAtomicWithTheOtherFeatureBatchesStillRegistrable)
{
	WorkbenchCommandRegistry registry;
	// One atomic call registers all eight; the Git batch must still register
	// beside it, proving no surface-slot or command-ID conflict between the
	// feature batches.
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterExplorerCommands({}).status);
	ASSERT_EQ(EWorkbenchCommandRegistrationStatus::Succeeded, registry.RegisterGitCommands({}).status);
	for (const auto* commandId : kExplorerCommandIds) {
		EXPECT_TRUE(registry.Find(commandId).has_value()) << commandId;
	}
	EXPECT_TRUE(registry.Find("git.init").has_value());
}

TEST(ExplorerCommandArguments, BuildAndParseRoundTripOneResourceUri)
{
	const ExplorerResourceArguments arguments{ L"file:///C:/work/%E8%B3%87%E6%96%99/a.txt" };
	const auto json = BuildExplorerResourceArguments(arguments);
	const auto parsed = ParseExplorerResourceArguments(json);
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(arguments, *parsed);
}

TEST(ExplorerCommandArguments, ParserFailsClosedOnEverythingTheContractDoesNotDefine)
{
	// Not an array / empty array / wrong element type.
	EXPECT_FALSE(ParseExplorerResourceArguments("").has_value());
	EXPECT_FALSE(ParseExplorerResourceArguments("{}").has_value());
	EXPECT_FALSE(ParseExplorerResourceArguments("[]").has_value());
	EXPECT_FALSE(ParseExplorerResourceArguments("[42]").has_value());
	EXPECT_FALSE(ParseExplorerResourceArguments("[null]").has_value());
	// An empty resource is a command with no operand.
	EXPECT_FALSE(ParseExplorerResourceArguments(R"([""])").has_value());
	// Upstream's multi-select second argument is deliberately not part of the
	// contract; it must be rejected, never accepted-and-ignored.
	EXPECT_FALSE(ParseExplorerResourceArguments(
		R"(["file:///C:/a.txt",["file:///C:/b.txt"]])").has_value());
	// Trailing bytes after the closing bracket.
	EXPECT_FALSE(ParseExplorerResourceArguments(R"(["file:///C:/a.txt"] )" "x").has_value());
	// Over-long payloads must fail rather than allocate without limit.
	const std::string overlong =
		"[\"" + std::string(kMaximumExplorerCommandStringLength + 1, 'a') + "\"]";
	EXPECT_FALSE(ParseExplorerResourceArguments(overlong).has_value());
}

TEST(ExplorerCommandArguments, WhitespaceInsideTheArrayIsJsonNotAnError)
{
	const auto parsed = ParseExplorerResourceArguments("[ \"file:///C:/a.txt\" ]");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(L"file:///C:/a.txt", parsed->resourceUri);
}
