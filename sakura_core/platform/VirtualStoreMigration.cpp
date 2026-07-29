/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"

#include "platform/VirtualStoreMigration.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>

namespace platform {
namespace {

std::wstring NormalizeSeparators(std::wstring value)
{
	for (auto& character : value) {
		if (character == L'/') {
			character = L'\\';
		}
	}
	return value;
}

void TrimTrailingSeparators(std::wstring& value)
{
	while (!value.empty() && value.back() == L'\\') {
		value.pop_back();
	}
}

std::wstring GetKnownFolderPath(REFKNOWNFOLDERID folderId)
{
	PWSTR rawPath = nullptr;
	if (FAILED(::SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT_PATH, nullptr, &rawPath)) || rawPath == nullptr) {
		return {};
	}
	std::wstring path(rawPath);
	::CoTaskMemFree(rawPath);
	return path;
}

bool IsSafeRelativePath(const std::filesystem::path& path)
{
	if (path.empty() || path.has_root_name() || path.has_root_directory()) {
		return false;
	}
	for (const auto& component : path) {
		if (component == L"..") {
			return false;
		}
	}
	return true;
}

bool IsSafeProfileName(std::wstring_view profileName)
{
	if (profileName.empty()) {
		return true;
	}
	if (profileName == L"." || profileName == L"..") {
		return false;
	}
	constexpr std::wstring_view invalidCharacters = L"<>:\"/\\|?*";
	for (const wchar_t character : profileName) {
		if (character < L' ' || invalidCharacters.find(character) != std::wstring_view::npos) {
			return false;
		}
	}
	return true;
}

class WindowsMigrationPathProvider final : public IVirtualStoreMigrationPathProvider {
public:
	WindowsMigrationPathProvider(std::wstring executablePath, std::wstring profileName)
		: m_executablePath(std::move(executablePath))
		, m_profileName(std::move(profileName))
	{
	}

	std::wstring GetLocalAppDataPath() override
	{
		return GetKnownFolderPath(FOLDERID_LocalAppData);
	}

	std::wstring GetCurrentUserSakuraIniPath() override
	{
		const std::filesystem::path executable(m_executablePath);
		std::filesystem::path executableSettings(m_executablePath);
		executableSettings += L".ini";
		if (::GetPrivateProfileIntW(L"Settings", L"MultiUser", 0, executableSettings.c_str()) == 0) {
			return {};
		}

		const int rootKind = ::GetPrivateProfileIntW(
			L"Settings", L"UserRootFolder", 0, executableSettings.c_str());
		const KNOWNFOLDERID* folderId = &FOLDERID_RoamingAppData;
		if (rootKind == 1 || rootKind == 3) {
			folderId = &FOLDERID_Profile;
		}
		else if (rootKind == 2) {
			folderId = &FOLDERID_Documents;
		}

		std::filesystem::path root(GetKnownFolderPath(*folderId));
		if (root.empty() || !IsSafeProfileName(m_profileName)) {
			return {};
		}
		if (rootKind == 3) {
			root /= L"Desktop";
		}

		std::array<wchar_t, 32768> subFolderBuffer{};
		::GetPrivateProfileStringW(
			L"Settings", L"UserSubFolder", L"sakura",
			subFolderBuffer.data(), static_cast<DWORD>(subFolderBuffer.size()), executableSettings.c_str());
		std::filesystem::path subFolder(subFolderBuffer.data());
		if (!IsSafeRelativePath(subFolder)) {
			subFolder = L"sakura";
		}

		std::filesystem::path destination = root / subFolder;
		if (!m_profileName.empty()) {
			destination /= m_profileName;
		}
		std::filesystem::path iniFile = executable;
		iniFile.replace_extension(L".ini");
		destination /= iniFile.filename();
		return destination.lexically_normal().wstring();
	}

	std::wstring GetMigrationRecordPath() override
	{
		const std::filesystem::path destination(GetCurrentUserSakuraIniPath());
		if (destination.empty()) {
			return {};
		}
		return (destination.parent_path() / L"sakura-code.virtualstore-migration-v1").wstring();
	}

private:
	std::wstring m_executablePath;
	std::wstring m_profileName;
};

bool EnsureParentDirectory(const std::wstring& path)
{
	const auto parent = std::filesystem::path(path).parent_path();
	if (parent.empty()) {
		return false;
	}
	std::error_code error;
	std::filesystem::create_directories(parent, error);
	return !error;
}

std::string ToUtf8(std::wstring_view text)
{
	if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
		return {};
	}
	const int required = ::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
		nullptr, 0, nullptr, nullptr);
	if (required <= 0) {
		return {};
	}
	std::string output(static_cast<size_t>(required), '\0');
	if (::WideCharToMultiByte(
		CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
		output.data(), required, nullptr, nullptr) != required) {
		return {};
	}
	return output;
}

bool WriteAll(HANDLE file, std::string_view bytes)
{
	size_t offset = 0;
	while (offset < bytes.size()) {
		const DWORD chunk = static_cast<DWORD>((std::min)(
			bytes.size() - offset, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD written = 0;
		if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
			return false;
		}
		offset += written;
	}
	return true;
}

class WindowsMigrationFileSystem final : public IVirtualStoreMigrationFileSystem {
public:
	bool Exists(const std::wstring& path) override
	{
		return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	bool CopyFileWithoutOverwrite(
		const std::wstring& sourcePath,
		const std::wstring& destinationPath) override
	{
		return EnsureParentDirectory(destinationPath)
			&& ::CopyFileW(sourcePath.c_str(), destinationPath.c_str(), TRUE) != FALSE;
	}

	bool WriteMigrationRecord(
		const std::wstring& recordPath,
		const std::wstring& recordText) override
	{
		if (!EnsureParentDirectory(recordPath)) {
			return false;
		}
		const std::wstring document = L"Sakura Editor NEXT VirtualStore migration v1\r\nsource="
			+ recordText + L"\r\n";
		const std::string utf8 = ToUtf8(document);
		if (utf8.empty()) {
			return false;
		}

		for (unsigned int attempt = 0; attempt < 8; ++attempt) {
			const std::wstring temporaryPath = recordPath
				+ L".tmp." + std::to_wstring(::GetCurrentProcessId())
				+ L"." + std::to_wstring(::GetCurrentThreadId())
				+ L"." + std::to_wstring(::GetTickCount64())
				+ L"." + std::to_wstring(attempt);
			HANDLE file = ::CreateFileW(
				temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				continue;
			}
			const bool written = WriteAll(file, utf8) && ::FlushFileBuffers(file) != FALSE;
			::CloseHandle(file);
			if (written && ::MoveFileExW(temporaryPath.c_str(), recordPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
				return true;
			}
			::DeleteFileW(temporaryPath.c_str());
			if (Exists(recordPath)) {
				return true;
			}
		}
		return false;
	}
};

} // namespace

std::wstring BuildLegacyVirtualStoreIniPath(
	const std::wstring& localAppDataPath,
	const std::wstring& legacyExecutablePath
)
{
	std::wstring localAppData = NormalizeSeparators(localAppDataPath);
	std::wstring executable = NormalizeSeparators(legacyExecutablePath);
	TrimTrailingSeparators(localAppData);

	if (localAppData.empty()
		|| executable.size() < 4
		|| !std::iswalpha(executable[0])
		|| executable[1] != L':'
		|| executable[2] != L'\\') {
		return {};
	}

	const std::wstring::size_type directoryEnd = executable.find_last_of(L'\\');
	if (directoryEnd == std::wstring::npos || directoryEnd < 3) {
		return {};
	}

	// Drop "C:\\"; VirtualStore stores the rest of the protected absolute path.
	const std::wstring relativeDirectory = executable.substr(3, directoryEnd - 3);
	if (relativeDirectory.empty()) {
		return {};
	}

	return localAppData + L"\\VirtualStore\\" + relativeDirectory + L"\\sakura.ini";
}

VirtualStoreMigrationRequest BuildVirtualStoreMigrationRequest(
	const std::wstring& legacyExecutablePath,
	IVirtualStoreMigrationPathProvider& pathProvider
)
{
	VirtualStoreMigrationRequest request;
	request.legacyIniPath = BuildLegacyVirtualStoreIniPath(
		pathProvider.GetLocalAppDataPath(), legacyExecutablePath);
	request.destinationIniPath = pathProvider.GetCurrentUserSakuraIniPath();
	request.backupIniPath = request.destinationIniPath.empty()
		? std::wstring{}
		: request.destinationIniPath + L".virtualstore.bak";
	request.migrationRecordPath = pathProvider.GetMigrationRecordPath();
	return request;
}

VirtualStoreMigrationResult MigrateVirtualStoreIni(
	const VirtualStoreMigrationRequest& request,
	IVirtualStoreMigrationFileSystem& fileSystem
) noexcept
{
	try {
		if (request.legacyIniPath.empty()
			|| request.destinationIniPath.empty()
			|| request.backupIniPath.empty()
			|| request.migrationRecordPath.empty()) {
			return VirtualStoreMigrationResult::Failed;
		}

		// The record has priority over destination existence so a successful migration
		// is idempotent even though it intentionally leaves the copied destination.
		if (fileSystem.Exists(request.migrationRecordPath)) {
			return VirtualStoreMigrationResult::AlreadyMigrated;
		}
		if (!fileSystem.Exists(request.legacyIniPath)) {
			return VirtualStoreMigrationResult::NoLegacy;
		}
		if (fileSystem.Exists(request.destinationIniPath)) {
			return VirtualStoreMigrationResult::DestinationExists;
		}

		// Preserve a user-settings-area backup before creating the active setting.
		// An existing backup is retained, including one left by an interrupted attempt.
		if (!fileSystem.Exists(request.backupIniPath)
			&& !fileSystem.CopyFileWithoutOverwrite(request.legacyIniPath, request.backupIniPath)) {
			return VirtualStoreMigrationResult::Failed;
		}

		// This operation is deliberately non-overwriting. A destination created by a
		// concurrent process fails safely instead of replacing user data.
		if (!fileSystem.CopyFileWithoutOverwrite(request.legacyIniPath, request.destinationIniPath)) {
			return VirtualStoreMigrationResult::Failed;
		}
		if (!fileSystem.WriteMigrationRecord(request.migrationRecordPath, request.legacyIniPath)) {
			return VirtualStoreMigrationResult::Failed;
		}
		return VirtualStoreMigrationResult::Copied;
	}
	catch (...) {
		return VirtualStoreMigrationResult::Failed;
	}
}

VirtualStoreMigrationResult MigrateVirtualStoreIniForCurrentUser(
	const std::wstring& executablePath,
	const std::wstring& profileName
) noexcept
{
	try {
		WindowsMigrationPathProvider pathProvider(executablePath, profileName);
		const auto request = BuildVirtualStoreMigrationRequest(executablePath, pathProvider);
		WindowsMigrationFileSystem fileSystem;
		return MigrateVirtualStoreIni(request, fileSystem);
	}
	catch (...) {
		return VirtualStoreMigrationResult::Failed;
	}
}

} // namespace platform
