/*! @file
	@brief Composite named-pipe handler for control-owned storage and Secret Vault RPC.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcNamedPipeTransport.h"
#include "platform/controlipc/ControlProfileRpc.h"
#include "platform/controlipc/ControlSecretVaultRpc.h"
#include "platform/controlipc/ControlStorageRpc.h"

#include <cstdint>
#include <memory>

namespace platform::storage {
class IStorageService;
}

namespace platform::secrets {
class ISecretVaultService;
class ISecretVaultCapabilityService;
class ISecretVaultExtensionGrantAuthority;
class ISecretVaultLegacyMigrationCoordinator;
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
	@brief Adapts one authenticated transport to storage and Secret Vault sessions.

	Every transport session owns both protocol sessions. Storage Hello is the sole
	connection handshake and must complete successfully before a Secret request is
	dispatched. The Secret session's identity is constructed from that transport
	connection's verified PID, the canonical control profile, and the authority
	generation; it is never supplied by a frame payload. The required migration
	coordinator remains alive with every session and is invoked only by the full
	capability-checked Secret Vault session.
*/
class CControlPlatformRpcServerAdapter final : public IControlIpcFrameHandler {
public:
	CControlPlatformRpcServerAdapter(ControlStorageRpcSessionIdentity identity,
		std::shared_ptr<storage::IStorageService> storage,
		std::shared_ptr<secrets::ISecretVaultService> vault,
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
		std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
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
	std::shared_ptr<storage::IStorageService> m_storage;
	std::shared_ptr<secrets::ISecretVaultService> m_vault;
	std::shared_ptr<secrets::ISecretVaultCapabilityService> m_capabilities;
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> m_grantAuthority;
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> m_migration;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profiles;
	std::shared_ptr<Gate> m_gate;
};

} // namespace platform::controlipc
