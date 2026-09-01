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

//! Immutable inputs resolved by the legacy process-composition layer before
//! this UI-independent runtime is started.
struct ControlPlatformRuntimeOptions {
	std::filesystem::path profileDirectory;
	std::filesystem::path storageDirectory;
	std::wstring legacyProfileAlias;
	std::size_t maximumCompletedOperations = storage::kMaximumStorageCompletedOperations;
	ControlIpcNamedPipeOptions pipeOptions;
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
	std::optional<ControlPlatformServiceHostResult> hostResult;
	std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult;
	std::optional<ControlPlatformRuntimeIdentity> identity;
	std::wstring diagnostic;
};

/*!
	@brief Owns the durable control-platform lifetime without UI or legacy shared data.

	Start order is fixed: validate resolved inputs, prepare the next profile-authority
	candidate, open durable storage as its pre-commit check, commit the authority,
	load the profile registry, then start the endpoint/pipe host. A failure closes
	everything created by that attempt and returns Stopped. Stop order is the exact
	reverse.
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
	//! Control-process composition entry point for profile selection and mutation.
	//! It is unavailable to editors and becomes terminally fenced during Stop().
	[[nodiscard]] std::shared_ptr<profiles::ControlUserDataProfileRegistry> UserDataProfiles() const;

private:
	[[nodiscard]] bool HasValidOptions(std::wstring& diagnostic) const;
	[[nodiscard]] ControlPlatformRuntimeResult Result(EControlPlatformRuntimeResultCode code,
		std::optional<profiles::ProfileAuthorityResult> authorityResult = std::nullopt,
		std::optional<storage::StorageAuthorityOpenResult> storageOpenResult = std::nullopt,
		std::optional<ControlPlatformServiceHostResult> hostResult = std::nullopt,
		std::wstring diagnostic = {},
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
	std::unique_ptr<CControlPlatformServiceHost> m_host;
};

} // namespace platform::controlipc
