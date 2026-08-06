/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/Win32UpdateLauncher.h"

#include <ShellAPI.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace update {
namespace {

constexpr std::wstring_view kHttpsScheme = L"https://";

//! Guards against a feed URL that would be rejected by the shell anyway, and
//! against one long enough to be interesting.
constexpr std::size_t kMaximumUrlLength = 2048;

} // namespace

bool IsLaunchableReleaseUrl(std::wstring_view url) noexcept
{
	if (url.size() <= kHttpsScheme.size() || url.size() > kMaximumUrlLength) return false;
	if (::CompareStringOrdinal(
			url.data(), static_cast<int>(kHttpsScheme.size()),
			kHttpsScheme.data(), static_cast<int>(kHttpsScheme.size()), TRUE) != CSTR_EQUAL) {
		return false;
	}
	// A control character or a quote in a URL means the string is not what it
	// claims to be; refuse rather than normalize it into something launchable.
	for (const wchar_t ch : url) {
		if (ch < 0x20 || ch == 0x7F || ch == L'"' || ch == L'<' || ch == L'>') return false;
	}
	return true;
}

bool Win32UpdateLauncher::LaunchInstaller(const InstallerInvocation& invocation)
{
	const auto commandLine = BuildInstallerCommandLine(invocation);
	if (!commandLine) return false;

	std::error_code error;
	if (!std::filesystem::exists(invocation.installerPath, error) || error) return false;

	// `CreateProcessW` may write to its command-line buffer.
	std::vector<wchar_t> mutableCommandLine(commandLine->begin(), commandLine->end());
	mutableCommandLine.push_back(L'\0');

	// The staged installer's own directory: Setup unpacks beside itself, and
	// leaving the working directory pointing at the installation being replaced
	// would keep a handle on the directory Setup is about to overwrite.
	const std::wstring workingDirectory =
		std::filesystem::path(invocation.installerPath).parent_path().wstring();

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	const BOOL created = ::CreateProcessW(
		nullptr,
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		// Detached and in its own process group, so neither this process exiting
		// nor a console Ctrl event can take the installer down with it.
		DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
		nullptr,
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
		&startup,
		&process);
	if (!created) return false;

	// Nothing waits on the installer; only the handles are ours to release.
	if (process.hThread != nullptr) ::CloseHandle(process.hThread);
	if (process.hProcess != nullptr) ::CloseHandle(process.hProcess);
	return true;
}

bool Win32UpdateLauncher::OpenReleasePage(std::wstring_view url)
{
	if (!IsLaunchableReleaseUrl(url)) return false;

	const std::wstring target(url);
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
	info.lpVerb = L"open";
	info.lpFile = target.c_str();
	info.nShow = SW_SHOWNORMAL;
	return ::ShellExecuteExW(&info) != FALSE;
}

} // namespace update
