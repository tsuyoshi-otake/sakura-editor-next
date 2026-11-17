/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/logs/LogFileSystem.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <system_error>

namespace workbench::logs {
namespace {

//! A long owner ID plus label can pass MAX_PATH under a deep profile path, and
//! this process is not long-path aware, so an absolute path that long is
//! passed in its extended-length form.
[[nodiscard]] std::filesystem::path Extended(const std::filesystem::path& path)
{
	constexpr std::size_t kPlainPathLimit = 240;
	const std::wstring& native = path.native();
	if (native.size() < kPlainPathLimit || !path.is_absolute() || native.starts_with(L"\\\\?\\")) return path;
	if (native.starts_with(L"\\\\")) return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2));
	return std::filesystem::path(L"\\\\?\\" + native);
}

class CWin32LogFileSystem final : public ILogFileSystem {
public:
	bool CreateDirectories(const std::filesystem::path& directory) noexcept override
	{
		try {
			const auto target = Extended(directory);
			std::error_code error;
			std::filesystem::create_directories(target, error);
			return !error && std::filesystem::is_directory(target, error);
		} catch (...) {
			return false;
		}
	}

	bool Append(const std::filesystem::path& file, std::string_view bytes) noexcept override
	{
		std::filesystem::path target;
		try {
			target = Extended(file);
		} catch (...) {
			return false;
		}
		const HANDLE handle = ::CreateFileW(target.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;
		bool written = true;
		while (!bytes.empty() && written) {
			const auto chunk = static_cast<DWORD>(
				(std::min)(bytes.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
			DWORD done = 0;
			written = ::WriteFile(handle, bytes.data(), chunk, &done, nullptr) != FALSE && done == chunk;
			bytes.remove_prefix(done);
		}
		return ::CloseHandle(handle) != FALSE && written;
	}

	std::optional<std::uint64_t> FileSize(const std::filesystem::path& file) noexcept override
	{
		try {
			const auto target = Extended(file);
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!::GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
			return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		} catch (...) {
			return std::nullopt;
		}
	}

	bool MoveReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept override
	{
		try {
			return ::MoveFileExW(Extended(from).c_str(), Extended(to).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
		} catch (...) {
			return false;
		}
	}

	bool ReplaceContent(const std::filesystem::path& file, std::string_view bytes) noexcept override
	{
		try {
			const auto target = Extended(file);
			auto temporary = target;
			temporary += L".tmp";
			const HANDLE handle = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;
			bool written = true;
			while (!bytes.empty() && written) {
				const auto chunk = static_cast<DWORD>(
					(std::min)(bytes.size(), static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
				DWORD done = 0;
				written = ::WriteFile(handle, bytes.data(), chunk, &done, nullptr) != FALSE && done == chunk;
				bytes.remove_prefix(done);
			}
			written = ::FlushFileBuffers(handle) != FALSE && written;
			written = ::CloseHandle(handle) != FALSE && written;
			if (written && ::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE) {
				return true;
			}
			(void)::DeleteFileW(temporary.c_str());
			return false;
		} catch (...) {
			return false;
		}
	}

	std::optional<std::vector<std::wstring>> ListDirectories(const std::filesystem::path& directory) noexcept override
	{
		try {
			std::vector<std::wstring> names;
			std::error_code error;
			for (std::filesystem::directory_iterator it(Extended(directory), error), end; !error && it != end; it.increment(error)) {
				std::error_code typeError;
				if (it->is_directory(typeError) && !it->is_symlink(typeError)) {
					names.push_back(it->path().filename().wstring());
				}
			}
			if (error) return std::nullopt;
			return names;
		} catch (...) {
			return std::nullopt;
		}
	}

	bool RemoveTree(const std::filesystem::path& directory) noexcept override
	{
		try {
			std::error_code error;
			(void)std::filesystem::remove_all(Extended(directory), error);
			return !error;
		} catch (...) {
			return false;
		}
	}
};

} // namespace

std::shared_ptr<ILogFileSystem> CreateWin32LogFileSystem()
{
	return std::make_shared<CWin32LogFileSystem>();
}

} // namespace workbench::logs
