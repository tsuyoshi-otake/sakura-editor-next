/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

//! The exact contract between this editor and the Inno Setup package it was
//! installed from. Pure string building: nothing here starts a process, so the
//! command line a real update would run is the command line the tests assert.
namespace update {

//! Everything the silent reinstall needs to know.
struct InstallerInvocation final {
	//! The staged `sakura_install<version>-x64.exe`.
	std::wstring installerPath;
	//! `InstallLocation` from the uninstall key. Empty means "let Setup choose",
	//! which is refused below rather than silently installing somewhere else.
	std::wstring installDirectory;
	//! Where Setup writes its own log, so a failed update can be explained after
	//! the fact instead of only being observed as "it did not update".
	std::wstring logPath;
	//! Whether Setup should start the editor again when it finishes. This is the
	//! whole "restart and come back" behaviour; see `ShouldRelaunchAfterUpdate`
	//! in `installer/sakura-common.iss`.
	bool relaunchAfterInstall = true;

	[[nodiscard]] bool operator==(const InstallerInvocation&) const = default;
};

//! The switch that `installer/sakura-common.iss` reads through
//! `{param:UPDATERELAUNCH|0}`. Named here so the two sides cannot drift.
inline constexpr std::wstring_view kUpdateRelaunchSwitch = L"/UPDATERELAUNCH=1";

//! The argument vector, in the order Setup receives it.
//!
//! `/CURRENTUSER` is deliberately absent: the package sets
//! `PrivilegesRequired=lowest` and does **not** allow a command-line override,
//! so Setup rejects that switch outright. `/NOICONS` is absent for a different
//! reason — it would *remove* the Start Menu folder the user already has, and a
//! silent reinstall must preserve their choices, which Inno restores from the
//! previous run on its own.
[[nodiscard]] std::optional<std::vector<std::wstring>> BuildInstallerArguments(const InstallerInvocation& invocation);

//! The same arguments as one `CreateProcessW` command line, with the installer
//! path as argv[0]. Returns nothing for the same refusals as above.
[[nodiscard]] std::optional<std::wstring> BuildInstallerCommandLine(const InstallerInvocation& invocation);

//! Quotes one argument so `CommandLineToArgvW` reproduces it exactly, including
//! the trailing-backslash-run rule that would otherwise escape the closing
//! quote of a directory path.
[[nodiscard]] std::wstring QuoteInstallerArgument(std::wstring_view value);

} // namespace update
