/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformRpcServerAdapter.h"

#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace platform::controlipc {
namespace {

ControlIpcFrame ErrorResponse(const ControlIpcFrame& request, EControlIpcTerminalStatus status,
	std::uint64_t generation) noexcept
{
	try {
		// This boundary must never turn request data, addresses, or values into a
		// diagnostic. The empty protocol diagnostic is intentional and value-free.
		const auto payload = EncodeControlIpcError({ status, {} });
		return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Error,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation },
			payload.value_or(std::vector<std::uint8_t>{}) };
	} catch (...) {
		return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Error,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation }, {} };
	}
}

bool IsStorageKind(EControlIpcKind kind) noexcept
{
	switch (kind) {
	case EControlIpcKind::Hello:
	case EControlIpcKind::StorageSnapshotRequest:
	case EControlIpcKind::StorageApplyRequest:
	case EControlIpcKind::CancelRequest:
		return true;
	default:
		return false;
	}
}

bool IsSecretKind(EControlIpcKind kind) noexcept
{
	return kind == EControlIpcKind::SecretGetRequest || kind == EControlIpcKind::SecretApplyRequest
		|| kind == EControlIpcKind::SecretCapabilityIssueRequest
		|| kind == EControlIpcKind::SecretCapabilityRevokeSessionRequest;
}

bool IsProfileKind(EControlIpcKind kind) noexcept
{
	return kind == EControlIpcKind::ProfileRequest;
}

void ValidateIdentity(const ControlStorageRpcSessionIdentity& identity,
	const std::shared_ptr<secrets::ISecretVaultService>& vault,
	const std::shared_ptr<secrets::ISecretVaultCapabilityService>& capabilities,
	const std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority>& grantAuthority,
	const std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator>& migration)
{
	if (identity.generation == 0 || !profiles::IsCanonicalProfileAuthorityId(identity.profileId)
		|| !vault || !capabilities || vault->GetProfileId() != identity.profileId
		|| capabilities->GetProfileId() != identity.profileId || !grantAuthority || !migration
		|| grantAuthority->GetProfileId() != identity.profileId
		|| grantAuthority->GetControlConnectionGeneration() != identity.generation) {
		throw std::invalid_argument("Control platform RPC adapter requires matching canonical authorities");
	}
}

} // namespace

struct CControlPlatformRpcServerAdapter::Gate final {
	mutable std::shared_mutex mutex;
	EControlPlatformRpcServerAdapterState state = EControlPlatformRpcServerAdapterState::Accepting;
};

class CControlPlatformRpcServerAdapter::SessionHandler final : public IControlIpcSessionHandler {
public:
	SessionHandler(ControlStorageRpcSessionIdentity identity, const ControlIpcSessionContext& context,
		std::shared_ptr<storage::IStorageService> storage,
		std::shared_ptr<secrets::ISecretVaultService> vault,
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
		std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
		std::shared_ptr<Gate> gate) :
		m_storageSession(std::move(identity), *storage),
		m_secretSession({ m_storageSession.GetIdentity().profileId, context.clientProcessId,
			m_storageSession.GetIdentity().generation }, *vault, *capabilities, *grantAuthority, *migration),
		m_storage(std::move(storage)), m_vault(std::move(vault)), m_capabilities(std::move(capabilities)),
		m_grantAuthority(std::move(grantAuthority)), m_migration(std::move(migration)), m_profiles(std::move(profiles)),
		m_profileSession(m_storageSession.GetIdentity(), m_profiles), m_gate(std::move(gate))
	{
	}

	ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame& frame) override
	{
		std::shared_lock lock(m_gate->mutex);
		const auto generation = m_storageSession.GetIdentity().generation;
		if (m_gate->state != EControlPlatformRpcServerAdapterState::Accepting) {
			return { { ErrorResponse(frame, EControlIpcTerminalStatus::ServerStopping, generation) }, EControlIpcSessionDecision::Close };
		}

		if (IsStorageKind(frame.header.kind)) {
			auto response = m_storageSession.Process(frame);
			if (frame.header.kind == EControlIpcKind::Hello && response.header.kind == EControlIpcKind::HelloAck) {
				m_storageHelloCompleted = true;
			}
			return { { std::move(response) }, EControlIpcSessionDecision::KeepOpen };
		}
		if (IsSecretKind(frame.header.kind)) {
			if (!m_storageHelloCompleted) {
				return { { ErrorResponse(frame, EControlIpcTerminalStatus::InvalidRequest, generation) }, EControlIpcSessionDecision::KeepOpen };
			}
			return { { m_secretSession.Process(frame) }, EControlIpcSessionDecision::KeepOpen };
		}
		if (IsProfileKind(frame.header.kind)) {
			if (!m_storageHelloCompleted) {
				return { { ErrorResponse(frame, EControlIpcTerminalStatus::InvalidRequest, generation) }, EControlIpcSessionDecision::KeepOpen };
			}
			return { { m_profileSession.Process(frame) }, EControlIpcSessionDecision::KeepOpen };
		}
		return { { ErrorResponse(frame, EControlIpcTerminalStatus::InvalidRequest, generation) }, EControlIpcSessionDecision::KeepOpen };
	}

private:
	CControlStorageRpcSession m_storageSession;
	CControlSecretVaultRpcSession m_secretSession;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profiles;
	CControlProfileRpcSession m_profileSession;
	bool m_storageHelloCompleted = false;
	std::shared_ptr<storage::IStorageService> m_storage;
	std::shared_ptr<secrets::ISecretVaultService> m_vault;
	std::shared_ptr<secrets::ISecretVaultCapabilityService> m_capabilities;
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> m_grantAuthority;
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> m_migration;
	std::shared_ptr<Gate> m_gate;
};

CControlPlatformRpcServerAdapter::CControlPlatformRpcServerAdapter(ControlStorageRpcSessionIdentity identity,
	std::shared_ptr<storage::IStorageService> storage, std::shared_ptr<secrets::ISecretVaultService> vault,
	std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities,
	std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration,
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles) :
	m_storage(std::move(storage)), m_vault(std::move(vault)), m_capabilities(std::move(capabilities)),
	m_grantAuthority(std::move(grantAuthority)), m_migration(std::move(migration)), m_profiles(std::move(profiles))
{
	if (!m_storage || !m_profiles) throw std::invalid_argument("Control platform RPC adapter requires storage and profile registry");
	ValidateIdentity(identity, m_vault, m_capabilities, m_grantAuthority, m_migration);
	m_identity = std::move(identity);
	m_gate = std::make_shared<Gate>();
}

CControlPlatformRpcServerAdapter::~CControlPlatformRpcServerAdapter()
{
	Stop();
}

bool CControlPlatformRpcServerAdapter::BeginStopping() noexcept
{
	std::unique_lock lock(m_gate->mutex);
	if (m_gate->state != EControlPlatformRpcServerAdapterState::Accepting) return false;
	m_gate->state = EControlPlatformRpcServerAdapterState::Stopping;
	return true;
}

void CControlPlatformRpcServerAdapter::Stop() noexcept
{
	std::unique_lock lock(m_gate->mutex);
	if (m_gate->state != EControlPlatformRpcServerAdapterState::Stopped) {
		m_gate->state = EControlPlatformRpcServerAdapterState::Stopped;
	}
}

EControlPlatformRpcServerAdapterState CControlPlatformRpcServerAdapter::State() const noexcept
{
	std::shared_lock lock(m_gate->mutex);
	return m_gate->state;
}

bool CControlPlatformRpcServerAdapter::IsAccepting() const noexcept
{
	return State() == EControlPlatformRpcServerAdapterState::Accepting;
}

std::unique_ptr<IControlIpcSessionHandler> CControlPlatformRpcServerAdapter::CreateSession(
	const ControlIpcSessionContext& session)
{
	std::shared_lock lock(m_gate->mutex);
	if (m_gate->state != EControlPlatformRpcServerAdapterState::Accepting) return nullptr;
	return std::make_unique<SessionHandler>(m_identity, session, m_storage, m_vault, m_capabilities, m_grantAuthority,
		m_migration, m_profiles, m_gate);
}

} // namespace platform::controlipc
