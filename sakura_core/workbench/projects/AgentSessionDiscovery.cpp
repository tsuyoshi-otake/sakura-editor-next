/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionDiscovery.h"

#include "platform/process/BoundedProcessRunner.h"
#include "platform/process/WindowsExecutableResolver.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <utility>

namespace workbench::projects {
namespace {

struct DirectoryFile final {
	std::wstring path;
	std::wstring name;
	std::int64_t lastWriteUnixSeconds{};
};

struct FileSlices final {
	std::string head;
	std::string tail;
};

bool IsStopped(const HANDLE stop) noexcept
{
	return stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

std::wstring EnvironmentValue(const wchar_t* name)
{
	std::wstring value(256, L'\0');
	for (int attempt = 0; attempt < 2; ++attempt) {
		const DWORD length = ::GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
		if (length == 0) return {};
		if (length < value.size()) {
			value.resize(length);
			return value;
		}
		value.assign(length, L'\0');
	}
	return {};
}

//! nullopt = unset; an empty string = set but unusable (relative).
std::optional<std::wstring> AbsoluteOverride(const wchar_t* name)
{
	auto value = EnvironmentValue(name);
	if (value.empty()) return std::nullopt;
	return platform::IsAbsoluteWindowsPath(value) ? value : std::wstring{};
}

std::wstring JoinPath(const std::wstring& base, const std::wstring_view leaf)
{
	if (base.empty()) return {};
	std::wstring result = base;
	if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
	result.append(leaf);
	return result;
}

std::int64_t ToUnixSeconds(const FILETIME& time) noexcept
{
	ULARGE_INTEGER value{};
	value.LowPart = time.dwLowDateTime;
	value.HighPart = time.dwHighDateTime;
	constexpr std::uint64_t kUnixEpochIn100ns = 116444736000000000ULL;
	if (value.QuadPart <= kUnixEpochIn100ns) return 0;
	return static_cast<std::int64_t>((value.QuadPart - kUnixEpochIn100ns) / 10000000ULL);
}

//! Plain files or plain directories directly inside `directory`. Reparse
//! points are skipped so a link can never lead discovery outside the root.
std::vector<DirectoryFile> ListDirectory(const std::wstring& directory, const bool directories,
	std::size_t& budget)
{
	std::vector<DirectoryFile> result;
	if (directory.empty() || budget == 0) return result;
	WIN32_FIND_DATAW data{};
	const HANDLE find = ::FindFirstFileExW(JoinPath(directory, L"*").c_str(), FindExInfoBasic,
		&data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (find == INVALID_HANDLE_VALUE) return result;
	do {
		if (budget == 0) break;
		--budget;
		const std::wstring_view name(data.cFileName);
		if (name == L"." || name == L"..") continue;
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
		const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (isDirectory != directories) continue;
		result.push_back({ JoinPath(directory, name), std::wstring(name), ToUnixSeconds(data.ftLastWriteTime) });
	} while (::FindNextFileW(find, &data) != FALSE);
	::FindClose(find);
	return result;
}

bool EndsWithInsensitive(const std::wstring_view value, const std::wstring_view suffix) noexcept
{
	return value.size() >= suffix.size()
		&& _wcsnicmp(value.data() + value.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

void SortNewestFirst(std::vector<DirectoryFile>& files)
{
	std::ranges::stable_sort(files, [](const auto& left, const auto& right) {
		return left.lastWriteUnixSeconds > right.lastWriteUnixSeconds;
	});
}

bool ReadExactly(const HANDLE file, std::string& target, const std::size_t bytes)
{
	target.assign(bytes, '\0');
	std::size_t done = 0;
	while (done < bytes) {
		DWORD read = 0;
		const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes - done, 1u << 20));
		if (::ReadFile(file, target.data() + done, chunk, &read, nullptr) == FALSE) return false;
		if (read == 0) break;
		done += read;
	}
	target.resize(done);
	return true;
}

//! A head and a tail of the file. When the file fits in both, the whole file
//! is the head and the tail stays empty, so no line is visited twice.
std::optional<FileSlices> ReadSlices(const std::wstring& path, const std::size_t headBytes,
	const std::size_t tailBytes)
{
	// Agents append to these files while they run, so share every access.
	const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) return std::nullopt;
	FileSlices slices;
	bool ok = false;
	LARGE_INTEGER size{};
	if (::GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0) {
		const auto total = static_cast<std::uint64_t>(size.QuadPart);
		if (total <= headBytes + tailBytes) {
			ok = ReadExactly(file, slices.head, static_cast<std::size_t>(total));
		} else if (ReadExactly(file, slices.head, headBytes)) {
			ok = true;
			if (tailBytes > 0) {
				LARGE_INTEGER offset{};
				offset.QuadPart = static_cast<LONGLONG>(total - tailBytes);
				ok = ::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) != FALSE
					&& ReadExactly(file, slices.tail, tailBytes);
			}
		}
	}
	::CloseHandle(file);
	if (!ok) return std::nullopt;
	return slices;
}

std::wstring FileStem(const std::wstring_view name)
{
	const auto dot = name.rfind(L'.');
	return std::wstring(dot == std::wstring_view::npos ? name : name.substr(0, dot));
}

bool IsRequired(const AgentSessionDiscoveryLimits& limits, const EAgentSessionProvider provider,
	const std::wstring_view id)
{
	if (limits.requiredIdentities.empty() || id.empty()) return false;
	const auto identity = AgentSessionIdentity({ .provider = provider, .id = std::wstring(id) });
	return std::ranges::find(limits.requiredIdentities, identity) != limits.requiredIdentities.end();
}

//! Moves the files of required sessions to the front, keeping the order
//! inside both halves, and returns how many there are.
template <typename IdOf>
std::size_t PromoteRequired(std::vector<DirectoryFile>& files, const AgentSessionDiscoveryLimits& limits,
	const EAgentSessionProvider provider, const IdOf& idOf)
{
	if (limits.requiredIdentities.empty()) return 0;
	const auto firstOther = std::ranges::stable_partition(files, [&](const DirectoryFile& file) {
		return IsRequired(limits, provider, idOf(file));
	}).begin();
	return static_cast<std::size_t>(firstOther - files.begin());
}

void DiscoverClaudeCode(const std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, const HANDLE stop, std::vector<AgentSessionRecord>& output)
{
	const auto directory = JoinPath(JoinPath(roots.claudeConfigDirectory, L"projects"),
		ClaudeCodeProjectDirectoryName(folder));
	std::size_t budget = limits.maximumDirectoryEntries;
	auto files = ListDirectory(directory, false, budget);
	std::erase_if(files, [](const auto& file) { return !EndsWithInsensitive(file.name, L".jsonl"); });
	SortNewestFirst(files);
	const auto required = PromoteRequired(files, limits, EAgentSessionProvider::ClaudeCode,
		[](const DirectoryFile& file) { return FileStem(file.name); });
	std::size_t accepted = 0;
	std::size_t reads = 0;
	for (std::size_t index = 0; index < files.size(); ++index) {
		const auto& file = files[index];
		const bool counted = index >= required;
		if ((counted && accepted == limits.maximumSessionsPerProvider)
			|| reads == limits.maximumFileReadsPerProvider || IsStopped(stop)) break;
		const auto stem = FileStem(file.name);
		if (!IsValidAgentSessionId(stem)) continue;
		++reads;
		const auto slices = ReadSlices(file.path, limits.headBytes, limits.tailBytes);
		if (!slices) continue;
		if (auto session = ParseClaudeCodeTranscript(slices->head, slices->tail, stem, folder,
			file.lastWriteUnixSeconds)) {
			output.push_back(std::move(*session));
			if (counted) ++accepted;
		}
	}
}

std::map<std::wstring, std::wstring> ReadCodexIndex(const std::wstring& path, const std::size_t maximumBytes)
{
	// The newest names are at the end; an oversized index keeps only its tail.
	const auto slices = ReadSlices(path, 0, maximumBytes);
	if (!slices) return {};
	return ParseCodexSessionIndex(slices->tail.empty() ? slices->head : slices->tail);
}

void DiscoverCodex(const std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, const HANDLE stop, std::vector<AgentSessionRecord>& output)
{
	if (roots.codexHome.empty()) return;
	const auto names = ReadCodexIndex(JoinPath(roots.codexHome, L"session_index.jsonl"),
		limits.maximumCodexIndexBytes);
	// sessions\YYYY\MM\DD\rollout-<timestamp>-<id>.jsonl
	std::size_t budget = limits.maximumDirectoryEntries;
	std::vector<DirectoryFile> files;
	for (const auto& year : ListDirectory(JoinPath(roots.codexHome, L"sessions"), true, budget)) {
		for (const auto& month : ListDirectory(year.path, true, budget)) {
			for (const auto& day : ListDirectory(month.path, true, budget)) {
				if (IsStopped(stop)) return;
				for (auto& file : ListDirectory(day.path, false, budget)) {
					if (file.name.starts_with(L"rollout-") && EndsWithInsensitive(file.name, L".jsonl")) {
						files.push_back(std::move(file));
					}
				}
			}
		}
	}
	// The file name ends with the thread id. Named (user) threads are read
	// first: sub-agent rollouts usually outnumber them and are never accepted.
	const auto idOf = [](const DirectoryFile& file) {
		const auto stem = FileStem(file.name);
		return stem.size() > 36 ? stem.substr(stem.size() - 36) : std::wstring{};
	};
	std::ranges::stable_sort(files, [&](const auto& left, const auto& right) {
		const bool leftNamed = names.contains(idOf(left));
		const bool rightNamed = names.contains(idOf(right));
		if (leftNamed != rightNamed) return leftNamed;
		return left.lastWriteUnixSeconds > right.lastWriteUnixSeconds;
	});
	const auto required = PromoteRequired(files, limits, EAgentSessionProvider::Codex, idOf);
	std::size_t accepted = 0;
	std::size_t reads = 0;
	for (std::size_t index = 0; index < files.size(); ++index) {
		const auto& file = files[index];
		const bool counted = index >= required;
		if ((counted && accepted == limits.maximumSessionsPerProvider)
			|| reads == limits.maximumFileReadsPerProvider || IsStopped(stop)) break;
		++reads;
		const auto slices = ReadSlices(file.path, limits.codexHeadBytes, 0);
		if (!slices) continue;
		if (auto session = ParseCodexRollout(slices->head, folder, file.lastWriteUnixSeconds, names)) {
			output.push_back(std::move(*session));
			if (counted) ++accepted;
		}
	}
}

void DiscoverPi(const std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, const HANDLE stop, std::vector<AgentSessionRecord>& output)
{
	const auto directory = JoinPath(JoinPath(roots.piAgentDirectory, L"sessions"),
		PiSessionDirectoryName(folder));
	std::size_t budget = limits.maximumDirectoryEntries;
	auto files = ListDirectory(directory, false, budget);
	std::erase_if(files, [](const auto& file) { return !EndsWithInsensitive(file.name, L".jsonl"); });
	SortNewestFirst(files);
	// <timestamp>_<id>.jsonl
	const auto required = PromoteRequired(files, limits, EAgentSessionProvider::Pi,
		[](const DirectoryFile& file) {
			const auto stem = FileStem(file.name);
			const auto separator = stem.rfind(L'_');
			return separator == std::wstring::npos ? std::wstring{} : stem.substr(separator + 1);
		});
	std::size_t accepted = 0;
	std::size_t reads = 0;
	for (std::size_t index = 0; index < files.size(); ++index) {
		const auto& file = files[index];
		const bool counted = index >= required;
		if ((counted && accepted == limits.maximumSessionsPerProvider)
			|| reads == limits.maximumFileReadsPerProvider || IsStopped(stop)) break;
		++reads;
		const auto slices = ReadSlices(file.path, limits.headBytes, limits.tailBytes);
		if (!slices) continue;
		if (auto session = ParsePiSession(slices->head, slices->tail, folder, file.lastWriteUnixSeconds)) {
			output.push_back(std::move(*session));
			if (counted) ++accepted;
		}
	}
}

void DiscoverDevin(const std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, const HANDLE stop, std::vector<AgentSessionRecord>& output)
{
	if (roots.devinExecutable.empty()) return;
	std::error_code error;
	if (!std::filesystem::is_directory(std::filesystem::path(folder), error)) return;
	platform::process::BoundedProcessRequest request(roots.devinExecutable, std::wstring(folder),
		{ L"list", L"--format", L"json" });
	request.SetTimeoutMilliseconds(limits.devinTimeoutMilliseconds);
	request.SetMaximumStandardOutputBytes(limits.devinMaximumOutputBytes);
	request.SetMaximumStandardErrorBytes(64u * 1024u);
	const auto result = platform::process::RunBoundedProcess(request, stop);
	if (!result.Succeeded() || result.ExitCode() != 0) return;
	const auto& bytes = result.StandardOutput();
	auto sessions = ParseDevinSessionList(
		std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), folder);
	// Newest first; the required ones are kept beyond the per-provider limit.
	const auto total = sessions.size();
	sessions = MergeAgentSessions(std::move(sessions), total);
	std::size_t accepted = 0;
	for (auto& session : sessions) {
		if (!IsRequired(limits, EAgentSessionProvider::Devin, session.id)) {
			if (accepted == limits.maximumSessionsPerProvider) continue;
			++accepted;
		}
		output.push_back(std::move(session));
	}
}

} // namespace

AgentSessionDiscoveryRoots ResolveAgentSessionDiscoveryRoots()
{
	AgentSessionDiscoveryRoots roots;
	const auto profile = EnvironmentValue(L"USERPROFILE");
	const auto home = platform::IsAbsoluteWindowsPath(profile) ? profile : std::wstring{};
	const auto choose = [&home](std::optional<std::wstring> overrideValue, const std::wstring_view fallback) {
		if (overrideValue) return std::move(*overrideValue);
		return home.empty() ? std::wstring{} : JoinPath(home, fallback);
	};
	roots.claudeConfigDirectory = choose(AbsoluteOverride(L"CLAUDE_CONFIG_DIR"), L".claude");
	roots.codexHome = choose(AbsoluteOverride(L"CODEX_HOME"), L".codex");
	auto piOverride = AbsoluteOverride(L"PI_CODING_AGENT_DIR");
	if (!piOverride) piOverride = AbsoluteOverride(L"TAU_CODING_AGENT_DIR");
	roots.piAgentDirectory = choose(std::move(piOverride), L".pi\\agent");
	if (auto devin = platform::ResolveWindowsExecutable(L"devin.exe")) roots.devinExecutable = std::move(*devin);
	return roots;
}

AgentSessionDiscoveryLimits AgentSessionDiscoveryLimitsFor(
	const std::size_t displayCount, std::vector<std::wstring> requiredIdentities)
{
	AgentSessionDiscoveryLimits limits;
	limits.maximumSessionsPerProvider = displayCount;
	// Four providers, each able to fill the view, plus every marked session.
	limits.maximumSessions = displayCount * 4 + requiredIdentities.size();
	limits.requiredIdentities = std::move(requiredIdentities);
	return limits;
}

std::optional<std::vector<AgentSessionRecord>> DiscoverAgentSessions(
	const std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, const HANDLE stop)
{
	std::vector<AgentSessionRecord> sessions;
	if (!platform::IsAbsoluteWindowsPath(folder)) return sessions;
	using Provider = void (*)(std::wstring_view, const AgentSessionDiscoveryRoots&,
		const AgentSessionDiscoveryLimits&, HANDLE, std::vector<AgentSessionRecord>&);
	for (const Provider provider : { &DiscoverClaudeCode, &DiscoverCodex, &DiscoverPi, &DiscoverDevin }) {
		if (IsStopped(stop)) return std::nullopt;
		// A provider that throws (allocation) keeps the records it already
		// produced and never prevents the remaining providers from running.
		try { provider(folder, roots, limits, stop, sessions); }
		catch (...) {}
	}
	if (IsStopped(stop)) return std::nullopt;
	try {
		return MergeAgentSessions(std::move(sessions), limits.maximumSessions);
	} catch (...) {
		return std::vector<AgentSessionRecord>{};
	}
}

} // namespace workbench::projects
