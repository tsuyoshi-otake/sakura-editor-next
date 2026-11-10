/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/projects/AgentSessionStoreOperations.h"

#include "platform/process/WindowsExecutableResolver.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>

namespace workbench::projects {
namespace {

using platform::process::BoundedProcessRequest;
using platform::process::BoundedProcessResult;
using platform::process::EBoundedProcessStatus;
using platform::process::EBoundedProcessStream;

constexpr std::size_t kMaximumLiveSessionFiles = 4096;
constexpr std::size_t kMaximumLiveSessionFileBytes = 64u * 1024u;
constexpr std::size_t kMaximumPiSessionFileBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaximumCodexReplyBytes = 1024u * 1024u;
constexpr std::size_t kMaximumDirectoryEntries = 20000;

class ScopedFile final {
public:
	explicit ScopedFile(const HANDLE handle) noexcept : m_handle(handle) {}
	~ScopedFile() { if (IsValid()) ::CloseHandle(m_handle); }
	ScopedFile(const ScopedFile&) = delete;
	ScopedFile& operator=(const ScopedFile&) = delete;
	[[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
private:
	HANDLE m_handle{};
};

std::wstring JoinPath(const std::wstring& base, const std::wstring_view leaf)
{
	if (base.empty()) return {};
	std::wstring result = base;
	if (result.back() != L'\\' && result.back() != L'/') result.push_back(L'\\');
	result.append(leaf);
	return result;
}

bool Exists(const std::wstring& path, const bool directory) noexcept
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES
		&& ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

bool EndsWithInsensitive(const std::wstring_view value, const std::wstring_view suffix) noexcept
{
	return value.size() >= suffix.size() && ::CompareStringOrdinal(
		value.data() + (value.size() - suffix.size()), static_cast<int>(suffix.size()),
		suffix.data(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
}

//! File names (not directories) directly inside `directory`, bounded.
std::vector<std::wstring> ListFileNames(const std::wstring& directory)
{
	std::vector<std::wstring> names;
	WIN32_FIND_DATAW data{};
	const HANDLE find = ::FindFirstFileExW(JoinPath(directory, L"*").c_str(), FindExInfoBasic, &data,
		FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (find == INVALID_HANDLE_VALUE) return names;
	std::size_t seen = 0;
	do {
		if (++seen > kMaximumDirectoryEntries) break;
		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) names.emplace_back(data.cFileName);
	} while (::FindNextFileW(find, &data));
	::FindClose(find);
	return names;
}

bool ReadAll(const HANDLE file, std::string& target, const std::size_t bytes)
{
	target.resize(bytes);
	std::size_t done = 0;
	while (done < bytes) {
		DWORD read{};
		const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes - done, 1u << 20));
		if (!::ReadFile(file, target.data() + done, chunk, &read, nullptr) || read == 0) return false;
		done += read;
	}
	return true;
}

bool WriteAll(const HANDLE file, const std::string_view bytes)
{
	std::size_t done = 0;
	while (done < bytes.size()) {
		DWORD written{};
		const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
		if (!::WriteFile(file, bytes.data() + done, chunk, &written, nullptr) || written == 0) return false;
		done += written;
	}
	return true;
}

std::optional<std::string> ReadWholeFile(const std::wstring& path, const std::size_t maximumBytes)
{
	const ScopedFile file(::CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, nullptr));
	LARGE_INTEGER size{};
	if (!file.IsValid() || !::GetFileSizeEx(file.Get(), &size)
		|| static_cast<std::uint64_t>(size.QuadPart) > maximumBytes) return std::nullopt;
	std::string content;
	if (!ReadAll(file.Get(), content, static_cast<std::size_t>(size.QuadPart))) return std::nullopt;
	return content;
}

bool IsStopped(const HANDLE stop) noexcept
{
	return stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0;
}

bool IsProcessRunning(const AgentSessionStoreContext& context, const std::uint32_t processId)
{
	if (context.services.isProcessRunning) return context.services.isProcessRunning(processId);
	const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == nullptr) {
		// A process that exists but cannot be opened still owns its session.
		return ::GetLastError() == ERROR_ACCESS_DENIED;
	}
	DWORD exitCode{};
	const bool running = ::GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
	::CloseHandle(process);
	return running;
}

//! Claude Code writes `sessions\<pid>.json` for each running CLI process.
bool IsClaudeCodeSessionRunning(const AgentSessionStoreContext& context, const std::wstring_view id)
{
	const auto directory = JoinPath(context.roots.claudeConfigDirectory, L"sessions");
	std::size_t inspected = 0;
	for (const auto& name : ListFileNames(directory)) {
		if (!EndsWithInsensitive(name, L".json") || ++inspected > kMaximumLiveSessionFiles) continue;
		const auto content = ReadWholeFile(JoinPath(directory, name), kMaximumLiveSessionFileBytes);
		if (!content) continue;
		const auto live = ParseClaudeCodeLiveSession(*content);
		if (live && live->sessionId == id && IsProcessRunning(context, live->processId)) return true;
	}
	return false;
}

struct ClaudeCodePaths final {
	std::wstring transcript;
	std::wstring sessionDirectory;	//! projects\<slug>\<id>
};

std::optional<ClaudeCodePaths> ResolveClaudeCodePaths(const AgentSessionStoreContext& context,
	const std::wstring_view folder, const std::wstring_view id)
{
	if (context.roots.claudeConfigDirectory.empty()) return std::nullopt;
	const auto project = JoinPath(JoinPath(context.roots.claudeConfigDirectory, L"projects"),
		ClaudeCodeProjectDirectoryName(folder));
	ClaudeCodePaths paths;
	paths.sessionDirectory = JoinPath(project, id);
	paths.transcript = paths.sessionDirectory + L".jsonl";
	return paths;
}

//! Appends one record. A file whose last line was cut gets a newline first,
//! so the record always starts its own line.
bool AppendRecord(const std::wstring& path, const std::string_view record)
{
	const ScopedFile file(::CreateFileW(path.c_str(), GENERIC_READ | FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	LARGE_INTEGER size{};
	if (!file.IsValid() || !::GetFileSizeEx(file.Get(), &size)) return false;
	std::string bytes;
	if (size.QuadPart > 0) {
		LARGE_INTEGER offset{};
		offset.QuadPart = size.QuadPart - 1;
		std::string last;
		if (!::SetFilePointerEx(file.Get(), offset, nullptr, FILE_BEGIN) || !ReadAll(file.Get(), last, 1)) return false;
		if (last.front() != '\n') bytes.push_back('\n');
	}
	bytes.append(record);
	return WriteAll(file.Get(), bytes) && ::FlushFileBuffers(file.Get()) != FALSE;
}

//! Replaces `path` through a sibling temporary file, so a reader never sees
//! half of the new content.
bool ReplaceFileContent(const std::wstring& path, const std::string_view content)
{
	const auto temporary = path + L".sakura-tmp";
	{
		const ScopedFile file(::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!file.IsValid() || !WriteAll(file.Get(), content) || !::FlushFileBuffers(file.Get())) {
			(void)::DeleteFileW(temporary.c_str());
			return false;
		}
	}
	if (::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
	(void)::DeleteFileW(temporary.c_str());
	return false;
}

EAgentSessionOperationStatus RenameClaudeCode(const AgentSessionRecord& session, const std::wstring_view folder,
	const std::wstring_view title, const AgentSessionStoreContext& context)
{
	const auto paths = ResolveClaudeCodePaths(context, folder, session.id);
	const auto record = BuildClaudeCodeCustomTitleRecord(session.id, title);
	const auto sidecar = BuildClaudeCodeCustomTitleSidecar(title);
	if (!paths || !record || !sidecar) return EAgentSessionOperationStatus::Failed;
	if (!Exists(paths->transcript, false)) return EAgentSessionOperationStatus::NotFound;
	if (IsClaudeCodeSessionRunning(context, session.id)) return EAgentSessionOperationStatus::SessionRunning;
	if (!AppendRecord(paths->transcript, *record)) return EAgentSessionOperationStatus::Failed;
	// The CLI's session list reads the sidecar before the transcript, so a
	// stale sidecar would hide the record just appended.
	if (!Exists(paths->sessionDirectory, true) && !::CreateDirectoryW(paths->sessionDirectory.c_str(), nullptr)
		&& ::GetLastError() != ERROR_ALREADY_EXISTS) return EAgentSessionOperationStatus::Failed;
	return ReplaceFileContent(JoinPath(paths->sessionDirectory, L"custom-title.json"), *sidecar)
		? EAgentSessionOperationStatus::Succeeded : EAgentSessionOperationStatus::Failed;
}

//! `sessions\--cwd--\<timestamp>_<id>.jsonl`.
std::optional<std::wstring> FindPiSessionFile(const AgentSessionStoreContext& context,
	const std::wstring_view folder, const std::wstring_view id)
{
	if (context.roots.piAgentDirectory.empty()) return std::nullopt;
	const auto directory = JoinPath(JoinPath(context.roots.piAgentDirectory, L"sessions"),
		PiSessionDirectoryName(folder));
	const auto suffix = L"_" + std::wstring(id) + L".jsonl";
	for (const auto& name : ListFileNames(directory)) {
		if (EndsWithInsensitive(name, suffix)) return JoinPath(directory, name);
	}
	return std::nullopt;
}

std::string NowIso8601Milliseconds()
{
	SYSTEMTIME now{};
	::GetSystemTime(&now);
	std::array<char, 32> text{};
	(void)std::snprintf(text.data(), text.size(), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
	return text.data();
}

//! Pi's generateId: 8 lowercase hex digits not used in the file yet.
std::optional<std::string> NewPiEntryId(const PiSessionChain& chain)
{
	static constexpr char kHex[] = "0123456789abcdef";
	std::random_device random;
	for (int attempt = 0; attempt < 100; ++attempt) {
		std::string id;
		std::uint32_t value = random();
		for (int digit = 0; digit < 8; ++digit) {
			id.push_back(kHex[value & 0x0fU]);
			value >>= 4;
		}
		if (std::ranges::find(chain.entryIds, id) == chain.entryIds.end()) return id;
	}
	return std::nullopt;
}

EAgentSessionOperationStatus RenamePi(const AgentSessionRecord& session, const std::wstring_view folder,
	const std::wstring_view title, const AgentSessionStoreContext& context)
{
	const auto path = FindPiSessionFile(context, folder, session.id);
	if (!path) return EAgentSessionOperationStatus::NotFound;
	// Pi appends without holding the file open, so writing stays shared. The
	// size is compared again right before the append: an entry Pi added while
	// this one was being built would otherwise share its parent.
	const ScopedFile file(::CreateFileW(path->c_str(), GENERIC_READ | FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	LARGE_INTEGER size{};
	if (!file.IsValid() || !::GetFileSizeEx(file.Get(), &size)) return EAgentSessionOperationStatus::Failed;
	if (static_cast<std::uint64_t>(size.QuadPart) > kMaximumPiSessionFileBytes) return EAgentSessionOperationStatus::Failed;
	std::string content;
	if (!ReadAll(file.Get(), content, static_cast<std::size_t>(size.QuadPart))) return EAgentSessionOperationStatus::Failed;
	const auto chain = ReadPiSessionChain(content, session.id);
	if (!chain) return EAgentSessionOperationStatus::Failed;
	const auto entryId = NewPiEntryId(*chain);
	if (!entryId) return EAgentSessionOperationStatus::Failed;
	const auto record = BuildPiSessionInfoRecord(*entryId, *chain, NowIso8601Milliseconds(), title);
	LARGE_INTEGER current{};
	if (!record || !::GetFileSizeEx(file.Get(), &current) || current.QuadPart != size.QuadPart) {
		return EAgentSessionOperationStatus::Failed;
	}
	return WriteAll(file.Get(), *record) && ::FlushFileBuffers(file.Get())
		? EAgentSessionOperationStatus::Succeeded : EAgentSessionOperationStatus::Failed;
}

bool IsBatchFile(const std::wstring_view path) noexcept
{
	return EndsWithInsensitive(path, L".cmd") || EndsWithInsensitive(path, L".bat");
}

//! A batch file is run by cmd.exe, which re-parses its command line. Only a
//! path without any character cmd.exe gives a meaning to is ever passed.
bool IsSafeForCommandProcessor(const std::wstring_view path) noexcept
{
	return !path.empty() && std::ranges::none_of(path, [](const wchar_t character) {
		return character < 0x20 || std::wstring_view(L"\"&|<>^%!()@").find(character) != std::wstring_view::npos;
	});
}

//! The request that runs `codex <arguments...>`. Every argument is a literal
//! or a validated session id, so nothing needs cmd.exe escaping.
std::optional<BoundedProcessRequest> CodexRequest(const AgentSessionStoreContext& context,
	const std::wstring_view folder, std::vector<std::wstring> arguments)
{
	const auto& codex = context.tools.codexCommand;
	if (codex.empty() || !platform::IsAbsoluteWindowsPath(codex)) return std::nullopt;
	if (!IsBatchFile(codex)) {
		BoundedProcessRequest request(codex, std::wstring(folder), std::move(arguments));
		request.SetTimeoutMilliseconds(context.commandTimeoutMilliseconds);
		return request;
	}
	const auto& processor = context.tools.commandProcessor;
	if (processor.empty() || !platform::IsAbsoluteWindowsPath(processor) || !IsSafeForCommandProcessor(codex)) {
		return std::nullopt;
	}
	std::vector<std::wstring> wrapped{ L"/d", L"/c", codex };
	for (auto& argument : arguments) wrapped.push_back(std::move(argument));
	BoundedProcessRequest request(processor, std::wstring(folder), std::move(wrapped));
	request.SetTimeoutMilliseconds(context.commandTimeoutMilliseconds);
	return request;
}

BoundedProcessResult Run(const AgentSessionStoreContext& context, const BoundedProcessRequest& request,
	const HANDLE stop)
{
	return context.services.run ? context.services.run(request, stop)
		: platform::process::RunBoundedProcess(request, stop);
}

EAgentSessionOperationStatus CommandStatus(const BoundedProcessResult& result) noexcept
{
	switch (result.Status()) {
	case EBoundedProcessStatus::Succeeded: return EAgentSessionOperationStatus::Succeeded;
	case EBoundedProcessStatus::Cancelled: return EAgentSessionOperationStatus::Cancelled;
	case EBoundedProcessStatus::InvalidRequest:
	case EBoundedProcessStatus::LaunchFailed: return EAgentSessionOperationStatus::Unsupported;
	default: return EAgentSessionOperationStatus::Failed;
	}
}

//! Lets the app-server reach EOF once it has answered thread/name/set.
class CodexRenameObserver final : public platform::process::IBoundedProcessOutputObserver {
public:
	explicit CodexRenameObserver(const HANDLE closeInput) noexcept : m_closeInput(closeInput) {}
	bool OnOutput(const EBoundedProcessStream stream, const std::span<const std::uint8_t> bytes) override
	{
		if (stream != EBoundedProcessStream::StandardOutput || m_reply != ECodexRenameReply::Pending) return true;
		if (m_output.size() + bytes.size() > kMaximumCodexReplyBytes) return false;
		m_output.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		m_reply = ParseCodexRenameReplies(m_output);
		if (m_reply != ECodexRenameReply::Pending) (void)::SetEvent(m_closeInput);
		return true;
	}
	[[nodiscard]] ECodexRenameReply Reply() const noexcept { return m_reply; }
private:
	HANDLE m_closeInput{};
	std::string m_output;
	ECodexRenameReply m_reply{ ECodexRenameReply::Pending };
};

EAgentSessionOperationStatus RenameCodex(const AgentSessionRecord& session, const std::wstring_view folder,
	const std::wstring_view title, const AgentSessionStoreContext& context, const HANDLE stop)
{
	const auto requests = BuildCodexRenameRequests(session.id, title);
	if (!requests) return EAgentSessionOperationStatus::Failed;
	auto request = CodexRequest(context, folder, { L"app-server" });
	if (!request) return EAgentSessionOperationStatus::Unsupported;
	const HANDLE closeInput = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (closeInput == nullptr) return EAgentSessionOperationStatus::Failed;
	const auto observer = std::make_shared<CodexRenameObserver>(closeInput);
	request->SetStandardInput(*requests);
	request->SetStandardInputCloseEvent(closeInput);
	request->SetOutputObserver(observer);
	request->SetMaximumStandardOutputBytes(kMaximumCodexReplyBytes);
	const auto result = Run(context, *request, stop);
	::CloseHandle(closeInput);
	// The reply decides: once thread/name/set answered, the name is stored,
	// however the server process ends afterwards.
	if (observer->Reply() == ECodexRenameReply::Succeeded) return EAgentSessionOperationStatus::Succeeded;
	if (observer->Reply() == ECodexRenameReply::Failed) return EAgentSessionOperationStatus::Failed;
	const auto status = CommandStatus(result);
	return status == EAgentSessionOperationStatus::Succeeded ? EAgentSessionOperationStatus::Failed : status;
}

bool MoveToShellRecycleBin(const std::vector<std::wstring>& paths)
{
	// pFrom is a double-NUL-terminated list, and SHFileOperationW rejects
	// extended-length paths. A path it cannot take fails the whole operation
	// instead of being deleted permanently.
	std::wstring list;
	for (const auto& path : paths) {
		if (path.empty() || path.size() >= MAX_PATH || path.starts_with(L"\\\\?\\")) return false;
		list.append(path);
		list.push_back(L'\0');
	}
	if (list.empty()) return false;
	list.push_back(L'\0');
	const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	SHFILEOPSTRUCTW operation{};
	operation.wFunc = FO_DELETE;
	operation.pFrom = list.c_str();
	operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	const int status = ::SHFileOperationW(&operation);
	if (SUCCEEDED(apartment)) ::CoUninitialize();
	return status == 0 && !operation.fAnyOperationsAborted;
}

EAgentSessionOperationStatus Recycle(const AgentSessionStoreContext& context, const std::vector<std::wstring>& paths)
{
	const bool moved = context.services.moveToRecycleBin
		? context.services.moveToRecycleBin(paths) : MoveToShellRecycleBin(paths);
	return moved ? EAgentSessionOperationStatus::Succeeded : EAgentSessionOperationStatus::Failed;
}

EAgentSessionOperationStatus DeleteClaudeCode(const AgentSessionRecord& session, const std::wstring_view folder,
	const AgentSessionStoreContext& context)
{
	const auto paths = ResolveClaudeCodePaths(context, folder, session.id);
	if (!paths) return EAgentSessionOperationStatus::Failed;
	if (!Exists(paths->transcript, false)) return EAgentSessionOperationStatus::NotFound;
	if (IsClaudeCodeSessionRunning(context, session.id)) return EAgentSessionOperationStatus::SessionRunning;
	// Everything Claude Code 2.1 keys by this session id.
	std::vector<std::wstring> targets{ paths->transcript };
	for (const auto& directory : {
		paths->sessionDirectory,
		JoinPath(JoinPath(context.roots.claudeConfigDirectory, L"file-history"), session.id),
		JoinPath(JoinPath(context.roots.claudeConfigDirectory, L"session-env"), session.id) }) {
		if (Exists(directory, true)) targets.push_back(directory);
	}
	return Recycle(context, targets);
}

EAgentSessionOperationStatus DeletePi(const AgentSessionRecord& session, const std::wstring_view folder,
	const AgentSessionStoreContext& context)
{
	const auto path = FindPiSessionFile(context, folder, session.id);
	if (!path) return EAgentSessionOperationStatus::NotFound;
	return Recycle(context, { *path });
}

} // namespace

AgentSessionCommandTools ResolveAgentSessionCommandTools()
{
	AgentSessionCommandTools tools;
	if (auto codex = platform::ResolveWindowsExecutable(L"codex.exe")) tools.codexCommand = std::move(*codex);
	else if (auto shim = platform::ResolveWindowsExecutable(L"codex.cmd")) tools.codexCommand = std::move(*shim);
	std::wstring processor(MAX_PATH, L'\0');
	const DWORD length = ::GetEnvironmentVariableW(L"ComSpec", processor.data(), static_cast<DWORD>(processor.size()));
	if (length > 0 && length < processor.size()) {
		processor.resize(length);
		if (platform::IsAbsoluteWindowsPath(processor)) tools.commandProcessor = std::move(processor);
	}
	return tools;
}

EAgentSessionOperationStatus RenameAgentSession(const AgentSessionRecord& session,
	const std::wstring_view folder, const std::wstring_view title,
	const AgentSessionStoreContext& context, const HANDLE stop)
{
	try {
		if (!IsValidAgentSessionId(session.id) || !platform::IsAbsoluteWindowsPath(folder)
			|| !PrepareAgentSessionRenameTitle(title)) return EAgentSessionOperationStatus::Failed;
		if (IsStopped(stop)) return EAgentSessionOperationStatus::Cancelled;
		switch (session.provider) {
		case EAgentSessionProvider::ClaudeCode: return RenameClaudeCode(session, folder, title, context);
		case EAgentSessionProvider::Codex: return RenameCodex(session, folder, title, context, stop);
		case EAgentSessionProvider::Pi: return RenamePi(session, folder, title, context);
		case EAgentSessionProvider::Devin: return EAgentSessionOperationStatus::Unsupported;
		}
		return EAgentSessionOperationStatus::Unsupported;
	} catch (...) {
		return EAgentSessionOperationStatus::Failed;
	}
}

EAgentSessionOperationStatus DeleteAgentSession(const AgentSessionRecord& session,
	const std::wstring_view folder, const AgentSessionStoreContext& context, const HANDLE stop)
{
	try {
		if (!IsValidAgentSessionId(session.id) || !platform::IsAbsoluteWindowsPath(folder)) {
			return EAgentSessionOperationStatus::Failed;
		}
		if (IsStopped(stop)) return EAgentSessionOperationStatus::Cancelled;
		switch (session.provider) {
		case EAgentSessionProvider::ClaudeCode: return DeleteClaudeCode(session, folder, context);
		case EAgentSessionProvider::Pi: return DeletePi(session, folder, context);
		case EAgentSessionProvider::Codex: {
			auto request = CodexRequest(context, folder, { L"delete", L"--force", session.id });
			if (!request) return EAgentSessionOperationStatus::Unsupported;
			return CommandStatus(Run(context, *request, stop));
		}
		case EAgentSessionProvider::Devin: {
			const auto& devin = context.roots.devinExecutable;
			if (devin.empty() || !platform::IsAbsoluteWindowsPath(devin)) return EAgentSessionOperationStatus::Unsupported;
			BoundedProcessRequest request(devin, std::wstring(folder), { L"rm", session.id, L"--force" });
			request.SetTimeoutMilliseconds(context.commandTimeoutMilliseconds);
			return CommandStatus(Run(context, request, stop));
		}
		}
		return EAgentSessionOperationStatus::Unsupported;
	} catch (...) {
		return EAgentSessionOperationStatus::Failed;
	}
}

} // namespace workbench::projects
