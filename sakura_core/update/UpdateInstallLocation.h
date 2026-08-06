/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "update/IUpdateService.h"

namespace update {

//! The Inno Setup uninstall subkey for `AppId=sakura editor`
//! (`installer/sakura-common.iss`). Inno appends `_is1` to the AppId.
inline constexpr std::wstring_view kUninstallSubKey =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\sakura editor_is1";

//! Reads `InstallLocation` from the uninstall key and checks that the running
//! executable actually lives under it.
//!
//! `PrivilegesRequired=lowest` puts the key under `HKEY_CURRENT_USER` for the
//! normal install, but an elevated install writes `HKEY_LOCAL_MACHINE` instead,
//! so both are consulted — per-user first, because that is the installation this
//! process would be allowed to overwrite.
//!
//! A build run from the output tree, or a copy someone unzipped somewhere else,
//! deliberately resolves to `EUpdateType::Archive`. Reinstalling over an
//! unrelated directory because a registry key happened to exist is exactly the
//! kind of "close enough" behavior this must not have.
class UpdateInstallLocation final : public IUpdateInstallLocation {
public:
	//! `executablePath` is injected rather than read from `GetModuleFileName`, so
	//! the containment rule can be exercised without moving the test binary.
	explicit UpdateInstallLocation(std::filesystem::path executablePath);

	//! The running executable's full path, or an empty path when it cannot be
	//! resolved — which resolves to `Archive`, never to a guess.
	[[nodiscard]] static std::filesystem::path CurrentExecutablePath();

	[[nodiscard]] UpdateInstallTarget Resolve() override;

private:
	std::filesystem::path m_executablePath;
};

//! `HKCU` then `HKLM` under `kUninstallSubKey`, both opened with
//! `KEY_WOW64_64KEY` because the package sets
//! `ArchitecturesInstallIn64BitMode`. Exposed for tests that need to reason
//! about a missing key separately from a missing containment.
[[nodiscard]] std::optional<std::wstring> ReadInstalledLocation();

//! Whether `executablePath` is `installDirectory` itself or below it, compared
//! as normalized paths rather than as strings.
[[nodiscard]] bool IsExecutableWithinInstallDirectory(
	const std::filesystem::path& executablePath, std::wstring_view installDirectory);

} // namespace update
