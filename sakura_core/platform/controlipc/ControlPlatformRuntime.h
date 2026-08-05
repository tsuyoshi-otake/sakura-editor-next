/*! @file
	@brief Durable control-platform startup and shutdown composition owner.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlPlatformServiceHost.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "platform/profiles/ProfileAuthorityStore.h"
#include "platform/secrets/CWindowsDpapiSecretVaultService.h"
#include "platform/secrets/ISecretVaultCapabilityService.h"
#include "platform/secrets/ISecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"
#include "platform/secrets/ISecretVaultService.h"

#include <sakura/storage/IStorageAuthority.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace platform::controlipc {

//! Starting and Stopping are serialized transitional states. Every public call
//! returns with the runtime in either Running or Stopped.
enum class EControlPlatformRuntimeState : std::uint8_t {
	Stopped,
	Starting,
	Running,
	Stopping,
};

//! One terminal result for a serialized Start() or Stop() call.
enum class EControlPlatformRuntimeResultCode : std::uint8_t {
	Running,
	AlreadyRunning,
	Stopped,
	AlreadyStopped,
	InvalidOptions,
	AuthorityFailed,
	StorageCreateFailed,
	StorageOpenFailed,
	VaultCreateFailed,
	VaultOpenFailed,
	MigrationCreateFailed,
	CapabilityCreateFailed,
	GrantAuthorityCreateFailed,
	HostCreateFailed,
	HostStartFailed,
	HostStopFailed,
	ProfileRegistryCreateFailed,
	ProfileRegistryLoadFailed,
	ProfileRegistrySaveFailed,
	UnexpectedFailure,
};

//! Immutable identity published by the running host. The profile and storage
//! directories remain caller-owned policy and are deliberately not identities.
struct ControlPlatformRuntimeIdentity {
	std::string profileId;
	std::uint64_t authorityGeneration = 0;

	friend bool operator==(const ControlPlatformRuntimeIdentity&, const ControlPlatformRuntimeIdentity&) = default;
};

//! Atomically acquired same-process Secret Vault authorities. This aggregate is
//! published only by a fully Running runtime; neither authority is optional in
//! a successful snapshot.
struct ControlPlatformRuntimeSecretVaultAuthorities {
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority;
	std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities;
};

//! Immutable inputs resolved by the legacy process-composition layer before
//! this UI-independent runtime is started.
struct ControlPlatformRuntimeOptions {
	std::filesystem::path profileDirectory;
	std::filesystem::path storageDirectory;
	std::wstring legacyProfileAlias;
	std::size_t maximumCompletedOperations = storage::kMaximumStorageCompletedOperations;
	ControlIpcNamedPipeOptions pipeOptions;
};

//! Typed creation seam for the one durable control-owned Secret Vault authority.
enum class EControlPlatformRuntimeVaultCreateStatus : std::uint8_t {
	Created,
	CreateFailed,
	OpenFailed,
};

struct ControlPlatformRuntimeVaultCreateResult {
	EControlPlatformRuntimeVaultCreateStatus status = EControlPlatformRuntimeVaultCreateStatus::CreateFailed;
	std::optional<secrets::WindowsDpapiSecretVaultOpenResult> openResult;
	std::shared_ptr<secrets::ISecretVaultService> vault;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EControlPlatformRuntimeVaultCreateStatus::Created && static_cast<bool>(vault)
			&& openResult && openResult->Succeeded();
	}
};

enum class EControlPlatformRuntimeMigrationCreateStatus : std::uint8_t {
	Created,
	CreateFailed,
};

//! Typed creation seam for the control-owned legacy SecretStorage import gate.
//! Production binds it only to the durable DPAPI vault created by this runtime.
struct ControlPlatformRuntimeMigrationCreateResult {
	EControlPlatformRuntimeMigrationCreateStatus status = EControlPlatformRuntimeMigrationCreateStatus::CreateFailed;
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EControlPlatformRuntimeMigrationCreateStatus::Created && static_cast<bool>(migration);
	}
};

enum class EControlPlatformRuntimeCapabilityCreateStatus : std::uint8_t {
	Created,
	CreateFailed,
};

struct ControlPlatformRuntimeCapabilityCreateResult {
	EControlPlatformRuntimeCapabilityCreateStatus status = EControlPlatformRuntimeCapabilityCreateStatus::CreateFailed;
	std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EControlPlatformRuntimeCapabilityCreateStatus::Created && static_cast<bool>(capabilities);
	}
};

//! Existing narrow dependency seams are forwarded to their owning components.
//! A disengaged hostDependencies selects the production host dependencies;
//! an engaged value must contain both factories.
struct ControlPlatformRuntimeDependencies {
	std::shared_ptr<profiles::IProfileAuthorityStoreBackend> profileAuthorityBackend;
	//! Control composition supplies the concrete authority. The default factory is
	//! bound by the Control process and is the only route that knows durable I/O.
	std::function<std::shared_ptr<storage::IStorageAuthority>(
		const std::filesystem::path&, std::uint64_t, std::size_t)> storageFactory;
	std::function<ControlPlatformRuntimeVaultCreateResult(const std::filesystem::path&, std::string)> vaultFactory;
	//! Test seam only. Production constructs a CSecretVaultLegacyMigrationCoordinator
	//! over the concrete DPAPI vault and this profile's legacy extensionData\\secrets root.
	std::function<ControlPlatformRuntimeMigrationCreateResult(
		std::shared_ptr<secrets::ISecretVaultService>, const std::filesystem::path&)> migrationFactory;
	std::function<ControlPlatformRuntimeCapabilityCreateResult(std::string)> capabilityFactory;
	//! Test seam only. Production leaves this empty and constructs the concrete
	//! CSecretVaultExtensionGrantAuthority bound to the acquired authority identity.
	std::function<std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority>(std::string, std::uint64_t)>
		extensionGrantAuthorityFactory;
	std::optional<ControlPlatformServiceHostDependencies> hostDependencies;
};

//! Phase results stay typed so callers never have to infer a durable failure
//! from a diagnostic string. identity is present only after authority acquisition
//! and is published by Identity() only while the complete runtime is Running.
struct ControlPlatformRuntimeResult {
	EControlPlatformRuntimeResultCode code = EControlPlatformRuntimeResultCode::UnexpectedFailure;
	EControlPlatformRuntimeState state = EControlPlatformRuntimeState::Stopped;
	std::optional<profiles::ProfileAuthorityResult> authorityResult;
	std::optional<storage::StorageAuthorityOpenResult> storageOpenResult;
	std::optional<ControlPlatformRuntimeVaultCreateResult> vaultCreateResult;
	std::optional<ControlPlatformRuntimeMigrationCreateResult> migrationCreateResult;
	std::optional<ControlPlatformRuntimeCapabilityCreateResult> capabilityCreateResult;
	std::optional<ControlPlatformServiceHostResult> hostResult;
	std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult;
	std::optional<ControlPlatformRuntimeIdentity> identity;
	std::wstring diagnostic;
};

/*!
	@brief Owns the durable control-platform lifetime without UI or legacy shared data.

	Start order is fixed: validate resolved inputs, acquire profile authority, open
	durable storage, open the durable vault, create the legacy migration coordinator, create the capability authority, create
	the extension-grant authority, then start the endpoint/pipe host. A failure
	closes everything created by that attempt and returns Stopped. Stop order is the
	exact reverse.
*/
class CControlPlatformRuntime final {
public:
	explicit CControlPlatformRuntime(ControlPlatformRuntimeOptions options);
	CControlPlatformRuntime(ControlPlatformRuntimeOptions options,
		ControlPlatformRuntimeDependencies dependencies);
	~CControlPlatformRuntime();
	CControlPlatformRuntime(const CControlPlatformRuntime&) = delete;
	CControlPlatformRuntime& operator=(const CControlPlatformRuntime&) = delete;

	[[nodiscard]] ControlPlatformRuntimeResult Start();
	[[nodiscard]] ControlPlatformRuntimeResult Stop();
	[[nodiscard]] EControlPlatformRuntimeState State() const noexcept;
	[[nodiscard]] std::optional<ControlPlatformRuntimeIdentity> Identity() const;
	//! Same-process approval owners may mutate this authority only while the
	//! runtime is Running. Its terminal Stop() fences any retained handle.
	[[nodiscard]] std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> ExtensionGrantAuthority() const;
	//! Same-process composition may revoke host-session bearer capabilities only
	//! while the runtime is Running. Retained handles are terminally fenced by
	//! Stop(); callers must not use this as a general editor-facing service
	//! locator.
	[[nodiscard]] std::shared_ptr<secrets::ISecretVaultCapabilityService> SecretVaultCapabilities() const;
	//! Snapshots both Secret Vault authorities under one runtime lock. Returns
	//! nullopt unless the runtime is Running and both authorities are present.
	[[nodiscard]] std::optional<ControlPlatformRuntimeSecretVaultAuthorities> SecretVaultAuthorities() const;
	//! Control-process composition entry point for profile selection and mutation.
	//! It is unavailable to editors and becomes terminally fenced during Stop().
	[[nodiscard]] std::shared_ptr<profiles::ControlUserDataProfileRegistry> UserDataProfiles() const;

private:
	[[nodiscard]] bool HasValidOptions(std::wstring& diagnostic) const;
	[[nodiscard]] ControlPlatformRuntimeResult Result(EControlPlatformRuntimeResultCode code,
		std::optional<profiles::ProfileAuthorityResult> authorityResult = std::nullopt,
		std::optional<storage::StorageAuthorityOpenResult> storageOpenResult = std::nullopt,
		std::optional<ControlPlatformRuntimeVaultCreateResult> vaultCreateResult = std::nullopt,
		std::optional<ControlPlatformRuntimeCapabilityCreateResult> capabilityCreateResult = std::nullopt,
		std::optional<ControlPlatformServiceHostResult> hostResult = std::nullopt,
		std::wstring diagnostic = {},
		std::optional<ControlPlatformRuntimeMigrationCreateResult> migrationCreateResult = std::nullopt,
		std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult = std::nullopt) const;
	void RollbackStart() noexcept;

	const ControlPlatformRuntimeOptions m_options;
	const ControlPlatformRuntimeDependencies m_dependencies;
	mutable std::mutex m_mutex;
	EControlPlatformRuntimeState m_state = EControlPlatformRuntimeState::Stopped;
	std::optional<ControlPlatformRuntimeIdentity> m_identity;
	std::shared_ptr<storage::IStorageAuthority> m_storage;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profileRegistry;
	std::string m_profileRegistryShutdownOperationId;
	std::shared_ptr<secrets::ISecretVaultService> m_vault;
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> m_migration;
	std::shared_ptr<secrets::ISecretVaultCapabilityService> m_capabilities;
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> m_grantAuthority;
	std::unique_ptr<CControlPlatformServiceHost> m_host;
};

} // namespace platform::controlipc
