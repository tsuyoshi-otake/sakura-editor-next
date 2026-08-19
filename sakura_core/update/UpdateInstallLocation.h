/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "update/IUpdateService.h"

namespace update {

//! The Inno Setup uninstall subkey for `AppId=sakura editor`
//! (`installer/sakura-common.iss`). Inno appends `_is1` to the AppId.
inline constexpr std::wstring_view kUninstallSubKey =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\sakura editor_is1";

//! Injected so the classification can be exercised without a filesystem.
using UpdateFileExistsPredicate = std::function<bool(const std::filesystem::path&)>;

//! Decides whether this build can install an update over an installation, and
//! which directory Setup has to write to.
//!
//! The primary signal is the one real VS Code uses: an Inno Setup installation
//! always has `unins000.exe` beside the program, so the question is answered
//! locally and never depends on a registry key being present, readable, or
//! current.
class UpdateInstallLocation final : public IUpdateInstallLocation {
public:
	//! `executablePath` is injected rather than read from `GetModuleFileName`, so
	//! the classification can be exercised without moving the test binary.
	explicit UpdateInstallLocation(std::filesystem::path executablePath);

	//! The running executable's full path, or an empty path when it cannot be
	//! resolved -- which resolves to `Archive`, never to a guess.
	[[nodiscard]] static std::filesystem::path CurrentExecutablePath();

	[[nodiscard]] UpdateInstallTarget Resolve() override;

private:
	std::filesystem::path m_executablePath;
};

//! `HKCU` then `HKLM` under `kUninstallSubKey`, both opened with
//! `KEY_WOW64_64KEY` because the package sets
//! `ArchitecturesInstallIn64BitMode`. Exposed for tests that need to reason
//! about a missing key separately from a missing installation.
[[nodiscard]] std::optional<std::wstring> ReadInstalledLocation();

//! Whether Inno's uninstaller sits beside `executablePath`, the same test
//! `isInnoSetupInstall` makes in VS Code's `win32UpdateType.ts`.
[[nodiscard]] bool IsInnoSetupInstall(
	const std::filesystem::path& executablePath, const UpdateFileExistsPredicate& fileExists);

//! The whole decision, with the registry read and both filesystem probes passed
//! in so it can be exercised without either.
//!
//! `Setup` when the running copy is an installation (its own directory is the
//! target), or when it is not but the uninstall key still records an
//! installation that exists (that directory is the target). `Archive` only when
//! no install directory can be determined at all, which is the one case left
//! that has to send the user to the release page.
[[nodiscard]] UpdateInstallTarget ResolveInstallTarget(
	const std::filesystem::path& executablePath,
	const std::optional<std::wstring>& installedLocation,
	const UpdateFileExistsPredicate& fileExists,
	const UpdateFileExistsPredicate& directoryExists);

} // namespace update
