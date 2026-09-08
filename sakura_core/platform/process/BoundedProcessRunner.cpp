/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/process/BoundedProcessRunner.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <exception>
#include <thread>

namespace platform::process {
namespace {

constexpr DWORD kPollMilliseconds = 10;

class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE handle) noexcept : m_handle(handle) {}
	~ScopedHandle() { Reset(); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.Release()) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) Reset(other.Release());
		return *this;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_handle; }
	[[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
	[[nodiscard]] HANDLE Release() noexcept
	{
		const HANDLE handle = m_handle;
		m_handle = nullptr;
		return handle;
	}
	void Reset(HANDLE handle = nullptr) noexcept
	{
		if (IsValid()) ::CloseHandle(m_handle);
		m_handle = handle;
	}

private:
	HANDLE m_handle{};
};

struct AttributeListGuard final {
	LPPROC_THREAD_ATTRIBUTE_LIST value{};
	~AttributeListGuard() { if (value != nullptr) ::DeleteProcThreadAttributeList(value); }
};

class WideEnvironmentGuard final {
public:
	explicit WideEnvironmentGuard(std::vector<std::wstring>& values) noexcept : m_values(&values) {}
	explicit WideEnvironmentGuard(std::vector<wchar_t>& block) noexcept : m_block(&block) {}
	~WideEnvironmentGuard()
	{
		if (m_values) {
			for (auto& value : *m_values) {
				if (!value.empty()) ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
			}
		}
		if (m_block && !m_block->empty()) {
			::SecureZeroMemory(m_block->data(), m_block->size() * sizeof(wchar_t));
		}
	}
	WideEnvironmentGuard(const WideEnvironmentGuard&) = delete;
	WideEnvironmentGuard& operator=(const WideEnvironmentGuard&) = delete;
private:
	std::vector<std::wstring>* m_values{};
	std::vector<wchar_t>* m_block{};
};

bool NeedsQuoting(std::wstring_view value) noexcept
{
	return value.empty() || value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
}

bool IsValidEnvironmentOverride(const std::pair<std::wstring, std::wstring>& entry) noexcept
{
	return !entry.first.empty() && entry.first.front() != L'=' && entry.first.find(L'=') == std::wstring::npos
		&& entry.first.size() <= kMaximumBoundedProcessArgumentLength
		&& entry.second.size() <= kMaximumBoundedProcessArgumentLength
		&& entry.first.find(L'\0') == std::wstring::npos
		&& entry.second.find(L'\0') == std::wstring::npos;
}

bool IsAbsoluteWindowsPath(std::wstring_view value) noexcept
{
	if (value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z')
		|| (value[0] >= L'a' && value[0] <= L'z'))
		&& value[1] == L':' && (value[2] == L'\\' || value[2] == L'/')) return true;
	return value.size() >= 3 && value[0] == L'\\' && value[1] == L'\\' && value[2] != L'\\';
}

bool EqualEnvironmentName(std::wstring_view left, std::wstring_view right) noexcept
{
	return left.size() == right.size()
		&& ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::vector<wchar_t> BuildEnvironmentBlock(
	const std::vector<std::pair<std::wstring, std::wstring>>& overrides,
	const std::vector<std::wstring>& removals)
{
	std::vector<std::wstring> entries;
	const WideEnvironmentGuard entriesGuard(entries);
	wchar_t* const parent = ::GetEnvironmentStringsW();
	if (parent != nullptr) {
		for (const wchar_t* cursor = parent; *cursor != L'\0';) {
			const std::wstring_view entry{ cursor };
			cursor += entry.size() + 1;
			const auto equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
			const auto name = equals == std::wstring_view::npos ? entry : entry.substr(0, equals);
			const bool replaced = std::any_of(overrides.begin(), overrides.end(),
				[name](const auto& overrideEntry) { return EqualEnvironmentName(name, overrideEntry.first); });
			const bool removed = std::any_of(removals.begin(), removals.end(),
				[name](const auto& removal) { return EqualEnvironmentName(name, removal); });
			if (!replaced && !removed) entries.emplace_back(entry);
		}
		::FreeEnvironmentStringsW(parent);
	}
	for (const auto& [name, value] : overrides) entries.push_back(name + L"=" + value);
	std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
		return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
			[](wchar_t a, wchar_t b) { return std::towupper(a) < std::towupper(b); });
	});

	std::vector<wchar_t> block;
	for (const auto& entry : entries) {
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

bool DrainPipe(HANDLE pipe, std::vector<std::uint8_t>& sink, std::size_t budget)
{
	for (;;) {
		DWORD available{};
		if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) return true;
		if (sink.size() >= budget) return false;
		std::array<std::uint8_t, 16384> buffer{};
		const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
		DWORD read{};
		if (!::ReadFile(pipe, buffer.data(), wanted, &read, nullptr) || read == 0) return true;
		const std::size_t room = budget - sink.size();
		if (read > room) {
			sink.insert(sink.end(), buffer.begin(), buffer.begin() + room);
			return false;
		}
		sink.insert(sink.end(), buffer.begin(), buffer.begin() + read);
	}
}

BoundedProcessResult Terminal(EBoundedProcessStatus status)
{
	return BoundedProcessResult(status);
}

std::size_t QuotedLength(std::wstring_view value) noexcept
{
	if (!NeedsQuoting(value)) return value.size();
	std::size_t length = 2;
	for (std::size_t index = 0; index < value.size(); ++index) {
		std::size_t backslashes{};
		while (index < value.size() && value[index] == L'\\') { ++backslashes; ++index; }
		if (index == value.size()) return length + backslashes * 2;
		length += value[index] == L'"' ? backslashes * 2 + 2 : backslashes + 1;
	}
	return length;
}

bool CommandLineLengthFits(const BoundedProcessRequest& request) noexcept
{
	std::size_t length = QuotedLength(request.ExecutablePath());
	for (const auto& argument : request.Arguments()) {
		const std::size_t argumentLength = QuotedLength(argument);
		if (length >= kMaximumBoundedProcessCommandLineLength
			|| argumentLength > kMaximumBoundedProcessCommandLineLength - length - 1) return false;
		length += argumentLength + 1;
	}
	return length <= kMaximumBoundedProcessCommandLineLength;
}

} // namespace

BoundedProcessRequest::BoundedProcessRequest(std::wstring executablePath, std::wstring workingDirectory,
	std::vector<std::wstring> arguments)
	: m_executablePath(std::move(executablePath)),
	  m_workingDirectory(std::move(workingDirectory)),
	  m_arguments(std::move(arguments))
{
}

BoundedProcessRequest::~BoundedProcessRequest()
{
	if (!m_standardInput.empty()) ::SecureZeroMemory(m_standardInput.data(), m_standardInput.size());
	for (auto& entry : m_environmentOverrides) {
		if (!entry.second.empty()) ::SecureZeroMemory(entry.second.data(), entry.second.size() * sizeof(wchar_t));
	}
}

std::wstring QuoteWindowsArgument(std::wstring_view value)
{
	if (!NeedsQuoting(value)) return std::wstring(value);
	std::wstring quoted;
	quoted.reserve(value.size() + 8);
	quoted.push_back(L'"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		std::size_t backslashes = 0;
		while (index < value.size() && value[index] == L'\\') { ++backslashes; ++index; }
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

std::wstring BuildWindowsCommandLine(
	std::wstring_view executable, const std::vector<std::wstring>& arguments)
{
	std::wstring line = QuoteWindowsArgument(executable);
	for (const auto& argument : arguments) {
		line.push_back(L' ');
		line += QuoteWindowsArgument(argument);
	}
	return line;
}

bool IsExecutableBoundedProcessRequest(const BoundedProcessRequest& request) noexcept
{
	if (!IsAbsoluteWindowsPath(request.ExecutablePath())
		|| !IsAbsoluteWindowsPath(request.WorkingDirectory()) || request.Arguments().empty()) return false;
	if (request.Arguments().size() > kMaximumBoundedProcessArguments
		|| request.EnvironmentOverrides().size() + request.EnvironmentRemovals().size()
			> kMaximumBoundedProcessEnvironmentOverrides) return false;
	if (request.StandardInput().size() > kMaximumBoundedProcessStandardInputBytes) return false;
	if (request.TimeoutMilliseconds() == 0
		|| request.MaximumStandardOutputBytes() == 0 || request.MaximumStandardErrorBytes() == 0
		|| request.MaximumStandardOutputBytes() > kMaximumBoundedProcessOutputBytes
		|| request.MaximumStandardErrorBytes() > kMaximumBoundedProcessErrorBytes) return false;
	if (request.ExecutablePath().size() > kMaximumBoundedProcessArgumentLength
		|| request.WorkingDirectory().size() > kMaximumBoundedProcessArgumentLength) return false;
	if (std::any_of(request.Arguments().begin(), request.Arguments().end(),
		[](const std::wstring& value) { return value.size() > kMaximumBoundedProcessArgumentLength; })) return false;
	if (std::any_of(request.EnvironmentOverrides().begin(), request.EnvironmentOverrides().end(),
		[](const auto& entry) { return !IsValidEnvironmentOverride(entry); })) return false;
	if (std::any_of(request.EnvironmentRemovals().begin(), request.EnvironmentRemovals().end(),
		[](const auto& name) { return name.empty() || name.front() == L'=' || name.find(L'=') != std::wstring::npos
			|| name.size() > kMaximumBoundedProcessArgumentLength || name.find(L'\0') != std::wstring::npos; })) return false;
	for (std::size_t index = 0; index < request.EnvironmentOverrides().size(); ++index) {
		for (std::size_t other = index + 1; other < request.EnvironmentOverrides().size(); ++other) {
			if (EqualEnvironmentName(request.EnvironmentOverrides()[index].first,
				request.EnvironmentOverrides()[other].first)) return false;
		}
	}
	for (std::size_t index = 0; index < request.EnvironmentRemovals().size(); ++index) {
		if (std::any_of(request.EnvironmentOverrides().begin(), request.EnvironmentOverrides().end(),
			[&](const auto& entry) { return EqualEnvironmentName(entry.first, request.EnvironmentRemovals()[index]); })) return false;
		for (std::size_t other = index + 1; other < request.EnvironmentRemovals().size(); ++other) {
			if (EqualEnvironmentName(request.EnvironmentRemovals()[index],
				request.EnvironmentRemovals()[other])) return false;
		}
	}
	return CommandLineLengthFits(request);
}

BoundedProcessResult RunBoundedProcess(const BoundedProcessRequest& request, HANDLE stop)
try {
	if (!IsExecutableBoundedProcessRequest(request)) return Terminal(EBoundedProcessStatus::InvalidRequest);

	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle outputRead, outputWrite, errorRead, errorWrite, inputRead, inputWrite;
	if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0)
		|| !::CreatePipe(errorRead.Put(), errorWrite.Put(), &security, 0)
		|| !::CreatePipe(inputRead.Put(), inputWrite.Put(), &security, 0)) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}
	if (!::SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0)
		|| !::SetHandleInformation(errorRead.Get(), HANDLE_FLAG_INHERIT, 0)
		|| !::SetHandleInformation(inputWrite.Get(), HANDLE_FLAG_INHERIT, 0)) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}

	ScopedHandle job(::CreateJobObjectW(nullptr, nullptr));
	if (!job.IsValid()) return Terminal(EBoundedProcessStatus::LaunchFailed);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}

	SIZE_T attributeBytes{};
	(void)::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
	if (attributeBytes == 0) return Terminal(EBoundedProcessStatus::LaunchFailed);
	std::vector<std::uint8_t> attributeStorage(attributeBytes);
	auto* const attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
	if (!::InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes)) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}
	AttributeListGuard attributeGuard{ attributes };
	const std::array<HANDLE, 3> inherited{ inputRead.Get(), outputWrite.Get(), errorWrite.Get() };
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
		const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr)) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}
	HANDLE jobHandle = job.Get();
	if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
		&jobHandle, sizeof(jobHandle), nullptr, nullptr)) {
		return Terminal(EBoundedProcessStatus::LaunchFailed);
	}

	auto environment = BuildEnvironmentBlock(request.EnvironmentOverrides(), request.EnvironmentRemovals());
	const WideEnvironmentGuard environmentGuard(environment);
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = inputRead.Get();
	startup.StartupInfo.hStdOutput = outputWrite.Get();
	startup.StartupInfo.hStdError = errorWrite.Get();
	startup.lpAttributeList = attributes;

	auto commandLine = BuildWindowsCommandLine(request.ExecutablePath(), request.Arguments());
	PROCESS_INFORMATION process{};
	const BOOL created = ::CreateProcessW(request.ExecutablePath().c_str(), commandLine.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment.data(),
		request.WorkingDirectory().c_str(), &startup.StartupInfo, &process);
	outputWrite.Reset();
	errorWrite.Reset();
	inputRead.Reset();
	if (!created) return Terminal(EBoundedProcessStatus::LaunchFailed);

	ScopedHandle processHandle(process.hProcess);
	ScopedHandle threadHandle(process.hThread);
	BoundedProcessResult result(EBoundedProcessStatus::Succeeded);
	result.m_standardOutput.reserve(request.MaximumStandardOutputBytes());
	result.m_standardError.reserve(request.MaximumStandardErrorBytes());
	std::jthread writer([ownedHandle = std::move(inputWrite), payload = request.StandardInput()]() {
		const HANDLE handle = ownedHandle.Get();
		std::size_t written{};
		while (written < payload.size()) {
			DWORD chunk{};
			const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(payload.size() - written, 16384));
			if (!::WriteFile(handle, payload.data() + written, wanted, &chunk, nullptr) || chunk == 0) break;
			written += chunk;
		}
	});

	const ULONGLONG deadline = ::GetTickCount64() + request.TimeoutMilliseconds();
	auto status = EBoundedProcessStatus::Succeeded;
	bool exited = false;
	for (;;) {
		if (!DrainPipe(outputRead.Get(), result.m_standardOutput, request.MaximumStandardOutputBytes())
			|| !DrainPipe(errorRead.Get(), result.m_standardError, request.MaximumStandardErrorBytes())) {
			status = EBoundedProcessStatus::OutputLimitExceeded;
			break;
		}
		if (exited) break;
		if (stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
			status = EBoundedProcessStatus::Cancelled;
			break;
		}
		if (::GetTickCount64() >= deadline) {
			status = EBoundedProcessStatus::TimedOut;
			break;
		}
		exited = ::WaitForSingleObject(processHandle.Get(), kPollMilliseconds) == WAIT_OBJECT_0;
	}
	if (!exited) {
		if (!::TerminateJobObject(job.Get(), ERROR_CANCELLED)) {
			(void)::TerminateProcess(processHandle.Get(), ERROR_CANCELLED);
		}
		(void)::WaitForSingleObject(processHandle.Get(), 1000);
	} else {
		// The root may exit while a descendant still owns stdin or another
		// inherited handle. End the owned tree before joining the writer.
		(void)::TerminateJobObject(job.Get(), ERROR_CANCELLED);
	}
	if (writer.joinable()) writer.join();

	if (status != EBoundedProcessStatus::Succeeded) {
		result.m_status = status;
		return result;
	}
	DWORD exitCode{};
	if (!::GetExitCodeProcess(processHandle.Get(), &exitCode)) {
		result.m_status = EBoundedProcessStatus::Failed;
		return result;
	}
	result.m_exitCode = static_cast<int>(exitCode);
	result.m_status = exitCode == 0 ? EBoundedProcessStatus::Succeeded : EBoundedProcessStatus::Failed;
	return result;
} catch (const std::exception&) {
	return Terminal(EBoundedProcessStatus::LaunchFailed);
}

} // namespace platform::process
