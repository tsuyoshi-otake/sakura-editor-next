/*! @file */
#include <sakura/harnessbridge/HarnessBridgeAuthenticatedSession.h>

#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <limits>
#include <string>

namespace platform::harnessbridge {
namespace {

constexpr std::size_t kNonceBytes = 16;
constexpr std::size_t kDigestBytes = 32;
constexpr std::uint32_t kMaximumOperationTimeoutMs = 30000;
constexpr std::uint32_t kKnownGrants = static_cast<std::uint32_t>(EHarnessGrant::Message)
	| static_cast<std::uint32_t>(EHarnessGrant::ConsoleRead)
	| static_cast<std::uint32_t>(EHarnessGrant::SendInput)
	| static_cast<std::uint32_t>(EHarnessGrant::ManageTerminal);

bool FillRandom(std::span<std::uint8_t> bytes) noexcept
{
	return BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool HasNonzero(const std::span<const std::uint8_t> bytes) noexcept
{
	return std::any_of(bytes.begin(), bytes.end(), [](const std::uint8_t value) { return value != 0; });
}

bool MatchesSessionFence(const HarnessBridgeTargetDescriptor& candidate,
	const HarnessBridgeTargetDescriptor& fence) noexcept
{
	if (candidate.profileId != fence.profileId
		|| candidate.profileGeneration != fence.profileGeneration
		|| candidate.editorId != fence.editorId
		|| candidate.bridgeEpoch != fence.bridgeEpoch
		|| candidate.runtimeGeneration != fence.runtimeGeneration) return false;
	const auto matches = [](const std::uint64_t value, const std::uint64_t expected) {
		return expected == 0 || value == expected;
	};
	return matches(candidate.instanceGeneration, fence.instanceGeneration)
		&& matches(candidate.sessionId, fence.sessionId)
		&& matches(candidate.windowId, fence.windowId)
		&& matches(candidate.paneId, fence.paneId)
		&& matches(candidate.instanceId, fence.instanceId);
}

void AppendU16(std::vector<std::uint8_t>& output, const std::uint16_t value)
{
	output.push_back(static_cast<std::uint8_t>(value));
	output.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>& output, const std::uint32_t value)
{
	for (unsigned int shift = 0; shift < 32; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void AppendU64(std::vector<std::uint8_t>& output, const std::uint64_t value)
{
	for (unsigned int shift = 0; shift < 64; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void AppendTarget(std::vector<std::uint8_t>& output, const HarnessBridgeTargetDescriptor& target)
{
	AppendU32(output, static_cast<std::uint32_t>(target.profileId.size()));
	output.insert(output.end(), target.profileId.begin(), target.profileId.end());
	output.insert(output.end(), target.editorId.begin(), target.editorId.end());
	AppendU64(output, target.profileGeneration);
	AppendU64(output, target.bridgeEpoch);
	AppendU64(output, target.runtimeGeneration);
	AppendU64(output, target.instanceGeneration);
	AppendU64(output, target.sessionId);
	AppendU64(output, target.windowId);
	AppendU64(output, target.paneId);
	AppendU64(output, target.instanceId);
}

bool IsExactFlags(const EHarnessBridgeFrameFlags actual,
	const EHarnessBridgeFrameFlags expected) noexcept
{
	return static_cast<std::uint16_t>(actual) == static_cast<std::uint16_t>(expected);
}

bool IsKnownOperation(const EHarnessOperationKind operation) noexcept
{
	switch (operation) {
	case EHarnessOperationKind::QueryOperation:
	case EHarnessOperationKind::ListSessions:
	case EHarnessOperationKind::ListWindows:
	case EHarnessOperationKind::ListPanes:
	case EHarnessOperationKind::CreateSession:
	case EHarnessOperationKind::CreateTerminalWindow:
	case EHarnessOperationKind::SplitPane:
	case EHarnessOperationKind::SelectWindow:
	case EHarnessOperationKind::SelectPane:
	case EHarnessOperationKind::ClosePane:
	case EHarnessOperationKind::CloseWindow:
	case EHarnessOperationKind::CloseSession:
	case EHarnessOperationKind::HasSession:
	case EHarnessOperationKind::SendInput:
	case EHarnessOperationKind::Capture:
	case EHarnessOperationKind::Display:
	case EHarnessOperationKind::WaitChannel:
	case EHarnessOperationKind::Resize:
	case EHarnessOperationKind::RegisterEndpoint:
	case EHarnessOperationKind::RenewEndpoint:
	case EHarnessOperationKind::ListEndpoints:
	case EHarnessOperationKind::SendEndpointMessage:
	case EHarnessOperationKind::ReceiveMessages:
	case EHarnessOperationKind::AcknowledgeMessage:
	case EHarnessOperationKind::PublishRun:
	case EHarnessOperationKind::WaitRun:
	case EHarnessOperationKind::CancelRun:
	case EHarnessOperationKind::ExecuteTmux:
		return true;
	}
	return false;
}

} // namespace

std::vector<std::uint8_t> BuildHarnessBridgeAuthenticationTranscript(
	const std::uint64_t requestId, const std::uint32_t clientProcessId,
	const std::array<std::uint8_t, 16>& bridgeId,
	const HarnessBridgeTargetDescriptor& target,
	const std::array<std::uint8_t, 16>& clientNonce,
	const std::array<std::uint8_t, 16>& serverNonce,
	const EHarnessGrant requestedGrants)
{
	static constexpr char kLabel[] = "SakuraHarnessBridge/HMAC-SHA256/v1";
	std::vector<std::uint8_t> transcript;
	transcript.reserve(192 + target.profileId.size());
	AppendU16(transcript, kHarnessBridgeMajorVersion);
	AppendU16(transcript, kHarnessBridgeMinorVersion);
	AppendU32(transcript, static_cast<std::uint32_t>(sizeof(kLabel) - 1));
	transcript.insert(transcript.end(), kLabel, kLabel + sizeof(kLabel) - 1);
	transcript.insert(transcript.end(), bridgeId.begin(), bridgeId.end());
	AppendU64(transcript, target.bridgeEpoch);
	AppendU32(transcript, clientProcessId);
	transcript.insert(transcript.end(), clientNonce.begin(), clientNonce.end());
	transcript.insert(transcript.end(), serverNonce.begin(), serverNonce.end());
	AppendTarget(transcript, target);
	AppendU32(transcript, static_cast<std::uint32_t>(requestedGrants));
	AppendU64(transcript, requestId);
	return transcript;
}

CHarnessBridgeAuthenticatedSession::CHarnessBridgeAuthenticatedSession(
	HarnessBridgeSessionFences fences, CHarnessBridgeCapabilityStore& capabilities,
	HarnessBridgePeerProcessCallbacks processCallbacks,
	std::shared_ptr<IHarnessBridgeOperationDispatcher> dispatcher,
	HarnessBridgeAuthenticatedSessionOptions options)
	: m_fences(std::move(fences)), m_capabilities(capabilities),
	  m_processCallbacks(std::move(processCallbacks)), m_dispatcher(std::move(dispatcher)),
	  m_options(options)
{
	if (m_options.handshakeTimeout <= std::chrono::milliseconds::zero()) {
		m_options.handshakeTimeout = std::chrono::milliseconds(1);
	}
	if (m_options.maximumCompletedOperations == 0) m_options.maximumCompletedOperations = 4096;
	if (m_options.maximumCompletedOperationBytes == 0) m_options.maximumCompletedOperationBytes = 1024u * 1024u;
	m_handshakeDeadline = std::chrono::steady_clock::now() + m_options.handshakeTimeout;
}

HarnessBridgeTransportResult CHarnessBridgeAuthenticatedSession::HandleFrame(
	const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
	std::vector<HarnessBridgeFrame>& responses)
{
	responses.clear();
	if (m_state == EHarnessBridgeAuthenticationState::Closed) {
		return { false, EHarnessBridgeDisconnectReason::ProtocolError, ERROR_INVALID_STATE };
	}
	if (m_state != EHarnessBridgeAuthenticationState::Ready
		&& std::chrono::steady_clock::now() > m_handshakeDeadline) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::DeadlineExceeded, responses);
	}
	switch (m_state) {
	case EHarnessBridgeAuthenticationState::AwaitingHello:
		return HandleHello(session, frame, responses);
	case EHarnessBridgeAuthenticationState::ChallengeSent:
		return HandleAuthenticate(session, frame, responses);
	case EHarnessBridgeAuthenticationState::Ready:
		return HandleOperation(session, frame, responses);
	case EHarnessBridgeAuthenticationState::Closed:
		break;
	}
	return { false, EHarnessBridgeDisconnectReason::ProtocolError, ERROR_INVALID_STATE };
}

HarnessBridgeTransportResult CHarnessBridgeAuthenticatedSession::HandleHello(
	const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
	std::vector<HarnessBridgeFrame>& responses)
{
	if (!CheckFrameBasics(frame, EHarnessBridgeFrameKind::Hello, EHarnessBridgeFrameFlags::Request)
		|| frame.header.bridgeEpoch != 0 || session.clientProcessId == 0) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	}
	const auto fields = DecodeFields(frame);
	if (!fields) return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	const auto* capabilityField = FindHarnessBridgeField(*fields, EHarnessBridgeFieldTag::Payload);
	const auto target = DecodeTarget(*fields, EHarnessBridgeFieldTag::CurrentTarget);
	const auto nonce = GetNonce(*fields, EHarnessBridgeFieldTag::ClientNonce);
	const auto grants = GetU32(*fields, EHarnessBridgeFieldTag::Grants);
	if (capabilityField == nullptr || capabilityField->value.size() != m_capabilityId.value.size()
		|| !target || !nonce || !grants || (*grants == 0) || ((*grants & ~kKnownGrants) != 0)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::InvalidRequest, responses);
	}
	std::copy(capabilityField->value.begin(), capabilityField->value.end(), m_capabilityId.value.begin());
	m_clientTarget = *target;
	m_clientNonce = *nonce;
	m_requestedGrants = static_cast<EHarnessGrant>(*grants);
	if (!MatchesSessionFence(m_clientTarget, m_fences.target)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::GenerationMismatch, responses);
	}
	if (m_fences.bridgeId == std::array<std::uint8_t, 16>{}) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::InternalError, responses);
	}
	if (!m_processCallbacks.queryCreationTime || !m_processCallbacks.isJobMember) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	const auto creation = m_processCallbacks.queryCreationTime(session.clientProcessId);
	if (!creation || *creation == 0
		|| !m_processCallbacks.isJobMember(m_clientTarget, session.clientProcessId, *creation)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	m_peerCreationTime = *creation;
	if (!FillRandom(m_serverNonce)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::InternalError, responses);
	}
	m_helloRequestId = frame.header.requestId;
	const HarnessBridgeFields challengeFields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::ServerNonce),
			std::vector<std::uint8_t>(m_serverNonce.begin(), m_serverNonce.end()) },
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::ConnectionLease),
			std::vector<std::uint8_t>(m_fences.bridgeId.begin(), m_fences.bridgeId.end()) },
	};
	responses.push_back(MakeFrame(EHarnessBridgeFrameKind::Challenge,
		EHarnessBridgeFrameFlags::Response, frame.header.requestId, challengeFields));
	m_state = EHarnessBridgeAuthenticationState::ChallengeSent;
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult CHarnessBridgeAuthenticatedSession::HandleAuthenticate(
	const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
	std::vector<HarnessBridgeFrame>& responses)
{
	if (!CheckFrameBasics(frame, EHarnessBridgeFrameKind::Authenticate, EHarnessBridgeFrameFlags::Request)
		|| frame.header.requestId != m_helloRequestId || frame.header.bridgeEpoch != m_fences.target.bridgeEpoch) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	}
	const auto fields = DecodeFields(frame);
	if (!fields) return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	const auto* digestField = FindHarnessBridgeField(*fields, EHarnessBridgeFieldTag::AuthenticationDigest);
	if (digestField == nullptr || digestField->value.size() != kDigestBytes) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::InvalidRequest, responses);
	}
	m_transcript = BuildTranscript(session);
	const auto result = m_capabilities.CheckAuthentication(m_capabilityId, m_requestedGrants,
		BuildCapabilityContext(session), EHarnessGrant::None, m_transcript, digestField->value,
		std::chrono::steady_clock::now());
	if (result != EHarnessCapabilityCheck::Granted) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	std::copy(digestField->value.begin(), digestField->value.end(), m_authenticationDigest.begin());
	const std::array<std::uint8_t, 4> grantBytes{
		static_cast<std::uint8_t>(static_cast<std::uint32_t>(m_requestedGrants)),
		static_cast<std::uint8_t>(static_cast<std::uint32_t>(m_requestedGrants) >> 8),
		static_cast<std::uint8_t>(static_cast<std::uint32_t>(m_requestedGrants) >> 16),
		static_cast<std::uint8_t>(static_cast<std::uint32_t>(m_requestedGrants) >> 24),
	};
	const HarnessBridgeFields readyFields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Grants),
			std::vector<std::uint8_t>(grantBytes.begin(), grantBytes.end()) },
	};
	responses.push_back(MakeFrame(EHarnessBridgeFrameKind::Ready,
		EHarnessBridgeFrameFlags::Response, frame.header.requestId, readyFields));
	m_state = EHarnessBridgeAuthenticationState::Ready;
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult CHarnessBridgeAuthenticatedSession::HandleOperation(
	const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& frame,
	std::vector<HarnessBridgeFrame>& responses)
{
	if (!CheckFrameBasics(frame, EHarnessBridgeFrameKind::OperationRequest, EHarnessBridgeFrameFlags::Request)
		|| frame.header.bridgeEpoch != m_fences.target.bridgeEpoch) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	}
	const auto fields = DecodeFields(frame);
	if (!fields) return Reject(frame.header.requestId, EHarnessTerminalStatus::ProtocolError, responses);
	const auto operationValue = GetU16(*fields, EHarnessBridgeFieldTag::OperationKind);
	const auto* operationIdField = FindHarnessBridgeField(*fields, EHarnessBridgeFieldTag::OperationId);
	const auto timeout = GetU32(*fields, EHarnessBridgeFieldTag::TimeoutMs);
	if (!operationValue || !operationIdField || operationIdField->value.size() != 16 || !timeout
		|| *timeout == 0 || *timeout > kMaximumOperationTimeoutMs) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::InvalidRequest, responses);
	}
	const auto operation = static_cast<EHarnessOperationKind>(*operationValue);
	if (!IsKnownOperation(operation)) return Reject(frame.header.requestId, EHarnessTerminalStatus::OperationUnknown, responses);
	HarnessOpaqueId operationId;
	std::copy(operationIdField->value.begin(), operationIdField->value.end(), operationId.value.begin());
	if (!operationId.IsValid()) return Reject(frame.header.requestId, EHarnessTerminalStatus::InvalidRequest, responses);
	for (const auto& completed : m_completedOperations) {
		if (completed.operationId == operationId) {
			HarnessBridgeFields resultFields{
				{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::TerminalStatus),
					{ static_cast<std::uint8_t>(static_cast<std::uint16_t>(completed.response.status)),
						static_cast<std::uint8_t>(static_cast<std::uint16_t>(completed.response.status) >> 8) } },
			};
			if (!completed.response.payload.empty()) resultFields.push_back({ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload), completed.response.payload });
			responses.push_back(MakeFrame(EHarnessBridgeFrameKind::OperationResponse,
				EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Terminal,
				frame.header.requestId, resultFields));
			return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
		}
	}
	if (!m_processCallbacks.queryCreationTime || !m_processCallbacks.isJobMember) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	const auto creation = m_processCallbacks.queryCreationTime(session.clientProcessId);
	if (!creation || *creation != m_peerCreationTime
		|| !m_processCallbacks.isJobMember(m_clientTarget, session.clientProcessId, *creation)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	const auto capabilityResult = m_capabilities.CheckAuthentication(m_capabilityId, m_requestedGrants,
		BuildCapabilityContext(session), EHarnessGrant::None, m_transcript, m_authenticationDigest,
		std::chrono::steady_clock::now());
	if (capabilityResult != EHarnessCapabilityCheck::Granted) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	}
	const auto target = DecodeTarget(*fields, EHarnessBridgeFieldTag::Target);
	if (FindHarnessBridgeField(*fields, EHarnessBridgeFieldTag::Target) != nullptr
		&& (!target || *target != m_clientTarget)) {
		return Reject(frame.header.requestId, EHarnessTerminalStatus::GenerationMismatch, responses);
	}
	const auto required = RequiredGrant(operation);
	if (!HasGrant(m_requestedGrants, required)) return Reject(frame.header.requestId, EHarnessTerminalStatus::AccessDenied, responses);
	HarnessBridgeOperationRequestDto request;
	request.operationId = operationId;
	request.operation = operation;
	request.target = m_clientTarget;
	if (const auto* payload = FindHarnessBridgeField(*fields, EHarnessBridgeFieldTag::Payload)) request.payload = payload->value;
	request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(*timeout);
	if (request.deadline <= std::chrono::steady_clock::now()) return Reject(frame.header.requestId, EHarnessTerminalStatus::DeadlineExceeded, responses);
	HarnessBridgeOperationResponseDto operationResponse;
	if (!m_dispatcher) {
		operationResponse.status = EHarnessTerminalStatus::UnsupportedCapability;
	} else {
		try {
			operationResponse = m_dispatcher->Dispatch(session, request);
		} catch (...) {
			operationResponse.status = EHarnessTerminalStatus::InternalError;
			operationResponse.payload.clear();
		}
	}
	if (operationResponse.payload.size() > kHarnessBridgeMaximumPayloadBytes) {
		operationResponse.status = EHarnessTerminalStatus::ResourceExhausted;
		operationResponse.payload.clear();
	}
	if (std::chrono::steady_clock::now() > request.deadline
		&& operationResponse.status == EHarnessTerminalStatus::Succeeded) {
		operationResponse.status = EHarnessTerminalStatus::DeadlineExceeded;
		operationResponse.payload.clear();
	}
	const bool cacheResponse = operationResponse.payload.size() <= m_options.maximumCompletedOperationBytes;
	if (cacheResponse) {
		while (!m_completedOperations.empty() && (m_completedOperations.size() >= m_options.maximumCompletedOperations
			|| m_completedOperationBytes + operationResponse.payload.size() > m_options.maximumCompletedOperationBytes)) {
			m_completedOperationBytes -= m_completedOperations.front().response.payload.size();
			m_completedOperations.pop_front();
		}
		m_completedOperationBytes += operationResponse.payload.size();
		m_completedOperations.push_back({ operationId, operationResponse });
	}
	const auto status = static_cast<std::uint16_t>(operationResponse.status);
	HarnessBridgeFields resultFields{
		{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::TerminalStatus),
			{ static_cast<std::uint8_t>(status), static_cast<std::uint8_t>(status >> 8) } },
	};
	if (!operationResponse.payload.empty()) resultFields.push_back({ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload), operationResponse.payload });
	responses.push_back(MakeFrame(EHarnessBridgeFrameKind::OperationResponse,
		EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Terminal,
		frame.header.requestId, resultFields));
	return { true, EHarnessBridgeDisconnectReason::None, ERROR_SUCCESS };
}

HarnessBridgeTransportResult CHarnessBridgeAuthenticatedSession::Reject(
	const std::uint64_t requestId, const EHarnessTerminalStatus status,
	std::vector<HarnessBridgeFrame>& responses) noexcept
{
	m_state = EHarnessBridgeAuthenticationState::Closed;
	try {
		const auto value = static_cast<std::uint16_t>(status);
		HarnessBridgeFields fields{
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::TerminalStatus),
				{ static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8) } },
		};
		responses.push_back(MakeFrame(EHarnessBridgeFrameKind::Error,
			EHarnessBridgeFrameFlags::Response | EHarnessBridgeFrameFlags::Terminal,
			requestId, fields));
		const auto reason = status == EHarnessTerminalStatus::DeadlineExceeded
			? EHarnessBridgeDisconnectReason::DeadlineExceeded : EHarnessBridgeDisconnectReason::ProtocolError;
		return { true, reason, ERROR_ACCESS_DENIED };
	} catch (...) {
		responses.clear();
		return { false, EHarnessBridgeDisconnectReason::ProtocolError, ERROR_NOT_ENOUGH_MEMORY };
	}
}

bool CHarnessBridgeAuthenticatedSession::CheckFrameBasics(
	const HarnessBridgeFrame& frame, const EHarnessBridgeFrameKind kind,
	const EHarnessBridgeFrameFlags requiredFlags) const noexcept
{
	return frame.header.majorVersion == kHarnessBridgeMajorVersion
		&& frame.header.minorVersion == kHarnessBridgeMinorVersion
		&& frame.header.kind == kind && frame.header.requestId != 0
		&& IsExactFlags(frame.header.flags, requiredFlags);
}

std::optional<HarnessBridgeFields> CHarnessBridgeAuthenticatedSession::DecodeFields(
	const HarnessBridgeFrame& frame) const noexcept
{
	try {
		const auto decoded = DecodeHarnessBridgeFields(frame.payload);
		if (decoded.outcome != EHarnessBridgeFieldDecodeOutcome::Decoded) return std::nullopt;
		return decoded.fields;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<HarnessBridgeTargetDescriptor> CHarnessBridgeAuthenticatedSession::DecodeTarget(
	const HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag) const
{
	const auto* value = FindHarnessBridgeField(fields, tag);
	if (value == nullptr || value->value.empty() || !IsValidHarnessBridgeUtf8(value->value)) return std::nullopt;
	std::wstring encoded;
	encoded.reserve(value->value.size());
	for (const auto byte : value->value) {
		if (byte > 0x7f) return std::nullopt;
		encoded.push_back(static_cast<wchar_t>(byte));
	}
	return DecodeHarnessTargetEnvironment(encoded);
}

std::optional<std::uint32_t> CHarnessBridgeAuthenticatedSession::GetU32(
	const HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag) const noexcept
{
	const auto* value = FindHarnessBridgeField(fields, tag);
	if (value == nullptr || value->value.size() != 4) return std::nullopt;
	return static_cast<std::uint32_t>(value->value[0]) | (static_cast<std::uint32_t>(value->value[1]) << 8)
		| (static_cast<std::uint32_t>(value->value[2]) << 16) | (static_cast<std::uint32_t>(value->value[3]) << 24);
}

std::optional<std::uint16_t> CHarnessBridgeAuthenticatedSession::GetU16(
	const HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag) const noexcept
{
	const auto* value = FindHarnessBridgeField(fields, tag);
	if (value == nullptr || value->value.size() != 2) return std::nullopt;
	return static_cast<std::uint16_t>(static_cast<std::uint16_t>(value->value[0])
		| (static_cast<std::uint16_t>(value->value[1]) << 8));
}

std::optional<std::array<std::uint8_t, 16>> CHarnessBridgeAuthenticatedSession::GetNonce(
	const HarnessBridgeFields& fields, const EHarnessBridgeFieldTag tag) const noexcept
{
	const auto* value = FindHarnessBridgeField(fields, tag);
	if (value == nullptr || value->value.size() != kNonceBytes || !HasNonzero(value->value)) return std::nullopt;
	std::array<std::uint8_t, 16> result{};
	std::copy(value->value.begin(), value->value.end(), result.begin());
	return result;
}

EHarnessGrant CHarnessBridgeAuthenticatedSession::RequiredGrant(const EHarnessOperationKind operation) noexcept
{
	switch (operation) {
	case EHarnessOperationKind::SendInput: return EHarnessGrant::SendInput;
	case EHarnessOperationKind::Capture:
	case EHarnessOperationKind::Display:
	case EHarnessOperationKind::WaitChannel: return EHarnessGrant::ConsoleRead;
	case EHarnessOperationKind::RegisterEndpoint:
	case EHarnessOperationKind::RenewEndpoint:
	case EHarnessOperationKind::ListEndpoints:
	case EHarnessOperationKind::SendEndpointMessage:
	case EHarnessOperationKind::ReceiveMessages:
	case EHarnessOperationKind::AcknowledgeMessage: return EHarnessGrant::Message;
	case EHarnessOperationKind::CreateSession:
	case EHarnessOperationKind::CreateTerminalWindow:
	case EHarnessOperationKind::SplitPane:
	case EHarnessOperationKind::SelectWindow:
	case EHarnessOperationKind::SelectPane:
	case EHarnessOperationKind::ClosePane:
	case EHarnessOperationKind::CloseWindow:
	case EHarnessOperationKind::CloseSession:
	case EHarnessOperationKind::Resize:
	case EHarnessOperationKind::PublishRun:
	case EHarnessOperationKind::WaitRun:
	case EHarnessOperationKind::CancelRun: return EHarnessGrant::ManageTerminal;
	case EHarnessOperationKind::ExecuteTmux: return EHarnessGrant::ManageTerminal;
	case EHarnessOperationKind::QueryOperation:
	case EHarnessOperationKind::ListSessions:
	case EHarnessOperationKind::ListWindows:
	case EHarnessOperationKind::ListPanes:
	case EHarnessOperationKind::HasSession: return EHarnessGrant::None;
	}
	return EHarnessGrant::ManageTerminal;
}

std::vector<std::uint8_t> CHarnessBridgeAuthenticatedSession::BuildTranscript(
	const HarnessBridgeSessionContext& session) const
{
	return BuildHarnessBridgeAuthenticationTranscript(m_helloRequestId, session.clientProcessId,
		m_fences.bridgeId, m_clientTarget, m_clientNonce, m_serverNonce, m_requestedGrants);
}

HarnessCapabilityContext CHarnessBridgeAuthenticatedSession::BuildCapabilityContext(
	const HarnessBridgeSessionContext& session) const
{
	HarnessCapabilityContext context;
	context.bridgeEpoch = m_clientTarget.bridgeEpoch;
	context.runtimeGeneration = m_clientTarget.runtimeGeneration;
	context.sessionId = m_clientTarget.sessionId;
	context.paneId = m_clientTarget.paneId;
	context.processId = session.clientProcessId;
	context.processCreationTime = m_peerCreationTime;
	context.profileId = m_clientTarget.profileId;
	context.profileGeneration = m_clientTarget.profileGeneration;
	context.editorId = m_clientTarget.editorId;
	context.instanceGeneration = m_clientTarget.instanceGeneration;
	context.windowId = m_clientTarget.windowId;
	context.instanceId = m_clientTarget.instanceId;
	return context;
}

HarnessBridgeFrame CHarnessBridgeAuthenticatedSession::MakeFrame(
	const EHarnessBridgeFrameKind kind, const EHarnessBridgeFrameFlags flags,
	const std::uint64_t requestId, const HarnessBridgeFields& fields) const
{
	HarnessBridgeFrame frame;
	frame.header.kind = kind;
	frame.header.flags = flags;
	frame.header.requestId = requestId;
	frame.header.bridgeEpoch = m_fences.target.bridgeEpoch;
	if (const auto encoded = EncodeHarnessBridgeFields(fields)) frame.payload = *encoded;
	return frame;
}

CHarnessBridgeAuthenticatedSessionFactory::CHarnessBridgeAuthenticatedSessionFactory(
	HarnessBridgeSessionFences fences, CHarnessBridgeCapabilityStore& capabilities,
	HarnessBridgePeerProcessCallbacks processCallbacks,
	HarnessBridgeOperationDispatcherFactory dispatcherFactory,
	HarnessBridgeAuthenticatedSessionOptions options)
	: m_fences(std::move(fences)), m_capabilities(capabilities),
	  m_processCallbacks(std::move(processCallbacks)), m_dispatcherFactory(std::move(dispatcherFactory)),
	  m_options(options)
{
}

std::unique_ptr<IHarnessBridgeSessionHandler> CHarnessBridgeAuthenticatedSessionFactory::CreateSession(
	const HarnessBridgeSessionContext& session, const HarnessBridgeFrame& firstFrame)
{
	if (session.clientProcessId == 0 || firstFrame.header.kind != EHarnessBridgeFrameKind::Hello
		|| firstFrame.header.flags != EHarnessBridgeFrameFlags::Request) return nullptr;
	std::shared_ptr<IHarnessBridgeOperationDispatcher> dispatcher;
	if (m_dispatcherFactory) {
		try {
			dispatcher = m_dispatcherFactory(session, firstFrame);
		} catch (...) {
			return nullptr;
		}
	}
	return std::make_unique<CHarnessBridgeAuthenticatedSession>(m_fences, m_capabilities,
		m_processCallbacks, std::move(dispatcher), m_options);
}

} // namespace platform::harnessbridge
