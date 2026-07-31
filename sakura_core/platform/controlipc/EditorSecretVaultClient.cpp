/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/EditorSecretVaultClient.h"
#include "platform/controlipc/ControlPlatformClient.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr auto kMaximumEditorSecretVaultCapabilityLifetime = std::chrono::minutes(5);

void Wipe(secrets::SecretVaultCapabilityToken& token) noexcept
{
	volatile std::uint8_t* byte = token.data();
	for (std::size_t index = 0; index != token.size(); ++index) byte[index] = 0;
}

void Wipe(std::string& value) noexcept
{
	volatile char* byte = value.data();
	for (std::size_t index = 0; index != value.size(); ++index) byte[index] = '\0';
	value.clear();
}

bool IsExactCanonicalExtensionId(std::string_view value) noexcept
{
	std::string canonical;
	return secrets::CanonicalizeSecretVaultExtensionId(value, canonical) && canonical == value;
}

bool IsExactTerminalResponse(const ControlIpcFrame& response, const ControlIpcFrame& request) noexcept
{
	return response.header.majorVersion == kControlIpcMajorVersion
		&& response.header.minorVersion == kControlIpcMinorVersion
		&& response.header.requestId == request.header.requestId
		&& response.header.generation != 0
		&& HasFlag(response.header.flags, EControlIpcFlags::Response)
		&& HasFlag(response.header.flags, EControlIpcFlags::Terminal)
		&& !HasFlag(response.header.flags, EControlIpcFlags::Request);
}

} // namespace

ControlPlatformEndpointDiscoveryResult CEditorSecretVaultEndpointReaderAdapter::Read(
	const ControlPlatformEndpointReadRequirements& requirements)
{
	return m_reader.ReadDetailed(requirements);
}

void CEditorSecretVaultEndpointReaderAdapter::Close() noexcept
{
	m_reader.Close();
}

ControlIpcTransportResult CEditorSecretVaultNamedPipeChannel::Connect(const ControlPlatformEndpointSnapshot& endpoint,
	std::chrono::milliseconds deadline)
{
	return m_client.Connect(endpoint, deadline);
}

ControlIpcTransportResult CEditorSecretVaultNamedPipeChannel::Exchange(const ControlIpcFrame& request,
	std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline)
{
	return m_client.Exchange(request, responses, deadline);
}

void CEditorSecretVaultNamedPipeChannel::Close() noexcept
{
	m_client.Close();
}

CEditorSecretVaultClient::CEditorSecretVaultClient(EditorSecretVaultClientOptions options,
	IEditorSecretVaultEndpointReader& endpointReader)
	: m_options(std::move(options)), m_endpointReader(endpointReader)
{
}

CEditorSecretVaultClient::~CEditorSecretVaultClient()
{
	Stop();
}

bool CEditorSecretVaultClient::IsOptionsValid() const noexcept
{
	return profiles::IsCanonicalProfileAuthorityId(m_options.profileId) && !m_options.profileHash.empty()
		&& m_options.pinnedControlGeneration != 0 && m_options.exchangeDeadline.count() > 0
		&& m_options.capabilityLifetime.count() > 0
		&& m_options.capabilityLifetime <= kMaximumEditorSecretVaultCapabilityLifetime
		&& static_cast<bool>(m_options.channelFactory);
}

bool CEditorSecretVaultClient::IsCallerValid(const EditorSecretVaultCallerIdentity& caller) const noexcept
{
	return caller.hostGeneration != 0
		&& secrets::IsValidSecretVaultIdentifier(caller.extensionHostSessionId,
			secrets::kMaximumSecretVaultCapabilitySessionIdBytes)
		&& IsExactCanonicalExtensionId(caller.canonicalExtensionId);
}

bool CEditorSecretVaultClient::IsMutationValid(const EditorSecretVaultApplyRequest& request,
	secrets::ESecretMutationKind expectedKind) const noexcept
{
	const auto& mutation = request.mutation;
	return IsCallerValid(request.caller) && mutation.kind == expectedKind
		&& mutation.extensionId == request.caller.canonicalExtensionId
		&& secrets::SecretAddress{ mutation.extensionId, mutation.key }.IsValid()
		&& secrets::IsValidSecretVaultIdentifier(mutation.operationId, secrets::kMaximumSecretVaultOperationIdBytes)
		&& ((expectedKind == secrets::ESecretMutationKind::Set
			&& mutation.value.size() <= secrets::kMaximumSecretVaultValueBytes
			&& secrets::IsValidSecretVaultUtf8(mutation.value))
			|| (expectedKind == secrets::ESecretMutationKind::Delete && mutation.value.empty()));
}

bool CEditorSecretVaultClient::Begin(std::shared_ptr<IEditorSecretVaultChannel>& channel) noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped || m_operationInFlight) return false;
	m_operationInFlight = true;
	return true;
}

void CEditorSecretVaultClient::Finish(const std::shared_ptr<IEditorSecretVaultChannel>& channel) noexcept
{
	if (channel) channel->Close();
	std::scoped_lock lock(m_mutex);
	if (m_activeChannel == channel) m_activeChannel.reset();
	m_operationInFlight = false;
}

std::optional<ControlPlatformEndpointSnapshot> CEditorSecretVaultClient::Discover(
	EControlPlatformEndpointDiscoveryDisposition& disposition) const
{
	const auto discovered = m_endpointReader.Read({ m_options.pinnedControlGeneration, true });
	disposition = discovered.disposition;
	if (!discovered.IsDiscovered()) return std::nullopt;
	const auto& endpoint = *discovered.snapshot;
	if (endpoint.profileId != m_options.profileId || endpoint.generation != m_options.pinnedControlGeneration
		|| CControlPlatformEndpoint::ClassifySnapshot(endpoint, m_options.profileHash,
			{ m_options.pinnedControlGeneration, true }) != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		disposition = EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor;
		return std::nullopt;
	}
	return endpoint;
}

std::optional<std::uint64_t> CEditorSecretVaultClient::NextRequestId() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped || m_nextRequestId == 0 || m_nextRequestId == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
	return m_nextRequestId++;
}

std::optional<ControlIpcFrame> CEditorSecretVaultClient::ExchangeTerminal(IEditorSecretVaultChannel& channel,
	const ControlIpcFrame& request, EControlIpcKind expectedKind, bool,
	EControlIpcTerminalStatus& terminalStatus, EControlIpcTransportDisconnectReason& transportReason) const
{
	std::vector<ControlIpcFrame> responses;
	const auto transport = channel.Exchange(request, responses, m_options.exchangeDeadline);
	transportReason = transport.reason;
	if (!transport.success || responses.size() != 1 || !IsExactTerminalResponse(responses.front(), request)) {
		terminalStatus = !transport.success && transport.reason == EControlIpcTransportDisconnectReason::Stopped
			? EControlIpcTerminalStatus::Cancelled : EControlIpcTerminalStatus::ProtocolError;
		return std::nullopt;
	}
	const auto& response = responses.front();
	if (response.header.generation != m_options.pinnedControlGeneration) {
		terminalStatus = EControlIpcTerminalStatus::GenerationMismatch;
		return std::nullopt;
	}
	if (response.header.kind == EControlIpcKind::Error) {
		const auto error = DecodeControlIpcError(response.payload);
		terminalStatus = error ? error->status : EControlIpcTerminalStatus::ProtocolError;
		return response;
	}
	if (response.header.kind != expectedKind) {
		terminalStatus = EControlIpcTerminalStatus::ProtocolError;
		return std::nullopt;
	}
	terminalStatus = EControlIpcTerminalStatus::Succeeded;
	return response;
}

EditorSecretVaultGetResult CEditorSecretVaultClient::GetStoppedResult() const noexcept
{
	return { EEditorSecretVaultOutcome::Stopped, EControlIpcTerminalStatus::Cancelled,
		EControlPlatformEndpointDiscoveryDisposition::Closed, EControlIpcTransportDisconnectReason::Stopped,
		{ secrets::ESecretGetStatus::Stopped, 0, std::nullopt } };
}

EditorSecretVaultApplyResult CEditorSecretVaultClient::ApplyStoppedResult() const noexcept
{
	return { EEditorSecretVaultOutcome::Stopped, EControlIpcTerminalStatus::Cancelled,
		EControlPlatformEndpointDiscoveryDisposition::Closed, EControlIpcTransportDisconnectReason::Stopped,
		{ secrets::ESecretMutationStatus::Stopped, 0, false, std::nullopt, {} } };
}

EditorSecretVaultGetResult CEditorSecretVaultClient::Get(const EditorSecretVaultGetRequest& request)
{
	EditorSecretVaultGetResult result;
	std::shared_ptr<IEditorSecretVaultChannel> channel;
	if (!Begin(channel)) return IsStopped() ? GetStoppedResult()
		: EditorSecretVaultGetResult{ EEditorSecretVaultOutcome::OperationInFlight, EControlIpcTerminalStatus::Cancelled };
	const auto finish = [this, &channel](EditorSecretVaultGetResult value) { Finish(channel); return value; };
	try {
	if (!IsOptionsValid() || !IsCallerValid(request.caller)
		|| !secrets::SecretAddress{ request.caller.canonicalExtensionId, request.key }.IsValid()) {
		return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	}
	EControlPlatformEndpointDiscoveryDisposition disposition;
	const auto endpoint = Discover(disposition);
	if (!endpoint) return finish({ EEditorSecretVaultOutcome::Unavailable, EControlIpcTerminalStatus::ProfileMismatch, disposition });
	auto created = m_options.channelFactory();
	if (!created) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InternalError });
	channel = std::shared_ptr<IEditorSecretVaultChannel>(std::move(created));
	bool stoppedBeforeConnect = false;
	{
		std::scoped_lock lock(m_mutex);
		stoppedBeforeConnect = m_stopped;
		if (!stoppedBeforeConnect) m_activeChannel = channel;
	}
	if (stoppedBeforeConnect) return finish(GetStoppedResult());
	const auto connected = channel->Connect(*endpoint, m_options.exchangeDeadline);
	if (!connected.success) return finish(IsStopped() ? GetStoppedResult() : EditorSecretVaultGetResult{ EEditorSecretVaultOutcome::Unavailable, EControlIpcTerminalStatus::DeadlineExceeded,
		disposition, connected.reason });
	const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
	const auto helloId = NextRequestId();
	if (!helloPayload || !helloId) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	EControlIpcTerminalStatus status;
	EControlIpcTransportDisconnectReason reason;
	const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
		EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
	const auto helloAck = ExchangeTerminal(*channel, hello, EControlIpcKind::HelloAck, false, status, reason);
	const auto acknowledgedProfile = helloAck && helloAck->header.kind != EControlIpcKind::Error
		? DecodeControlStorageHello(helloAck->payload) : std::nullopt;
	if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) {
		return finish(IsStopped() ? GetStoppedResult() : EditorSecretVaultGetResult{ EEditorSecretVaultOutcome::Failed, status, disposition, reason });
	}
	const auto issuePayload = EncodeControlSecretVaultCapabilityIssueRequest({ request.caller.extensionHostSessionId,
		request.caller.hostGeneration, request.caller.canonicalExtensionId, m_options.capabilityLifetime });
	const auto issueId = NextRequestId();
	if (!issuePayload || !issueId) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	const ControlIpcFrame issue{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::SecretCapabilityIssueRequest,
		EControlIpcFlags::Request, *issueId, m_options.pinnedControlGeneration }, *issuePayload };
	const auto issued = ExchangeTerminal(*channel, issue, EControlIpcKind::SecretCapabilityIssueResponse, false, status, reason);
	if (!issued || issued->header.kind == EControlIpcKind::Error) return finish(IsStopped() ? GetStoppedResult()
		: EditorSecretVaultGetResult{ EEditorSecretVaultOutcome::Failed, status, disposition, reason });
	auto capability = DecodeControlSecretVaultCapabilityIssueResponse(issued->payload);
	if (!capability) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::ProtocolError, disposition, reason });
	const auto getPayload = EncodeControlSecretVaultGetRequest({ capability->capability, request.caller.extensionHostSessionId,
		{ request.caller.canonicalExtensionId, request.key } });
	const auto getId = NextRequestId();
	if (!getPayload || !getId) { Wipe(capability->capability); return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest }); }
	const ControlIpcFrame get{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::SecretGetRequest,
		EControlIpcFlags::Request, *getId, m_options.pinnedControlGeneration }, *getPayload };
	const auto response = ExchangeTerminal(*channel, get, EControlIpcKind::SecretGetResponse, false, status, reason);
	Wipe(capability->capability);
	if (!response || response->header.kind == EControlIpcKind::Error) return finish({ IsStopped() || status == EControlIpcTerminalStatus::ServerStopping ? EEditorSecretVaultOutcome::Stopped : EEditorSecretVaultOutcome::Failed,
		status, disposition, reason });
	auto secret = DecodeControlSecretVaultGetResponse(response->payload);
	if (!secret) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::ProtocolError, disposition, reason });
	const auto outcome = secret->status == secrets::ESecretGetStatus::Found ? EEditorSecretVaultOutcome::Succeeded
		: secret->status == secrets::ESecretGetStatus::NotFound ? EEditorSecretVaultOutcome::NotFound
		: secret->status == secrets::ESecretGetStatus::Stopped ? EEditorSecretVaultOutcome::Stopped : EEditorSecretVaultOutcome::Failed;
	return finish({ outcome,
		EControlIpcTerminalStatus::Succeeded, disposition, reason, std::move(*secret) });
	}
	catch (...) {
		return finish(IsStopped() ? GetStoppedResult()
			: EditorSecretVaultGetResult{ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InternalError });
	}
}

EditorSecretVaultApplyResult CEditorSecretVaultClient::Store(const EditorSecretVaultApplyRequest& request)
{
	return ApplyInternal(request, secrets::ESecretMutationKind::Set);
}

EditorSecretVaultApplyResult CEditorSecretVaultClient::Delete(const EditorSecretVaultApplyRequest& request)
{
	return ApplyInternal(request, secrets::ESecretMutationKind::Delete);
}

EditorSecretVaultRevokeResult CEditorSecretVaultClient::RevokeSession()
{
	std::shared_ptr<IEditorSecretVaultChannel> channel;
	if (!Begin(channel)) return IsStopped()
		? EditorSecretVaultRevokeResult{ EEditorSecretVaultOutcome::Stopped,
			EControlIpcTerminalStatus::Cancelled,
			EControlPlatformEndpointDiscoveryDisposition::Closed,
			EControlIpcTransportDisconnectReason::Stopped }
		: EditorSecretVaultRevokeResult{ EEditorSecretVaultOutcome::OperationInFlight,
			EControlIpcTerminalStatus::Cancelled };
	const auto finish = [this, &channel](EditorSecretVaultRevokeResult value) {
		Finish(channel);
		return value;
	};
	try {
		if (!IsOptionsValid()) {
			return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
		}
		EControlPlatformEndpointDiscoveryDisposition disposition;
		const auto endpoint = Discover(disposition);
		if (!endpoint) {
			return finish({ EEditorSecretVaultOutcome::Unavailable,
				EControlIpcTerminalStatus::ProfileMismatch, disposition });
		}
		auto created = m_options.channelFactory();
		if (!created) {
			return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InternalError });
		}
		channel = std::shared_ptr<IEditorSecretVaultChannel>(std::move(created));
		bool stoppedBeforeConnect = false;
		{
			std::scoped_lock lock(m_mutex);
			stoppedBeforeConnect = m_stopped;
			if (!stoppedBeforeConnect) m_activeChannel = channel;
		}
		if (stoppedBeforeConnect) {
			return finish({ EEditorSecretVaultOutcome::Stopped,
				EControlIpcTerminalStatus::Cancelled, disposition,
				EControlIpcTransportDisconnectReason::Stopped });
		}
		const auto connected = channel->Connect(*endpoint, m_options.exchangeDeadline);
		if (!connected.success) {
			return finish({ IsStopped() ? EEditorSecretVaultOutcome::Stopped
					: EEditorSecretVaultOutcome::Unavailable,
				IsStopped() ? EControlIpcTerminalStatus::Cancelled
					: EControlIpcTerminalStatus::DeadlineExceeded,
				disposition, connected.reason });
		}

		const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
		const auto helloId = NextRequestId();
		if (!helloPayload || !helloId) {
			return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
		}
		EControlIpcTerminalStatus status;
		EControlIpcTransportDisconnectReason reason;
		const ControlIpcFrame hello{ {
			kControlIpcMajorVersion,
			kControlIpcMinorVersion,
			EControlIpcKind::Hello,
			EControlIpcFlags::Request,
			*helloId,
			0,
		}, *helloPayload };
		const auto helloAck = ExchangeTerminal(
			*channel, hello, EControlIpcKind::HelloAck, false, status, reason);
		const auto acknowledgedProfile = helloAck && helloAck->header.kind != EControlIpcKind::Error
			? DecodeControlStorageHello(helloAck->payload) : std::nullopt;
		if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) {
			return finish({ IsStopped() ? EEditorSecretVaultOutcome::Stopped
					: EEditorSecretVaultOutcome::Failed,
				status, disposition, reason });
		}

		const auto revokePayload = EncodeControlSecretVaultCapabilityRevokeSessionRequest();
		const auto revokeId = NextRequestId();
		if (!revokePayload || !revokeId) {
			return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
		}
		const ControlIpcFrame revoke{ {
			kControlIpcMajorVersion,
			kControlIpcMinorVersion,
			EControlIpcKind::SecretCapabilityRevokeSessionRequest,
			EControlIpcFlags::Request,
			*revokeId,
			m_options.pinnedControlGeneration,
		}, *revokePayload };
		const auto response = ExchangeTerminal(*channel, revoke,
			EControlIpcKind::SecretCapabilityRevokeSessionResponse, false, status, reason);
		if (!response || response->header.kind == EControlIpcKind::Error) {
			return finish({ IsStopped() || status == EControlIpcTerminalStatus::ServerStopping
					? EEditorSecretVaultOutcome::Stopped : EEditorSecretVaultOutcome::Failed,
				status, disposition, reason });
		}
		if (!DecodeControlSecretVaultCapabilityRevokeSessionResponse(response->payload)) {
			return finish({ EEditorSecretVaultOutcome::Failed,
				EControlIpcTerminalStatus::ProtocolError, disposition, reason });
		}
		return finish({ EEditorSecretVaultOutcome::Succeeded,
			EControlIpcTerminalStatus::Succeeded, disposition, reason });
	}
	catch (...) {
		return finish({ IsStopped() ? EEditorSecretVaultOutcome::Stopped
				: EEditorSecretVaultOutcome::Failed,
			IsStopped() ? EControlIpcTerminalStatus::Cancelled
				: EControlIpcTerminalStatus::InternalError });
	}
}

EditorSecretVaultApplyResult CEditorSecretVaultClient::ApplyInternal(const EditorSecretVaultApplyRequest& request,
	secrets::ESecretMutationKind expectedKind)
{
	std::shared_ptr<IEditorSecretVaultChannel> channel;
	if (!Begin(channel)) return IsStopped() ? ApplyStoppedResult()
		: EditorSecretVaultApplyResult{ EEditorSecretVaultOutcome::OperationInFlight, EControlIpcTerminalStatus::Cancelled };
	const auto finish = [this, &channel](EditorSecretVaultApplyResult value) { Finish(channel); return value; };
	try {
	if (!IsOptionsValid() || !IsMutationValid(request, expectedKind)) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	EControlPlatformEndpointDiscoveryDisposition disposition;
	const auto endpoint = Discover(disposition);
	if (!endpoint) return finish({ EEditorSecretVaultOutcome::Unavailable, EControlIpcTerminalStatus::ProfileMismatch, disposition });
	auto created = m_options.channelFactory();
	if (!created) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InternalError });
	channel = std::shared_ptr<IEditorSecretVaultChannel>(std::move(created));
	bool stoppedBeforeConnect = false;
	{
		std::scoped_lock lock(m_mutex);
		stoppedBeforeConnect = m_stopped;
		if (!stoppedBeforeConnect) m_activeChannel = channel;
	}
	if (stoppedBeforeConnect) return finish(ApplyStoppedResult());
	const auto connected = channel->Connect(*endpoint, m_options.exchangeDeadline);
	if (!connected.success) return finish(IsStopped() ? ApplyStoppedResult()
		: EditorSecretVaultApplyResult{ EEditorSecretVaultOutcome::Unavailable, EControlIpcTerminalStatus::DeadlineExceeded, disposition, connected.reason });
	const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
	const auto helloId = NextRequestId();
	EControlIpcTerminalStatus status;
	EControlIpcTransportDisconnectReason reason;
	if (!helloPayload || !helloId) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello, EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
	const auto helloAck = ExchangeTerminal(*channel, hello, EControlIpcKind::HelloAck, false, status, reason);
	const auto acknowledgedProfile = helloAck && helloAck->header.kind != EControlIpcKind::Error
		? DecodeControlStorageHello(helloAck->payload) : std::nullopt;
	if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) return finish(IsStopped() ? ApplyStoppedResult()
		: EditorSecretVaultApplyResult{ EEditorSecretVaultOutcome::Failed, status, disposition, reason });
	const auto issuePayload = EncodeControlSecretVaultCapabilityIssueRequest({ request.caller.extensionHostSessionId, request.caller.hostGeneration,
		request.caller.canonicalExtensionId, m_options.capabilityLifetime });
	const auto issueId = NextRequestId();
	if (!issuePayload || !issueId) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest });
	const ControlIpcFrame issue{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::SecretCapabilityIssueRequest, EControlIpcFlags::Request,
		*issueId, m_options.pinnedControlGeneration }, *issuePayload };
	const auto issued = ExchangeTerminal(*channel, issue, EControlIpcKind::SecretCapabilityIssueResponse, false, status, reason);
	if (!issued || issued->header.kind == EControlIpcKind::Error) return finish(IsStopped() ? ApplyStoppedResult()
		: EditorSecretVaultApplyResult{ EEditorSecretVaultOutcome::Failed, status, disposition, reason });
	auto capability = DecodeControlSecretVaultCapabilityIssueResponse(issued->payload);
	if (!capability) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::ProtocolError, disposition, reason });
	const auto applyPayload = EncodeControlSecretVaultApplyRequest({ capability->capability, request.caller.extensionHostSessionId, request.mutation });
	const auto applyId = NextRequestId();
	if (!applyPayload || !applyId) { Wipe(capability->capability); return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest }); }
	const ControlIpcFrame apply{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::SecretApplyRequest, EControlIpcFlags::Request,
		*applyId, m_options.pinnedControlGeneration }, *applyPayload };
	const auto response = ExchangeTerminal(*channel, apply, EControlIpcKind::SecretApplyResponse, true, status, reason);
	Wipe(capability->capability);
	if (!response) {
		if (IsStopped()) return finish(ApplyStoppedResult());
		const auto outcome = reason == EControlIpcTransportDisconnectReason::None ? EEditorSecretVaultOutcome::Failed
			: EEditorSecretVaultOutcome::RetryWithSameOperationId;
		return finish({ outcome, status, disposition, reason });
	}
	if (response->header.kind == EControlIpcKind::Error) return finish({ status == EControlIpcTerminalStatus::ServerStopping ? EEditorSecretVaultOutcome::Stopped : EEditorSecretVaultOutcome::Failed,
		status, disposition, reason });
	auto mutation = DecodeControlSecretVaultApplyResponse(response->payload);
	if (!mutation) return finish({ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::ProtocolError, disposition, reason });
	const auto outcome = mutation->status == secrets::ESecretMutationStatus::Succeeded ? EEditorSecretVaultOutcome::Succeeded
		: mutation->status == secrets::ESecretMutationStatus::NotApplicable ? EEditorSecretVaultOutcome::NotApplicable
		: mutation->status == secrets::ESecretMutationStatus::Conflict ? EEditorSecretVaultOutcome::Conflict
		: mutation->status == secrets::ESecretMutationStatus::Stopped ? EEditorSecretVaultOutcome::Stopped : EEditorSecretVaultOutcome::Failed;
	return finish({ outcome, EControlIpcTerminalStatus::Succeeded, disposition, reason, std::move(*mutation) });
	}
	catch (...) {
		return finish(IsStopped() ? ApplyStoppedResult()
			: EditorSecretVaultApplyResult{ EEditorSecretVaultOutcome::Failed, EControlIpcTerminalStatus::InternalError });
	}
}

void CEditorSecretVaultClient::Stop() noexcept
{
	std::shared_ptr<IEditorSecretVaultChannel> channel;
	{
		std::scoped_lock lock(m_mutex);
		m_stopped = true;
		channel = m_activeChannel;
	}
	if (channel) channel->Close();
	m_endpointReader.Close();
}

bool CEditorSecretVaultClient::IsStopped() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_stopped;
}

} // namespace platform::controlipc
