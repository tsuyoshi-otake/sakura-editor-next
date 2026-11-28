/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionLiveRecord.h"

#include "platform/process/WindowsExecutableResolver.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::projects {
namespace {

constexpr std::size_t kMaximumClaudeCodeRecordBytes = 64u * 1024u;
constexpr std::size_t kMaximumDevinLockBytes = 32;
//! Devin never removes a lock, so the directory only grows.
constexpr std::size_t kMaximumDevinLocks = 20000;
constexpr std::wstring_view kDevinLockSuffix = L".lock";

using UniqueHandle = std::unique_ptr<void, decltype(&::CloseHandle)>;

//! The CLI keeps writing its record while it runs, so every access is shared.
UniqueHandle OpenRecord(const std::wstring& path)
{
	const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	return { file == INVALID_HANDLE_VALUE ? nullptr : file, &::CloseHandle };
}

std::optional<std::wstring> ReadClaudeCodeRecord(const std::wstring& configDirectory,
	const AgentProcessCoordinates& process)
{
	const auto file = OpenRecord(configDirectory + L"\\sessions\\"
		+ std::to_wstring(process.processId) + L".json");
	if (!file) return std::nullopt;
	std::string content(kMaximumClaudeCodeRecordBytes + 1, '\0');
	DWORD read = 0;
	if (::ReadFile(file.get(), content.data(), static_cast<DWORD>(content.size()), &read, nullptr) == FALSE
		|| read > kMaximumClaudeCodeRecordBytes) return std::nullopt;
	content.resize(read);
	const auto live = ParseClaudeCodeLiveSession(content);
	if (!live || live->processId != process.processId) return std::nullopt;
	// A record left by an earlier process with this pid names another start.
	if (live->processStart && process.creationTime != 0 && *live->processStart != process.creationTime) {
		return std::nullopt;
	}
	return AgentSessionIdentity({ .provider = EAgentSessionProvider::ClaudeCode, .id = live->sessionId });
}

//! The pid a Devin lock names. ReadFile fails on a held lock with
//! ERROR_LOCK_VIOLATION; a mapped view reads it.
std::optional<std::uint32_t> ReadDevinLockHolder(const HANDLE file, const std::size_t size)
{
	if (size == 0 || size > kMaximumDevinLockBytes) return std::nullopt;
	const UniqueHandle mapping(::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr),
		&::CloseHandle);
	if (!mapping) return std::nullopt;
	// Fails rather than reading past the end if the file shrank meanwhile.
	void* const view = ::MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, size);
	if (view == nullptr) return std::nullopt;
	std::array<char, kMaximumDevinLockBytes> bytes{};
	std::memcpy(bytes.data(), view, size);
	(void)::UnmapViewOfFile(view);
	std::string_view text(bytes.data(), size);
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
		text.remove_suffix(1);
	}
	std::uint32_t processId = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), processId);
	if (error != std::errc{} || end != text.data() + text.size() || processId == 0) return std::nullopt;
	return processId;
}

std::optional<std::wstring> ReadDevinRecord(const std::wstring& stateDirectory,
	const std::span<const AgentProcessCoordinates> processes)
{
	std::uint64_t earliestStart = std::numeric_limits<std::uint64_t>::max();
	for (const auto& process : processes) {
		if (process.creationTime != 0 && process.creationTime < earliestStart) {
			earliestStart = process.creationTime;
		}
	}
	// Without a start time a lock left by an earlier holder of the pid could not
	// be told apart, so nothing is claimed.
	if (earliestStart == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;

	const std::wstring directory = stateDirectory + L"\\session_locks\\";
	WIN32_FIND_DATAW data{};
	const HANDLE found = ::FindFirstFileExW((directory + L"*.lock").c_str(), FindExInfoBasic,
		&data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (found == INVALID_HANDLE_VALUE) return std::nullopt;
	const std::unique_ptr<void, decltype(&::FindClose)> find(found, &::FindClose);
	// For each process, the newest lock it wrote: (write time, session id).
	std::vector<std::pair<std::uint64_t, std::wstring>> newest(processes.size());
	std::size_t remaining = kMaximumDevinLocks;
	do {
		const std::wstring_view name(data.cFileName);
		if (!name.ends_with(kDevinLockSuffix)) continue;
		const std::wstring_view id = name.substr(0, name.size() - kDevinLockSuffix.size());
		if (!IsValidAgentSessionId(id)) continue;
		const auto file = OpenRecord(directory + std::wstring(name));
		// The directory entry of a file that is still open can keep an old
		// write time, so the time comes from the handle.
		BY_HANDLE_FILE_INFORMATION information{};
		if (!file || ::GetFileInformationByHandle(file.get(), &information) == FALSE) continue;
		const std::uint64_t written =
			(static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32)
			| information.ftLastWriteTime.dwLowDateTime;
		if (written < earliestStart || information.nFileSizeHigh != 0) continue;
		const auto holder = ReadDevinLockHolder(file.get(), information.nFileSizeLow);
		if (!holder) continue;
		for (std::size_t index = 0; index < processes.size(); ++index) {
			const auto& process = processes[index];
			if (process.processId == *holder && process.creationTime != 0
				&& written >= process.creationTime && written > newest[index].first) {
				newest[index] = { written, std::wstring(id) };
			}
		}
	} while (--remaining != 0 && ::FindNextFileW(find.get(), &data) != FALSE);

	for (const auto& [written, id] : newest) {
		if (written != 0) {
			return AgentSessionIdentity({ .provider = EAgentSessionProvider::Devin, .id = id });
		}
	}
	return std::nullopt;
}

} // namespace

AgentLiveSessionRoots ResolveAgentLiveSessionRoots(const AgentSessionDiscoveryRoots& discovery)
{
	std::wstring appData(32768, L'\0');
	const DWORD length = ::GetEnvironmentVariableW(L"APPDATA", appData.data(),
		static_cast<DWORD>(appData.size()));
	appData.resize(length < appData.size() ? length : 0);
	return {
		.claudeConfigDirectory = discovery.claudeConfigDirectory,
		.devinStateDirectory = platform::IsAbsoluteWindowsPath(appData)
			? appData + L"\\devin\\cli" : std::wstring{},
	};
}

std::optional<std::wstring> ReadLiveAgentSessionIdentity(const EAgentSessionProvider provider,
	const std::span<const AgentProcessCoordinates> processes, const AgentLiveSessionRoots& roots)
{
	if (provider == EAgentSessionProvider::ClaudeCode && !roots.claudeConfigDirectory.empty()) {
		for (const auto& process : processes) {
			if (auto identity = ReadClaudeCodeRecord(roots.claudeConfigDirectory, process)) {
				return identity;
			}
		}
	}
	if (provider == EAgentSessionProvider::Devin && !roots.devinStateDirectory.empty()) {
		return ReadDevinRecord(roots.devinStateDirectory, processes);
	}
	return std::nullopt;
}

} // namespace workbench::projects
