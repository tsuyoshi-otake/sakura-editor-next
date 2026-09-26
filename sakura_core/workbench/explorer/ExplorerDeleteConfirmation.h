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

//! Localizable words for delete prompts. The model stays independent of native
//! resources; production callers inject the active language's text.
struct ExplorerDeleteConfirmationText final {
	std::wstring trashFileTemplate{ L"Are you sure you want to delete '{0}'?" };
	std::wstring trashFolderTemplate{ L"Are you sure you want to delete '{0}' and its contents?" };
	std::wstring permanentFileTemplate{ L"Are you sure you want to permanently delete '{0}'?" };
	std::wstring permanentFolderTemplate{ L"Are you sure you want to permanently delete '{0}' and its contents?" };
	std::wstring trashDetail{ L"You can restore this file from the Recycle Bin." };
	std::wstring permanentDetail{ L"This action is irreversible!" };
	std::wstring trashButton{ L"&Move to Recycle Bin" };
	std::wstring deleteButton{ L"&Delete" };
	std::wstring trashFailedInstruction{ L"Failed to delete using the Recycle Bin. Do you want to permanently delete instead?" };
};

[[nodiscard]] inline std::wstring FormatExplorerDeleteInstruction(
	std::wstring_view format, std::wstring_view resourceName)
{
	std::wstring result(format);
	constexpr std::wstring_view placeholder = L"{0}";
	const auto position = result.find(placeholder);
	if (position != std::wstring::npos) {
		result.replace(position, placeholder.size(), resourceName.data(), resourceName.size());
	}
	return result;
}

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
	std::wstring_view resourceName, bool isDirectory, bool useTrash,
	const ExplorerDeleteConfirmationText& text = {})
{
	ExplorerDeleteConfirmation confirmation;

	const std::wstring_view instructionTemplate = useTrash
		? (isDirectory ? text.trashFolderTemplate : text.trashFileTemplate)
		: (isDirectory ? text.permanentFolderTemplate : text.permanentFileTemplate);
	confirmation.instruction = FormatExplorerDeleteInstruction(instructionTemplate, resourceName);

	if (useTrash) {
		confirmation.detail = text.trashDetail;
		confirmation.primaryButton = text.trashButton;
		confirmation.isWarning = false;
	}
	else {
		confirmation.detail = text.permanentDetail;
		confirmation.primaryButton = text.deleteButton;
		confirmation.isWarning = true;
	}

	return confirmation;
}

//!
//! @brief Builds the follow-up prompt upstream shows when the Recycle Bin
//! delete itself failed (fileActions.ts `onBinError`): the user may retry as
//! a permanent delete or cancel.
//!
[[nodiscard]] inline ExplorerDeleteConfirmation BuildExplorerTrashFailedConfirmation(
	const ExplorerDeleteConfirmationText& text = {})
{
	ExplorerDeleteConfirmation confirmation;
	confirmation.instruction = text.trashFailedInstruction;
	confirmation.detail = text.permanentDetail;
	confirmation.primaryButton = text.deleteButton;
	confirmation.isWarning = true;
	return confirmation;
}

} // namespace workbench::explorer
