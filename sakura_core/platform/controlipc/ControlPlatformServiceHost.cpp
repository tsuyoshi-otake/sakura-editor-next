/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformServiceHost.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/controlipc/ControlPlatformRpcServerAdapter.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <Windows.h>

#include <chrono>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr auto kMaximumPipeIoTimeout = std::chrono::seconds(60);
class CProductionEndpoint final : public IControlPlatformServiceEndpoint {
public:
	bool CreateForControl(const std::filesystem::path& profileDirectory, std::wstring& diagnostic) override
	{
		return m_endpoint.CreateForControl(profileDirectory, diagnostic);
	}

	void Close() noexcept override { m_endpoint.Close(); }

	bool Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic) override
	{
		return m_endpoint.Publish(snapshot, diagnostic);
	}

	const std::wstring& ProfileHash() const noexcept override { return m_endpoint.ProfileHash(); }

private:
	CControlPlatformEndpoint m_endpoint;
};

class CProductionPipeServer final : public IControlPlatformServicePipeServer {
public:
	explicit CProductionPipeServer(std::shared_ptr<IControlIpcFrameHandler> handler) : m_server(std::move(handler)) {}

	ControlIpcTransportResult Start(const ControlIpcNamedPipeOptions& options) override { return m_server.Start(options); }
	void Stop() noexcept override { m_server.Stop(); }

private:
	CControlIpcNamedPipeServer m_server;
};

ControlPlatformServiceHostDependencies ProductionDependencies()
{
	return {
		[] { return std::make_unique<CProductionEndpoint>(); },
		[](std::shared_ptr<IControlIpcFrameHandler> handler) {
			return std::make_unique<CProductionPipeServer>(std::move(handler));
		},
	};
}

} // namespace

CControlPlatformServiceHost::CControlPlatformServiceHost(ControlPlatformServiceHostOptions options,
	std::shared_ptr<storage::IStorageAuthority> storage, std::shared_ptr<secrets::ISecretVaultService> vault,
	std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles) :
	CControlPlatformServiceHost(std::move(options), std::move(storage), std::move(vault), std::move(capabilities),
		std::move(grantAuthority), std::move(migration), std::move(profiles), ProductionDependencies())
{
}

CControlPlatformServiceHost::CControlPlatformServiceHost(ControlPlatformServiceHostOptions options,
	std::shared_ptr<storage::IStorageAuthority> storage,
	std::shared_ptr<secrets::ISecretVaultService> vault,
	std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
	ControlPlatformServiceHostDependencies dependencies) :
	m_options(std::move(options)),
	m_storage(std::move(storage)),
	m_vault(std::move(vault)),
	m_capabilities(std::move(capabilities)),
	m_grantAuthority(std::move(grantAuthority)),
	m_migration(std::move(migration)),
	m_profiles(std::move(profiles)),
	m_dependencies(std::move(dependencies))
{
}

CControlPlatformServiceHost::~CControlPlatformServiceHost()
{
	try {
		(void)Stop();
	} catch (...) {
		// A destructor may not expose lifecycle cleanup failures.
	}
}

bool CControlPlatformServiceHost::HasValidOptions(std::wstring& diagnostic) const
{
	if (m_options.profileDirectory.empty()) diagnostic = L"profileDirectory is required";
	else if (m_options.authorityGeneration == 0) diagnostic = L"authorityGeneration is required";
	else if (!profiles::IsCanonicalProfileAuthorityId(m_options.profileId)) diagnostic = L"profileId must be a canonical profile authority identifier";
	else if (!m_storage) diagnostic = L"storage is required";
	else if (!m_profiles) diagnostic = L"profile registry is required";
	else if (!m_vault || !m_capabilities || !m_grantAuthority || !m_migration) diagnostic = L"secret vault authorities are required";
	else if (m_vault->GetProfileId() != m_options.profileId || m_capabilities->GetProfileId() != m_options.profileId) diagnostic = L"secret authorities must match profileId";
	else if (m_grantAuthority->GetProfileId() != m_options.profileId
		|| m_grantAuthority->GetControlConnectionGeneration() != m_options.authorityGeneration) diagnostic = L"secret grant authority must match control identity";
	else if (!m_dependencies.endpointFactory || !m_dependencies.pipeServerFactory) diagnostic = L"control IPC factories are required";
	else if (m_options.pipeOptions.maximumSessions == 0 || m_options.pipeOptions.maximumSessions > 63) diagnostic = L"maximumSessions must be between 1 and 63";
	else if (m_options.pipeOptions.maximumQueuedBytes == 0 ||
		m_options.pipeOptions.maximumQueuedBytes > kControlIpcMaximumFrameBytes) diagnostic = L"maximumQueuedBytes is out of range";
	else if (m_options.pipeOptions.readBufferBytes == 0 ||
		m_options.pipeOptions.readBufferBytes > m_options.pipeOptions.maximumQueuedBytes) diagnostic = L"readBufferBytes is out of range";
	else if (m_options.pipeOptions.ioTimeout <= std::chrono::milliseconds::zero() ||
		m_options.pipeOptions.ioTimeout > kMaximumPipeIoTimeout) diagnostic = L"ioTimeout is out of range";
	else return true;
	return false;
}

ControlPlatformEndpointSnapshot CControlPlatformServiceHost::Snapshot(ControlPlatformEndpointLifecycle lifecycle) const
{
	const auto profileHash = m_endpoint ? m_endpoint->ProfileHash() : std::wstring{};
	return {
		.controlProcessId = ::GetCurrentProcessId(),
		.generation = m_options.authorityGeneration,
		.lifecycle = lifecycle,
		.profileHash = profileHash,
		.pipeName = BuildControlPipeName(profileHash),
		.profileId = m_options.profileId,
	};
}

ControlPlatformServiceHostResult CControlPlatformServiceHost::Result(EControlPlatformServiceHostResultCode code,
	std::wstring diagnostic) const
{
	return { code, m_state, std::move(diagnostic) };
}

ControlPlatformServiceHostResult CControlPlatformServiceHost::Start()
{
	std::lock_guard lock(m_mutex);
	if (m_state == EControlPlatformServiceHostState::Started) return Result(EControlPlatformServiceHostResultCode::AlreadyStarted);

	try {
		std::wstring diagnostic;
		if (!HasValidOptions(diagnostic)) return Result(EControlPlatformServiceHostResultCode::InvalidOptions, std::move(diagnostic));

		m_state = EControlPlatformServiceHostState::Starting;
		try {
			m_endpoint = m_dependencies.endpointFactory();
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::EndpointFactoryFailed, L"endpoint factory failed");
		}
		if (!m_endpoint) {
			m_state = EControlPlatformServiceHostState::Stopped;
			return Result(EControlPlatformServiceHostResultCode::EndpointFactoryFailed, L"endpoint factory returned null");
		}
		bool endpointCreated = false;
		try {
			endpointCreated = m_endpoint->CreateForControl(m_options.profileDirectory, diagnostic);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::EndpointCreateFailed, L"endpoint creation failed");
		}
		if (!endpointCreated) {
			m_endpoint->Close();
			m_endpoint.reset();
			m_state = EControlPlatformServiceHostState::Stopped;
			return Result(EControlPlatformServiceHostResultCode::EndpointCreateFailed, std::move(diagnostic));
		}

		const auto starting = Snapshot(ControlPlatformEndpointLifecycle::Starting);
		bool publishedStarting = false;
		try {
			publishedStarting = !starting.profileHash.empty() && !starting.pipeName.empty() &&
				m_endpoint->Publish(starting, diagnostic);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::StartingPublishFailed, L"starting endpoint publish failed");
		}
		if (!publishedStarting) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::StartingPublishFailed, std::move(diagnostic));
		}

		try {
			m_adapter = std::make_shared<CControlPlatformRpcServerAdapter>(
				ControlStorageRpcSessionIdentity{ m_options.profileId, m_options.authorityGeneration },
				m_storage, m_vault, m_capabilities, m_grantAuthority, m_migration, m_profiles);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::AdapterCreateFailed, L"control platform RPC adapter creation failed");
		}
		try {
			m_pipeServer = m_dependencies.pipeServerFactory(m_adapter);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::PipeServerFactoryFailed, L"pipe server factory failed");
		}
		if (!m_pipeServer) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::PipeServerFactoryFailed, L"pipe server factory returned null");
		}

		auto pipeOptions = m_options.pipeOptions;
		pipeOptions.pipeName = starting.pipeName;
		ControlIpcTransportResult pipeStart;
		try {
			pipeStart = m_pipeServer->Start(pipeOptions);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::PipeStartFailed, L"pipe startup failed");
		}
		if (!pipeStart.success) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::PipeStartFailed, pipeStart.diagnostic);
		}

		bool publishedAccepting = false;
		try {
			publishedAccepting = m_endpoint->Publish(Snapshot(ControlPlatformEndpointLifecycle::Accepting), diagnostic);
		} catch (...) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::AcceptingPublishFailed, L"accepting endpoint publish failed");
		}
		if (!publishedAccepting) {
			RollbackStart();
			return Result(EControlPlatformServiceHostResultCode::AcceptingPublishFailed, std::move(diagnostic));
		}

		m_state = EControlPlatformServiceHostState::Started;
		return Result(EControlPlatformServiceHostResultCode::Started);
	} catch (...) {
		RollbackStart();
		return Result(EControlPlatformServiceHostResultCode::UnexpectedFailure, L"control platform service startup failed");
	}
}

void CControlPlatformServiceHost::ReleaseStoppedResources() noexcept
{
	m_pipeServer.reset();
	m_adapter.reset();
	if (m_endpoint) {
		m_endpoint->Close();
		m_endpoint.reset();
	}
}

void CControlPlatformServiceHost::RollbackStart() noexcept
{
	m_state = EControlPlatformServiceHostState::Stopping;
	if (m_adapter) (void)m_adapter->BeginStopping();
	if (m_pipeServer) m_pipeServer->Stop();
	if (m_adapter) m_adapter->Stop();
	if (m_endpoint) {
		try {
			std::wstring ignored;
			(void)m_endpoint->Publish(Snapshot(ControlPlatformEndpointLifecycle::Stopped), ignored);
		} catch (...) {
			// Rollback is best-effort publication; resource withdrawal still has an owner below.
		}
	}
	ReleaseStoppedResources();
	m_state = EControlPlatformServiceHostState::Stopped;
}

ControlPlatformServiceHostResult CControlPlatformServiceHost::Stop()
{
	std::lock_guard lock(m_mutex);
	if (m_state == EControlPlatformServiceHostState::Stopped) return Result(EControlPlatformServiceHostResultCode::AlreadyStopped);

	m_state = EControlPlatformServiceHostState::Stopping;
	try {
		if (m_adapter) (void)m_adapter->BeginStopping();

		std::wstring diagnostic;
		const bool publishedStopping = !m_endpoint || m_endpoint->Publish(Snapshot(ControlPlatformEndpointLifecycle::Stopping), diagnostic);
		if (m_pipeServer) m_pipeServer->Stop();
		if (m_adapter) m_adapter->Stop();
		if (m_endpoint) {
			std::wstring ignored;
			(void)m_endpoint->Publish(Snapshot(ControlPlatformEndpointLifecycle::Stopped), ignored);
		}
		ReleaseStoppedResources();
		m_state = EControlPlatformServiceHostState::Stopped;
		return Result(publishedStopping ? EControlPlatformServiceHostResultCode::Stopped :
			EControlPlatformServiceHostResultCode::StoppingPublishFailed, std::move(diagnostic));
	} catch (...) {
		// Preserve the terminal-state guarantee even if a test seam or platform call throws.
		try {
			RollbackStart();
		} catch (...) {
			m_state = EControlPlatformServiceHostState::Stopped;
		}
		return Result(EControlPlatformServiceHostResultCode::UnexpectedFailure, L"control platform service shutdown failed");
	}
}

EControlPlatformServiceHostState CControlPlatformServiceHost::State() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_state;
}

} // namespace platform::controlipc
