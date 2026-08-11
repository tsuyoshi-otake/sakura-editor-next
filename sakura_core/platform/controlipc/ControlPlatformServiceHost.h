/*! @file
	@brief UI-independent composition root for the control-process platform service.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "platform/controlipc/ControlPlatformEndpoint.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace platform::storage {
class IStorageAuthority;
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

class CControlPlatformRpcServerAdapter;

//! Observable lifecycle. Starting and Stopping are internal serialized states, never terminal results.
enum class EControlPlatformServiceHostState : std::uint8_t {
	Stopped,
	Starting,
	Started,
	Stopping,
};

//! The terminal result of one serialized Start() or Stop() call.
enum class EControlPlatformServiceHostResultCode : std::uint8_t {
	Started,
	AlreadyStarted,
	Stopped,
	AlreadyStopped,
	InvalidOptions,
	EndpointFactoryFailed,
	EndpointCreateFailed,
	StartingPublishFailed,
	AdapterCreateFailed,
	PipeServerFactoryFailed,
	PipeStartFailed,
	AcceptingPublishFailed,
	StoppingPublishFailed,
	UnexpectedFailure,
};

struct ControlPlatformServiceHostResult {
	EControlPlatformServiceHostResultCode code = EControlPlatformServiceHostResultCode::UnexpectedFailure;
	EControlPlatformServiceHostState state = EControlPlatformServiceHostState::Stopped;
	std::wstring diagnostic;
};

/*!
	@brief Immutable process-composition inputs.

	profileDirectory is passed only to the endpoint, which remains authoritative for
	canonical identity and profile hashing. pipeOptions.pipeName is deliberately
	overwritten by the host with BuildControlPipeName(endpoint.ProfileHash()).
*/
struct ControlPlatformServiceHostOptions {
	std::filesystem::path profileDirectory;
	std::string profileId;
	std::uint64_t authorityGeneration = 0;
	ControlIpcNamedPipeOptions pipeOptions;
};

//! Narrow seam for deterministic host tests; implementations must make Close() idempotent.
class IControlPlatformServiceEndpoint {
public:
	virtual ~IControlPlatformServiceEndpoint() = default;
	virtual bool CreateForControl(const std::filesystem::path& profileDirectory, std::wstring& diagnostic) = 0;
	virtual void Close() noexcept = 0;
	virtual bool Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic) = 0;
	[[nodiscard]] virtual const std::wstring& ProfileHash() const noexcept = 0;
};

//! Narrow seam for deterministic host tests; Stop() must cancel and join all sessions before returning.
class IControlPlatformServicePipeServer {
public:
	virtual ~IControlPlatformServicePipeServer() = default;
	[[nodiscard]] virtual ControlIpcTransportResult Start(const ControlIpcNamedPipeOptions& options) = 0;
	virtual void Stop() noexcept = 0;
};

struct ControlPlatformServiceHostDependencies {
	std::function<std::unique_ptr<IControlPlatformServiceEndpoint>()> endpointFactory;
	std::function<std::unique_ptr<IControlPlatformServicePipeServer>(std::shared_ptr<IControlIpcFrameHandler>)> pipeServerFactory;
};

/*!
	@brief Bounded control-process composition root with serialized, rollback-safe lifecycle.

	The host owns the server. The server owns the shared composite frame handler;
	the handler keeps the control-owned storage, vault, capability authority,
	extension-grant authority, and migration coordinator alive through its last joined session. No callback or
	session handler retains or calls this host.
*/
class CControlPlatformServiceHost final {
public:
	CControlPlatformServiceHost(ControlPlatformServiceHostOptions options,
		std::shared_ptr<storage::IStorageAuthority> storage,
		std::shared_ptr<secrets::ISecretVaultService> vault,
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
		std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles);
	CControlPlatformServiceHost(ControlPlatformServiceHostOptions options,
		std::shared_ptr<storage::IStorageAuthority> storage,
		std::shared_ptr<secrets::ISecretVaultService> vault,
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
		std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
		ControlPlatformServiceHostDependencies dependencies);
	~CControlPlatformServiceHost();
	CControlPlatformServiceHost(const CControlPlatformServiceHost&) = delete;
	CControlPlatformServiceHost& operator=(const CControlPlatformServiceHost&) = delete;

	[[nodiscard]] ControlPlatformServiceHostResult Start();
	[[nodiscard]] ControlPlatformServiceHostResult Stop();
	[[nodiscard]] EControlPlatformServiceHostState State() const noexcept;

private:
	[[nodiscard]] bool HasValidOptions(std::wstring& diagnostic) const;
	[[nodiscard]] ControlPlatformEndpointSnapshot Snapshot(ControlPlatformEndpointLifecycle lifecycle) const;
	[[nodiscard]] ControlPlatformServiceHostResult Result(EControlPlatformServiceHostResultCode code,
		std::wstring diagnostic = {}) const;
	void RollbackStart() noexcept;
	void ReleaseStoppedResources() noexcept;

	const ControlPlatformServiceHostOptions m_options;
	const std::shared_ptr<storage::IStorageAuthority> m_storage;
	const std::shared_ptr<secrets::ISecretVaultService> m_vault;
	const std::shared_ptr<secrets::ISecretVaultCapabilityService> m_capabilities;
	const std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> m_grantAuthority;
	const std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> m_migration;
	const std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profiles;
	const ControlPlatformServiceHostDependencies m_dependencies;
	mutable std::mutex m_mutex;
	EControlPlatformServiceHostState m_state = EControlPlatformServiceHostState::Stopped;
	std::unique_ptr<IControlPlatformServiceEndpoint> m_endpoint;
	std::shared_ptr<CControlPlatformRpcServerAdapter> m_adapter;
	std::unique_ptr<IControlPlatformServicePipeServer> m_pipeServer;
};

} // namespace platform::controlipc
