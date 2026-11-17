/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/logs/LogSession.h"

#include "workbench/logs/LogFileSystem.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>

namespace workbench::logs {

LogTimestamp CurrentLocalLogTimestamp() noexcept
{
	SYSTEMTIME now{};
	::GetLocalTime(&now);
	return LogTimestamp{
		.year = now.wYear,
		.month = now.wMonth,
		.day = now.wDay,
		.hour = now.wHour,
		.minute = now.wMinute,
		.second = now.wSecond,
		.millisecond = now.wMilliseconds,
	};
}

std::wstring FormatLogSessionFolderName(const LogTimestamp& started, std::uint32_t processId)
{
	wchar_t buffer[48]{};
	(void)std::swprintf(buffer, std::size(buffer), L"%04d%02d%02dT%02d%02d%02d-%u",
		started.year, started.month, started.day, started.hour, started.minute, started.second,
		static_cast<unsigned>(processId));
	return buffer;
}

bool IsLogSessionFolderName(std::wstring_view name) noexcept
{
	// YYYYMMDDTHHMMSS-<1..10 digits>
	if (name.size() < 17 || name.size() > 26) return false;
	for (std::size_t index = 0; index < 15; ++index) {
		const wchar_t character = name[index];
		if (index == 8) {
			if (character != L'T') return false;
		} else if (character < L'0' || character > L'9') {
			return false;
		}
	}
	if (name[15] != L'-') return false;
	for (std::size_t index = 16; index < name.size(); ++index) {
		if (name[index] < L'0' || name[index] > L'9') return false;
	}
	return true;
}

std::vector<std::wstring> SelectExpiredLogSessions(
	std::vector<std::wstring> names, std::wstring_view current, std::size_t retain)
{
	std::erase_if(names, [current](const std::wstring& name) {
		return !IsLogSessionFolderName(name) || name == current;
	});
	std::ranges::sort(names, std::ranges::greater{});
	// The current session counts toward the retained ones.
	const std::size_t keepOthers = retain == 0 ? 0 : retain - 1;
	if (names.size() <= keepOthers) return {};
	return { names.begin() + static_cast<std::ptrdiff_t>(keepOthers), names.end() };
}

bool IsLogSessionProcessRunning(std::wstring_view sessionName) noexcept
{
	if (!IsLogSessionFolderName(sessionName)) return false;
	try {
		const auto digits = [&](std::size_t offset, std::size_t count) {
			int value = 0;
			for (std::size_t index = 0; index < count; ++index) value = value * 10 + (sessionName[offset + index] - L'0');
			return value;
		};
		unsigned long long pid = 0;
		for (std::size_t index = 16; index < sessionName.size(); ++index) pid = pid * 10 + (sessionName[index] - L'0');
		if (pid == 0 || pid > 0xFFFFFFFFULL) return false;
		const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
		if (process == nullptr) {
			// Gone: not live. Access denied: it exists and might be this
			// user's elevated editor, so its folder is kept.
			return ::GetLastError() == ERROR_ACCESS_DENIED;
		}
		DWORD exitCode = 0;
		FILETIME created{}, exited{}, kernel{}, user{};
		const bool running = ::GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE
			&& ::GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
		::CloseHandle(process);
		if (!running) return false;
		SYSTEMTIME localStart{
			.wYear = static_cast<WORD>(digits(0, 4)),
			.wMonth = static_cast<WORD>(digits(4, 2)),
			.wDay = static_cast<WORD>(digits(6, 2)),
			.wHour = static_cast<WORD>(digits(9, 2)),
			.wMinute = static_cast<WORD>(digits(11, 2)),
			.wSecond = static_cast<WORD>(digits(13, 2)),
		};
		SYSTEMTIME utcStart{};
		FILETIME start{};
		if (!::TzSpecificLocalTimeToSystemTime(nullptr, &localStart, &utcStart)
			|| !::SystemTimeToFileTime(&utcStart, &start)) {
			// Unparseable time: a running process with that PID keeps the folder.
			return true;
		}
		const auto ticks = [](const FILETIME& time) {
			return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
		};
		// The folder is named after the process started; a later process that
		// reused the PID was created after it. Two seconds absorbs the name's
		// one-second resolution and clock rounding.
		constexpr unsigned long long kSlack = 2ULL * 10'000'000ULL;
		return ticks(created) <= ticks(start) + kSlack;
	} catch (...) {
		return true;
	}
}

bool PrepareLogSession(ILogFileSystem& files, const LogSessionLayout& layout, const LogSessionLiveness& isLive,
	const std::function<bool()>& stopRequested) noexcept
{
	try {
		if (!files.CreateDirectories(layout.WindowFolder())) return false;
		if (auto names = files.ListDirectories(layout.root)) {
			for (const auto& expired : SelectExpiredLogSessions(std::move(*names), layout.sessionName)) {
				if (stopRequested && stopRequested()) break;
				if (isLive && isLive(expired)) continue;
				(void)files.RemoveTree(layout.root / expired);
			}
		}
		return true;
	} catch (...) {
		return false;
	}
}

} // namespace workbench::logs
