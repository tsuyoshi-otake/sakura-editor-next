/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/explorer/ExplorerResourcePath.h"

#include <string>

//!
//! @brief Label coverage for `copyRelativeFilePath` against one workspace
//! root.
//!
//! `BuildExplorerRelativePathLabel` must reproduce upstream VS Code's
//! `ILabelService.getUriLabel({ relative: true })` shape for a single-root
//! workspace (labelService.ts `doGetUriLabel`): the root itself is the empty
//! label, a resource under the root keeps the platform separators of its
//! remainder, and a resource outside the root keeps its absolute path.
//!

using workbench::explorer::BuildExplorerRelativePathLabel;
using workbench::explorer::IsValidExplorerEntryName;

TEST(ExplorerResourcePath, ResourceUnderTheRootDropsTheRootPrefix)
{
	EXPECT_EQ(L"notes.txt",
		BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(C:\workspace\notes.txt)"));
	EXPECT_EQ(LR"(src\lib\a.cpp)",
		BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(C:\workspace\src\lib\a.cpp)"));
}

TEST(ExplorerResourcePath, TheRootItselfLabelsAsTheEmptyString)
{
	// Upstream's `isEqual(folder.uri, resource)` branch: in a single-root
	// workspace no root-name prefix exists, so the root's own label is empty.
	EXPECT_EQ(L"", BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(C:\workspace)"));
	EXPECT_EQ(L"", BuildExplorerRelativePathLabel(LR"(C:\workspace\)", LR"(C:\workspace)"));
	EXPECT_EQ(L"", BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(C:\workspace\)"));
}

TEST(ExplorerResourcePath, ResourceOutsideTheRootKeepsItsAbsolutePath)
{
	EXPECT_EQ(LR"(D:\elsewhere\notes.txt)",
		BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(D:\elsewhere\notes.txt)"));
	// A sibling whose name merely extends the root's last segment is not
	// under the root.
	EXPECT_EQ(LR"(C:\workspace2\notes.txt)",
		BuildExplorerRelativePathLabel(LR"(C:\workspace)", LR"(C:\workspace2\notes.txt)"));
}

TEST(ExplorerResourcePath, ComparisonFoldsCaseAndSeparatorFlavor)
{
	EXPECT_EQ(L"notes.txt",
		BuildExplorerRelativePathLabel(LR"(c:\WORKSPACE)", LR"(C:\workspace\notes.txt)"));
	EXPECT_EQ(L"notes.txt",
		BuildExplorerRelativePathLabel(L"C:/workspace", LR"(C:\workspace\notes.txt)"));
}

TEST(ExplorerResourcePath, DriveRootKeepsItsMeaningAfterSeparatorTrimming)
{
	EXPECT_EQ(L"notes.txt", BuildExplorerRelativePathLabel(LR"(C:\)", LR"(C:\notes.txt)"));
	EXPECT_EQ(L"", BuildExplorerRelativePathLabel(LR"(C:\)", LR"(C:\)"));
}

TEST(ExplorerResourcePath, DegenerateRootFailsTowardTheAbsolutePath)
{
	EXPECT_EQ(LR"(C:\workspace\notes.txt)",
		BuildExplorerRelativePathLabel(L"", LR"(C:\workspace\notes.txt)"));
	EXPECT_EQ(LR"(C:\workspace\notes.txt)",
		BuildExplorerRelativePathLabel(LR"(\)", LR"(C:\workspace\notes.txt)"));
}

//! Transcription of upstream `isValidBasename` (extpath.ts, Windows flavor):
//! the accepted and rejected cases below are upstream's own rules, not a
//! restatement of the implementation.
TEST(ExplorerResourcePath, OrdinaryEntryNamesAreValid)
{
	EXPECT_TRUE(IsValidExplorerEntryName(L"notes.txt"));
	EXPECT_TRUE(IsValidExplorerEntryName(L"src"));
	EXPECT_TRUE(IsValidExplorerEntryName(L".gitignore"));
	// Non-ASCII names are ordinary names (the source stays ASCII per the
	// encoding gate, so the code points are spelled as escapes).
	EXPECT_TRUE(IsValidExplorerEntryName(L"\u8CC7\u6599.md"));
	EXPECT_TRUE(IsValidExplorerEntryName(L"comet"));
	EXPECT_TRUE(IsValidExplorerEntryName(L"com10"));
	EXPECT_TRUE(IsValidExplorerEntryName(L"console.log"));
}

TEST(ExplorerResourcePath, EmptyWhitespaceAndReservedDotNamesAreInvalid)
{
	EXPECT_FALSE(IsValidExplorerEntryName(L""));
	EXPECT_FALSE(IsValidExplorerEntryName(L"   "));
	EXPECT_FALSE(IsValidExplorerEntryName(L"."));
	EXPECT_FALSE(IsValidExplorerEntryName(L".."));
	EXPECT_FALSE(IsValidExplorerEntryName(L" leading"));
	EXPECT_FALSE(IsValidExplorerEntryName(L"trailing "));
	EXPECT_FALSE(IsValidExplorerEntryName(L"trailing."));
	EXPECT_FALSE(IsValidExplorerEntryName(std::wstring(256, L'a')));
}

TEST(ExplorerResourcePath, WindowsInvalidCharactersAndDeviceNamesAreInvalid)
{
	for (const auto* name : { L"a\\b", L"a/b", L"a:b", L"a*b", L"a?b", L"a\"b", L"a<b", L"a>b", L"a|b" }) {
		EXPECT_FALSE(IsValidExplorerEntryName(name)) << name;
	}
	for (const auto* name : { L"CON", L"con", L"PRN", L"AUX", L"NUL", L"clock$", L"COM1", L"lpt9" }) {
		EXPECT_FALSE(IsValidExplorerEntryName(name)) << name;
	}
}
