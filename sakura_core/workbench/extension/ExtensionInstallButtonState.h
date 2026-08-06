/*! @file
	@brief Pure Install/Update/Installed decision for the extension detail action button
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <optional>
#include <string>

namespace workbench::extension {

enum class InstallButtonAction {
	NoExtension, //!< No extension is currently shown; the button has nothing to act on.
	Install,     //!< Not installed.
	Update,      //!< Installed at a version different from the shown extension's version.
	UpToDate,    //!< Installed at the shown extension's exact version.
};

struct InstallButtonState {
	InstallButtonAction action = InstallButtonAction::NoExtension;
	std::wstring label;
	bool enabled = false;
};

//! Pure decision for the extension detail surface's single action button. This
//! surface has no uninstall callback, so UpToDate intentionally disables the
//! button rather than repurposing "Install" text for an action it cannot
//! perform (uninstall). An empty `currentVersion` never counts as up to date,
//! since an extension with no known version cannot be compared for equality.
[[nodiscard]] InstallButtonState ComputeInstallButtonState(
	bool hasExtension,
	const std::optional<std::wstring>& installedVersion,
	const std::wstring& currentVersion,
	bool hasInstallCallback) noexcept;

} // namespace workbench::extension
