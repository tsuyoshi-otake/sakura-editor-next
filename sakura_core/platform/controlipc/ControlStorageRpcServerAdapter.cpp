/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlStorageRpcServerAdapter.h"

#include <shared_mutex>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace platform::controlipc {
namespace {

ControlIpcFrame ServerStoppingResponse(const ControlIpcFrame& request, std::uint64_t generation) noexcept
{
	try {
		const auto payload = EncodeControlIpcError({ EControlIpcTerminalStatus::ServerStopping, "storage RPC server is stopping" });
		return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Error,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation },
			payload.value_or(std::vector<std::uint8_t>{}) };
	} catch (...) {
		return { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Error,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation }, {} };
	}
}

void ValidateIdentity(const ControlStorageRpcSessionIdentity& identity)
{
	// The storage RPC codec is the authoritative validation for the externally
	// visible profile identifier. Validate before creating gate/session state.
	if (identity.generation == 0 || !EncodeControlStorageHello(identity.profileId)) {
		throw std::invalid_argument("Control storage RPC server identity is invalid");
	}
}

} // namespace

struct CControlStorageRpcServerAdapter::Gate final {
	mutable std::shared_mutex mutex;
	EControlStorageRpcServerAdapterState state = EControlStorageRpcServerAdapterState::Accepting;
};

class CControlStorageRpcServerAdapter::SessionHandler final : public IControlIpcSessionHandler {
public:
	SessionHandler(ControlStorageRpcSessionIdentity identity,
		std::shared_ptr<storage::IStorageService> storage,
		std::shared_ptr<Gate> gate)
		: m_session(std::move(identity), *storage), m_storage(std::move(storage)), m_gate(std::move(gate))
	{
	}

	ControlIpcFrameDispatchResult HandleFrame(const ControlIpcSessionContext&, const ControlIpcFrame& frame) override;

private:
	CControlStorageRpcSession m_session;
	// The connection must keep the control-owned service alive through its last response.
	std::shared_ptr<storage::IStorageService> m_storage;
	std::shared_ptr<Gate> m_gate;
};

ControlIpcFrameDispatchResult CControlStorageRpcServerAdapter::SessionHandler::HandleFrame(
	const ControlIpcSessionContext&, const ControlIpcFrame& frame)
{
	// The shared lock makes a frame accepted before shutdown linearize before the
	// state transition. Shutdown waits for such calls to settle before closing.
	std::shared_lock lock(m_gate->mutex);
	if (m_gate->state != EControlStorageRpcServerAdapterState::Accepting) {
		return { { ServerStoppingResponse(frame, m_session.GetIdentity().generation) }, EControlIpcSessionDecision::Close };
	}
	return { { m_session.Process(frame) }, EControlIpcSessionDecision::KeepOpen };
}

CControlStorageRpcServerAdapter::CControlStorageRpcServerAdapter(
	ControlStorageRpcSessionIdentity identity, std::shared_ptr<storage::IStorageService> storage)
	: m_storage(std::move(storage))
{
	if (!m_storage) {
		throw std::invalid_argument("Control storage RPC server requires a storage service");
	}
	ValidateIdentity(identity);
	m_identity = std::move(identity);
	m_gate = std::make_shared<Gate>();
}

CControlStorageRpcServerAdapter::~CControlStorageRpcServerAdapter()
{
	// A direct owner teardown has the same externally visible gate semantics as
	// coordinated shutdown, even if a caller still retains a local session.
	Stop();
}

bool CControlStorageRpcServerAdapter::BeginStopping() noexcept
{
	std::unique_lock lock(m_gate->mutex);
	if (m_gate->state != EControlStorageRpcServerAdapterState::Accepting) return false;
	m_gate->state = EControlStorageRpcServerAdapterState::Stopping;
	return true;
}

void CControlStorageRpcServerAdapter::Stop() noexcept
{
	std::unique_lock lock(m_gate->mutex);
	if (m_gate->state == EControlStorageRpcServerAdapterState::Stopped) return;
	m_gate->state = EControlStorageRpcServerAdapterState::Stopped;
}

EControlStorageRpcServerAdapterState CControlStorageRpcServerAdapter::State() const noexcept
{
	std::shared_lock lock(m_gate->mutex);
	return m_gate->state;
}

bool CControlStorageRpcServerAdapter::IsAccepting() const noexcept
{
	return State() == EControlStorageRpcServerAdapterState::Accepting;
}

std::unique_ptr<IControlIpcSessionHandler> CControlStorageRpcServerAdapter::CreateSession(
	const ControlIpcSessionContext&)
{
	std::shared_lock lock(m_gate->mutex);
	if (m_gate->state != EControlStorageRpcServerAdapterState::Accepting) return nullptr;
	return std::make_unique<SessionHandler>(m_identity, m_storage, m_gate);
}

} // namespace platform::controlipc
