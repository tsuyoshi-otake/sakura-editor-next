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

std::filesystem::path Normalize(const std::filesystem::path& path)
{
	std::error_code error;
	// `weakly_canonical` resolves `..`, links, and short names; falling back to
	// the lexical form keeps a non-existent directory comparable instead of
	// collapsing it to an empty path that would match everything.
	auto canonical = std::filesystem::weakly_canonical(path, error);
	if (error || canonical.empty()) return path.lexically_normal();
	return canonical;
}

//! Inno writes `InstallLocation` with a trailing backslash. Left in place it
//! becomes an empty final path component, which would silently consume the
//! executable's file name during the component-wise comparison below.
std::wstring StripTrailingSeparators(std::wstring_view value)
{
	std::wstring stripped(value);
	while (stripped.size() > 1 && (stripped.back() == L'\\' || stripped.back() == L'/')) {
		// Keep a root's own separator: `C:\` and `\\server\share\` are not the
		// same locations as `C:` and `\\server\share`.
		if (stripped.size() == 3 && stripped[1] == L':') break;
		stripped.pop_back();
	}
	return stripped;
}

} // namespace

std::optional<std::wstring> ReadInstalledLocation()
{
	if (auto perUser = ReadStringValue(HKEY_CURRENT_USER, kUninstallSubKey, kInstallLocationValue)) {
		return perUser;
	}
	return ReadStringValue(HKEY_LOCAL_MACHINE, kUninstallSubKey, kInstallLocationValue);
}

bool IsExecutableWithinInstallDirectory(
	const std::filesystem::path& executablePath, std::wstring_view installDirectory)
{
	if (executablePath.empty() || installDirectory.empty()) return false;

	const auto directory = Normalize(std::filesystem::path(StripTrailingSeparators(installDirectory)));
	const auto executable = Normalize(executablePath);
	if (directory.empty() || executable.empty()) return false;

	auto directoryPart = directory.begin();
	auto executablePart = executable.begin();
	for (; directoryPart != directory.end(); ++directoryPart, ++executablePart) {
		if (executablePart == executable.end()) return false;
		// Windows paths are case-insensitive, and the registry value and the
		// module path routinely disagree on case.
		const std::wstring left = directoryPart->wstring();
		const std::wstring right = executablePart->wstring();
		if (::CompareStringOrdinal(
				left.c_str(), static_cast<int>(left.size()),
				right.c_str(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) {
			return false;
		}
	}
	// Every component of the directory matched, and the executable still has its
	// own file name left over.
	return executablePart != executable.end();
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
	UpdateInstallTarget target;
	target.type = EUpdateType::Archive;

	const auto installDirectory = ReadInstalledLocation();
	if (!installDirectory) return target;
	if (!IsExecutableWithinInstallDirectory(m_executablePath, *installDirectory)) return target;

	target.type = EUpdateType::Setup;
	target.installDirectory = *installDirectory;
	return target;
}

} // namespace update
