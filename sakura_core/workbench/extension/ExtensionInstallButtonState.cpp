/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extension/ExtensionInstallButtonState.h"

namespace workbench::extension {

InstallButtonState ComputeInstallButtonState(
	bool hasExtension,
	const std::optional<std::wstring>& installedVersion,
	const std::wstring& currentVersion,
	bool hasInstallCallback) noexcept
{
	if (!hasExtension) return InstallButtonState{ InstallButtonAction::NoExtension, L"Install", false };

	const bool installed = installedVersion.has_value();
	const bool upToDate = installed && !currentVersion.empty() && *installedVersion == currentVersion;
	if (!installed) return InstallButtonState{ InstallButtonAction::Install, L"Install", hasInstallCallback };
	if (upToDate) return InstallButtonState{ InstallButtonAction::UpToDate, L"Installed", false };
	return InstallButtonState{ InstallButtonAction::Update, L"Update", hasInstallCallback };
}

} // namespace workbench::extension
