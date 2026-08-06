/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitCommandRunner.h"

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

namespace workbench::scm {
namespace {

//! One poll turn. Short enough that cancellation feels immediate, long enough
//! that a multi-second fetch does not spin a core.
constexpr DWORD kPollMilliseconds = 10;

bool NeedsQuoting(std::wstring_view value) noexcept
{
	if (value.empty()) return true;
	return value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
}

//! RAII for a handle that must not leak on any early return.
class ScopedHandle final {
public:
	ScopedHandle() = default;
	explicit ScopedHandle(HANDLE handle) noexcept : m_handle(handle) {}
	~ScopedHandle() { Reset(); }
	ScopedHandle(const ScopedHandle&) = delete;
	ScopedHandle& operator=(const ScopedHandle&) = delete;
	ScopedHandle(ScopedHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
	ScopedHandle& operator=(ScopedHandle&& other) noexcept
	{
		if (this != &other) { Reset(); m_handle = std::exchange(other.m_handle, nullptr); }
		return *this;
	}
	void Reset(HANDLE handle = nullptr) noexcept
	{
		if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) ::CloseHandle(m_handle);
		m_handle = handle;
	}
	[[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
	[[nodiscard]] HANDLE* Put() noexcept { return &m_handle; }
	[[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

private:
	HANDLE m_handle{};
};

//! Read whatever is already buffered on `pipe` without ever blocking.
//! Returns false when the caller's budget was exceeded.
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

//!
//! @brief Copy the parent environment, replacing the entries this runner owns.
//!
//! `GIT_TERMINAL_PROMPT=0` is the important one: without it a fetch, pull, or
//! push against a repository that needs credentials blocks forever on a prompt
//! nobody can see, and the editor would only ever observe a timeout.
//!
std::vector<wchar_t> BuildEnvironmentBlock()
{
	static constexpr std::array<std::wstring_view, 2> kOwnedNames{ L"GIT_TERMINAL_PROMPT=", L"GIT_FLUSH=" };
	static constexpr std::array<std::wstring_view, 2> kOwnedEntries{ L"GIT_TERMINAL_PROMPT=0", L"GIT_FLUSH=1" };

	std::vector<wchar_t> block;
	wchar_t* const parent = ::GetEnvironmentStringsW();
	if (parent != nullptr) {
		for (const wchar_t* cursor = parent; *cursor != L'\0';) {
			const std::wstring_view entry{ cursor };
			cursor += entry.size() + 1;
			const bool owned = std::any_of(kOwnedNames.begin(), kOwnedNames.end(),
				[entry](std::wstring_view name) {
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

std::string BoundedUtf8(const std::vector<std::uint8_t>& bytes)
{
	const std::size_t length = std::min(bytes.size(), kMaximumGitStandardErrorBytes);
	return std::string(reinterpret_cast<const char*>(bytes.data()), length);
}

GitExecutionResult Terminal(EGitExecutionStatus status)
{
	GitExecutionResult result;
	result.status = status;
	return result;
}

} // namespace

std::wstring QuoteGitArgument(std::wstring_view value)
{
	if (!NeedsQuoting(value)) return std::wstring(value);

	std::wstring quoted;
	quoted.reserve(value.size() + 8);
	quoted.push_back(L'"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		std::size_t backslashes = 0;
		while (index < value.size() && value[index] == L'\\') { ++backslashes; ++index; }
		if (index == value.size()) {
			// Trailing backslashes precede the closing quote, so they must be doubled.
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

std::wstring BuildGitCommandLine(std::wstring_view executable, const std::vector<std::wstring>& arguments)
{
	std::wstring line = QuoteGitArgument(executable);
	for (const auto& argument : arguments) {
		line.push_back(L' ');
		line += QuoteGitArgument(argument);
	}
	return line;
}

std::vector<std::wstring> BuildEffectiveGitArguments(const GitExecutionRequest& request)
{
	std::vector<std::wstring> effective;
	effective.reserve(request.arguments.size() + 2);
	effective.emplace_back(L"-C");
	effective.push_back(request.workingDirectory);
	effective.insert(effective.end(), request.arguments.begin(), request.arguments.end());
	return effective;
}

std::wstring ResolveGitExecutable()
{
	wchar_t path[MAX_PATH]{};
	const DWORD length = ::SearchPathW(nullptr, L"git.exe", nullptr, MAX_PATH, path, nullptr);
	return length != 0 && length < MAX_PATH ? std::wstring(path, length) : std::wstring{};
}

bool IsExecutableGitRequest(const GitExecutionRequest& request) noexcept
{
	if (request.workingDirectory.empty()) return false;
	if (request.arguments.empty()) return false;
	if (request.arguments.size() > kMaximumGitArguments) return false;
	if (request.standardInput.size() > kMaximumGitStandardInputBytes) return false;
	if (request.maximumOutputBytes == 0) return false;
	if (request.timeoutMilliseconds == 0) return false;
	if (request.workingDirectory.size() > kMaximumGitArgumentLength) return false;
	return std::none_of(request.arguments.begin(), request.arguments.end(),
		[](const std::wstring& argument) { return argument.size() > kMaximumGitArgumentLength; });
}

GitExecutionResult RunGit(const GitExecutionRequest& request, HANDLE stop)
{
	if (!IsExecutableGitRequest(request)) return Terminal(EGitExecutionStatus::InvalidRequest);

	const auto executable = ResolveGitExecutable();
	if (executable.empty()) return Terminal(EGitExecutionStatus::GitUnavailable);

	std::wstring commandLine = BuildGitCommandLine(executable, BuildEffectiveGitArguments(request));
	if (commandLine.size() > kMaximumGitCommandLineLength) return Terminal(EGitExecutionStatus::InvalidRequest);

	SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
	ScopedHandle outputRead;
	ScopedHandle outputWrite;
	ScopedHandle errorRead;
	ScopedHandle errorWrite;
	ScopedHandle inputRead;
	ScopedHandle inputWrite;
	if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0)) return Terminal(EGitExecutionStatus::LaunchFailed);
	if (!::CreatePipe(errorRead.Put(), errorWrite.Put(), &security, 0)) return Terminal(EGitExecutionStatus::LaunchFailed);
	if (!::CreatePipe(inputRead.Put(), inputWrite.Put(), &security, 0)) return Terminal(EGitExecutionStatus::LaunchFailed);
	// Only the child's ends may be inherited; ours must not leak into it.
	::SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0);
	::SetHandleInformation(errorRead.Get(), HANDLE_FLAG_INHERIT, 0);
	::SetHandleInformation(inputWrite.Get(), HANDLE_FLAG_INHERIT, 0);

	auto environment = BuildEnvironmentBlock();

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = inputRead.Get();
	startup.hStdOutput = outputWrite.Get();
	startup.hStdError = errorWrite.Get();

	PROCESS_INFORMATION process{};
	const BOOL created = ::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
		request.workingDirectory.c_str(), &startup, &process);
	// The child owns its ends now; holding them would keep the pipes from ever reporting EOF.
	outputWrite.Reset();
	errorWrite.Reset();
	inputRead.Reset();
	if (!created) return Terminal(EGitExecutionStatus::LaunchFailed);

	ScopedHandle processHandle(process.hProcess);
	ScopedHandle threadHandle(process.hThread);

	// stdin is written on its own thread: a commit message can exceed the pipe
	// buffer, and a blocking write here would deadlock against a child that is
	// waiting for us to drain its stdout.
	std::thread writer([handle = inputWrite.Get(), payload = request.standardInput]() {
		std::size_t written = 0;
		while (written < payload.size()) {
			DWORD chunk{};
			const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(payload.size() - written, 16384));
			if (!::WriteFile(handle, payload.data() + written, wanted, &chunk, nullptr) || chunk == 0) break;
			written += chunk;
		}
	});

	std::vector<std::uint8_t> output;
	std::vector<std::uint8_t> error;
	output.reserve(std::min<std::size_t>(request.maximumOutputBytes, 64u * 1024u));

	const ULONGLONG deadline = ::GetTickCount64() + request.timeoutMilliseconds;
	auto status = EGitExecutionStatus::Succeeded;
	bool exited = false;
	for (;;) {
		if (!DrainPipe(outputRead.Get(), output, request.maximumOutputBytes)) {
			status = EGitExecutionStatus::OutputLimitExceeded;
			break;
		}
		(void)DrainPipe(errorRead.Get(), error, kMaximumGitStandardErrorBytes);
		if (exited) break;
		if (stop != nullptr && ::WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
			status = EGitExecutionStatus::Cancelled;
			break;
		}
		if (::GetTickCount64() >= deadline) {
			status = EGitExecutionStatus::TimedOut;
			break;
		}
		// Drain once more after the exit is observed so the final bytes are never lost.
		exited = ::WaitForSingleObject(processHandle.Get(), kPollMilliseconds) == WAIT_OBJECT_0;
	}

	if (!exited) {
		::TerminateProcess(processHandle.Get(), ERROR_CANCELLED);
		(void)::WaitForSingleObject(processHandle.Get(), 1000);
	}
	// Closing our write end unblocks a writer still waiting on a child that quit early.
	inputWrite.Reset();
	if (writer.joinable()) writer.join();

	GitExecutionResult result;
	result.standardOutput = std::move(output);
	result.standardError = BoundedUtf8(error);
	if (status == EGitExecutionStatus::Succeeded) {
		DWORD exitCode{};
		if (!::GetExitCodeProcess(processHandle.Get(), &exitCode)) {
			result.status = EGitExecutionStatus::Failed;
			return result;
		}
		result.exitCode = static_cast<int>(exitCode);
		result.status = exitCode == 0 ? EGitExecutionStatus::Succeeded : EGitExecutionStatus::Failed;
		return result;
	}
	result.status = status;
	return result;
}

} // namespace workbench::scm
