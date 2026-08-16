/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/explorer/ExplorerFileIcon.h"

using workbench::explorer::ResolveExplorerFileIconCodicon;

TEST(ExplorerFileIcon, FoldersAndRootsUseTheGenericFolderGlyphs)
{
	EXPECT_EQ(L"folder", ResolveExplorerFileIconCodicon(L"src", true, false, false));
	EXPECT_EQ(L"folder-opened", ResolveExplorerFileIconCodicon(L"src", true, true, false));
	EXPECT_EQ(L"root-folder", ResolveExplorerFileIconCodicon(L"workspace", true, false, true));
	EXPECT_EQ(L"root-folder-opened", ResolveExplorerFileIconCodicon(L"workspace", true, true, true));
}

TEST(ExplorerFileIcon, CommonExtensionsMapOntoDistinctCodicons)
{
	EXPECT_EQ(L"markdown", ResolveExplorerFileIconCodicon(L"README.md", false, false, false));
	EXPECT_EQ(L"json", ResolveExplorerFileIconCodicon(L"package.json", false, false, false));
	EXPECT_EQ(L"python", ResolveExplorerFileIconCodicon(L"main.py", false, false, false));
	EXPECT_EQ(L"ruby", ResolveExplorerFileIconCodicon(L"app.rb", false, false, false));
	EXPECT_EQ(L"file-code", ResolveExplorerFileIconCodicon(L"CExplorerTool.cpp", false, false, false));
	EXPECT_EQ(L"file-code", ResolveExplorerFileIconCodicon(L"types.d.ts", false, false, false));
	EXPECT_EQ(L"file-media", ResolveExplorerFileIconCodicon(L"logo.png", false, false, false));
	EXPECT_EQ(L"file-zip", ResolveExplorerFileIconCodicon(L"archive.zip", false, false, false));
	EXPECT_EQ(L"file-pdf", ResolveExplorerFileIconCodicon(L"spec.pdf", false, false, false));
	EXPECT_EQ(L"file-binary", ResolveExplorerFileIconCodicon(L"sakura.exe", false, false, false));
	EXPECT_EQ(L"terminal-powershell", ResolveExplorerFileIconCodicon(L"build.ps1", false, false, false));
	EXPECT_EQ(L"database", ResolveExplorerFileIconCodicon(L"schema.sql", false, false, false));
}

TEST(ExplorerFileIcon, MatchingIsCaseInsensitiveAndFallsBackToFile)
{
	EXPECT_EQ(L"markdown", ResolveExplorerFileIconCodicon(L"Notes.MD", false, false, false));
	EXPECT_EQ(L"json", ResolveExplorerFileIconCodicon(L"Package.JSON", false, false, false));
	EXPECT_EQ(L"file", ResolveExplorerFileIconCodicon(L"mystery.xyz", false, false, false));
	EXPECT_EQ(L"file", ResolveExplorerFileIconCodicon(L"noextension", false, false, false));
}
