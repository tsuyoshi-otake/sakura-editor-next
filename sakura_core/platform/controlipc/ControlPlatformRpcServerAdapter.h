/*! @file
	@brief Composite named-pipe handler for control-owned storage and profile RPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "platform/controlipc/ControlProfileRpc.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include <sakura/storage/IStorageAuthority.h>

#include <cstdint>
#include <memory>

namespace platform::storage {
class IStorageService;
}

namespace platform::profiles {
class ControlUserDataProfileRegistry;
}

namespace platform::controlipc {

//! Lifecycle of the composite RPC accept gate. It never reopens after stopping begins.
enum class EControlPlatformRpcServerAdapterState : std::uint8_t {
	Accepting,
	Stopping,
	Stopped,
};

/*! 
	@brief Adapts one authenticated transport to storage and profile sessions.

	Storage Hello is the sole connection handshake and must complete successfully
	before profile requests are dispatched.
*/
class CControlPlatformRpcServerAdapter final : public IControlIpcFrameHandler {
public:
	CControlPlatformRpcServerAdapter(ControlStorageRpcSessionIdentity identity,
		std::shared_ptr<storage::IStorageAuthority> storage,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles);
	~CControlPlatformRpcServerAdapter() override;
	CControlPlatformRpcServerAdapter(const CControlPlatformRpcServerAdapter&) = delete;
	CControlPlatformRpcServerAdapter& operator=(const CControlPlatformRpcServerAdapter&) = delete;

	[[nodiscard]] bool BeginStopping() noexcept;
	void Stop() noexcept;
	[[nodiscard]] EControlPlatformRpcServerAdapterState State() const noexcept;
	[[nodiscard]] bool IsAccepting() const noexcept;

	[[nodiscard]] std::unique_ptr<IControlIpcSessionHandler> CreateSession(
		const ControlIpcSessionContext& session) override;

private:
	struct Gate;
	class SessionHandler;

	ControlStorageRpcSessionIdentity m_identity;
	std::shared_ptr<storage::IStorageAuthority> m_storage;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profiles;
	std::shared_ptr<Gate> m_gate;
};

} // namespace platform::controlipc
