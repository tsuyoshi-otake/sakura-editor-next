/*! @file */
#include <sakura/harnessbridge/HarnessBridgeClientSession.h>

#include <sakura/harnessbridge/HarnessBridgeAuthenticatedSession.h>
#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <Windows.h>
#include <bcrypt.h>

// Windows SDK convenience macros collide with protocol operation names.
#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

namespace platform::harnessbridge {
namespace {

constexpr std::size_t kNonceBytes = 16;
constexpr std::size_t kDigestBytes = 32;
constexpr std::uint32_t kMaximumWireTimeoutMs = 30000;

bool FillRandom(std::span<std::uint8_t> output) noexcept
{
	return output.size() <= static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())
		&& BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, output.data(),
			static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::optional<std::array<std::uint8_t, kDigestBytes>> ComputeHmac(
	const std::array<std::uint8_t, 32>& key,
	const std::span<const std::uint8_t> input) noexcept
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	std::vector<std::uint8_t> object;
	std::array<std::uint8_t, kDigestBytes> result{};
	DWORD objectBytes = 0;
	DWORD resultBytes = 0;
	if( !BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
		&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)) ) return std::nullopt;
	const auto closeAlgorithm = [&] { if( algorithm ) ::BCryptCloseAlgorithmProvider(algorithm, 0); };
	if( !BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &resultBytes, 0)) ) {
		closeAlgorithm();
		return std::nullopt;
	}
	try {
		object.resize(objectBytes);
	} catch( ... ) {
		closeAlgorithm();
		return std::nullopt;
	}
	if( !BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
		const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0))
		|| (input.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()))
		|| !BCRYPT_SUCCESS(::BCryptHashData(hash, const_cast<PUCHAR>(input.data()),
			static_cast<ULONG>(input.size()), 0))
		|| !BCRYPT_SUCCESS(::BCryptFinishHash(hash, result.data(),
			static_cast<ULONG>(result.size()), 0)) ) {
		if( hash ) ::BCryptDestroyHash(hash);
		closeAlgorithm();
		return std::nullopt;
	}
	::BCryptDestroyHash(hash);
	closeAlgorithm();
	return result;
}

std::vector<std::uint8_t> U16(const std::uint16_t value)
{
	return { static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8) };
}

std::vector<std::uint8_t> U32(const std::uint32_t value)
{
	return {
		static_cast<std::uint8_t>(value),
		static_cast<std::uint8_t>(value >> 8),
		static_cast<std::uint8_t>(value >> 16),
		static_cast<std::uint8_t>(value >> 24),
	};
}

std::optional<std::uint16_t> ReadU16(const HarnessBridgeFields& fields,
	const EHarnessBridgeFieldTag tag) noexcept
{
	const auto* field = FindHarnessBridgeField(fields, tag);
	if( !field || field->value.size() != 2 ) return std::nullopt;
	return static_cast<std::uint16_t>(field->value[0]
		| static_cast<std::uint16_t>(field->value[1]) << 8);
}

std::optional<std::uint32_t> ReadU32(const HarnessBridgeFields& fields,
	const EHarnessBridgeFieldTag tag) noexcept
{
	const auto* field = FindHarnessBridgeField(fields, tag);
	if( !field || field->value.size() != 4 ) return std::nullopt;
	return static_cast<std::uint32_t>(field->value[0])
		| static_cast<std::uint32_t>(field->value[1]) << 8
		| static_cast<std::uint32_t>(field->value[2]) << 16
		| static_cast<std::uint32_t>(field->value[3]) << 24;
}

HarnessBridgeClientConnectResult ConnectFailure(
	const EHarnessTerminalStatus status, const std::uint32_t errorCode = 0) noexcept
{
	return { false, status, errorCode };
}

std::optional<std::wstring> Utf8ToWide(const std::string_view value)
{
	if( value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ) {
		return std::nullopt;
	}
	const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if( required <= 0 ) return std::nullopt;
	std::wstring result(static_cast<std::size_t>(required), L'\0');
	if( ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), required) != required ) return std::nullopt;
	return result;
}

struct SecretGuard final {
	std::array<std::uint8_t, 32>* secret = nullptr;
	~SecretGuard()
	{
		if( secret ) std::fill(secret->begin(), secret->end(), std::uint8_t{ 0 });
	}
};

} // namespace

struct CHarnessBridgeClientSession::Impl final {
	explicit Impl(HarnessBridgeClientSessionOptions value) : options(value) {}

	void Close() noexcept
	{
		pipe.Close();
		target = {};
		descriptor = {};
		ready = false;
		nextRequestId = 2;
	}

	HarnessBridgeClientSessionOptions options;
	CHarnessBridgeNamedPipeClient pipe;
	HarnessEditorEndpointDescriptor descriptor;
	HarnessBridgeTargetDescriptor target;
	std::uint64_t nextRequestId = 2;
	bool ready = false;
	mutable std::mutex mutex;
};

CHarnessBridgeClientSession::CHarnessBridgeClientSession(HarnessBridgeClientSessionOptions options)
	: m_impl(std::make_unique<Impl>(options))
{
	if( m_impl->options.connectTimeout <= std::chrono::milliseconds::zero() ) {
		m_impl->options.connectTimeout = std::chrono::milliseconds(1);
	}
	if( m_impl->options.handshakeTimeout <= std::chrono::milliseconds::zero() ) {
		m_impl->options.handshakeTimeout = std::chrono::milliseconds(1);
	}
	if( m_impl->options.maximumOperationTimeout <= std::chrono::milliseconds::zero()
		|| m_impl->options.maximumOperationTimeout > std::chrono::milliseconds(kMaximumWireTimeoutMs) ) {
		m_impl->options.maximumOperationTimeout = std::chrono::milliseconds(kMaximumWireTimeoutMs);
	}
}

CHarnessBridgeClientSession::~CHarnessBridgeClientSession()
{
	Close();
}

HarnessBridgeClientConnectResult CHarnessBridgeClientSession::Connect(
	const std::wstring_view endpointEnvironment,
	const std::wstring_view targetEnvironment,
	const std::wstring_view capabilityEnvironment)
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->Close();
	try {
		const auto endpointHash = DecodeHarnessEndpointEnvironment(endpointEnvironment);
		const auto target = DecodeHarnessTargetEnvironment(targetEnvironment);
		auto credential = DecodeHarnessCapabilityEnvironment(capabilityEnvironment);
		if( !endpointHash || !target || !credential ) {
			return ConnectFailure(EHarnessTerminalStatus::InvalidRequest, ERROR_INVALID_DATA);
		}
		SecretGuard secretGuard{ &credential->secret };
		CHarnessBridgeEndpointReader reader;
		const auto endpoint = reader.Read(BuildHarnessEndpointMappingName(*endpointHash), {
			.expectedEndpointHash = *endpointHash,
			.minimumBridgeEpoch = target->bridgeEpoch,
			.requireLiveServer = true,
		});
		if( endpoint.disposition != EHarnessBridgeEndpointDisposition::Discovered || !endpoint.descriptor ) {
			return ConnectFailure(EHarnessTerminalStatus::AccessDenied, ERROR_NOT_FOUND);
		}
		const auto& descriptor = *endpoint.descriptor;
		const auto targetProfileId = Utf8ToWide(target->profileId);
		if( !targetProfileId || descriptor.profileId != *targetProfileId
			|| descriptor.profileGeneration != target->profileGeneration
			|| descriptor.editorId != target->editorId
			|| descriptor.bridgeEpoch != target->bridgeEpoch
			|| descriptor.runtimeGeneration != target->runtimeGeneration) {
			return ConnectFailure(EHarnessTerminalStatus::GenerationMismatch, ERROR_INVALID_DATA);
		}
		const auto connected = m_impl->pipe.Connect(descriptor.pipeName,
			descriptor.serverPid, m_impl->options.connectTimeout);
		if( !connected.success ) {
			return ConnectFailure(EHarnessTerminalStatus::AccessDenied, connected.errorCode);
		}

		std::array<std::uint8_t, kNonceBytes> clientNonce{};
		if( !FillRandom(clientNonce) ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::InternalError, ERROR_GEN_FAILURE);
		}
		const auto encodedTarget = EncodeHarnessTargetEnvironment(*target);
		if( !encodedTarget ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::InvalidRequest, ERROR_INVALID_DATA);
		}
		std::vector<std::uint8_t> targetBytes;
		targetBytes.reserve(encodedTarget->size());
		for( const auto value : *encodedTarget ) {
			if( value > 0x7f ) {
				m_impl->Close();
				return ConnectFailure(EHarnessTerminalStatus::InvalidRequest, ERROR_INVALID_DATA);
			}
			targetBytes.push_back(static_cast<std::uint8_t>(value));
		}
		HarnessBridgeFields helloFields{
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload),
				std::vector<std::uint8_t>(credential->id.value.begin(), credential->id.value.end()) },
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::CurrentTarget), std::move(targetBytes) },
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Grants),
				U32(static_cast<std::uint32_t>(credential->grants)) },
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::ClientNonce),
				std::vector<std::uint8_t>(clientNonce.begin(), clientNonce.end()) },
		};
		const auto encodedHello = EncodeHarnessBridgeFields(helloFields);
		if( !encodedHello ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::InvalidRequest, ERROR_INVALID_DATA);
		}
		HarnessBridgeFrame hello;
		hello.header.kind = EHarnessBridgeFrameKind::Hello;
		hello.header.flags = EHarnessBridgeFrameFlags::Request;
		hello.header.requestId = 1;
		hello.header.bridgeEpoch = 0;
		hello.payload = *encodedHello;
		std::vector<HarnessBridgeFrame> responses;
		auto exchanged = m_impl->pipe.Exchange(hello, responses, m_impl->options.handshakeTimeout);
		if( !exchanged.success || responses.size() != 1
			|| responses[0].header.kind != EHarnessBridgeFrameKind::Challenge
			|| responses[0].header.requestId != 1
			|| responses[0].header.bridgeEpoch != target->bridgeEpoch ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::ProtocolError, exchanged.errorCode);
		}
		const auto challenge = DecodeHarnessBridgeFields(responses[0].payload);
		if( challenge.outcome != EHarnessBridgeFieldDecodeOutcome::Decoded ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::ProtocolError, ERROR_INVALID_DATA);
		}
		const auto* nonceField = FindHarnessBridgeField(challenge.fields, EHarnessBridgeFieldTag::ServerNonce);
		const auto* bridgeField = FindHarnessBridgeField(challenge.fields, EHarnessBridgeFieldTag::ConnectionLease);
		if( !nonceField || nonceField->value.size() != kNonceBytes || !bridgeField
			|| bridgeField->value.size() != descriptor.bridgeId.size()
			|| !std::equal(bridgeField->value.begin(), bridgeField->value.end(), descriptor.bridgeId.begin()) ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::ProtocolError, ERROR_INVALID_DATA);
		}
		std::array<std::uint8_t, kNonceBytes> serverNonce{};
		std::copy(nonceField->value.begin(), nonceField->value.end(), serverNonce.begin());
		const auto transcript = BuildHarnessBridgeAuthenticationTranscript(
			1, ::GetCurrentProcessId(), descriptor.bridgeId, *target,
			clientNonce, serverNonce, credential->grants);
		const auto digest = ComputeHmac(credential->secret, transcript);
		if( !digest ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::InternalError, ERROR_GEN_FAILURE);
		}
		const HarnessBridgeFields authenticateFields{
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::AuthenticationDigest),
				std::vector<std::uint8_t>(digest->begin(), digest->end()) },
		};
		const auto encodedAuthentication = EncodeHarnessBridgeFields(authenticateFields);
		if( !encodedAuthentication ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::InternalError, ERROR_INVALID_DATA);
		}
		HarnessBridgeFrame authentication;
		authentication.header.kind = EHarnessBridgeFrameKind::Authenticate;
		authentication.header.flags = EHarnessBridgeFrameFlags::Request;
		authentication.header.requestId = 1;
		authentication.header.bridgeEpoch = target->bridgeEpoch;
		authentication.payload = *encodedAuthentication;
		responses.clear();
		exchanged = m_impl->pipe.Exchange(authentication, responses, m_impl->options.handshakeTimeout);
		if( !exchanged.success || responses.size() != 1
			|| responses[0].header.kind != EHarnessBridgeFrameKind::Ready
			|| responses[0].header.requestId != 1
			|| responses[0].header.bridgeEpoch != target->bridgeEpoch ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::AccessDenied, exchanged.errorCode);
		}
		const auto ready = DecodeHarnessBridgeFields(responses[0].payload);
		const auto readyGrants = ready.outcome == EHarnessBridgeFieldDecodeOutcome::Decoded
			? ReadU32(ready.fields, EHarnessBridgeFieldTag::Grants) : std::nullopt;
		if( !readyGrants || *readyGrants != static_cast<std::uint32_t>(credential->grants) ) {
			m_impl->Close();
			return ConnectFailure(EHarnessTerminalStatus::AccessDenied, ERROR_ACCESS_DENIED);
		}
		m_impl->descriptor = descriptor;
		m_impl->target = *target;
		m_impl->ready = true;
		return { true, EHarnessTerminalStatus::Succeeded, ERROR_SUCCESS };
	} catch( ... ) {
		m_impl->Close();
		return ConnectFailure(EHarnessTerminalStatus::InternalError, ERROR_NOT_ENOUGH_MEMORY);
	}
}

HarnessBridgeClientOperationResult CHarnessBridgeClientSession::Execute(
	const EHarnessOperationKind operation,
	const std::span<const std::uint8_t> payload,
	std::chrono::milliseconds timeout)
{
	std::lock_guard lock(m_impl->mutex);
	if( !m_impl->ready ) return { EHarnessTerminalStatus::AccessDenied, {}, ERROR_INVALID_STATE };
	if( payload.size() > kHarnessBridgeMaximumPayloadBytes ) {
		return { EHarnessTerminalStatus::ResourceExhausted, {}, ERROR_BUFFER_OVERFLOW };
	}
	timeout = (std::min)(timeout, m_impl->options.maximumOperationTimeout);
	if( timeout <= std::chrono::milliseconds::zero() ) {
		return { EHarnessTerminalStatus::InvalidRequest, {}, ERROR_INVALID_PARAMETER };
	}
	try {
		HarnessOpaqueId operationId;
		if( !FillRandom(operationId.value) ) {
			return { EHarnessTerminalStatus::InternalError, {}, ERROR_GEN_FAILURE };
		}
		if( m_impl->nextRequestId == (std::numeric_limits<std::uint64_t>::max)() ) {
			m_impl->Close();
			return { EHarnessTerminalStatus::ResourceExhausted, {}, ERROR_ARITHMETIC_OVERFLOW };
		}
		const auto requestId = m_impl->nextRequestId++;
		const auto timeoutMs = static_cast<std::uint32_t>(timeout.count());
		HarnessBridgeFields fields{
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::OperationKind),
				U16(static_cast<std::uint16_t>(operation)) },
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::OperationId),
				std::vector<std::uint8_t>(operationId.value.begin(), operationId.value.end()) },
			{ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::TimeoutMs), U32(timeoutMs) },
		};
		if( !payload.empty() ) {
			fields.push_back({ static_cast<std::uint16_t>(EHarnessBridgeFieldTag::Payload),
				std::vector<std::uint8_t>(payload.begin(), payload.end()) });
		}
		const auto encoded = EncodeHarnessBridgeFields(fields);
		if( !encoded ) return { EHarnessTerminalStatus::InvalidRequest, {}, ERROR_INVALID_DATA };
		HarnessBridgeFrame request;
		request.header.kind = EHarnessBridgeFrameKind::OperationRequest;
		request.header.flags = EHarnessBridgeFrameFlags::Request;
		request.header.requestId = requestId;
		request.header.bridgeEpoch = m_impl->target.bridgeEpoch;
		request.payload = *encoded;
		std::vector<HarnessBridgeFrame> responses;
		const auto exchanged = m_impl->pipe.Exchange(request, responses,
			timeout + std::chrono::milliseconds(250));
		if( !exchanged.success || responses.empty() ) {
			m_impl->Close();
			return { EHarnessTerminalStatus::Ambiguous, {}, exchanged.errorCode };
		}
		const auto& response = responses.back();
		if( response.header.requestId != requestId
			|| response.header.bridgeEpoch != m_impl->target.bridgeEpoch
			|| (response.header.kind != EHarnessBridgeFrameKind::OperationResponse
				&& response.header.kind != EHarnessBridgeFrameKind::Error) ) {
			m_impl->Close();
			return { EHarnessTerminalStatus::ProtocolError, {}, ERROR_INVALID_DATA };
		}
		const auto decoded = DecodeHarnessBridgeFields(response.payload);
		if( decoded.outcome != EHarnessBridgeFieldDecodeOutcome::Decoded ) {
			m_impl->Close();
			return { EHarnessTerminalStatus::ProtocolError, {}, ERROR_INVALID_DATA };
		}
		const auto statusValue = ReadU16(decoded.fields, EHarnessBridgeFieldTag::TerminalStatus);
		if( !statusValue || *statusValue > static_cast<std::uint16_t>(EHarnessTerminalStatus::InternalError) ) {
			m_impl->Close();
			return { EHarnessTerminalStatus::ProtocolError, {}, ERROR_INVALID_DATA };
		}
		HarnessBridgeClientOperationResult result;
		result.status = static_cast<EHarnessTerminalStatus>(*statusValue);
		if( const auto* resultPayload = FindHarnessBridgeField(decoded.fields, EHarnessBridgeFieldTag::Payload) ) {
			result.payload = resultPayload->value;
		}
		return result;
	} catch( ... ) {
		return { EHarnessTerminalStatus::InternalError, {}, ERROR_NOT_ENOUGH_MEMORY };
	}
}

void CHarnessBridgeClientSession::Close() noexcept
{
	if( !m_impl ) return;
	std::lock_guard lock(m_impl->mutex);
	m_impl->Close();
}

bool CHarnessBridgeClientSession::IsReady() const noexcept
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->ready;
}

} // namespace platform::harnessbridge
