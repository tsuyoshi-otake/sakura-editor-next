/*! @file
 * @brief Safe, bounded runner for the fixed GitHub CLI account query.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "platform/process/WindowsExecutableResolver.h"
#include "workbench/account/AccountDiscovery.h"

#include <algorithm>
#include <array>
#include <utility>

namespace workbench::account {
namespace {

constexpr DWORD kPollMilliseconds = 10;

class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE handle) noexcept : m_handle(handle) {}
	~ScopedHandle() { Reset(); }

	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept
		: m_handle(std::exchange(other.m_handle, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) {
			Reset();
			m_handle = std::exchange(other.m_handle, nullptr);
		}
		return *this;
	}

	void Reset(HANDLE handle = nullptr) noexcept
	{
		if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) ::CloseHandle(m_handle);
		m_handle = handle;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_handle; }
	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
	}

private:
	HANDLE m_handle{};
};

bool NeedsQuoting(std::wstring_view value) noexcept
{
	return value.empty() || value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
}

std::wstring QuoteWindowsArgument(std::wstring_view value)
{
	if (!NeedsQuoting(value)) return std::wstring(value);
	std::wstring quoted;
	quoted.reserve(value.size() + 8);
	quoted.push_back(L'"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		std::size_t backslashes = 0;
		while (index < value.size() && value[index] == L'\\') {
			++backslashes;
			++index;
		}
		if (index == value.size()) {
			quoted.append(backslashes * 2, L'\\');
			break;
		}
		if (value[index] == L'"') {
			quoted.append(backslashes * 2 + 1, L'\\');
			quoted.push_back(L'"');
		} else {
			quoted.append(backslashes, L'\\');
			quoted.push_back(value[index]);
		}
	}
	quoted.push_back(L'"');
	return quoted;
}

std::wstring BuildCommandLine(std::wstring_view executable,
	const std::vector<std::wstring>& arguments)
{
	std::wstring commandLine = QuoteWindowsArgument(executable);
	for (const auto& argument : arguments) {
		commandLine.push_back(L' ');
		commandLine += QuoteWindowsArgument(argument);
	}
	return commandLine;
}

bool DrainPipe(HANDLE pipe, std::vector<std::uint8_t>& output, const std::size_t budget)
{
	for (;;) {
		DWORD available = 0;
		if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return true;
		if (output.size() >= budget) return false;
		std::array<std::uint8_t, 16384> buffer{};
		const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
		DWORD read = 0;
		if (!::ReadFile(pipe, buffer.data(), wanted, &read, nullptr) || read == 0) return true;
		const std::size_t room = budget - output.size();
		if (read > room) {
			output.insert(output.end(), buffer.begin(), buffer.begin() + room);
			return false;
		}
		output.insert(output.end(), buffer.begin(), buffer.begin() + read);
	}
}

void DrainAndDiscard(HANDLE pipe)
{
	std::array<std::uint8_t, 16384> buffer{};
	for (;;) {
		DWORD available = 0;
		if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return;
		const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
		DWORD read = 0;
		if (!::ReadFile(pipe, buffer.data(), wanted, &read, nullptr) || read == 0) return;
	}
}

//! Preserve the user's gh configuration/authentication variables, but make
//! this non-interactive JSON query deterministic. In particular, inherited
//! GH_DEBUG is suppressed because API debug output can contain request
//! metadata, while the output contract intentionally contains no diagnostics.
std::vector<wchar_t> BuildEnvironmentBlock()
{
	static constexpr std::array<std::wstring_view, 6> kOwnedNames{
		L"GH_DEBUG=",
		L"GH_FORCE_TTY=",
		L"GH_NO_UPDATE_NOTIFIER=",
		L"GH_PAGER=",
		L"GH_PROMPT_DISABLED=",
		L"NO_COLOR=",
	};
	static constexpr std::array<std::wstring_view, 5> kOwnedEntries{
		L"GH_FORCE_TTY=0",
		L"GH_NO_UPDATE_NOTIFIER=1",
		L"GH_PAGER=cat",
		L"GH_PROMPT_DISABLED=1",
		L"NO_COLOR=1",
	};

	std::vector<wchar_t> block;
	LPWCH parent = ::GetEnvironmentStringsW();
	if (parent != nullptr) {
		for (const wchar_t* cursor = parent; *cursor != L'\0';) {
			const std::wstring_view entry{ cursor };
			cursor += entry.size() + 1;
			const bool owned = std::any_of(kOwnedNames.begin(), kOwnedNames.end(),
				[entry](const std::wstring_view name) {
					return entry.size() >= name.size()
						&& ::CompareStringOrdinal(entry.data(), static_cast<int>(name.size()),
							name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
				});
			if (owned) continue;
			block.insert(block.end(), entry.begin(), entry.end());
			block.push_back(L'\0');
		}
		::FreeEnvironmentStringsW(parent);
	}
	for (const auto entry : kOwnedEntries) {
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

GhAuthStatusResult Terminal(EAccountCommandStatus status)
{
	GhAuthStatusResult result;
	result.status = status;
	return result;
}

} // namespace

GhAuthStatusResult RunGhAuthStatus(const GhAuthStatusRequest& request, HANDLE stop)
{
	if (!request.IsValid()) return Terminal(EAccountCommandStatus::InvalidRequest);
	const auto executable = platform::ResolveWindowsExecutable(L"gh.exe");
	if (!executable) return Terminal(EAccountCommandStatus::Unavailable);

	const auto arguments = BuildGhAuthStatusArguments();
	const auto commandLine = BuildCommandLine(*executable, arguments);
	if (commandLine.size() > 32000) return Terminal(EAccountCommandStatus::InvalidRequest);

	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle outputRead;
	ScopedHandle outputWrite;
	ScopedHandle errorRead;
	ScopedHandle errorWrite;
	if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0)
		|| !::CreatePipe(errorRead.Put(), errorWrite.Put(), &security, 0)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}
	if (!::SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0)
		|| !::SetHandleInformation(errorRead.Get(), HANDLE_FLAG_INHERIT, 0)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}

	// A closed NUL input prevents a broken credential prompt from waiting on
	// the editor's console. The gh request itself is read-only and never needs
	// stdin, so no caller-controlled input reaches it.
	ScopedHandle nullInput(::CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!nullInput.IsValid()) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}

	ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
	if (!job.IsValid()) return Terminal(EAccountCommandStatus::LaunchFailed);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
	jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
		&jobLimits, sizeof(jobLimits))) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}

	SIZE_T attributeBytes = 0;
	(void)::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
	if (attributeBytes == 0) return Terminal(EAccountCommandStatus::LaunchFailed);
	std::vector<std::uint8_t> attributeStorage(attributeBytes);
	auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if (!::InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}
	struct AttributeListGuard final {
		LPPROC_THREAD_ATTRIBUTE_LIST value{};
		~AttributeListGuard() { if (value != nullptr) ::DeleteProcThreadAttributeList(value); }
	} attributeGuard{ attributes };

	const std::array<HANDLE, 3> inheritedHandles{ nullInput.Get(), outputWrite.Get(), errorWrite.Get() };
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		const_cast<HANDLE*>(inheritedHandles.data()), sizeof(inheritedHandles), nullptr, nullptr)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}
	HANDLE jobHandle = job.Get();
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
		&jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullInput.Get();
	startup.StartupInfo.hStdOutput = outputWrite.Get();
	startup.StartupInfo.hStdError = errorWrite.Get();
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION process{};
	std::wstring mutableCommandLine = commandLine;
	auto environment = BuildEnvironmentBlock();
	const BOOL created = ::CreateProcessW(executable->c_str(), mutableCommandLine.data(),
		nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
		| EXTENDED_STARTUPINFO_PRESENT,
		environment.data(), request.workingDirectory.c_str(), &startup.StartupInfo, &process);
	outputWrite.Reset();
	errorWrite.Reset();
	nullInput.Reset();
	if (!created) return Terminal(EAccountCommandStatus::LaunchFailed);

	ScopedHandle processHandle(process.hProcess);
	ScopedHandle threadHandle(process.hThread);
	std::vector<std::uint8_t> output;
	output.reserve(std::min<std::size_t>(request.maximumOutputBytes, 64u * 1024u));
	const ULONGLONG deadline = ::GetTickCount64() + request.timeoutMilliseconds;
	auto status = EAccountCommandStatus::Succeeded;
	bool exited = false;
	for (;;) {
		if (!DrainPipe(outputRead.Get(), output, request.maximumOutputBytes)) {
			status = EAccountCommandStatus::OutputLimitExceeded;
			break;
		}
		DrainAndDiscard(errorRead.Get());
		if (exited) break;
		if (stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
			status = EAccountCommandStatus::Cancelled;
			break;
		}
		if (::GetTickCount64() >= deadline) {
			status = EAccountCommandStatus::TimedOut;
			break;
		}
		exited = ::WaitForSingleObject(processHandle.Get(), kPollMilliseconds) == WAIT_OBJECT_0;
	}
	if (!exited) {
		if (!::TerminateJobObject(job.Get(), ERROR_CANCELLED)) {
			(void)::TerminateProcess(processHandle.Get(), ERROR_CANCELLED);
		}
		(void)::WaitForSingleObject(processHandle.Get(), 1000);
	}

	GhAuthStatusResult result;
	std::string standardOutput;
	if (!output.empty()) standardOutput.assign(
		reinterpret_cast<const char*>(output.data()), output.size());
	if (status != EAccountCommandStatus::Succeeded) {
		result.status = status;
	} else {
		DWORD exitCode = 0;
		if (!::GetExitCodeProcess(processHandle.Get(), &exitCode)) {
			result.status = EAccountCommandStatus::Failed;
		} else {
			result.exitCode = static_cast<int>(exitCode);
			result.status = exitCode == 0 ? EAccountCommandStatus::Succeeded : EAccountCommandStatus::Failed;
		}
	}
	if (!standardOutput.empty()) {
		const auto parsed = ParseGitHubAuthStatus(standardOutput, request.limits);
		result.parseStatus = parsed.status;
		result.accounts = parsed.accounts;
	}
	return result;
}

} // namespace workbench::account
