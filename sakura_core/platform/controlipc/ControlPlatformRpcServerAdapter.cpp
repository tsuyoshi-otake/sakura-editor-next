/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlPlatformRpcServerAdapter.h"

#include <sakura/controlipc/ProfileAuthorityIdentity.h>

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

bool IsProfileKind(EControlIpcKind kind) noexcept
{
	return kind == EControlIpcKind::ProfileRequest;
}

void ValidateIdentity(const ControlStorageRpcSessionIdentity& identity)
{
	if (identity.generation == 0 || !profiles::IsCanonicalProfileAuthorityId(identity.profileId)) {
		throw std::invalid_argument("Control platform RPC adapter requires a canonical identity");
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
		std::shared_ptr<storage::IStorageAuthority> storage,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
		std::shared_ptr<Gate> gate, std::shared_ptr<IControlIpcFrameHandler> senp) :
		m_storageSession(std::move(identity), *storage),
		m_storage(std::move(storage)), m_profiles(std::move(profiles)),
		m_profileSession(m_storageSession.GetIdentity(), m_profiles), m_gate(std::move(gate)),
		m_context(context), m_senp(std::move(senp))
	{
	}

	ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext& context, const ControlIpcFrame& frame) override
	{
		std::shared_lock lock(m_gate->mutex);
		const auto generation = m_storageSession.GetIdentity().generation;
		if (m_closed || m_gate->state != EControlPlatformRpcServerAdapterState::Accepting) {
			m_closed = true;
			m_senpSession.reset();
			return { { ErrorResponse(frame, EControlIpcTerminalStatus::ServerStopping, generation) }, EControlIpcSessionDecision::Close };
		}
		if (frame.header.kind == EControlIpcKind::SenpRequest) return ProcessSenp(context, frame);

		if (IsStorageKind(frame.header.kind)) {
			auto response = m_storageSession.Process(frame);
			if (frame.header.kind == EControlIpcKind::Hello && response.header.kind == EControlIpcKind::HelloAck) {
				m_storageHelloCompleted = true;
			}
			return { { std::move(response) }, EControlIpcSessionDecision::KeepOpen };
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
	ControlIpcFrameDispatchResult ProcessSenp(const ControlIpcSessionContext& context, const ControlIpcFrame& frame)
	{
		const auto generation = m_storageSession.GetIdentity().generation;
		const auto fail = [&](EControlIpcTerminalStatus status, bool close = false) {
			if (close) { m_closed = true; m_senpSession.reset(); }
			return ControlIpcFrameDispatchResult{ { ErrorResponse(frame, status, generation) },
				close ? EControlIpcSessionDecision::Close : EControlIpcSessionDecision::KeepOpen };
		};
		if (!m_storageHelloCompleted) return fail(EControlIpcTerminalStatus::InvalidRequest);
		if (context.sessionId != m_context.sessionId || context.clientProcessId != m_context.clientProcessId
			|| m_context.sessionId == 0 || m_context.clientProcessId == 0) return fail(EControlIpcTerminalStatus::AccessDenied, true);
		if (frame.header.majorVersion != kControlIpcMajorVersion) return fail(EControlIpcTerminalStatus::UnsupportedVersion, true);
		if (frame.header.generation != generation) return fail(EControlIpcTerminalStatus::GenerationMismatch, true);
		if (frame.header.requestId == 0 || frame.header.flags != EControlIpcFlags::Request
			|| frame.payload.size() > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes)
			return fail(EControlIpcTerminalStatus::InvalidRequest, true);
		if (!m_senp) return fail(EControlIpcTerminalStatus::UnsupportedVersion);
		try {
			// The transport's captured peer is authoritative, never a payload PID.
			// No automatic retry follows a refused or throwing factory.
			if (!m_senpSession) m_senpSession = m_senp->CreateSession(m_context);
			if (!m_senpSession) return fail(EControlIpcTerminalStatus::ResourceExhausted, true);
			auto result = m_senpSession->HandleFrame(m_context, frame);
			if (result.responseFrames.size() != 1
				|| (result.decision != EControlIpcSessionDecision::KeepOpen && result.decision != EControlIpcSessionDecision::Close))
				return fail(EControlIpcTerminalStatus::InternalError, true);
			const auto& response = result.responseFrames.front();
			if ((response.header.kind != EControlIpcKind::SenpResponse && response.header.kind != EControlIpcKind::Error)
				|| response.header.majorVersion != kControlIpcMajorVersion || response.header.generation != generation
				|| response.header.requestId != frame.header.requestId
				|| response.header.flags != (EControlIpcFlags::Response | EControlIpcFlags::Terminal)
				|| response.payload.size() > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes
				|| (response.header.kind == EControlIpcKind::Error && !DecodeControlIpcError(response.payload)))
				return fail(EControlIpcTerminalStatus::InternalError, true);
			if (result.decision == EControlIpcSessionDecision::Close) {
				m_closed = true;
				m_senpSession.reset();
			}
			return result;
		} catch (...) {
			// Destruction owns grant revocation; close prevents retry or reuse.
			return fail(EControlIpcTerminalStatus::InternalError, true);
		}
	}

	CControlStorageRpcSession m_storageSession;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_profiles;
	CControlProfileRpcSession m_profileSession;
	bool m_storageHelloCompleted = false;
	std::shared_ptr<storage::IStorageAuthority> m_storage;
	std::shared_ptr<Gate> m_gate;
	const ControlIpcSessionContext m_context;
	std::shared_ptr<IControlIpcFrameHandler> m_senp;
	std::unique_ptr<IControlIpcSessionHandler> m_senpSession;
	bool m_closed{};
};

CControlPlatformRpcServerAdapter::CControlPlatformRpcServerAdapter(ControlStorageRpcSessionIdentity identity,
	std::shared_ptr<storage::IStorageAuthority> storage,
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> profiles,
	std::shared_ptr<IControlIpcFrameHandler> senp) :
	m_storage(std::move(storage)), m_profiles(std::move(profiles)), m_senp(std::move(senp))
{
	if (!m_storage || !m_profiles) throw std::invalid_argument("Control platform RPC adapter requires storage and profile registry");
	ValidateIdentity(identity);
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
	return std::make_unique<SessionHandler>(m_identity, session, m_storage, m_profiles, m_gate, m_senp);
}

} // namespace platform::controlipc
