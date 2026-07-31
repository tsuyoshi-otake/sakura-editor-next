/*! @file
	@brief Bounded current-user named-pipe frame transport for control IPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcProtocol.h"
#include "platform/controlipc/ControlPlatformEndpoint.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace platform::controlipc {

//! Transport-local final states. These deliberately do not encode storage/RPC outcomes.
enum class EControlIpcTransportDisconnectReason : std::uint8_t {
	None,
	Stopped,
	PeerClosed,
	ConnectFailed,
	AccessDenied,
	DeadlineExceeded,
	ProtocolError,
	CallbackFailed,
	ResourceExhausted,
	IoError,
};

struct ControlIpcNamedPipeOptions {
	std::wstring pipeName;
	std::size_t maximumSessions = 4;
	std::size_t maximumQueuedBytes = kControlIpcMaximumFrameBytes;
	std::size_t readBufferBytes = 16 * 1024;
	std::chrono::milliseconds ioTimeout = std::chrono::seconds(5);
};

struct ControlIpcTransportResult {
	bool success = false;
	EControlIpcTransportDisconnectReason reason = EControlIpcTransportDisconnectReason::None;
	std::uint32_t errorCode = 0;
	std::wstring diagnostic;
};

struct ControlIpcSessionContext {
	std::uint64_t sessionId = 0;
	std::uint32_t clientProcessId = 0;
};

enum class EControlIpcSessionDecision : std::uint8_t {
	KeepOpen,
	Close,
};

//! Responses are sent synchronously and their total encoded size is bounded by the server options.
struct ControlIpcFrameDispatchResult {
	std::vector<ControlIpcFrame> responseFrames;
	EControlIpcSessionDecision decision = EControlIpcSessionDecision::KeepOpen;
};

//! A per-connection callback object. Calls are serial for this instance; separate sessions may run concurrently.
//! Its unique_ptr lifetime is the session lifecycle and ends exactly once when that session settles.
class IControlIpcSessionHandler {
public:
	virtual ~IControlIpcSessionHandler() = default;
	virtual ControlIpcFrameDispatchResult HandleFrame(
		const ControlIpcSessionContext& session, const ControlIpcFrame& frame) = 0;
};

//! The server owns this shared factory until Stop() joins all session workers.
//! CreateSession is called only after a bounded client read and current-user SID verification.
//! It may run concurrently for different connections; factory implementations must synchronize shared state.
class IControlIpcFrameHandler {
public:
	virtual ~IControlIpcFrameHandler() = default;
	virtual std::unique_ptr<IControlIpcSessionHandler> CreateSession(const ControlIpcSessionContext& session) = 0;
};

class CControlIpcNamedPipeServer final {
public:
	explicit CControlIpcNamedPipeServer(std::shared_ptr<IControlIpcFrameHandler> handler);
	~CControlIpcNamedPipeServer();
	CControlIpcNamedPipeServer(const CControlIpcNamedPipeServer&) = delete;
	CControlIpcNamedPipeServer& operator=(const CControlIpcNamedPipeServer&) = delete;

	//! Starts exactly one bounded accept loop. It fails without binding if options/name are unsafe.
	[[nodiscard]] ControlIpcTransportResult Start(const ControlIpcNamedPipeOptions& options);
	//! Safe to repeat. It cancels accept and session I/O and joins every worker before returning.
	//! A handler may request Stop(), but must not destroy this server from its own callback.
	void Stop() noexcept;
	[[nodiscard]] bool IsRunning() const noexcept;
	[[nodiscard]] std::size_t ActiveSessionCount() const noexcept;
	[[nodiscard]] std::size_t RejectedSessionCount() const noexcept;
	[[nodiscard]] std::vector<ControlIpcTransportResult> CompletedSessions() const;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

class CControlIpcNamedPipeClient final {
public:
	CControlIpcNamedPipeClient();
	~CControlIpcNamedPipeClient();
	CControlIpcNamedPipeClient(const CControlIpcNamedPipeClient&) = delete;
	CControlIpcNamedPipeClient& operator=(const CControlIpcNamedPipeClient&) = delete;

	//! Connects once (with a bounded wait only while the exact pipe is busy), then verifies DACL and PID.
	[[nodiscard]] ControlIpcTransportResult Connect(std::wstring pipeName,
		std::uint32_t expectedServerProcessId, std::chrono::milliseconds deadline);
	//! Convenience overload for an already validated endpoint snapshot in Accepting state.
	[[nodiscard]] ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot& endpoint,
		std::chrono::milliseconds deadline);
	[[nodiscard]] ControlIpcTransportResult Send(const ControlIpcFrame& frame,
		std::chrono::milliseconds deadline);
	//! Returns all frames completed by one bounded read. Calls are serialized with Send/Close.
	[[nodiscard]] ControlIpcTransportResult Receive(std::vector<ControlIpcFrame>& frames,
		std::chrono::milliseconds deadline);
	//! Atomic request/terminal-response exchange. P0 intentionally rejects unsolicited or different-request frames.
	[[nodiscard]] ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline);
	void Close() noexcept;
	[[nodiscard]] bool IsConnected() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace platform::controlipc
