/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateInstallLocation.h"

#include <system_error>
#include <vector>

namespace update {
namespace {

constexpr std::wstring_view kInstallLocationValue = L"InstallLocation";

//! Long enough for any real path, short enough that a corrupt registry value
//! cannot make this allocate without bound.
constexpr DWORD kMaximumValueBytes = 8 * 1024;

std::optional<std::wstring> ReadStringValue(HKEY root, std::wstring_view subKey, std::wstring_view valueName)
{
	HKEY key = nullptr;
	const LSTATUS opened = ::RegOpenKeyExW(
		root, std::wstring(subKey).c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
	if (opened != ERROR_SUCCESS || key == nullptr) return std::nullopt;

	DWORD type = 0;
	DWORD bytes = 0;
	const std::wstring name(valueName);
	LSTATUS queried = ::RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &bytes);
	if (queried != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0
		|| bytes > kMaximumValueBytes) {
		::RegCloseKey(key);
		return std::nullopt;
	}

	std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
	DWORD read = bytes;
	queried = ::RegQueryValueExW(
		key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &read);
	::RegCloseKey(key);
	if (queried != ERROR_SUCCESS) return std::nullopt;

	// `RegQueryValueExW` does not promise a terminator, and a value written
	// without one would otherwise read past the stored data.
	buffer[buffer.size() - 1] = L'\0';
	std::wstring value(buffer.data());
	if (value.empty()) return std::nullopt;
	return value;
}

//! Inno always installs its own uninstaller beside the program.
constexpr std::wstring_view kUninstallerFileName = L"unins000.exe";

bool RealFileExists(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_regular_file(path, error);
}

bool RealDirectoryExists(const std::filesystem::path& path)
{
	std::error_code error;
	return std::filesystem::is_directory(path, error);
}

} // namespace

std::optional<std::wstring> ReadInstalledLocation()
{
	if (auto perUser = ReadStringValue(HKEY_CURRENT_USER, kUninstallSubKey, kInstallLocationValue)) {
		return perUser;
	}
	return ReadStringValue(HKEY_LOCAL_MACHINE, kUninstallSubKey, kInstallLocationValue);
}

bool IsInnoSetupInstall(
	const std::filesystem::path& executablePath, const UpdateFileExistsPredicate& fileExists)
{
	if (executablePath.empty() || !fileExists) return false;
	const std::filesystem::path directory = executablePath.parent_path();
	if (directory.empty()) return false;
	return fileExists(directory / kUninstallerFileName);
}

UpdateInstallTarget ResolveInstallTarget(
	const std::filesystem::path& executablePath,
	const std::optional<std::wstring>& installedLocation,
	const UpdateFileExistsPredicate& fileExists,
	const UpdateFileExistsPredicate& directoryExists)
{
	UpdateInstallTarget target;
	target.type = EUpdateType::Archive;

	// The running copy is itself an installation, so it is the one to update,
	// and its own directory is where Setup has to write. Asking the executable
	// rather than the registry is what keeps a correctly installed copy
	// updatable when the uninstall key is missing or unreadable.
	if (IsInnoSetupInstall(executablePath, fileExists)) {
		target.type = EUpdateType::Setup;
		target.installDirectory = executablePath.parent_path().wstring();
		return target;
	}

	// This copy was not installed, but a real installation is recorded and
	// still on disk. It is updated rather than handed to a browser, because
	// every release of this fork ships an Inno installer; there is no
	// archive-only payload the application would be unable to apply. The
	// reason for diverging from upstream here is written down in
	// `sakura_core/update/CLAUDE.md`.
	if (installedLocation && !installedLocation->empty() && directoryExists
		&& directoryExists(std::filesystem::path(*installedLocation))) {
		target.type = EUpdateType::Setup;
		target.installDirectory = *installedLocation;
	}
	return target;
}

UpdateInstallLocation::UpdateInstallLocation(std::filesystem::path executablePath)
	: m_executablePath(std::move(executablePath))
{
}

std::filesystem::path UpdateInstallLocation::CurrentExecutablePath()
{
	std::vector<wchar_t> buffer(MAX_PATH, L'\0');
	for (;;) {
		const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (written == 0) return {};
		if (written < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), written));
		if (buffer.size() >= 32768) return {};
		buffer.resize(buffer.size() * 2);
	}
}

UpdateInstallTarget UpdateInstallLocation::Resolve()
{
	return ResolveInstallTarget(
		m_executablePath, ReadInstalledLocation(), RealFileExists, RealDirectoryExists);
}

} // namespace update
