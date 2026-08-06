/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string_view>

#include "update/IUpdateService.h"

namespace update {

//! Whether a URL is safe to hand to the shell.
//!
//! `ShellExecute` will happily run a local executable, a `file:` path, or a
//! registered protocol handler, and the string in question arrived over the
//! network in a release feed. Only `https://` is accepted, so a hostile or
//! merely wrong feed cannot turn "open the release page" into "run this".
[[nodiscard]] bool IsLaunchableReleaseUrl(std::wstring_view url) noexcept;

//! The only component in the update path that starts a process.
class Win32UpdateLauncher final : public IUpdateLauncher {
public:
	//! Starts the staged installer fully detached, so it survives this process
	//! exiting a moment later — which is the whole point: the installer must
	//! outlive the editor it is replacing.
	[[nodiscard]] bool LaunchInstaller(const InstallerInvocation& invocation) override;

	[[nodiscard]] bool OpenReleasePage(std::wstring_view url) override;
};

} // namespace update
