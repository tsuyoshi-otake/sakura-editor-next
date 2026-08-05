/*! @file
	@brief Named-pipe handler adapter for per-connection storage RPC sessions.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcNamedPipeTransport.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include <sakura/storage/IStorageAuthority.h>

#include <cstdint>
#include <memory>

namespace platform::controlipc {

//! Lifecycle of the storage-RPC accept gate. It never reopens after stopping begins.
enum class EControlStorageRpcServerAdapterState : std::uint8_t {
	Accepting,
	Stopping,
	Stopped,
};

/*!
	@brief Adapts verified named-pipe connections to independent storage-RPC sessions.

	The identity is copied at construction and applies to every connection. Each
	CreateSession() call creates a fresh CControlStorageRpcSession, so a completed
	Hello never authenticates another connection. BeginStopping()/Stop() close the
	shared gate: no new session is created and existing connections receive one
	terminal ServerStopping error before their transport closes. The adapter borrows
	an already-open Control-owned authority; lifecycle ownership remains with the
	Control composition root.
*/
class CControlStorageRpcServerAdapter final : public IControlIpcFrameHandler {
public:
	CControlStorageRpcServerAdapter(ControlStorageRpcSessionIdentity identity,
		std::shared_ptr<storage::IStorageAuthority> storage);
	~CControlStorageRpcServerAdapter() override;
	CControlStorageRpcServerAdapter(const CControlStorageRpcServerAdapter&) = delete;
	CControlStorageRpcServerAdapter& operator=(const CControlStorageRpcServerAdapter&) = delete;

	//! Closes the gate without destroying existing session handlers. Returns true only for Accepting -> Stopping.
	[[nodiscard]] bool BeginStopping() noexcept;
	//! Makes the terminal lifecycle state explicit. It also closes an accepting gate if shutdown bypasses BeginStopping().
	void Stop() noexcept;
	[[nodiscard]] EControlStorageRpcServerAdapterState State() const noexcept;
	[[nodiscard]] bool IsAccepting() const noexcept;

	//! Returns nullptr after the gate closes. A non-null result owns only its own handshake state.
	[[nodiscard]] std::unique_ptr<IControlIpcSessionHandler> CreateSession(
		const ControlIpcSessionContext& session) override;

private:
	struct Gate;
	class SessionHandler;

	ControlStorageRpcSessionIdentity m_identity;
	std::shared_ptr<storage::IStorageAuthority> m_storage;
	std::shared_ptr<Gate> m_gate;
};

} // namespace platform::controlipc
