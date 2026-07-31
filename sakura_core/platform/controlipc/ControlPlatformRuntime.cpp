/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformRuntime.h"

#include "platform/controlipc/ControlIpcProtocol.h"
#include "platform/secrets/CSecretVaultCapabilityService.h"
#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/CSecretVaultLegacyMigrationCoordinator.h"

#include <chrono>
#include <string>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr auto kMaximumPipeIoTimeout = std::chrono::seconds(60);

bool IsResolvedPath(const std::filesystem::path& path)
{
	if (path.empty() || !path.is_absolute()) return false;
	const auto& native = path.native();
	if (native.find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos) return false;
	for (const auto& part : path) {
		if (part == std::filesystem::path(".") || part == std::filesystem::path("..")) return false;
	}
	return true;
}

bool HostStarted(const ControlPlatformServiceHostResult& result) noexcept
{
	return result.state == EControlPlatformServiceHostState::Started &&
		(result.code == EControlPlatformServiceHostResultCode::Started ||
			result.code == EControlPlatformServiceHostResultCode::AlreadyStarted);
}

bool HostStopped(const ControlPlatformServiceHostResult& result) noexcept
{
	return result.state == EControlPlatformServiceHostState::Stopped &&
		(result.code == EControlPlatformServiceHostResultCode::Stopped ||
			result.code == EControlPlatformServiceHostResultCode::AlreadyStopped);
}

ControlPlatformRuntimeVaultCreateResult CreateProductionVault(const std::filesystem::path& metadataRoot,
	std::string profileId)
{
	try {
		auto created = secrets::CWindowsDpapiSecretVaultService::Create(metadataRoot, std::move(profileId));
		if (!created.Succeeded()) {
			return { EControlPlatformRuntimeVaultCreateStatus::OpenFailed, std::move(created.open), nullptr };
		}
		std::shared_ptr<secrets::ISecretVaultService> vault(std::move(created.service));
		return { EControlPlatformRuntimeVaultCreateStatus::Created, std::move(created.open), std::move(vault) };
	} catch (...) {
		return { EControlPlatformRuntimeVaultCreateStatus::CreateFailed, std::nullopt, nullptr };
	}
}

ControlPlatformRuntimeCapabilityCreateResult CreateProductionCapabilities(std::string profileId)
{
	try {
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities =
			std::make_shared<secrets::CSecretVaultCapabilityService>(std::move(profileId));
		return { EControlPlatformRuntimeCapabilityCreateStatus::Created, std::move(capabilities), {} };
	} catch (...) {
		return { EControlPlatformRuntimeCapabilityCreateStatus::CreateFailed, nullptr,
			L"secret vault capability authority creation failed" };
	}
}

std::filesystem::path SecretVaultMetadataDirectory(const std::filesystem::path& metadataRoot)
{
	return metadataRoot / L"secretVault";
}

std::filesystem::path LegacyExtensionSecretVaultDirectory(const std::filesystem::path& profileDirectory)
{
	// This is the legacy CExtensionService owner root. It is profile-owned and is
	// intentionally distinct from the new control-owned .sakura-platform storage.
	return profileDirectory / L"extensionData" / L"secrets";
}

ControlPlatformRuntimeMigrationCreateResult CreateProductionMigration(
	std::shared_ptr<secrets::ISecretVaultService> vault, const std::filesystem::path& legacyBasePath)
{
	try {
		auto dpapiVault = std::dynamic_pointer_cast<secrets::CWindowsDpapiSecretVaultService>(std::move(vault));
		if (!dpapiVault) {
			return { EControlPlatformRuntimeMigrationCreateStatus::CreateFailed, nullptr,
				L"production legacy migration requires the durable DPAPI vault" };
		}
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration =
			std::make_shared<secrets::CSecretVaultLegacyMigrationCoordinator>(*dpapiVault, legacyBasePath);
		return { EControlPlatformRuntimeMigrationCreateStatus::Created, std::move(migration), {} };
	} catch (...) {
		return { EControlPlatformRuntimeMigrationCreateStatus::CreateFailed, nullptr,
			L"secret vault legacy migration coordinator creation failed" };
	}
}

} // namespace

CControlPlatformRuntime::CControlPlatformRuntime(ControlPlatformRuntimeOptions options) :
	CControlPlatformRuntime(std::move(options), {})
{
}

CControlPlatformRuntime::CControlPlatformRuntime(ControlPlatformRuntimeOptions options,
	ControlPlatformRuntimeDependencies dependencies) :
	m_options(std::move(options)),
	m_dependencies(std::move(dependencies))
{
}

CControlPlatformRuntime::~CControlPlatformRuntime()
{
	try {
		(void)Stop();
	} catch (...) {
		RollbackStart();
	}
}

bool CControlPlatformRuntime::HasValidOptions(std::wstring& diagnostic) const
{
	if (!IsResolvedPath(m_options.profileDirectory)) diagnostic = L"profileDirectory must be an absolute resolved path";
	else if (!IsResolvedPath(m_options.storageDirectory)) diagnostic = L"storageDirectory must be an absolute resolved path";
	else if (m_options.maximumCompletedOperations == 0 ||
		m_options.maximumCompletedOperations > storage::CAtomicFileStorageService::kMaximumCompletedOperations) {
		diagnostic = L"maximumCompletedOperations is out of range";
	}
	else if (m_options.pipeOptions.maximumSessions == 0 || m_options.pipeOptions.maximumSessions > 63) {
		diagnostic = L"maximumSessions must be between 1 and 63";
	}
	else if (m_options.pipeOptions.maximumQueuedBytes == 0 ||
		m_options.pipeOptions.maximumQueuedBytes > kControlIpcMaximumFrameBytes) {
		diagnostic = L"maximumQueuedBytes is out of range";
	}
	else if (m_options.pipeOptions.readBufferBytes == 0 ||
		m_options.pipeOptions.readBufferBytes > m_options.pipeOptions.maximumQueuedBytes) {
		diagnostic = L"readBufferBytes is out of range";
	}
	else if (m_options.pipeOptions.ioTimeout <= std::chrono::milliseconds::zero() ||
		m_options.pipeOptions.ioTimeout > kMaximumPipeIoTimeout) {
		diagnostic = L"ioTimeout is out of range";
	}
	else if (m_dependencies.hostDependencies &&
		(!m_dependencies.hostDependencies->endpointFactory || !m_dependencies.hostDependencies->pipeServerFactory)) {
		diagnostic = L"engaged hostDependencies must contain both factories";
	}
	else {
		return true;
	}
	return false;
}

ControlPlatformRuntimeResult CControlPlatformRuntime::Result(EControlPlatformRuntimeResultCode code,
	std::optional<profiles::ProfileAuthorityResult> authorityResult,
	std::optional<storage::AtomicFileStorageOpenResult> storageOpenResult,
	std::optional<ControlPlatformRuntimeVaultCreateResult> vaultCreateResult,
	std::optional<ControlPlatformRuntimeCapabilityCreateResult> capabilityCreateResult,
	std::optional<ControlPlatformServiceHostResult> hostResult,
	std::wstring diagnostic,
	std::optional<ControlPlatformRuntimeMigrationCreateResult> migrationCreateResult,
	std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult) const
{
	return {
		.code = code,
		.state = m_state,
		.authorityResult = std::move(authorityResult),
		.storageOpenResult = std::move(storageOpenResult),
		.vaultCreateResult = std::move(vaultCreateResult),
		.migrationCreateResult = std::move(migrationCreateResult),
		.capabilityCreateResult = std::move(capabilityCreateResult),
		.hostResult = std::move(hostResult),
		.profileRegistryResult = std::move(profileRegistryResult),
		.identity = m_state == EControlPlatformRuntimeState::Running ? m_identity : std::nullopt,
		.diagnostic = std::move(diagnostic),
	};
}

void CControlPlatformRuntime::RollbackStart() noexcept
{
	m_state = EControlPlatformRuntimeState::Stopping;
	if (m_host) {
		try {
			(void)m_host->Stop();
		} catch (...) {
			// The concrete host already owns its rollback. Continue releasing storage.
		}
		m_host.reset();
	}
	if (m_grantAuthority) {
		(void)m_grantAuthority->Stop();
		m_grantAuthority.reset();
	}
	if (m_capabilities) {
		(void)m_capabilities->Stop();
		m_capabilities.reset();
	}
	if (m_migration) {
		(void)m_migration->Stop();
		m_migration.reset();
	}
	if (m_vault) {
		(void)m_vault->Stop();
		m_vault.reset();
	}
	if (m_profileRegistry) {
		try {
			(void)m_profileRegistry->Stop({ m_profileRegistryShutdownOperationId, m_profileRegistry->StorageRevision() });
		} catch (...) {
			// Rollback still owns closing the durable storage writer.
		}
		m_profileRegistry.reset();
	}
	if (m_storage) {
		m_storage->Close();
		m_storage.reset();
	}
	m_identity.reset();
	m_profileRegistryShutdownOperationId.clear();
	m_state = EControlPlatformRuntimeState::Stopped;
}

ControlPlatformRuntimeResult CControlPlatformRuntime::Start()
{
	std::lock_guard lock(m_mutex);
	if (m_state == EControlPlatformRuntimeState::Running) {
		return Result(EControlPlatformRuntimeResultCode::AlreadyRunning);
	}

	std::wstring validationDiagnostic;
	if (!HasValidOptions(validationDiagnostic)) {
		m_state = EControlPlatformRuntimeState::Stopped;
		return Result(EControlPlatformRuntimeResultCode::InvalidOptions, std::nullopt, std::nullopt,
			std::nullopt, std::nullopt, std::nullopt, std::move(validationDiagnostic));
	}

	std::optional<profiles::ProfileAuthorityResult> authorityResult;
	std::optional<storage::AtomicFileStorageOpenResult> storageOpenResult;
	std::optional<ControlPlatformRuntimeVaultCreateResult> vaultCreateResult;
	std::optional<ControlPlatformRuntimeMigrationCreateResult> migrationCreateResult;
	std::optional<ControlPlatformRuntimeCapabilityCreateResult> capabilityCreateResult;
	std::optional<ControlPlatformServiceHostResult> hostResult;
	m_state = EControlPlatformRuntimeState::Starting;
	try {
		profiles::ProfileAuthorityStore authority(m_options.profileDirectory,
			m_dependencies.profileAuthorityBackend);
		authorityResult = authority.Acquire(m_options.legacyProfileAlias);
		if (!authorityResult->Succeeded()) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::AuthorityFailed, std::move(authorityResult));
		}

		try {
			m_storage = std::make_shared<storage::CAtomicFileStorageService>(m_options.storageDirectory,
				authorityResult->authorityGeneration, m_options.maximumCompletedOperations,
				m_dependencies.storageFileOperations);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::StorageCreateFailed, std::move(authorityResult),
				std::nullopt, std::nullopt, std::nullopt, std::nullopt, L"durable storage creation failed");
		}

		storageOpenResult = m_storage->Open();
		switch (storageOpenResult->status) {
		case storage::EAtomicFileStorageOpenStatus::Opened:
		case storage::EAtomicFileStorageOpenStatus::AlreadyOpen:
			break;
		case storage::EAtomicFileStorageOpenStatus::InvalidArgument:
		case storage::EAtomicFileStorageOpenStatus::WriterBusy:
		case storage::EAtomicFileStorageOpenStatus::IoError:
		case storage::EAtomicFileStorageOpenStatus::CorruptData:
		case storage::EAtomicFileStorageOpenStatus::UnsupportedFormat:
		default:
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::StorageOpenFailed, std::move(authorityResult),
				std::move(storageOpenResult));
		}

		profiles::ControlUserDataProfileRegistryResult profileRegistryResult;
		try {
			m_profileRegistryShutdownOperationId = "profile-registry-shutdown-"
				+ std::to_string(authorityResult->authorityGeneration);
			m_profileRegistry = std::make_shared<profiles::ControlUserDataProfileRegistry>(m_storage);
			profileRegistryResult = m_profileRegistry->Start();
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::ProfileRegistryCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::nullopt, std::nullopt, std::nullopt,
				L"durable user-data profile registry creation failed");
		}
		if (!profileRegistryResult.Succeeded()) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::ProfileRegistryLoadFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::nullopt, std::nullopt, std::nullopt,
				L"durable user-data profile registry load failed", std::nullopt, std::move(profileRegistryResult));
		}

		auto vaultFactory = m_dependencies.vaultFactory;
		if (!vaultFactory) vaultFactory = CreateProductionVault;
		try {
			vaultCreateResult = vaultFactory(SecretVaultMetadataDirectory(m_options.storageDirectory), authorityResult->profileId);
		} catch (...) {
			vaultCreateResult = ControlPlatformRuntimeVaultCreateResult{ EControlPlatformRuntimeVaultCreateStatus::CreateFailed,
				std::nullopt, nullptr };
		}
		if (!vaultCreateResult->Succeeded() || vaultCreateResult->vault->GetProfileId() != authorityResult->profileId) {
			if (vaultCreateResult->vault) (void)vaultCreateResult->vault->Stop();
			if (vaultCreateResult->Succeeded()) {
				// A factory returned a live but incorrectly bound authority. It is still
				// owned by this failed attempt and must reach its terminal state.
				vaultCreateResult->status = EControlPlatformRuntimeVaultCreateStatus::CreateFailed;
			}
			const auto code = vaultCreateResult->status == EControlPlatformRuntimeVaultCreateStatus::OpenFailed
				? EControlPlatformRuntimeResultCode::VaultOpenFailed : EControlPlatformRuntimeResultCode::VaultCreateFailed;
			RollbackStart();
			return Result(code, std::move(authorityResult), std::move(storageOpenResult), std::move(vaultCreateResult));
		}
		m_vault = vaultCreateResult->vault;

		auto migrationFactory = m_dependencies.migrationFactory;
		if (!migrationFactory) migrationFactory = CreateProductionMigration;
		try {
			migrationCreateResult = migrationFactory(m_vault, LegacyExtensionSecretVaultDirectory(m_options.profileDirectory));
		} catch (...) {
			migrationCreateResult = ControlPlatformRuntimeMigrationCreateResult{
				EControlPlatformRuntimeMigrationCreateStatus::CreateFailed, nullptr,
				L"secret vault legacy migration coordinator creation failed" };
		}
		if (!migrationCreateResult->Succeeded()) {
			// Keep the failure diagnostic before moving the typed phase result into
			// the terminal response. Function-argument evaluation order would
			// otherwise allow the move to empty diagnostic first.
			auto migrationDiagnostic = migrationCreateResult->diagnostic;
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::MigrationCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::nullopt, std::nullopt,
				std::move(migrationDiagnostic), std::move(migrationCreateResult));
		}
		m_migration = migrationCreateResult->migration;

		auto capabilityFactory = m_dependencies.capabilityFactory;
		if (!capabilityFactory) capabilityFactory = CreateProductionCapabilities;
		try {
			capabilityCreateResult = capabilityFactory(authorityResult->profileId);
		} catch (...) {
			capabilityCreateResult = ControlPlatformRuntimeCapabilityCreateResult{
				EControlPlatformRuntimeCapabilityCreateStatus::CreateFailed, nullptr,
				L"secret vault capability authority creation failed" };
		}
		if (!capabilityCreateResult->Succeeded()
			|| capabilityCreateResult->capabilities->GetProfileId() != authorityResult->profileId) {
			if (capabilityCreateResult->capabilities) (void)capabilityCreateResult->capabilities->Stop();
			if (capabilityCreateResult->Succeeded()) {
				capabilityCreateResult->status = EControlPlatformRuntimeCapabilityCreateStatus::CreateFailed;
				capabilityCreateResult->diagnostic = L"secret vault capability authority profile mismatch";
			}
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::CapabilityCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult), std::nullopt,
				{}, std::move(migrationCreateResult));
		}
		m_capabilities = capabilityCreateResult->capabilities;

		try {
			if (m_dependencies.extensionGrantAuthorityFactory) {
				m_grantAuthority = m_dependencies.extensionGrantAuthorityFactory(
					authorityResult->profileId, authorityResult->authorityGeneration);
			}
			else {
				m_grantAuthority = std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>(
					authorityResult->profileId, authorityResult->authorityGeneration);
			}
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::GrantAuthorityCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult),
				std::nullopt, L"secret vault extension grant authority creation failed", std::move(migrationCreateResult));
		}
		if (!m_grantAuthority || m_grantAuthority->GetProfileId() != authorityResult->profileId
			|| m_grantAuthority->GetControlConnectionGeneration() != authorityResult->authorityGeneration) {
			if (m_grantAuthority) (void)m_grantAuthority->Stop();
			m_grantAuthority.reset();
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::GrantAuthorityCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult),
				std::nullopt, L"secret vault extension grant authority identity mismatch", std::move(migrationCreateResult));
		}

		ControlPlatformServiceHostOptions hostOptions;
		hostOptions.profileDirectory = m_options.profileDirectory;
		hostOptions.profileId = authorityResult->profileId;
		hostOptions.authorityGeneration = authorityResult->authorityGeneration;
		hostOptions.pipeOptions = m_options.pipeOptions;
		try {
			if (m_dependencies.hostDependencies) {
				m_host = std::make_unique<CControlPlatformServiceHost>(std::move(hostOptions), m_storage, m_vault,
					m_capabilities, m_grantAuthority, m_migration, m_profileRegistry, *m_dependencies.hostDependencies);
			}
			else {
				m_host = std::make_unique<CControlPlatformServiceHost>(std::move(hostOptions), m_storage, m_vault,
					m_capabilities, m_grantAuthority, m_migration, m_profileRegistry);
			}
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::HostCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult),
				std::nullopt, L"control platform host creation failed", std::move(migrationCreateResult));
		}

		hostResult = m_host->Start();
		if (!HostStarted(*hostResult)) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::HostStartFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult), std::move(hostResult),
				{}, std::move(migrationCreateResult));
		}

		m_identity = ControlPlatformRuntimeIdentity{
			.profileId = authorityResult->profileId,
			.authorityGeneration = authorityResult->authorityGeneration,
		};
		m_state = EControlPlatformRuntimeState::Running;
		return Result(EControlPlatformRuntimeResultCode::Running, std::move(authorityResult),
			std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult), std::move(hostResult),
			{}, std::move(migrationCreateResult), std::move(profileRegistryResult));
	}
	catch (...) {
		RollbackStart();
		return Result(EControlPlatformRuntimeResultCode::UnexpectedFailure, std::move(authorityResult),
			std::move(storageOpenResult), std::move(vaultCreateResult), std::move(capabilityCreateResult),
			std::move(hostResult), L"control platform runtime startup failed", std::move(migrationCreateResult));
	}
}

ControlPlatformRuntimeResult CControlPlatformRuntime::Stop()
{
	std::lock_guard lock(m_mutex);
	if (m_state == EControlPlatformRuntimeState::Stopped) {
		return Result(EControlPlatformRuntimeResultCode::AlreadyStopped);
	}

	std::optional<ControlPlatformServiceHostResult> hostResult;
	std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult;
	m_state = EControlPlatformRuntimeState::Stopping;
	try {
		if (m_host) hostResult = m_host->Stop();
		m_host.reset();
		if (m_grantAuthority) {
			(void)m_grantAuthority->Stop();
			m_grantAuthority.reset();
		}
		if (m_capabilities) {
			(void)m_capabilities->Stop();
			m_capabilities.reset();
		}
		if (m_migration) {
			(void)m_migration->Stop();
			m_migration.reset();
		}
		if (m_vault) {
			(void)m_vault->Stop();
			m_vault.reset();
		}
		if (m_profileRegistry) {
			profileRegistryResult = m_profileRegistry->Stop(
				{ m_profileRegistryShutdownOperationId, m_profileRegistry->StorageRevision() });
			m_profileRegistry.reset();
		}
		if (m_storage) {
			m_storage->Close();
			m_storage.reset();
		}
		m_identity.reset();
		m_profileRegistryShutdownOperationId.clear();
		m_state = EControlPlatformRuntimeState::Stopped;
		if (hostResult && !HostStopped(*hostResult)) {
			return Result(EControlPlatformRuntimeResultCode::HostStopFailed, std::nullopt,
				std::nullopt, std::nullopt, std::nullopt, std::move(hostResult), {}, std::nullopt,
				std::move(profileRegistryResult));
		}
		if (profileRegistryResult && !profileRegistryResult->Succeeded()) {
			return Result(EControlPlatformRuntimeResultCode::ProfileRegistrySaveFailed, std::nullopt,
				std::nullopt, std::nullopt, std::nullopt, std::move(hostResult), {}, std::nullopt,
				std::move(profileRegistryResult));
		}
		return Result(EControlPlatformRuntimeResultCode::Stopped, std::nullopt,
			std::nullopt, std::nullopt, std::nullopt, std::move(hostResult), {}, std::nullopt,
			std::move(profileRegistryResult));
	}
	catch (...) {
		RollbackStart();
		return Result(EControlPlatformRuntimeResultCode::UnexpectedFailure, std::nullopt,
			std::nullopt, std::nullopt, std::nullopt, std::move(hostResult),
			L"control platform runtime shutdown failed");
	}
}

EControlPlatformRuntimeState CControlPlatformRuntime::State() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_state;
}

std::optional<ControlPlatformRuntimeIdentity> CControlPlatformRuntime::Identity() const
{
	std::lock_guard lock(m_mutex);
	return m_state == EControlPlatformRuntimeState::Running ? m_identity : std::nullopt;
}

std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> CControlPlatformRuntime::ExtensionGrantAuthority() const
{
	std::lock_guard lock(m_mutex);
	return m_state == EControlPlatformRuntimeState::Running ? m_grantAuthority : nullptr;
}

std::shared_ptr<secrets::ISecretVaultCapabilityService> CControlPlatformRuntime::SecretVaultCapabilities() const
{
	std::lock_guard lock(m_mutex);
	return m_state == EControlPlatformRuntimeState::Running ? m_capabilities : nullptr;
}

std::optional<ControlPlatformRuntimeSecretVaultAuthorities> CControlPlatformRuntime::SecretVaultAuthorities() const
{
	std::lock_guard lock(m_mutex);
	if (m_state != EControlPlatformRuntimeState::Running || !m_grantAuthority || !m_capabilities) {
		return std::nullopt;
	}
	return ControlPlatformRuntimeSecretVaultAuthorities{ m_grantAuthority, m_capabilities };
}

std::shared_ptr<profiles::ControlUserDataProfileRegistry> CControlPlatformRuntime::UserDataProfiles() const
{
	std::lock_guard lock(m_mutex);
	return m_state == EControlPlatformRuntimeState::Running ? m_profileRegistry : nullptr;
}

} // namespace platform::controlipc
