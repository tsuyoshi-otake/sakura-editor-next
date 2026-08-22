/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace terminal {

enum class TerminalSessionState {
	Idle,
	Starting,
	Running,
	Closing,
	Exited,
	Failed,
};

struct TerminalSize {
	std::uint16_t columns = 120;
	std::uint16_t rows = 30;
};

struct TerminalLaunchOptions {
	std::wstring executablePath;
	std::vector<std::wstring> arguments;
	std::wstring workingDirectory;
	TerminalSize initialSize;
};

struct TerminalStartResult {
	bool succeeded = false;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;

	static TerminalStartResult Success();
	static TerminalStartResult Failure( std::uint32_t errorCode, std::wstring diagnostic );
	//! A close request interrupted Start; no I/O workers were left running.
	static TerminalStartResult Aborted();
};

enum class TerminalBackendReadStatus {
	Data,
	Timeout,
	EndOfFile,
	Failed,
};

struct TerminalBackendReadResult {
	TerminalBackendReadStatus status = TerminalBackendReadStatus::Timeout;
	std::size_t bytesTransferred = 0;
	std::uint32_t errorCode = 0;
};

enum class TerminalBackendWriteStatus {
	Completed,
	Closed,
	Failed,
};

struct TerminalBackendWriteResult {
	TerminalBackendWriteStatus status = TerminalBackendWriteStatus::Completed;
	std::size_t bytesTransferred = 0;
	std::uint32_t errorCode = 0;
};

struct TerminalBackendOperationResult {
	bool succeeded = false;
	std::uint32_t errorCode = 0;
};

//! Result of observing the launched root process after all job-owned
//! descendants have exited.  `exitCode` is meaningful only for `Exited`.
enum class TerminalBackendExitStatus {
	Exited,
	TimedOut,
	Failed,
};

struct TerminalBackendExitResult {
	TerminalBackendExitStatus status = TerminalBackendExitStatus::TimedOut;
	std::uint32_t exitCode = 0;
	std::uint32_t errorCode = 0;
};

// This is the only boundary between the session state machine and ConPTY. It
// deliberately contains no HWND, HANDLE, HPCON, or vendored terminal type so
// the state machine can be tested without creating operating-system objects.
class ITerminalBackend {
public:
	virtual ~ITerminalBackend() = default;
	virtual TerminalStartResult Start( const TerminalLaunchOptions& options ) = 0;
	virtual TerminalBackendReadResult ReadOutput( std::span<std::uint8_t> destination, std::chrono::milliseconds timeout ) = 0;
	virtual TerminalBackendWriteResult WriteInput( std::span<const std::uint8_t> source ) = 0;
	virtual TerminalBackendOperationResult Resize( TerminalSize size ) = 0;
	virtual void RequestGracefulClose() noexcept = 0;
	virtual TerminalBackendExitResult WaitForExit( std::chrono::milliseconds timeout ) noexcept = 0;
	virtual void ForceTerminate() noexcept = 0;
	virtual void Close() noexcept = 0;
};

// Creates the Windows 11 ConPTY implementation. Its Win32 types stay private
// to ConPtyTerminalBackend.cpp.
std::unique_ptr<ITerminalBackend> CreateConPtyTerminalBackend();

enum class TerminalQueueInputResult {
	Accepted,
	NotRunning,
	QueueFull,
};

//! Result of waiting for an initiated terminal close.
//!
//! `Closed` and `DeadlineExceeded` both guarantee that the backend and every
//! session worker have quiesced.  `InProgress` is returned only to prevent a
//! callback running on a session/close worker from waiting for itself; it keeps
//! ownership with the session and an external owner must call WaitForClose. If
//! a close-worker launch fails, the pre-admitted fixed retirement service runs
//! the same close body on a reaper thread; no live worker is detached or left
//! for a UI destructor to join.
enum class TerminalSessionCloseWaitStatus {
	Closed,
	DeadlineExceeded,
	InProgress,
};

struct TerminalSessionCloseResult {
	TerminalSessionCloseWaitStatus status = TerminalSessionCloseWaitStatus::Closed;

	[[nodiscard]] constexpr bool IsQuiescent() const noexcept
	{
		return status == TerminalSessionCloseWaitStatus::Closed
			|| status == TerminalSessionCloseWaitStatus::DeadlineExceeded;
	}
};

//! The durable outcome for a successfully started terminal.  It is delivered
//! exactly once after the backend and both I/O workers have quiesced.
enum class TerminalSessionCompletionKind {
	Exited,
	Closed,
	Failed,
};

struct TerminalSessionCompletionResult {
	TerminalSessionCompletionKind kind = TerminalSessionCompletionKind::Closed;
	//! Observed root-process exit code when `kind` is Exited or Closed and the
	//! backend could observe one before its handles were released.
	std::uint32_t exitCode = 0;
	//! Nonzero only for Failed.
	std::uint32_t errorCode = 0;
};

struct TerminalSessionCallbacks {
	// Callbacks run on a session worker. A window implementation should only
	// post/coalesce a UI message here and perform model work on the UI thread.
	std::function<void()> outputAvailable;
	std::function<void(TerminalSessionState, std::uint32_t)> stateChanged;
	//! Called by the lifecycle worker only after `closeFinished` is published and
	//! the backend plus reader/writer workers have quiesced.  It is never emitted
	//! when Start fails before a backend has started.  Callback-origin Close or
	//! destruction is safe; an external owner remains responsible for any
	//! self-wait that returned InProgress.
	std::function<void(TerminalSessionCompletionResult)> completed;
};

class CTerminalSession final {
public:
	static constexpr std::size_t kOutputHighWaterBytes = 4u * 1024u * 1024u;
	static constexpr std::size_t kOutputLowWaterBytes = 2u * 1024u * 1024u;
	static constexpr std::size_t kInputLimitBytes = 1u * 1024u * 1024u;
	static constexpr std::size_t kMaximumDrainBytes = 64u * 1024u;
	static constexpr auto kMaximumDrainTime = std::chrono::milliseconds(4);
	static constexpr auto kGracefulCloseTimeout = std::chrono::milliseconds(1500);
	static constexpr auto kForcedCloseTimeout = std::chrono::milliseconds(250);

	explicit CTerminalSession( std::unique_ptr<ITerminalBackend> backend, TerminalSessionCallbacks callbacks = {} );
	//! Requests close without waiting.  This is safe for UI-owned destruction;
	//! the pre-admitted lifecycle worker and fixed retirement reaper own joins.
	~CTerminalSession();

	CTerminalSession( const CTerminalSession& ) = delete;
	CTerminalSession& operator=( const CTerminalSession& ) = delete;

	TerminalStartResult Start( const TerminalLaunchOptions& options );
	//! Starts shutdown on a dedicated lifecycle worker. Idempotent, thread-safe,
	//! non-throwing, and never waits for backend or I/O-worker completion.
	void BeginClose() noexcept;
	//! Waits for shutdown after BeginClose using one absolute deadline. A returned
	//! terminal result always represents quiesced backend/worker ownership: if the
	//! deadline expires, shutdown still joins before returning DeadlineExceeded.
	//! InProgress is solely the self-wait guard for callback/worker callers.
	[[nodiscard]] TerminalSessionCloseResult WaitForClose( std::chrono::steady_clock::time_point deadline ) noexcept;
	//! Explicit external close. Begins close and waits until quiescent. A session
	//! worker callback only initiates close to avoid self-wait; destruction uses
	//! the nonblocking destructor path above.
	void Close() noexcept;

	TerminalQueueInputResult QueueInput( std::span<const std::uint8_t> bytes );
	bool RequestResize( TerminalSize size );
	std::vector<std::uint8_t> DrainOutput();

	TerminalSessionState GetState() const noexcept;
	std::uint32_t GetLastError() const noexcept;
	std::size_t GetQueuedOutputBytes() const noexcept;
	std::size_t GetQueuedInputBytes() const noexcept;
	bool IsOutputNotificationPending() const noexcept;

private:
	struct Impl;
	std::shared_ptr<Impl> m_impl;
};

} // namespace terminal
