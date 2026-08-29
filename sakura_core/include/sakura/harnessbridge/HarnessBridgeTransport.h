/*! @file
    @brief Bounded local named-pipe transport for the independent Harness Bridge.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeProtocol.h>

#include <Windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace platform::harnessbridge {

enum class EHarnessBridgeDisconnectReason : std::uint8_t {
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

struct HarnessBridgeTransportOptions final {
	std::wstring pipeName;
	std::size_t maximumSessions = 4;
	std::size_t maximumQueuedBytes = kHarnessBridgeMaximumFrameBytes;
	std::size_t readBufferBytes = 16 * 1024;
	std::chrono::milliseconds ioTimeout = std::chrono::seconds(5);
};

struct HarnessBridgeTransportResult final {
	bool success = false;
	EHarnessBridgeDisconnectReason reason = EHarnessBridgeDisconnectReason::None;
	std::uint32_t errorCode = 0;
};

struct HarnessBridgeSessionContext final {
	std::uint64_t sessionId = 0;
	std::uint32_t clientProcessId = 0;
};

class IHarnessBridgeSessionHandler {
public:
	virtual ~IHarnessBridgeSessionHandler() = default;
	virtual HarnessBridgeTransportResult HandleFrame(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
		std::vector<HarnessBridgeFrame>& responses) = 0;
};

class IHarnessBridgeSessionFactory {
public:
	virtual ~IHarnessBridgeSessionFactory() = default;
	virtual std::unique_ptr<IHarnessBridgeSessionHandler> CreateSession(
		const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& firstFrame) = 0;
};

class CHarnessBridgeNamedPipeServer final {
public:
	explicit CHarnessBridgeNamedPipeServer(std::shared_ptr<IHarnessBridgeSessionFactory> factory);
	~CHarnessBridgeNamedPipeServer();
	CHarnessBridgeNamedPipeServer(const CHarnessBridgeNamedPipeServer&) = delete;
	CHarnessBridgeNamedPipeServer& operator=(const CHarnessBridgeNamedPipeServer&) = delete;

	[[nodiscard]] HarnessBridgeTransportResult Start(const HarnessBridgeTransportOptions& options);
	void Stop() noexcept;
	[[nodiscard]] bool IsRunning() const noexcept;
	[[nodiscard]] std::size_t ActiveSessionCount() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

class CHarnessBridgeNamedPipeClient final {
public:
	CHarnessBridgeNamedPipeClient();
	~CHarnessBridgeNamedPipeClient();
	CHarnessBridgeNamedPipeClient(const CHarnessBridgeNamedPipeClient&) = delete;
	CHarnessBridgeNamedPipeClient& operator=(const CHarnessBridgeNamedPipeClient&) = delete;

	[[nodiscard]] HarnessBridgeTransportResult Connect(std::wstring pipeName,
		std::uint32_t expectedServerProcessId, std::chrono::milliseconds deadline);
	[[nodiscard]] HarnessBridgeTransportResult Exchange(const HarnessBridgeFrame& request,
		std::vector<HarnessBridgeFrame>& responses, std::chrono::milliseconds deadline);
	void Close() noexcept;
	[[nodiscard]] bool IsConnected() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace platform::harnessbridge
