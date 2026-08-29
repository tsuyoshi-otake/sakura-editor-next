/*! @file */
#include "StdAfx.h"
#include "terminal/session/TerminalEnvironment.h"
#include "terminal/session/TerminalSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <limits>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

namespace terminal {
namespace {

class UniqueHandle final {
public:
	UniqueHandle() = default;
	explicit UniqueHandle( HANDLE value ) noexcept : m_value(value) {}
	~UniqueHandle() { Reset(); }
	UniqueHandle( const UniqueHandle& ) = delete;
	UniqueHandle& operator=( const UniqueHandle& ) = delete;
	UniqueHandle( UniqueHandle&& other ) noexcept : m_value(other.Release()) {}
	UniqueHandle& operator=( UniqueHandle&& other ) noexcept {
		if( this != &other ) Reset(other.Release());
		return *this;
	}
	HANDLE Get() const noexcept { return m_value; }
	explicit operator bool() const noexcept { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }
	HANDLE Release() noexcept { return std::exchange(m_value, nullptr); }
	void Reset( HANDLE value = nullptr ) noexcept {
		if( *this ) ::CloseHandle(m_value);
		m_value = value;
	}
private:
	HANDLE m_value = nullptr;
};

class UniquePseudoConsole final {
public:
	UniquePseudoConsole() = default;
	explicit UniquePseudoConsole( HPCON value ) noexcept : m_value(value) {}
	~UniquePseudoConsole() { Reset(); }
	UniquePseudoConsole( const UniquePseudoConsole& ) = delete;
	UniquePseudoConsole& operator=( const UniquePseudoConsole& ) = delete;
	UniquePseudoConsole( UniquePseudoConsole&& other ) noexcept : m_value(other.Release()) {}
	UniquePseudoConsole& operator=( UniquePseudoConsole&& other ) noexcept {
		if( this != &other ) Reset(other.Release());
		return *this;
	}
	HPCON Get() const noexcept { return m_value; }
	explicit operator bool() const noexcept { return m_value != nullptr; }
	HPCON Release() noexcept { return std::exchange(m_value, nullptr); }
	void Reset( HPCON value = nullptr ) noexcept {
		if( m_value ) ::ClosePseudoConsole(m_value);
		m_value = value;
	}
private:
	HPCON m_value = nullptr;
};

std::wstring FormatWindowsError( DWORD error )
{
	wchar_t* raw = nullptr;
	const DWORD count = ::FormatMessageW( FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, error, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr );
	std::wstring result = count && raw ? std::wstring(raw, count) : L"Windows error " + std::to_wstring(error);
	if( raw ) ::LocalFree(raw);
	while( !result.empty() && (result.back() == L'\r' || result.back() == L'\n') ) result.pop_back();
	return result;
}

TerminalStartResult StartFailure( DWORD error, const wchar_t* operation )
{
	return TerminalStartResult::Failure( error, std::wstring(operation) + L": " + FormatWindowsError(error) );
}

std::wstring QuoteWindowsArgument( const std::wstring& argument )
{
	if( !argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos ) return argument;
	std::wstring result(1, L'\"');
	std::size_t backslashes = 0;
	for( const wchar_t character : argument ) {
		if( character == L'\\' ) {
			++backslashes;
			continue;
		}
		if( character == L'\"' ) {
			result.append(backslashes * 2 + 1, L'\\');
			result.push_back(character);
			backslashes = 0;
			continue;
		}
		result.append(backslashes, L'\\');
		backslashes = 0;
		result.push_back(character);
	}
	result.append(backslashes * 2, L'\\');
	result.push_back(L'\"');
	return result;
}

std::vector<wchar_t> BuildCommandLine( const TerminalLaunchOptions& options )
{
	std::wstring commandLine = QuoteWindowsArgument(options.executablePath);
	for( const auto& argument : options.arguments ) {
		commandLine.push_back(L' ');
		commandLine.append(QuoteWindowsArgument(argument));
	}
	std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
	mutableCommandLine.push_back(L'\0');
	return mutableCommandLine;
}

UniqueHandle DuplicateLocalHandle( HANDLE source )
{
	if( !source || source == INVALID_HANDLE_VALUE ) return {};
	HANDLE duplicate = nullptr;
	if( !::DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) ) return {};
	return UniqueHandle(duplicate);
}

std::optional<bool> IsJobEmpty( HANDLE job )
{
	if( !job ) return std::nullopt;
	JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
	if( !::QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ) return std::nullopt;
	return accounting.ActiveProcesses == 0;
}

bool HaveAllProcessesExited( HANDLE process, HANDLE job ) noexcept
{
	const bool rootExited = !process || ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
	// A newly created process can briefly be absent from job accounting even
	// though its process handle is still signalled as running.  Treat the job as
	// an additional descendant gate, never as a replacement for the root-process
	// handle, otherwise startup can be mistaken for EOF after the drain timeout.
	if( process && !rootExited ) return false;
	if( job ) {
		if( const auto jobEmpty = IsJobEmpty(job) ) return *jobEmpty;
	}
	return rootExited;
}

void JoinPseudoConsoleClose( std::thread& worker ) noexcept
{
	if( !worker.joinable() ) return;
	// Closing pipe clients and the kill-on-close job happens before this join.
	// It is therefore safe to wait indefinitely: an externally reported close
	// result must not claim quiescence while ClosePseudoConsole is still live.
	const auto handle = reinterpret_cast<HANDLE>(worker.native_handle());
	::CancelSynchronousIo(handle);
	try {
		worker.join();
	} catch( ... ) {
		// std::thread::join has no recoverable failure for a joinable, non-self
		// worker.  Detaching here would violate the backend's ownership contract.
		std::terminate();
	}
}

class ConPtyTerminalBackend final : public ITerminalBackend {
public:
	~ConPtyTerminalBackend() override { Close(); }

	TerminalStartResult Start( const TerminalLaunchOptions& options ) override
	{
		const std::lock_guard lock(m_mutex);
		if( m_started || m_closed ) return StartFailure(ERROR_INVALID_STATE, L"ConPTY backend state");
		if( options.initialSize.columns > static_cast<std::uint16_t>(std::numeric_limits<SHORT>::max()) ||
			options.initialSize.rows > static_cast<std::uint16_t>(std::numeric_limits<SHORT>::max()) ) return StartFailure(ERROR_INVALID_PARAMETER, L"ConPTY dimensions");

		UniqueHandle pseudoConsoleInput;
		UniqueHandle clientInput;
		UniqueHandle clientOutput;
		UniqueHandle pseudoConsoleOutput;
		HANDLE inputRead = nullptr, inputWrite = nullptr;
		if( !::CreatePipe(&inputRead, &inputWrite, nullptr, 0) ) return StartFailure(::GetLastError(), L"Create ConPTY input pipe");
		pseudoConsoleInput.Reset(inputRead);
		clientInput.Reset(inputWrite);
		HANDLE outputRead = nullptr, outputWrite = nullptr;
		if( !::CreatePipe(&outputRead, &outputWrite, nullptr, 0) ) return StartFailure(::GetLastError(), L"Create ConPTY output pipe");
		clientOutput.Reset(outputRead);
		pseudoConsoleOutput.Reset(outputWrite);

		// No process inherits a pipe or an ambient parent handle. The
		// pseudoconsole connection is conveyed only through the explicit
		// STARTUPINFOEX attribute below (a stricter zero-entry inheritance set).
		::SetHandleInformation(clientInput.Get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(clientOutput.Get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(pseudoConsoleInput.Get(), HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(pseudoConsoleOutput.Get(), HANDLE_FLAG_INHERIT, 0);

		HPCON rawPseudoConsole = nullptr;
		const COORD dimensions = { static_cast<SHORT>(options.initialSize.columns), static_cast<SHORT>(options.initialSize.rows) };
		const HRESULT pseudoConsoleResult = ::CreatePseudoConsole( dimensions, pseudoConsoleInput.Get(), pseudoConsoleOutput.Get(), 0, &rawPseudoConsole );
		if( FAILED(pseudoConsoleResult) ) return StartFailure(HRESULT_CODE(pseudoConsoleResult), L"CreatePseudoConsole");
		UniquePseudoConsole pseudoConsole(rawPseudoConsole);
		// Keep ConPTY's server pipe ends alive until CreateProcess consumes the
		// pseudoconsole attribute. Retaining them after that point would prevent
		// EOF from propagating when the pseudoconsole closes.

		UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
		if( !job ) return StartFailure(::GetLastError(), L"Create terminal job object");
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
		jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if( !::SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)) ) return StartFailure(::GetLastError(), L"Configure terminal job object");

		SIZE_T attributeBytes = 0;
		::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
		if( attributeBytes == 0 ) return StartFailure(::GetLastError(), L"Measure process attribute list");
		std::vector<std::uint8_t> attributeStorage(attributeBytes);
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
		if( !::InitializeProcThreadAttributeList(attributes, 2, 0, &attributeBytes) ) return StartFailure(::GetLastError(), L"Initialize process attribute list");
		struct AttributeListGuard {
			LPPROC_THREAD_ATTRIBUTE_LIST value;
			~AttributeListGuard() { if( value ) ::DeleteProcThreadAttributeList(value); }
		} attributeGuard{ attributes };
		if( !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pseudoConsole.Get(), sizeof(HPCON), nullptr, nullptr) ) return StartFailure(::GetLastError(), L"Attach pseudoconsole attribute");
		HANDLE jobHandle = job.Get();
		if( !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &jobHandle, sizeof(jobHandle), nullptr, nullptr) ) return StartFailure(::GetLastError(), L"Attach terminal job attribute");

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		// A console parent, debugger, or redirected test runner can otherwise
		// leak its standard handles into the child before the pseudoconsole has
		// replaced them.  Explicit null handles are console pseudo-handles and
		// are replaced while attaching to ConPTY; no ambient HANDLE is inherited.
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = nullptr;
		startup.StartupInfo.hStdOutput = nullptr;
		startup.StartupInfo.hStdError = nullptr;
		startup.lpAttributeList = attributes;
		PROCESS_INFORMATION processInfo{};
		auto commandLine = BuildCommandLine(options);
		auto environment = BuildTerminalEnvironmentBlock(options);
		if( !environment.Succeeded() ) {
			const auto error = environment.status == TerminalEnvironmentBuildStatus::TooLarge
				? ERROR_BUFFER_OVERFLOW : ERROR_INVALID_DATA;
			return StartFailure(error, L"Build terminal environment");
		}
		const DWORD creationFlags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
		const wchar_t* workingDirectory = options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str();
		if( !::CreateProcessW(options.executablePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, creationFlags,
			environment.block.data(), workingDirectory,
			&startup.StartupInfo, &processInfo) ) return StartFailure(::GetLastError(), L"Create terminal process");
		UniqueHandle process(processInfo.hProcess);
		UniqueHandle primaryThread(processInfo.hThread);
		FILETIME creationTime{}, exitTime{}, kernelTime{}, userTime{};
		if( !::GetProcessTimes(process.Get(), &creationTime, &exitTime, &kernelTime, &userTime) ) {
			return StartFailure(::GetLastError(), L"Query terminal process identity");
		}
		ULARGE_INTEGER creationValue{};
		creationValue.LowPart = creationTime.dwLowDateTime;
		creationValue.HighPart = creationTime.dwHighDateTime;
		// The job-list attribute assigns the process atomically at creation, so a
		// fast shell cannot spawn descendants before job ownership is established.
		// The process is already running here and has consumed the ConPTY attribute.
		pseudoConsoleInput.Reset();
		pseudoConsoleOutput.Reset();

		m_input = std::move(clientInput);
		m_output = std::move(clientOutput);
		m_process = std::move(process);
		m_job = std::move(job);
		m_pseudoConsole = std::move(pseudoConsole);
		m_rootProcessIdentity = TerminalBackendProcessIdentity{
			processInfo.dwProcessId, creationValue.QuadPart };
		m_started = true;
		return TerminalStartResult::Success();
	}

	std::optional<TerminalBackendProcessIdentity> GetProcessIdentity() const noexcept override
	{
		const std::lock_guard lock(m_mutex);
		return m_rootProcessIdentity && m_rootProcessIdentity->IsValid()
			? m_rootProcessIdentity : std::nullopt;
	}

	bool OwnsProcess(
		const std::uint32_t processId, const std::uint64_t creationTime ) const noexcept override
	{
		if( processId == 0 || creationTime == 0 ) return false;
		UniqueHandle job;
		{
			const std::lock_guard lock(m_mutex);
			job = DuplicateLocalHandle(m_job.Get());
		}
		if( !job ) return false;
		UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
		if( !process ) return false;
		FILETIME observedCreation{}, exitTime{}, kernelTime{}, userTime{};
		if( !::GetProcessTimes(process.Get(), &observedCreation, &exitTime, &kernelTime, &userTime) ) return false;
		ULARGE_INTEGER observed{};
		observed.LowPart = observedCreation.dwLowDateTime;
		observed.HighPart = observedCreation.dwHighDateTime;
		if( observed.QuadPart != creationTime ) return false;
		BOOL member = FALSE;
		return ::IsProcessInJob(process.Get(), job.Get(), &member) != FALSE && member != FALSE;
	}

	TerminalBackendReadResult ReadOutput( std::span<std::uint8_t> destination, std::chrono::milliseconds timeout ) override
	{
		if( destination.empty() ) return { TerminalBackendReadStatus::Failed, 0, ERROR_INSUFFICIENT_BUFFER };
		UniqueHandle output;
		UniqueHandle process;
		UniqueHandle job;
		{
			const std::lock_guard lock(m_mutex);
			output = DuplicateLocalHandle(m_output.Get());
			process = DuplicateLocalHandle(m_process.Get());
			job = DuplicateLocalHandle(m_job.Get());
		}
		if( !output ) return { TerminalBackendReadStatus::EndOfFile, 0, 0 };

		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for( ;; ) {
			DWORD available = 0;
			if( !::PeekNamedPipe(output.Get(), nullptr, 0, nullptr, &available, nullptr) ) {
				const DWORD error = ::GetLastError();
				if( error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_INVALID_HANDLE ) return { TerminalBackendReadStatus::EndOfFile, 0, 0 };
				return { TerminalBackendReadStatus::Failed, 0, error };
			}
			if( available != 0 ) {
				DWORD transferred = 0;
				const DWORD request = static_cast<DWORD>(std::min<std::size_t>(destination.size(), available));
				if( !::ReadFile(output.Get(), destination.data(), request, &transferred, nullptr) ) {
					const DWORD error = ::GetLastError();
					if( error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED ) return { TerminalBackendReadStatus::EndOfFile, 0, 0 };
					return { TerminalBackendReadStatus::Failed, 0, error };
				}
				return { TerminalBackendReadStatus::Data, transferred, 0 };
			}
			const auto now = std::chrono::steady_clock::now();
			const bool rootExited = process && ::WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0;
			if( HaveAllProcessesExited(process.Get(), job.Get()) ) {
				const auto closeResult = BeginPseudoConsoleClose();
				if( !closeResult.succeeded ) return { TerminalBackendReadStatus::Failed, 0, closeResult.errorCode };
			}
			if( now >= deadline ) return { TerminalBackendReadStatus::Timeout, 0, 0 };
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
			const DWORD slice = static_cast<DWORD>(std::max<std::int64_t>(1, std::min<std::int64_t>(10, remaining.count())));
			if( process && !rootExited ) ::WaitForSingleObject(process.Get(), slice);
			else ::Sleep(slice);
		}
	}

	TerminalBackendWriteResult WriteInput( std::span<const std::uint8_t> source ) override
	{
		if( source.empty() ) return { TerminalBackendWriteStatus::Completed, 0, 0 };
		UniqueHandle input;
		{
			const std::lock_guard lock(m_mutex);
			input = DuplicateLocalHandle(m_input.Get());
		}
		if( !input ) return { TerminalBackendWriteStatus::Closed, 0, ERROR_BROKEN_PIPE };
		DWORD transferred = 0;
		const DWORD request = static_cast<DWORD>(std::min<std::size_t>(source.size(), std::numeric_limits<DWORD>::max()));
		if( !::WriteFile(input.Get(), source.data(), request, &transferred, nullptr) ) {
			const DWORD error = ::GetLastError();
			if( error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED || error == ERROR_INVALID_HANDLE ) return { TerminalBackendWriteStatus::Closed, 0, error };
			return { TerminalBackendWriteStatus::Failed, 0, error };
		}
		return { TerminalBackendWriteStatus::Completed, transferred, 0 };
	}

	TerminalBackendOperationResult Resize( TerminalSize size ) override
	{
		const std::lock_guard lock(m_mutex);
		if( !m_pseudoConsole || m_closed ) return { false, ERROR_INVALID_STATE };
		if( size.columns == 0 || size.rows == 0 || size.columns > static_cast<std::uint16_t>(std::numeric_limits<SHORT>::max()) || size.rows > static_cast<std::uint16_t>(std::numeric_limits<SHORT>::max()) ) return { false, ERROR_INVALID_PARAMETER };
		const COORD dimensions = { static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows) };
		const HRESULT result = ::ResizePseudoConsole(m_pseudoConsole.Get(), dimensions);
		return FAILED(result) ? TerminalBackendOperationResult{ false, static_cast<std::uint32_t>(HRESULT_CODE(result)) } : TerminalBackendOperationResult{ true, 0 };
	}

	void RequestGracefulClose() noexcept override
	{
		UniqueHandle input;
		{
			const std::lock_guard lock(m_mutex);
			input.Reset(m_input.Release());
		}
		// Closing the client input requests EOF without injecting commands into
		// the user's shell or suppressing its PowerShell profile.
	}

	TerminalBackendExitResult WaitForExit( std::chrono::milliseconds timeout ) noexcept override
	{
		UniqueHandle process;
		UniqueHandle job;
		std::optional<std::uint32_t> cachedExitCode;
		{
			const std::lock_guard lock(m_mutex);
			process = DuplicateLocalHandle(m_process.Get());
			job = DuplicateLocalHandle(m_job.Get());
			cachedExitCode = m_rootExitCode;
		}
		if( !process && !job ) return cachedExitCode ? TerminalBackendExitResult{ TerminalBackendExitStatus::Exited, *cachedExitCode, 0 }
			: TerminalBackendExitResult{ TerminalBackendExitStatus::Failed, 0, ERROR_INVALID_STATE };
		const auto bounded = std::chrono::milliseconds(std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<DWORD>::max() - 1ll));
		const auto deadline = std::chrono::steady_clock::now() + bounded;
		for( ;; ) {
			bool rootExited = !process;
			if( process ) {
				const auto wait = ::WaitForSingleObject(process.Get(), 0);
				if( wait == WAIT_FAILED ) return { TerminalBackendExitStatus::Failed, 0, ::GetLastError() };
				rootExited = wait == WAIT_OBJECT_0;
				if( rootExited && !cachedExitCode ) {
					DWORD exitCode = 0;
					if( !::GetExitCodeProcess(process.Get(), &exitCode) ) return { TerminalBackendExitStatus::Failed, 0, ::GetLastError() };
					cachedExitCode = exitCode;
					const std::lock_guard lock(m_mutex);
					m_rootExitCode = exitCode;
				}
			}
			const auto now = std::chrono::steady_clock::now();
			if( rootExited ) {
				if( job ) {
					const auto jobEmpty = IsJobEmpty(job.Get());
					if( !jobEmpty ) return { TerminalBackendExitStatus::Failed, 0, ::GetLastError() };
					if( !*jobEmpty ) {
						if( now >= deadline ) return { TerminalBackendExitStatus::TimedOut, 0, 0 };
					} else return { TerminalBackendExitStatus::Exited, cachedExitCode.value_or(0), 0 };
				} else return { TerminalBackendExitStatus::Exited, cachedExitCode.value_or(0), 0 };
			}
			if( now >= deadline ) return { TerminalBackendExitStatus::TimedOut, 0, 0 };
			const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
			const DWORD slice = static_cast<DWORD>(std::max<std::int64_t>(1, std::min<std::int64_t>(10, remaining.count())));
			if( process && !rootExited ) ::WaitForSingleObject(process.Get(), slice);
			else ::Sleep(slice);
		}
	}

	void ForceTerminate() noexcept override
	{
		UniqueHandle job;
		{
			const std::lock_guard lock(m_mutex);
			job = DuplicateLocalHandle(m_job.Get());
		}
		if( job ) ::TerminateJobObject(job.Get(), 1);
	}

	void Close() noexcept override
	{
		UniqueHandle input;
		UniqueHandle output;
		UniqueHandle process;
		UniqueHandle job;
		HPCON pseudoConsole = nullptr;
		std::thread pseudoConsoleCloseThread;
		{
			const std::lock_guard lock(m_mutex);
			if( !m_closed ) {
				m_closed = true;
				input.Reset(m_input.Release());
				output.Reset(m_output.Release());
				process.Reset(m_process.Release());
				job.Reset(m_job.Release());
				pseudoConsole = m_pseudoConsole.Release();
			}
			if( m_pseudoConsoleCloseThread.joinable() ) pseudoConsoleCloseThread = std::move(m_pseudoConsoleCloseThread);
		}
		if( !input && !output && !process && !job && pseudoConsole == nullptr && !pseudoConsoleCloseThread.joinable() ) return;
		// Pipe clients are closed before ClosePseudoConsole, preventing a full
		// output pipe from turning backend finalization into an unbounded wait.
		input.Reset();
		output.Reset();
		// Close the kill-on-close job before the pseudoconsole so even a client
		// that ignored the graceful request cannot keep pre-24H2 ConPTY alive.
		job.Reset();
		process.Reset();
		if( pseudoConsole != nullptr ) {
			try {
				pseudoConsoleCloseThread = std::thread([pseudoConsole] { ::ClosePseudoConsole(pseudoConsole); });
			} catch( ... ) {
				// With the output pipe closed and the job gone, the documented
				// pre-24H2 deadlock condition is absent. Preserve resource cleanup
				// even if a close worker cannot be allocated.
				::ClosePseudoConsole(pseudoConsole);
			}
		}
		JoinPseudoConsoleClose(pseudoConsoleCloseThread);
	}

private:
	TerminalBackendOperationResult BeginPseudoConsoleClose() noexcept
	{
		const std::lock_guard lock(m_mutex);
		if( m_closed || !m_pseudoConsole || m_pseudoConsoleCloseThread.joinable() ) return { true, 0 };
		const HPCON pseudoConsole = m_pseudoConsole.Get();
		try {
			// ClosePseudoConsole can wait for its final output to be drained on
			// Windows 11 builds before 24H2.  Run it beside the reader so the pipe
			// remains serviced until it reports a real broken channel; do not infer
			// EOF from a quiet-period timer and lose a shell's final frame.
			m_pseudoConsoleCloseThread = std::thread([pseudoConsole] { ::ClosePseudoConsole(pseudoConsole); });
			m_pseudoConsole.Release();
			return { true, 0 };
		} catch( const std::system_error& error ) {
			const auto value = error.code().value();
			return { false, value > 0 ? static_cast<std::uint32_t>(value) : ERROR_NOT_ENOUGH_MEMORY };
		} catch( ... ) {
			return { false, ERROR_NOT_ENOUGH_MEMORY };
		}
	}

	mutable std::mutex m_mutex;
	UniqueHandle m_input;
	UniqueHandle m_output;
	UniqueHandle m_process;
	UniqueHandle m_job;
	UniquePseudoConsole m_pseudoConsole;
	std::thread m_pseudoConsoleCloseThread;
	std::optional<std::uint32_t> m_rootExitCode;
	std::optional<TerminalBackendProcessIdentity> m_rootProcessIdentity;
	bool m_started = false;
	bool m_closed = false;
};

} // namespace

std::unique_ptr<ITerminalBackend> CreateConPtyTerminalBackend()
{
	return std::make_unique<ConPtyTerminalBackend>();
}

} // namespace terminal
