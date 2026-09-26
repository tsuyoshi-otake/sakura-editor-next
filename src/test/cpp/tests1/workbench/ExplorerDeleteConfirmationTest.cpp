/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/explorer/ExplorerDeleteConfirmation.h"

#include <string>

//!
//! @brief Wording coverage for the Explorer delete confirmations.
//!
//! `BuildExplorerDeleteConfirmation` must reproduce upstream VS Code's
//! single-resource `deleteFiles` prompts (fileActions.ts): the trash prompt
//! with the Recycle Bin detail and no elevated severity, the permanent
//! prompt as a warning, the folder "and its contents" variant of each, and
//! the bin-failure fallback prompt.  The expected strings below are literal
//! transcriptions of upstream's wording on purpose - the test must not be
//! derived from the same builder it checks.
//!

using workbench::explorer::BuildExplorerDeleteConfirmation;
using workbench::explorer::BuildExplorerTrashFailedConfirmation;

TEST(ExplorerDeleteConfirmation, TrashFilePromptMatchesUpstreamWording)
{
	const auto confirmation = BuildExplorerDeleteConfirmation(L"notes.txt", false, true);
	EXPECT_EQ(L"Are you sure you want to delete 'notes.txt'?", confirmation.instruction);
	EXPECT_EQ(L"You can restore this file from the Recycle Bin.", confirmation.detail);
	EXPECT_EQ(L"&Move to Recycle Bin", confirmation.primaryButton);
	EXPECT_FALSE(confirmation.isWarning);
}

TEST(ExplorerDeleteConfirmation, TrashFolderPromptAsksAboutContents)
{
	const auto confirmation = BuildExplorerDeleteConfirmation(L"src", true, true);
	EXPECT_EQ(L"Are you sure you want to delete 'src' and its contents?", confirmation.instruction);
	EXPECT_EQ(L"You can restore this file from the Recycle Bin.", confirmation.detail);
	EXPECT_EQ(L"&Move to Recycle Bin", confirmation.primaryButton);
	EXPECT_FALSE(confirmation.isWarning);
}

TEST(ExplorerDeleteConfirmation, PermanentFilePromptIsAnIrreversibleWarning)
{
	const auto confirmation = BuildExplorerDeleteConfirmation(L"notes.txt", false, false);
	EXPECT_EQ(L"Are you sure you want to permanently delete 'notes.txt'?", confirmation.instruction);
	// Recorded divergence: upstream's file-only "You can restore this file
	// using the Undo command." presumes an undo capability this product does
	// not have, so the irreversible detail covers files too.
	EXPECT_EQ(L"This action is irreversible!", confirmation.detail);
	EXPECT_EQ(L"&Delete", confirmation.primaryButton);
	EXPECT_TRUE(confirmation.isWarning);
}

TEST(ExplorerDeleteConfirmation, PermanentFolderPromptAsksAboutContents)
{
	const auto confirmation = BuildExplorerDeleteConfirmation(L"src", true, false);
	EXPECT_EQ(L"Are you sure you want to permanently delete 'src' and its contents?",
		confirmation.instruction);
	EXPECT_EQ(L"This action is irreversible!", confirmation.detail);
	EXPECT_EQ(L"&Delete", confirmation.primaryButton);
	EXPECT_TRUE(confirmation.isWarning);
}

TEST(ExplorerDeleteConfirmation, TrashFailureFallsBackToThePermanentPrompt)
{
	const auto confirmation = BuildExplorerTrashFailedConfirmation();
	EXPECT_EQ(L"Failed to delete using the Recycle Bin. Do you want to permanently delete instead?",
		confirmation.instruction);
	EXPECT_EQ(L"This action is irreversible!", confirmation.detail);
	EXPECT_EQ(L"&Delete", confirmation.primaryButton);
	EXPECT_TRUE(confirmation.isWarning);
}

TEST(ExplorerDeleteConfirmation, InjectedLanguageTextIsUsedWithoutChangingTheModelBoundary)
{
	workbench::explorer::ExplorerDeleteConfirmationText text;
	text.trashFileTemplate = L"Localized file: {0}";
	text.trashFolderTemplate = L"Localized folder: {0}";
	text.trashDetail = L"Localized detail";
	text.trashButton = L"Localized button";
	const auto confirmation = BuildExplorerDeleteConfirmation(L"memo.txt", false, true, text);
	EXPECT_EQ(L"Localized file: memo.txt", confirmation.instruction);
	EXPECT_EQ(L"Localized detail", confirmation.detail);
	EXPECT_EQ(L"Localized button", confirmation.primaryButton);
	EXPECT_FALSE(confirmation.isWarning);
	// User filenames can themselves contain template or printf characters.
	EXPECT_EQ(L"Localized folder: a{0}%s", BuildExplorerDeleteConfirmation(L"a{0}%s", true, true, text).instruction);
	text.permanentFileTemplate = L"{0}: localized permanent file";
	text.permanentFolderTemplate = L"{0}: localized permanent folder";
	text.permanentDetail = L"Localized warning";
	text.deleteButton = L"Localized delete";
	text.trashFailedInstruction = L"Localized trash failure";
	EXPECT_EQ(L"memo.txt: localized permanent file", BuildExplorerDeleteConfirmation(L"memo.txt", false, false, text).instruction);
	const auto folder = BuildExplorerDeleteConfirmation(L"folder", true, false, text);
	EXPECT_EQ(L"folder: localized permanent folder", folder.instruction);
	EXPECT_EQ(L"Localized warning", folder.detail);
	EXPECT_EQ(L"Localized delete", folder.primaryButton);
	EXPECT_TRUE(folder.isWarning);
	const auto failure = BuildExplorerTrashFailedConfirmation(text);
	EXPECT_EQ(L"Localized trash failure", failure.instruction);
	EXPECT_EQ(L"Localized warning", failure.detail);
	EXPECT_EQ(L"Localized delete", failure.primaryButton);
	EXPECT_TRUE(failure.isWarning);
}
