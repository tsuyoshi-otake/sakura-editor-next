/*! @file
 * @brief HWND-free wording model for the Explorer delete confirmations,
 * in VS Code's wording.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <string>
#include <string_view>

namespace workbench::explorer {

//! One modal delete-confirmation prompt.  The projection renders it with
//! `TaskDialogIndirect` as one custom primary button plus Cancel; the model
//! owns only the words and the severity so the dialog code never becomes a
//! second wording authority.
struct ExplorerDeleteConfirmation final {
	std::wstring instruction;
	std::wstring detail;
	//! Win32 mnemonic form: upstream's `&&` prefix collapses to one `&`.
	std::wstring primaryButton;
	//! True renders the warning icon, matching upstream's severity for the
	//! irreversible prompts; the trash prompt has no elevated severity.
	bool isWarning = false;

	[[nodiscard]] bool operator==(const ExplorerDeleteConfirmation&) const = default;
};

//!
//! @brief Builds the confirmation upstream VS Code shows before deleting one
//! Explorer resource (fileActions.ts `deleteFiles`, single-resource case).
//!
//! - Trash: "Are you sure you want to delete '{name}'?" - a folder appends
//!   " and its contents?" - with the Windows detail "You can restore this
//!   file from the Recycle Bin." and primary button "Move to Recycle Bin".
//! - Permanent: "Are you sure you want to permanently delete '{name}'?" -
//!   same folder variant - as a warning with primary button "Delete".  The
//!   detail is "This action is irreversible!" for files as well as folders:
//!   upstream's file-only "You can restore this file using the Undo
//!   command." presumes an undo capability this product does not have, and
//!   promising it would fake the capability.  Recorded divergence in this
//!   directory's `CLAUDE.md`, together with the omitted
//!   "Do not ask me again" checkbox.
//!
[[nodiscard]] inline ExplorerDeleteConfirmation BuildExplorerDeleteConfirmation(
	std::wstring_view resourceName, bool isDirectory, bool useTrash)
{
	ExplorerDeleteConfirmation confirmation;

	confirmation.instruction = useTrash
		? L"Are you sure you want to delete '"
		: L"Are you sure you want to permanently delete '";
	confirmation.instruction += resourceName;
	confirmation.instruction += isDirectory ? L"' and its contents?" : L"'?";

	if (useTrash) {
		confirmation.detail = L"You can restore this file from the Recycle Bin.";
		confirmation.primaryButton = L"&Move to Recycle Bin";
		confirmation.isWarning = false;
	}
	else {
		confirmation.detail = L"This action is irreversible!";
		confirmation.primaryButton = L"&Delete";
		confirmation.isWarning = true;
	}

	return confirmation;
}

//!
//! @brief Builds the follow-up prompt upstream shows when the Recycle Bin
//! delete itself failed (fileActions.ts `onBinError`): the user may retry as
//! a permanent delete or cancel.
//!
[[nodiscard]] inline ExplorerDeleteConfirmation BuildExplorerTrashFailedConfirmation()
{
	ExplorerDeleteConfirmation confirmation;
	confirmation.instruction =
		L"Failed to delete using the Recycle Bin. Do you want to permanently delete instead?";
	confirmation.detail = L"This action is irreversible!";
	confirmation.primaryButton = L"&Delete";
	confirmation.isWarning = true;
	return confirmation;
}

} // namespace workbench::explorer
