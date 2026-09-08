/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/controlipc/ControlSenpClient.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include <sakura/controlipc/ProfileAuthorityIdentity.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace platform::controlipc {
namespace {

ControlSenpClientResult Result(EControlSenpClientOutcome outcome, EControlIpcTerminalStatus status,
	std::wstring diagnostic = {},
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::Discovered,
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None)
{
	ControlSenpClientResult result;
	result.outcome = outcome;
	result.terminalStatus = status;
	result.discoveryDisposition = discoveryDisposition;
	result.transportReason = transportReason;
	result.diagnostic = std::move(diagnostic);
	return result;
}

bool IsExactTerminalResponse(const ControlIpcFrame& frame) noexcept
{
	return frame.header.flags == (EControlIpcFlags::Response | EControlIpcFlags::Terminal);
}

/*!
	@brief Whether a terminal error leaves the peer's session usable.

	The control adapter answers a SENP frame it cannot route with InvalidRequest
	or UnsupportedVersion and keeps the session open; every other terminal status
	is paired with a session close on that side. Mirroring exactly that set keeps
	one malformed request from destroying grants the connection still holds, while
	a status that really did close the peer never leaves this client believing it
	still owns an authenticated connection.
*/
bool KeepsConnectionOpen(EControlIpcTerminalStatus status) noexcept
{
	return status == EControlIpcTerminalStatus::InvalidRequest
		|| status == EControlIpcTerminalStatus::UnsupportedVersion;
}

} // namespace

CControlSenpClient::CControlSenpClient(ControlSenpClientOptions options,
	IControlPlatformEndpointReader& endpointReader)
	: m_options(std::move(options)), m_endpointReader(endpointReader),
	m_minimumGeneration(m_options.minimumGeneration)
{
}

CControlSenpClient::~CControlSenpClient()
{
	Stop();
}

std::optional<std::uint64_t> CControlSenpClient::NextRequestId() noexcept
{
	if (m_nextRequestId == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	return m_nextRequestId++;
}

void CControlSenpClient::Release(const std::shared_ptr<IControlPlatformClientChannel>& channel,
	bool drop) noexcept
{
	std::shared_ptr<IControlPlatformClientChannel> closing;
	{
		std::scoped_lock lock(m_mutex);
		m_busy = false;
		if (drop && m_activeChannel == channel) {
			closing = std::move(m_activeChannel);
			m_activeChannel.reset();
			m_pinnedGeneration = 0;
			if (!m_stopped) m_state = EControlSenpClientState::Disconnected;
		}
	}
	if (closing) closing->Close();
	else if (drop && channel) channel->Close();
}

ControlSenpClientResult CControlSenpClient::Connect()
{
	std::uint64_t floor = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) return Result(EControlSenpClientOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		if (m_busy) {
			return Result(EControlSenpClientOutcome::OperationInFlight,
				EControlIpcTerminalStatus::DeadlineExceeded, L"another SENP client operation is in flight");
		}
		if (m_state == EControlSenpClientState::Connected && m_activeChannel) {
			return Result(EControlSenpClientOutcome::AlreadyConnected, EControlIpcTerminalStatus::Succeeded);
		}
		m_busy = true;
		m_state = EControlSenpClientState::Connecting;
		floor = m_minimumGeneration;
	}

	std::shared_ptr<IControlPlatformClientChannel> channel;
	const auto fail = [this, &channel](ControlSenpClientResult result) {
		Release(channel, true);
		std::scoped_lock lock(m_mutex);
		if (m_stopped) return Result(EControlSenpClientOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		m_state = EControlSenpClientState::Disconnected;
		return result;
	};

	try {
		ControlPlatformEndpointReadRequirements requirements;
		requirements.minimumGeneration = floor;
		const auto discovered = m_endpointReader.ReadDetailed(requirements);
		if (!discovered.IsDiscovered() || !discovered.snapshot) {
			return fail(Result(EControlSenpClientOutcome::EndpointUnavailable,
				EControlIpcTerminalStatus::ServerStopping, L"control platform endpoint is not available",
				discovered.disposition));
		}
		const auto& endpoint = *discovered.snapshot;
		if (!profiles::IsCanonicalProfileAuthorityId(endpoint.profileId)
			|| endpoint.profileId != m_options.profileId) {
			return fail(Result(EControlSenpClientOutcome::EndpointUnavailable,
				EControlIpcTerminalStatus::ProfileMismatch, L"control platform endpoint profile mismatch"));
		}
		// Answered before the general snapshot classification so a rolled-back
		// control process is reported as what it is, not as a stale endpoint.
		if (endpoint.generation == 0 || endpoint.generation < floor) {
			return fail(Result(EControlSenpClientOutcome::GenerationChanged,
				EControlIpcTerminalStatus::GenerationMismatch, L"control platform generation rolled back"));
		}
		const auto disposition = CControlPlatformEndpoint::ClassifySnapshot(endpoint, m_options.profileHash,
			requirements);
		if (disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
			return fail(Result(EControlSenpClientOutcome::EndpointUnavailable,
				EControlIpcTerminalStatus::InvalidRequest, L"control platform endpoint snapshot is not usable",
				disposition));
		}

		auto created = m_options.channelFactory ? m_options.channelFactory() : nullptr;
		if (!created) {
			return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::InternalError,
				L"SENP client channel factory returned null"));
		}
		channel = std::shared_ptr<IControlPlatformClientChannel>(std::move(created));
		{
			std::scoped_lock lock(m_mutex);
			if (m_stopped) {
				m_busy = false;
				channel->Close();
				return Result(EControlSenpClientOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
			}
			m_activeChannel = channel;
		}

		const auto connected = channel->Connect(endpoint, m_options.exchangeDeadline);
		if (!connected.success) {
			return fail(Result(EControlSenpClientOutcome::ConnectionLost,
				connected.reason == EControlIpcTransportDisconnectReason::DeadlineExceeded
					? EControlIpcTerminalStatus::DeadlineExceeded : EControlIpcTerminalStatus::InternalError,
				L"SENP client connection failed", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				connected.reason));
		}

		std::optional<std::uint64_t> helloId;
		{
			std::scoped_lock lock(m_mutex);
			helloId = NextRequestId();
		}
		const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
		if (!helloPayload || !helloId) {
			return fail(Result(EControlSenpClientOutcome::InvalidRequest, EControlIpcTerminalStatus::InvalidRequest,
				L"invalid SENP client hello request"));
		}
		const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
			EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
		std::vector<ControlIpcFrame> responses;
		const auto exchange = channel->Exchange(hello, responses, m_options.exchangeDeadline);
		if (!exchange.success) {
			return fail(Result(EControlSenpClientOutcome::ConnectionLost,
				exchange.reason == EControlIpcTransportDisconnectReason::DeadlineExceeded
					? EControlIpcTerminalStatus::DeadlineExceeded : EControlIpcTerminalStatus::InternalError,
				L"SENP client hello transport loss", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				exchange.reason));
		}
		if (responses.size() != 1) {
			return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"SENP client hello returned other than one terminal response"));
		}
		const auto& helloAck = responses.front();
		if (!IsExactTerminalResponse(helloAck) || helloAck.header.requestId != hello.header.requestId
			|| helloAck.header.majorVersion != kControlIpcMajorVersion
			|| helloAck.header.minorVersion > kControlIpcMinorVersion || helloAck.header.generation == 0) {
			return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"malformed SENP client hello response"));
		}
		if (helloAck.header.kind == EControlIpcKind::Error) {
			const auto error = DecodeControlIpcError(helloAck.payload);
			return fail(Result(EControlSenpClientOutcome::ProtocolError,
				error ? error->status : EControlIpcTerminalStatus::ProtocolError,
				L"control platform refused the SENP client hello"));
		}
		if (helloAck.header.kind != EControlIpcKind::HelloAck) {
			return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"unexpected SENP client hello response"));
		}
		const auto acknowledged = DecodeControlStorageHello(helloAck.payload);
		if (!acknowledged || *acknowledged != m_options.profileId) {
			return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProfileMismatch,
				L"SENP client hello profile mismatch"));
		}
		if (helloAck.header.generation != endpoint.generation || helloAck.header.generation < floor) {
			return fail(Result(EControlSenpClientOutcome::GenerationChanged,
				EControlIpcTerminalStatus::GenerationMismatch, L"SENP client hello generation mismatch"));
		}

		{
			std::scoped_lock lock(m_mutex);
			if (m_stopped || m_activeChannel != channel) {
				m_busy = false;
				channel->Close();
				return Result(EControlSenpClientOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
			}
			m_state = EControlSenpClientState::Connected;
			m_pinnedGeneration = helloAck.header.generation;
			m_minimumGeneration = helloAck.header.generation;
			++m_connectionEpoch;
			m_busy = false;
		}
		return Result(EControlSenpClientOutcome::Connected, EControlIpcTerminalStatus::Succeeded);
	} catch (...) {
		return fail(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::InternalError,
			L"SENP client connection raised"));
	}
}

ControlSenpClientResult CControlSenpClient::Execute(const ControlSenpRpcRequest& request)
{
	auto encoded = EncodeControlSenpRpcRequest(request);
	if (!encoded) {
		return Result(EControlSenpClientOutcome::InvalidRequest, EControlIpcTerminalStatus::InvalidRequest,
			L"invalid SENP request");
	}
	auto fields = EncodeControlIpcFields(
		{ { static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload), std::move(*encoded) } });
	if (!fields || fields->size() > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes) {
		return Result(EControlSenpClientOutcome::InvalidRequest, EControlIpcTerminalStatus::InvalidRequest,
			L"SENP request exceeds the frame bound");
	}

	std::shared_ptr<IControlPlatformClientChannel> channel;
	std::uint64_t generation = 0;
	std::optional<std::uint64_t> requestId;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) return Result(EControlSenpClientOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		if (m_busy) {
			return Result(EControlSenpClientOutcome::OperationInFlight,
				EControlIpcTerminalStatus::DeadlineExceeded, L"another SENP client operation is in flight");
		}
		if (m_state != EControlSenpClientState::Connected || !m_activeChannel || m_pinnedGeneration == 0) {
			return Result(EControlSenpClientOutcome::NotConnected, EControlIpcTerminalStatus::ServerStopping,
				L"SENP client is not connected");
		}
		requestId = NextRequestId();
		if (!requestId) {
			return Result(EControlSenpClientOutcome::InvalidRequest, EControlIpcTerminalStatus::InvalidRequest,
				L"SENP client exhausted its request identifiers");
		}
		channel = m_activeChannel;
		generation = m_pinnedGeneration;
		m_busy = true;
	}

	const auto finish = [this, &channel](ControlSenpClientResult result, bool drop) {
		Release(channel, drop);
		return result;
	};

	try {
		const ControlIpcFrame frame{ { kControlIpcMajorVersion, kControlIpcMinorVersion,
			EControlIpcKind::SenpRequest, EControlIpcFlags::Request, *requestId, generation },
			std::move(*fields) };
		std::vector<ControlIpcFrame> responses;
		const auto exchange = channel->Exchange(frame, responses, m_options.exchangeDeadline);
		if (!exchange.success) {
			// The answer is unknown, so the grant state of this connection is
			// unknown too. Dropping it is the only fail-closed outcome.
			return finish(Result(EControlSenpClientOutcome::ConnectionLost,
				exchange.reason == EControlIpcTransportDisconnectReason::DeadlineExceeded
					? EControlIpcTerminalStatus::DeadlineExceeded : EControlIpcTerminalStatus::InternalError,
				L"SENP request transport loss", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				exchange.reason), true);
		}
		if (responses.size() != 1) {
			return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"SENP exchange returned other than one terminal response"), true);
		}
		const auto& candidate = responses.front();
		if (!IsExactTerminalResponse(candidate) || candidate.header.requestId != frame.header.requestId
			|| candidate.header.majorVersion != kControlIpcMajorVersion
			|| candidate.header.minorVersion > kControlIpcMinorVersion) {
			return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"malformed SENP terminal response"), true);
		}
		if (candidate.header.generation != generation) {
			return finish(Result(EControlSenpClientOutcome::GenerationChanged,
				EControlIpcTerminalStatus::GenerationMismatch, L"SENP response generation mismatch"), true);
		}
		if (candidate.header.kind == EControlIpcKind::Error) {
			const auto error = DecodeControlIpcError(candidate.payload);
			if (!error) {
				return finish(Result(EControlSenpClientOutcome::ProtocolError,
					EControlIpcTerminalStatus::ProtocolError, L"malformed SENP error response"), true);
			}
			return finish(Result(error->status == EControlIpcTerminalStatus::GenerationMismatch
				? EControlSenpClientOutcome::GenerationChanged : EControlSenpClientOutcome::ProtocolError,
				error->status, L"control platform returned a terminal SENP error"),
				!KeepsConnectionOpen(error->status));
		}
		if (candidate.header.kind != EControlIpcKind::SenpResponse) {
			return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"unexpected SENP response kind"), true);
		}
		auto decodedFields = DecodeControlIpcFields(candidate.payload);
		if (decodedFields.outcome != EControlIpcFieldDecodeOutcome::Decoded || decodedFields.fields.size() != 1
			|| decodedFields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::SenpPayload)) {
			return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"malformed SENP response envelope"), true);
		}
		auto decoded = DecodeControlSenpRpcResponse(decodedFields.fields.front().value);
		if (!decoded) {
			return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::ProtocolError,
				L"malformed SENP response payload"), true);
		}
		// A refusal is a normal answer on a healthy connection: the caller decides
		// what an Unauthorized, Closed or Expired status means for its own grant.
		auto result = Result(EControlSenpClientOutcome::Answered, EControlIpcTerminalStatus::Succeeded);
		result.response = std::move(*decoded);
		return finish(std::move(result), false);
	} catch (...) {
		return finish(Result(EControlSenpClientOutcome::ProtocolError, EControlIpcTerminalStatus::InternalError,
			L"SENP request raised"), true);
	}
}

void CControlSenpClient::Disconnect() noexcept
{
	std::shared_ptr<IControlPlatformClientChannel> closing;
	{
		std::scoped_lock lock(m_mutex);
		closing = std::move(m_activeChannel);
		m_activeChannel.reset();
		m_pinnedGeneration = 0;
		if (!m_stopped) m_state = EControlSenpClientState::Disconnected;
	}
	// Closing outside the lock cancels a blocked exchange instead of waiting for it.
	if (closing) closing->Close();
}

void CControlSenpClient::Stop() noexcept
{
	std::shared_ptr<IControlPlatformClientChannel> closing;
	{
		std::scoped_lock lock(m_mutex);
		m_stopped = true;
		m_state = EControlSenpClientState::Stopped;
		m_pinnedGeneration = 0;
		closing = std::move(m_activeChannel);
		m_activeChannel.reset();
	}
	if (closing) closing->Close();
}

EControlSenpClientState CControlSenpClient::State() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_state;
}

std::uint64_t CControlSenpClient::PinnedGeneration() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_pinnedGeneration;
}

std::uint64_t CControlSenpClient::ConnectionEpoch() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_connectionEpoch;
}

} // namespace platform::controlipc
