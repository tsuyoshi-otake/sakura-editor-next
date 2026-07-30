/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionHostProcess.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <windows.h>

namespace {

class UniqueHandle final {
public:
	UniqueHandle() = default;
	explicit UniqueHandle(HANDLE value) noexcept : m_value(value) {}
	~UniqueHandle() { Reset(); }
	UniqueHandle(const UniqueHandle&) = delete;
	UniqueHandle& operator=(const UniqueHandle&) = delete;
	UniqueHandle(UniqueHandle&& other) noexcept : m_value(other.Release()) {}
	UniqueHandle& operator=(UniqueHandle&& other) noexcept
	{
		if (this != &other) {
			Reset(other.Release());
		}
		return *this;
	}
	HANDLE Get() const noexcept { return m_value; }
	explicit operator bool() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }
	HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
	void Reset(HANDLE value = nullptr) noexcept
	{
		if (*this) {
			::CloseHandle(m_value);
		}
		m_value = value;
	}
private:
	HANDLE m_value = nullptr;
};

std::wstring Uppercase(std::wstring_view value)
{
	std::wstring result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towupper(character));
	});
	return result;
}

bool IsValidEnvironmentName(std::wstring_view name)
{
	return !name.empty() && name.front() != L'=' && name.find(L'=') == std::wstring_view::npos &&
		name.find(L'\0') == std::wstring_view::npos;
}

std::wstring FormatWindowsError(DWORD error)
{
	wchar_t* raw = nullptr;
	const DWORD count = ::FormatMessageW(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		error,
		0,
		reinterpret_cast<wchar_t*>(&raw),
		0,
		nullptr);
	std::wstring result = count && raw ? std::wstring(raw, count) : L"Windows error " + std::to_wstring(error);
	if (raw) {
		::LocalFree(raw);
	}
	while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
		result.pop_back();
	}
	return result;
}

std::vector<CExtensionHostProcess::EnvironmentEntry> ReadCurrentEnvironment()
{
	std::vector<CExtensionHostProcess::EnvironmentEntry> entries;
	wchar_t* block = ::GetEnvironmentStringsW();
	if (!block) {
		return entries;
	}
	for (const wchar_t* current = block; *current != L'\0'; current += std::wcslen(current) + 1) {
		const std::wstring_view line(current);
		const auto separator = line.find(L'=');
		if (separator == std::wstring_view::npos || separator == 0) {
			continue;
		}
		entries.emplace_back(std::wstring(line.substr(0, separator)), std::wstring(line.substr(separator + 1)));
	}
	::FreeEnvironmentStringsW(block);
	return entries;
}

SExtensionHostProcessStartResult Failure(DWORD error, std::wstring operation)
{
	return {
		false,
		0,
		error,
		std::move(operation) + L": " + FormatWindowsError(error),
	};
}

bool IsSafeIdentifier(std::wstring_view value)
{
	return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t character) {
		return (character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') || character == L'-' || character == L'_';
	});
}

} // namespace

class CExtensionHostProcess::Impl {
public:
	UniqueHandle job;
	UniqueHandle process;
	std::uint32_t processId = 0;
};

CExtensionHostProcess::CExtensionHostProcess()
	: m_impl(std::make_unique<Impl>())
{
}

CExtensionHostProcess::~CExtensionHostProcess()
{
	Terminate(ERROR_PROCESS_ABORTED);
}

std::wstring CExtensionHostProcess::QuoteWindowsArgument(std::wstring_view argument)
{
	if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
		return std::wstring(argument);
	}
	std::wstring result(1, L'"');
	std::size_t backslashes = 0;
	for (const wchar_t character : argument) {
		if (character == L'\\') {
			++backslashes;
			continue;
		}
		if (character == L'"') {
			result.append(backslashes * 2 + 1, L'\\');
			result.push_back(L'"');
			backslashes = 0;
			continue;
		}
		result.append(backslashes, L'\\');
		backslashes = 0;
		result.push_back(character);
	}
	result.append(backslashes * 2, L'\\');
	result.push_back(L'"');
	return result;
}

std::vector<wchar_t> CExtensionHostProcess::BuildSanitizedEnvironmentBlock(
	const std::vector<EnvironmentEntry>& inherited,
	const std::vector<EnvironmentEntry>& trustedOverrides)
{
	static const std::set<std::wstring> allowlist = {
		L"ALLUSERSPROFILE", L"APPDATA", L"COMSPEC", L"HOMEDRIVE", L"HOMEPATH",
		L"LOCALAPPDATA", L"NUMBER_OF_PROCESSORS", L"OS", L"PATH", L"PATHEXT",
		L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL",
		L"PROCESSOR_REVISION", L"PROGRAMDATA", L"PROGRAMFILES", L"PROGRAMFILES(X86)",
		L"PROGRAMW6432", L"SYSTEMDRIVE", L"SYSTEMROOT", L"TEMP", L"TMP",
		L"USERDOMAIN", L"USERNAME", L"USERPROFILE", L"WINDIR",
	};

	std::map<std::wstring, EnvironmentEntry> selected;
	for (const auto& [name, value] : inherited) {
		if (!IsValidEnvironmentName(name) || value.find(L'\0') != std::wstring::npos) {
			continue;
		}
		const auto upper = Uppercase(name);
		if (allowlist.contains(upper)) {
			selected[upper] = { name, value };
		}
	}
	for (const auto& [name, value] : trustedOverrides) {
		if (!IsValidEnvironmentName(name) || value.find(L'\0') != std::wstring::npos) {
			continue;
		}
		const auto upper = Uppercase(name);
		if (upper == L"NODE_OPTIONS" || upper == L"NODE_PATH") {
			continue;
		}
		selected[upper] = { name, value };
	}

	std::vector<wchar_t> block;
	for (const auto& [upper, entry] : selected) {
		(void)upper;
		block.insert(block.end(), entry.first.begin(), entry.first.end());
		block.push_back(L'=');
		block.insert(block.end(), entry.second.begin(), entry.second.end());
		block.push_back(L'\0');
	}
	if (block.empty()) {
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

SExtensionHostProcessStartResult CExtensionHostProcess::Start(const SExtensionHostLaunchOptions& options)
{
	if (m_impl->process) {
		return Failure(ERROR_ALREADY_EXISTS, L"Start extension host");
	}
	std::error_code nodeStatusError;
	std::error_code bundleStatusError;
	std::error_code shimStatusError;
	const bool nodeExists = std::filesystem::is_regular_file(options.nodeExecutable, nodeStatusError);
	const bool bundleExists = std::filesystem::is_regular_file(options.hostBundle, bundleStatusError);
	const bool shimExists = std::filesystem::is_regular_file(options.securityShim, shimStatusError);
	if (options.nodeExecutable.empty() || options.hostBundle.empty() || options.securityShim.empty() ||
		!nodeExists || !bundleExists || !shimExists) {
		return Failure(ERROR_FILE_NOT_FOUND, L"Validate extension host executable, bundle, and security shim");
	}
	const std::wstring expectedPipeName = L"\\\\.\\pipe\\sakura-exthost-" +
		options.profileHash + L"-" + options.bootId;
	if (!IsSafeIdentifier(options.profileHash) || !IsSafeIdentifier(options.bootId) ||
		options.pipeName != expectedPipeName || options.generation == 0 || options.brokerProcessId == 0) {
		return Failure(ERROR_INVALID_PARAMETER, L"Validate extension host identity");
	}

	UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
	if (!job) {
		return Failure(::GetLastError(), L"Create extension host job object");
	}
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
	jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!::SetInformationJobObject(
		job.Get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits))) {
		return Failure(::GetLastError(), L"Configure extension host job object");
	}

	SIZE_T attributeBytes = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
	if (attributeBytes == 0) {
		return Failure(::GetLastError(), L"Measure extension host process attributes");
	}
	std::vector<std::uint8_t> attributeStorage(attributeBytes);
	auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) {
		return Failure(::GetLastError(), L"Initialize extension host process attributes");
	}
	struct AttributeListGuard {
		LPPROC_THREAD_ATTRIBUTE_LIST value;
		~AttributeListGuard() { if (value) ::DeleteProcThreadAttributeList(value); }
	} attributeGuard{ attributes };
	HANDLE jobHandle = job.Get();
	if (!::UpdateProcThreadAttribute(
		attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
		return Failure(::GetLastError(), L"Attach extension host job attribute");
	}

	std::wstring commandLine = QuoteWindowsArgument(options.nodeExecutable.wstring());
	if (options.developerInspect) {
		commandLine += L" --inspect=127.0.0.1:0";
	}
	commandLine += L" --require ";
	commandLine += QuoteWindowsArgument(options.securityShim.wstring());
	commandLine.push_back(L' ');
	commandLine += QuoteWindowsArgument(options.hostBundle.wstring());

	const std::vector<EnvironmentEntry> trustedOverrides = {
		{ L"SAKURA_EXTENSION_HOST", L"1" },
		{ L"SAKURA_PROFILE_HASH", options.profileHash },
		{ L"SAKURA_BOOT_ID", options.bootId },
		{ L"SAKURA_PIPE_NAME", options.pipeName },
		{ L"SAKURA_GENERATION", std::to_wstring(options.generation) },
		{ L"SAKURA_BROKER_PID", std::to_wstring(options.brokerProcessId) },
	};
	auto environment = BuildSanitizedEnvironmentBlock(ReadCurrentEnvironment(), trustedOverrides);

	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullptr;
	startup.StartupInfo.hStdOutput = nullptr;
	startup.StartupInfo.hStdError = nullptr;
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION processInfo{};
	const auto workingDirectory = options.workingDirectory.empty()
		? options.hostBundle.parent_path()
		: options.workingDirectory;
	const DWORD creationFlags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
		CREATE_NO_WINDOW;
	if (!::CreateProcessW(
		options.nodeExecutable.c_str(),
		commandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		creationFlags,
		environment.data(),
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
		&startup.StartupInfo,
		&processInfo)) {
		return Failure(::GetLastError(), L"Create extension host process");
	}

	UniqueHandle process(processInfo.hProcess);
	UniqueHandle primaryThread(processInfo.hThread);
	m_impl->processId = processInfo.dwProcessId;
	m_impl->process = std::move(process);
	m_impl->job = std::move(job);
	return { true, m_impl->processId, 0, {} };
}

std::optional<std::uint32_t> CExtensionHostProcess::PollExitCode() const noexcept
{
	if (!m_impl->process) {
		return std::nullopt;
	}
	DWORD exitCode = STILL_ACTIVE;
	if (!::GetExitCodeProcess(m_impl->process.Get(), &exitCode) || exitCode == STILL_ACTIVE) {
		return std::nullopt;
	}
	return exitCode;
}

bool CExtensionHostProcess::WaitForExit(std::chrono::milliseconds timeout) noexcept
{
	if (!m_impl->process) {
		return true;
	}
	const auto count = std::clamp<std::int64_t>(
		timeout.count(), 0, static_cast<std::int64_t>(MAXDWORD) - 1);
	return ::WaitForSingleObject(m_impl->process.Get(), static_cast<DWORD>(count)) == WAIT_OBJECT_0;
}

void CExtensionHostProcess::Terminate(std::uint32_t exitCode) noexcept
{
	if (m_impl->job) {
		::TerminateJobObject(m_impl->job.Get(), exitCode);
	}
	if (m_impl->process) {
		::WaitForSingleObject(m_impl->process.Get(), 250);
	}
	m_impl->process.Reset();
	m_impl->job.Reset();
	m_impl->processId = 0;
}

std::uint32_t CExtensionHostProcess::GetProcessId() const noexcept
{
	return m_impl->processId;
}
