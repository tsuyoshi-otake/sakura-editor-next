/*! @file
    @brief Authenticated client session for the local Harness Bridge.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeEnvironment.h>
#include <sakura/harnessbridge/HarnessBridgeEndpoint.h>
#include <sakura/harnessbridge/HarnessBridgeTransport.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace platform::harnessbridge {

struct HarnessBridgeClientConnectResult final {
	bool succeeded = false;
	EHarnessTerminalStatus status = EHarnessTerminalStatus::InternalError;
	std::uint32_t errorCode = 0;
};

struct HarnessBridgeClientOperationResult final {
	EHarnessTerminalStatus status = EHarnessTerminalStatus::InternalError;
	std::vector<std::uint8_t> payload;
	std::uint32_t errorCode = 0;
};

struct HarnessBridgeClientSessionOptions final {
	std::chrono::milliseconds connectTimeout = std::chrono::seconds(5);
	std::chrono::milliseconds handshakeTimeout = std::chrono::seconds(5);
	std::chrono::milliseconds maximumOperationTimeout = std::chrono::seconds(30);
};

//! Reads only public endpoint metadata and a child-scoped capability. The
//! capability secret is never written to the wire and is zeroized immediately
//! after the challenge digest is computed.
class CHarnessBridgeClientSession final {
public:
	explicit CHarnessBridgeClientSession(HarnessBridgeClientSessionOptions options = {});
	~CHarnessBridgeClientSession();
	CHarnessBridgeClientSession(const CHarnessBridgeClientSession&) = delete;
	CHarnessBridgeClientSession& operator=(const CHarnessBridgeClientSession&) = delete;

	[[nodiscard]] HarnessBridgeClientConnectResult Connect(
		std::wstring_view endpointEnvironment,
		std::wstring_view targetEnvironment,
		std::wstring_view capabilityEnvironment);
	[[nodiscard]] HarnessBridgeClientOperationResult Execute(
		EHarnessOperationKind operation,
		std::span<const std::uint8_t> payload,
		std::chrono::milliseconds timeout);
	void Close() noexcept;
	[[nodiscard]] bool IsReady() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace platform::harnessbridge
