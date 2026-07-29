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
	virtual bool WaitForExit( std::chrono::milliseconds timeout ) noexcept = 0;
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

struct TerminalSessionCallbacks {
	// Callbacks run on a session worker. A window implementation should only
	// post/coalesce a UI message here and perform model work on the UI thread.
	std::function<void()> outputAvailable;
	std::function<void(TerminalSessionState, std::uint32_t)> stateChanged;
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
	~CTerminalSession();

	CTerminalSession( const CTerminalSession& ) = delete;
	CTerminalSession& operator=( const CTerminalSession& ) = delete;

	TerminalStartResult Start( const TerminalLaunchOptions& options );
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
	std::unique_ptr<Impl> m_impl;
};

} // namespace terminal
