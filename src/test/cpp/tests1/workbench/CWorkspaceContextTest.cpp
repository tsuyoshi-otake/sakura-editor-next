/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/CWorkspaceContext.h"

namespace {

TEST(WorkspaceContext, ExplicitRootWinsOverSelectedFileAndCurrentDirectory)
{
	workbench::CWorkspaceContext context(L"C:\\fallback");
	context.SetSelectedFile(L"D:\\project\\source\\main.cpp");
	context.SetExplicitRoot(L"E:\\workspace\\");
	EXPECT_EQ(L"E:\\workspace", context.GetRoot());
}

TEST(WorkspaceContext, SelectedFileParentIsUsedWhenNoExplicitRootExists)
{
	workbench::CWorkspaceContext context(L"C:\\fallback");
	context.SetSelectedFile(L"D:\\project\\source\\main.cpp");
	EXPECT_EQ(L"D:\\project\\source", context.GetRoot());
	context.ClearSelectedFile();
	EXPECT_EQ(L"C:\\fallback", context.GetRoot());
}

TEST(WorkspaceContext, RootDriveAndUnqualifiedFilesHaveSafeParentHandling)
{
	EXPECT_EQ(L"C:\\", workbench::CWorkspaceContext::ParentDirectory(L"C:\\document.txt"));
	EXPECT_TRUE(workbench::CWorkspaceContext::ParentDirectory(L"untitled.txt").empty());
	EXPECT_EQ(L"\\", workbench::CWorkspaceContext::ParentDirectory(L"\\document.txt"));
}

TEST(WorkspaceContext, NewTerminalCapturesRootAndDoesNotFollowLaterFileSelection)
{
	workbench::CWorkspaceContext context(L"C:\\fallback");
	context.SetSelectedFile(L"D:\\first\\file.txt");
	const auto firstTerminalCwd = context.GetNewTerminalWorkingDirectory();
	context.SetSelectedFile(L"E:\\second\\file.txt");
	EXPECT_EQ(L"D:\\first", firstTerminalCwd);
	EXPECT_EQ(L"E:\\second", context.GetNewTerminalWorkingDirectory());
}

TEST(WorkspaceContext, PickedFolderBecomesTheWindowLocalRootForFutureTerminals)
{
	workbench::CWorkspaceContext context(L"C:\\fallback");
	context.SetSelectedFile(L"D:\\project\\source\\main.cpp");
	const auto existingTerminalCwd = context.GetNewTerminalWorkingDirectory();

	// This is the transition performed only after the native picker succeeds.
	// Existing terminal launch options retain their already captured CWD.
	context.SetExplicitRoot(L"E:\\picked workspace\\");
	EXPECT_EQ(L"D:\\project\\source", existingTerminalCwd);
	EXPECT_EQ(L"E:\\picked workspace", context.GetRoot());
	EXPECT_EQ(L"E:\\picked workspace", context.GetNewTerminalWorkingDirectory());
}

TEST(ExplorerRefreshCoordinator, GenerationCancelsEarlierEnumerationAndAllowsOneWorker)
{
	workbench::CExplorerRefreshCoordinator coordinator;
	const auto first = coordinator.RequestEnumeration();
	EXPECT_TRUE(coordinator.TryAcquireWorker(first));
	const auto second = coordinator.RequestEnumeration();
	EXPECT_FALSE(coordinator.IsCurrent(first));
	EXPECT_TRUE(coordinator.IsCurrent(second));
	EXPECT_FALSE(coordinator.TryAcquireWorker(second));
	coordinator.FinishWorker(first);
	EXPECT_TRUE(coordinator.TryAcquireWorker(second));
	coordinator.FinishWorker(second);
}

TEST(ExplorerRefreshCoordinator, DirectoryChangesAreDebouncedAndCoalesced)
{
	workbench::CExplorerRefreshCoordinator coordinator;
	(void)coordinator.RequestEnumeration();
	coordinator.NotifyDirectoryChange(1000);
	coordinator.NotifyDirectoryChange(1100);
	EXPECT_FALSE(coordinator.TakeDueRefresh(1249).has_value());
	const auto due = coordinator.TakeDueRefresh(1250);
	ASSERT_TRUE(due.has_value());
	EXPECT_TRUE(coordinator.IsCurrent(*due));
	EXPECT_FALSE(coordinator.TakeDueRefresh(2000).has_value());
}

TEST(ExplorerRefreshCoordinator, NeverAutomaticallyRecursesIntoJunctions)
{
	EXPECT_TRUE(workbench::CExplorerRefreshCoordinator::ShouldRecurseIntoEntry(true, false));
	EXPECT_FALSE(workbench::CExplorerRefreshCoordinator::ShouldRecurseIntoEntry(true, true));
	EXPECT_FALSE(workbench::CExplorerRefreshCoordinator::ShouldRecurseIntoEntry(false, false));
}

} // namespace
