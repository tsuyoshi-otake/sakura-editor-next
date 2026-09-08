/*! @file
	@brief Editor-side SENP tool client bound to one authenticated connection.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include <sakura/controlipc/ControlPlatformEndpoint.h>
#include "platform/controlipc/ControlPlatformClient.h"
#include "platform/controlipc/ControlSenpRpc.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace platform::controlipc {

//! Observable lifecycle of the one connection this client keeps authenticated.
enum class EControlSenpClientState : std::uint8_t {
	Disconnected,
	Connecting,
	Connected,
	Stopped,
};

//! Terminal outcome of one client call. Answered means the broker replied on a
//! healthy connection; the SENP status inside that answer may still be a refusal.
enum class EControlSenpClientOutcome : std::uint8_t {
	Answered,
	Connected,
	AlreadyConnected,
	NotConnected,
	OperationInFlight,
	EndpointUnavailable,
	ConnectionLost,
	GenerationChanged,
	ProtocolError,
	InvalidRequest,
	Stopped,
};

struct ControlSenpClientResult {
	EControlSenpClientOutcome outcome = EControlSenpClientOutcome::Stopped;
	//! Meaningful only when outcome is Answered.
	ControlSenpRpcResponse response;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	//! Deliberately generic transport/protocol text. It never carries an owner
	//! identity, a grant id, a resource handle, a tool argument or a path.
	std::wstring diagnostic;

	[[nodiscard]] bool Answered() const noexcept
	{
		return outcome == EControlSenpClientOutcome::Answered;
	}
	[[nodiscard]] bool IsConnected() const noexcept
	{
		return outcome == EControlSenpClientOutcome::Connected
			|| outcome == EControlSenpClientOutcome::AlreadyConnected;
	}
};

struct ControlSenpClientOptions {
	//! Canonical control authority identity, the same value the storage Hello pins.
	std::string profileId;
	std::wstring profileHash;
	//! Anti-rollback floor. A later connection may never pin a lower generation.
	std::uint64_t minimumGeneration = 0;
	std::chrono::milliseconds exchangeDeadline = std::chrono::seconds(5);
	std::function<std::unique_ptr<IControlPlatformClientChannel>()> channelFactory;
};

/*!
	@brief One authenticated connection retained for connection-bound SENP grants.

	Every grant the control broker mints is bound to the connection that asked for
	it, so an operation issued over a different connection is a different security
	scope. This client therefore owns exactly one channel: Connect performs
	discovery, connect and storage Hello and then keeps that channel, and every
	Execute reuses it without a hidden bootstrap.

	Losing the connection is never repaired silently. A transport loss, a
	generation change or a fatal protocol answer drops the channel and leaves the
	client Disconnected, so the caller learns that its grants are gone instead of
	continuing on a connection that no longer holds them. ConnectionEpoch()
	increments once per successful Connect, which lets a caller prove that a grant
	it still holds was minted on the connection it is about to use.

	Only one call may be in flight; a second caller observes OperationInFlight
	rather than sharing one request/response channel. Disconnect and Stop close the
	active channel, so a blocked exchange is cancelled rather than waited on.
*/
class CControlSenpClient final {
public:
	CControlSenpClient(ControlSenpClientOptions options, IControlPlatformEndpointReader& endpointReader);
	~CControlSenpClient();
	CControlSenpClient(const CControlSenpClient&) = delete;
	CControlSenpClient& operator=(const CControlSenpClient&) = delete;

	//! Discovers, connects and completes storage Hello, then retains the channel.
	[[nodiscard]] ControlSenpClientResult Connect();
	//! Sends one SENP operation over the retained connection. It never connects.
	[[nodiscard]] ControlSenpClientResult Execute(const ControlSenpRpcRequest& request);
	//! Drops the connection. Every grant minted on it is void afterwards.
	void Disconnect() noexcept;
	//! Terminal. Later calls report Stopped and no connection is created again.
	void Stop() noexcept;

	[[nodiscard]] EControlSenpClientState State() const noexcept;
	[[nodiscard]] std::uint64_t PinnedGeneration() const noexcept;
	[[nodiscard]] std::uint64_t ConnectionEpoch() const noexcept;

private:
	[[nodiscard]] std::optional<std::uint64_t> NextRequestId() noexcept;
	//! Releases the channel this call owns and, when it is still the active one,
	//! returns the client to Disconnected.
	void Release(const std::shared_ptr<IControlPlatformClientChannel>& channel, bool drop) noexcept;

	ControlSenpClientOptions m_options;
	IControlPlatformEndpointReader& m_endpointReader;
	mutable std::mutex m_mutex;
	EControlSenpClientState m_state = EControlSenpClientState::Disconnected;
	bool m_stopped = false;
	bool m_busy = false;
	std::uint64_t m_pinnedGeneration = 0;
	std::uint64_t m_minimumGeneration = 0;
	std::uint64_t m_connectionEpoch = 0;
	std::uint64_t m_nextRequestId = 1;
	std::shared_ptr<IControlPlatformClientChannel> m_activeChannel;
};

} // namespace platform::controlipc
