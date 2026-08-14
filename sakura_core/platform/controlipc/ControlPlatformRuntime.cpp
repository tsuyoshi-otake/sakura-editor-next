/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformRuntime.h"

#include <sakura/controlipc/ControlIpcProtocol.h>
#include <sakura/storage/StorageAuthorityFactory.h>

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

std::shared_ptr<storage::IStorageAuthority> CreateProductionStorage(
	const std::filesystem::path& directory, std::uint64_t generation, std::size_t maxCompletedOperations)
{
	return storage::CreateAtomicFileStorageAuthority(directory, generation, maxCompletedOperations);
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
		m_options.maximumCompletedOperations > storage::kMaximumStorageCompletedOperations) {
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
	std::optional<storage::StorageAuthorityOpenResult> storageOpenResult,
	std::optional<ControlPlatformServiceHostResult> hostResult,
	std::wstring diagnostic,
	std::optional<profiles::ControlUserDataProfileRegistryResult> profileRegistryResult) const
{
	return {
		.code = code,
		.state = m_state,
		.authorityResult = std::move(authorityResult),
		.storageOpenResult = std::move(storageOpenResult),
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
			std::nullopt, std::move(validationDiagnostic));
	}

	std::optional<profiles::ProfileAuthorityResult> authorityResult;
	std::optional<storage::StorageAuthorityOpenResult> storageOpenResult;
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

		auto storageFactory = m_dependencies.storageFactory;
		if (!storageFactory) storageFactory = CreateProductionStorage;
		try {
			m_storage = storageFactory(m_options.storageDirectory, authorityResult->authorityGeneration,
				m_options.maximumCompletedOperations);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::StorageCreateFailed, std::move(authorityResult),
				std::nullopt, std::nullopt, L"durable storage creation failed");
		}
		if (!m_storage) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::StorageCreateFailed, std::move(authorityResult),
				std::nullopt, std::nullopt, L"storage factory returned null");
		}

		storageOpenResult = m_storage->Open();
		switch (storageOpenResult->status) {
		case storage::EStorageAuthorityOpenStatus::Opened:
		case storage::EStorageAuthorityOpenStatus::AlreadyOpen:
			break;
		case storage::EStorageAuthorityOpenStatus::InvalidArgument:
		case storage::EStorageAuthorityOpenStatus::WriterBusy:
		case storage::EStorageAuthorityOpenStatus::IoError:
		case storage::EStorageAuthorityOpenStatus::CorruptData:
		case storage::EStorageAuthorityOpenStatus::UnsupportedFormat:
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
				std::move(storageOpenResult), std::nullopt,
				L"durable user-data profile registry creation failed");
		}
		if (!profileRegistryResult.Succeeded()) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::ProfileRegistryLoadFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::nullopt,
				L"durable user-data profile registry load failed", std::move(profileRegistryResult));
		}

		ControlPlatformServiceHostOptions hostOptions;
		hostOptions.profileDirectory = m_options.profileDirectory;
		hostOptions.profileId = authorityResult->profileId;
		hostOptions.authorityGeneration = authorityResult->authorityGeneration;
		hostOptions.pipeOptions = m_options.pipeOptions;
		try {
			if (m_dependencies.hostDependencies) {
				m_host = std::make_unique<CControlPlatformServiceHost>(std::move(hostOptions), m_storage,
					m_profileRegistry, *m_dependencies.hostDependencies);
			}
			else {
				m_host = std::make_unique<CControlPlatformServiceHost>(std::move(hostOptions), m_storage,
					m_profileRegistry);
			}
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::HostCreateFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::nullopt, L"control platform host creation failed");
		}

		hostResult = m_host->Start();
		if (!HostStarted(*hostResult)) {
			RollbackStart();
			return Result(EControlPlatformRuntimeResultCode::HostStartFailed, std::move(authorityResult),
				std::move(storageOpenResult), std::move(hostResult));
		}

		m_identity = ControlPlatformRuntimeIdentity{
			.profileId = authorityResult->profileId,
			.authorityGeneration = authorityResult->authorityGeneration,
		};
		m_state = EControlPlatformRuntimeState::Running;
		return Result(EControlPlatformRuntimeResultCode::Running, std::move(authorityResult),
			std::move(storageOpenResult), std::move(hostResult), {}, std::move(profileRegistryResult));
	}
	catch (...) {
		RollbackStart();
		return Result(EControlPlatformRuntimeResultCode::UnexpectedFailure, std::move(authorityResult),
			std::move(storageOpenResult), std::move(hostResult), L"control platform runtime startup failed");
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
				std::nullopt, std::move(hostResult), {},
				std::move(profileRegistryResult));
		}
		if (profileRegistryResult && !profileRegistryResult->Succeeded()) {
			return Result(EControlPlatformRuntimeResultCode::ProfileRegistrySaveFailed, std::nullopt,
				std::nullopt, std::move(hostResult), {},
				std::move(profileRegistryResult));
		}
		return Result(EControlPlatformRuntimeResultCode::Stopped, std::nullopt,
			std::nullopt, std::move(hostResult), {},
			std::move(profileRegistryResult));
	}
	catch (...) {
		RollbackStart();
		return Result(EControlPlatformRuntimeResultCode::UnexpectedFailure, std::nullopt,
			std::nullopt, std::move(hostResult),
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

std::shared_ptr<profiles::ControlUserDataProfileRegistry> CControlPlatformRuntime::UserDataProfiles() const
{
	std::lock_guard lock(m_mutex);
	return m_state == EControlPlatformRuntimeState::Running ? m_profileRegistry : nullptr;
}

} // namespace platform::controlipc
