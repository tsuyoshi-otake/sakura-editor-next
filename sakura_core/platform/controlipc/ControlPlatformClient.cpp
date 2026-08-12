/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/controlipc/ControlPlatformClient.h"
#include <sakura/controlipc/ProfileAuthorityIdentity.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr auto kNoRetry = EControlPlatformRetryClass::None;
constexpr std::size_t kEncodedSnapshotHeaderBytes = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t);
constexpr std::size_t kEncodedAddressFixedBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) * 3;
constexpr std::size_t kEncodedEntryFixedBytes = kEncodedAddressFixedBytes + sizeof(std::uint8_t)
	+ sizeof(std::uint32_t) + sizeof(std::uint64_t);
constexpr std::size_t kEncodedMutationFixedBytes = kEncodedAddressFixedBytes + sizeof(std::uint8_t) * 2;

ControlPlatformClientResult Result(EControlPlatformClientOutcome outcome, EControlPlatformRetryClass retryClass,
	EControlIpcTerminalStatus status, std::wstring diagnostic = {},
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::Discovered,
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None)
{
	return { outcome, retryClass, status, discoveryDisposition, transportReason, std::move(diagnostic) };
}

ControlPlatformMutationResult MutationResult(EControlPlatformMutationOutcome outcome,
	EControlIpcTerminalStatus status, std::wstring diagnostic = {},
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::Discovered,
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None,
	storage::StorageMutationResult storageResult = {})
{
	return { outcome, std::move(storageResult), status, discoveryDisposition, transportReason, std::move(diagnostic) };
}

ControlPlatformProfileResult ProfileResult(EControlPlatformProfileOutcome outcome,
	EControlIpcTerminalStatus status, std::wstring diagnostic = {},
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::Discovered,
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None,
	std::optional<ControlProfileRpcResponse> response = std::nullopt)
{
	return { outcome, std::move(response), status, discoveryDisposition, transportReason, std::move(diagnostic) };
}

bool IsProfileMutation(EControlProfileRpcOperation operation) noexcept
{
	switch (operation) {
	case EControlProfileRpcOperation::CreateNamed:
	case EControlProfileRpcOperation::CreateTransient:
	case EControlProfileRpcOperation::Rename:
	case EControlProfileRpcOperation::Delete:
	case EControlProfileRpcOperation::AssociateWorkspace:
	case EControlProfileRpcOperation::AssociateEmptyWindow:
	case EControlProfileRpcOperation::Import:
		return true;
	case EControlProfileRpcOperation::Snapshot:
	case EControlProfileRpcOperation::List:
	case EControlProfileRpcOperation::Current:
	case EControlProfileRpcOperation::Resolve:
	case EControlProfileRpcOperation::Export:
		return false;
	}
	return false;
}

bool IsValidProfileMutationId(const ControlProfileRpcRequest& request) noexcept
{
	return !IsProfileMutation(request.operation) ||
		(!request.mutation.operationId.empty()
			&& request.mutation.operationId.size() <= storage::kMaximumStorageOperationIdBytes
			&& storage::IsValidStorageUtf8(request.mutation.operationId, false));
}

bool IsExactTerminalResponse(const ControlIpcFrame& frame) noexcept
{
	return frame.header.flags == (EControlIpcFlags::Response | EControlIpcFlags::Terminal);
}

bool CheckedAdd(std::size_t& value, std::size_t addend, std::size_t maximum) noexcept
{
	if (value > maximum || addend > maximum - value) return false;
	value += addend;
	return true;
}

bool IsUsableSnapshot(const storage::StorageSnapshot& snapshot, std::uint64_t expectedGeneration) noexcept
{
	if (snapshot.generation == 0 || snapshot.generation != expectedGeneration ||
		snapshot.entries.size() > kControlStorageRpcMaximumItems) return false;
	std::set<storage::StorageAddress> addresses;
	std::size_t encodedBytes = kEncodedSnapshotHeaderBytes;
	for (const auto& entry : snapshot.entries) {
		const std::size_t stringBytes = entry.address.scopeId.size() + entry.address.owner.size()
			+ entry.address.key.size() + entry.value.size();
		if (!entry.address.IsValid() || !storage::IsValidStorageTarget(entry.target)
			|| entry.value.size() > storage::kMaximumStorageStringBytes
			|| !storage::IsValidStorageUtf8(entry.value) || entry.revision == 0
			|| entry.revision > snapshot.revision
			|| !CheckedAdd(encodedBytes, kEncodedEntryFixedBytes, storage::kMaximumStorageSnapshotPayloadBytes)
			|| !CheckedAdd(encodedBytes, stringBytes, storage::kMaximumStorageSnapshotPayloadBytes) ||
			!addresses.insert(entry.address).second) return false;
	}
	return true;
}

} // namespace

std::optional<ControlPlatformEndpointSnapshot> CControlPlatformEndpointReader::Read(
	const ControlPlatformEndpointReadRequirements& requirements)
{
	return m_endpoint.Read(requirements);
}

ControlPlatformEndpointDiscoveryResult CControlPlatformEndpointReader::ReadDetailed(
	const ControlPlatformEndpointReadRequirements& requirements)
{
	return m_endpoint.ReadDetailed(requirements);
}

ControlIpcTransportResult CControlPlatformNamedPipeChannel::Connect(const ControlPlatformEndpointSnapshot& endpoint,
	std::chrono::milliseconds deadline)
{
	if (endpoint.lifecycle != ControlPlatformEndpointLifecycle::Accepting) {
		return { false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_INVALID_STATE,
			L"Control endpoint is not accepting connections" };
	}
	return m_client.Connect(endpoint.pipeName, endpoint.controlProcessId, deadline);
}

ControlIpcTransportResult CControlPlatformNamedPipeChannel::Exchange(const ControlIpcFrame& request,
	std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline)
{
	return m_client.Exchange(request, responses, deadline);
}

void CControlPlatformNamedPipeChannel::Close() noexcept
{
	m_client.Close();
}

CControlPlatformClient::CControlPlatformClient(ControlPlatformClientOptions options,
	IControlPlatformEndpointReader& endpointReader, storage::CStorageSnapshotCache& cache)
	: m_options(std::move(options))
	, m_endpointReader(endpointReader)
	, m_cache(cache)
	, m_minimumGeneration(m_options.minimumGeneration)
{
	if (!m_options.now) m_options.now = [] { return std::chrono::steady_clock::now(); };
	if (!m_options.channelFactory) {
		m_options.channelFactory = [] { return std::make_unique<CControlPlatformNamedPipeChannel>(); };
	}
	// A copied cache is never authoritative until this client completes Hello and
	// one full generation-pinned snapshot.
	InvalidateCache();
}

CControlPlatformClient::~CControlPlatformClient()
{
	Stop();
}

ControlPlatformClientResult CControlPlatformClient::EnsureReady()
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopping || m_state == EControlPlatformClientState::Stopped || m_state == EControlPlatformClientState::Stopping) {
			return Result(EControlPlatformClientOutcome::Stopped, kNoRetry, EControlIpcTerminalStatus::Cancelled);
		}
		if (m_state == EControlPlatformClientState::Ready) {
			return Result(EControlPlatformClientOutcome::AlreadyReady, kNoRetry, EControlIpcTerminalStatus::Succeeded);
		}
		if (m_attemptInFlight || m_mutationInFlight) {
			return Result(EControlPlatformClientOutcome::ConnectionInFlight, EControlPlatformRetryClass::ReadOnlyRetry,
				EControlIpcTerminalStatus::DeadlineExceeded);
		}
		if (m_state == EControlPlatformClientState::Unavailable) {
			return Result(EControlPlatformClientOutcome::Unavailable, kNoRetry, EControlIpcTerminalStatus::InternalError);
		}
		if (!profiles::IsCanonicalProfileAuthorityId(m_options.profileId) || m_options.profileHash.empty() || m_options.exchangeDeadline.count() <= 0 ||
			m_options.retryBaseDelay.count() <= 0 || m_options.retryMaximumDelay < m_options.retryBaseDelay) {
			m_state = EControlPlatformClientState::Unavailable;
			return Result(EControlPlatformClientOutcome::Unavailable, kNoRetry, EControlIpcTerminalStatus::InvalidRequest,
				L"invalid control-platform client options");
		}
		m_attemptInFlight = true;
	}
	std::chrono::steady_clock::time_point now;
	try {
		now = Now();
	}
	catch (...) {
		return FinishUnavailable(EControlIpcTerminalStatus::InternalError, L"control-platform clock failed");
	}
	bool interruptedBeforeDiscovery = false;
	{
		std::scoped_lock lock(m_mutex);
		interruptedBeforeDiscovery = m_stopping || m_resnapshotRequested;
		if (!interruptedBeforeDiscovery && m_nextRetry && now < *m_nextRetry) {
			m_attemptInFlight = false;
			return Result(EControlPlatformClientOutcome::RetryScheduled, EControlPlatformRetryClass::ReadOnlyRetry,
				EControlIpcTerminalStatus::DeadlineExceeded);
		}
		if (!interruptedBeforeDiscovery) m_state = EControlPlatformClientState::Discovering;
	}
	if (interruptedBeforeDiscovery) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
			L"control-platform bootstrap was cancelled before discovery", 0);
	}
	try {
		auto created = m_options.channelFactory();
		if (!created) return FinishUnavailable(EControlIpcTerminalStatus::InternalError, L"control-platform channel factory returned null");
		channel = std::shared_ptr<IControlPlatformClientChannel>(std::move(created));
		bool interrupted = false;
		{
			std::scoped_lock lock(m_mutex);
			interrupted = m_stopping || m_resnapshotRequested;
			if (!interrupted) m_activeChannel = channel;
		}
		if (interrupted) {
			channel->Close();
			return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
				L"control-platform bootstrap was cancelled before channel activation", 0);
		}
		return RunAttempt(std::move(channel));
	}
	catch (...) {
		return FinishUnavailable(EControlIpcTerminalStatus::InternalError,
			L"control-platform bootstrap raised an unexpected exception");
	}
}

void CControlPlatformClient::Stop() noexcept
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopping || m_state == EControlPlatformClientState::Stopped) return;
		m_stopping = true;
		m_state = EControlPlatformClientState::Stopping;
		m_resnapshotRequested = false;
		m_nextRetry.reset();
		channel = m_activeChannel;
	}
	if (channel) channel->Close();
	{
		std::scoped_lock lock(m_mutex);
		m_activeChannel.reset();
		m_attemptInFlight = false;
		m_pinnedGeneration = 0;
		try {
			InvalidateCache();
		}
		catch (...) {
			// Stop is noexcept and remains the terminal ownership boundary even
			// when the cache lock cannot be acquired.
		}
		m_state = EControlPlatformClientState::Stopped;
	}
}

ControlPlatformClientResult CControlPlatformClient::RequireResnapshot(std::uint64_t observedGeneration)
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	std::chrono::steady_clock::time_point now;
	try {
		now = Now();
	}
	catch (...) {
		return FinishUnavailable(EControlIpcTerminalStatus::InternalError, L"control-platform clock failed");
	}
	EControlPlatformClientOutcome outcome = EControlPlatformClientOutcome::ReconnectRequired;
	EControlPlatformRetryClass retryClass = EControlPlatformRetryClass::ReadOnlyRetry;
	bool cacheInvalidationFailed = false;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopping || m_state == EControlPlatformClientState::Stopped || m_state == EControlPlatformClientState::Stopping) {
			return Result(EControlPlatformClientOutcome::Stopped, kNoRetry, EControlIpcTerminalStatus::Cancelled);
		}
		try {
			InvalidateCache();
		}
		catch (...) {
			m_resnapshotRequested = false;
			m_attemptInFlight = false;
			m_pinnedGeneration = 0;
			m_state = EControlPlatformClientState::Unavailable;
			m_nextRetry.reset();
			outcome = EControlPlatformClientOutcome::Unavailable;
			retryClass = kNoRetry;
			channel = std::move(m_activeChannel);
			cacheInvalidationFailed = true;
		}
		if (!cacheInvalidationFailed) {
			m_pinnedGeneration = 0;
			m_minimumGeneration = std::max(m_minimumGeneration, observedGeneration);
			m_resnapshotRequested = m_attemptInFlight || m_mutationInFlight;
			channel = m_activeChannel;
			if (m_retryAttempts >= m_options.maximumRetryAttempts) {
				m_nextRetry.reset();
				m_state = EControlPlatformClientState::Unavailable;
				outcome = EControlPlatformClientOutcome::Unavailable;
				retryClass = kNoRetry;
			} else {
				++m_retryAttempts;
				m_nextRetry = now + RetryDelayFor(m_retryAttempts);
				m_state = EControlPlatformClientState::ReconnectRequired;
			}
		}
	}
	if (channel) channel->Close();
	return Result(outcome, retryClass, cacheInvalidationFailed ? EControlIpcTerminalStatus::InternalError
		: EControlIpcTerminalStatus::GenerationMismatch,
		cacheInvalidationFailed ? L"control-platform cache invalidation failed" : L"");
}

ControlPlatformMutationResult CControlPlatformClient::Apply(const storage::StorageMutationRequest& request)
{
	if (!IsMutationRequestReplaySafe(request)) {
		return MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
			L"invalid control-platform storage mutation request");
	}

	std::uint64_t generation = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopping || m_state == EControlPlatformClientState::Stopped || m_state == EControlPlatformClientState::Stopping) {
			return MutationResult(EControlPlatformMutationOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		}
		if (m_attemptInFlight || m_mutationInFlight) {
			return MutationResult(EControlPlatformMutationOutcome::OperationInFlight, EControlIpcTerminalStatus::DeadlineExceeded,
				L"another control-platform operation is in flight");
		}
		if (m_state != EControlPlatformClientState::Ready || m_pinnedGeneration == 0 ||
			m_cache.GetGeneration() != m_pinnedGeneration) {
			return MutationResult(EControlPlatformMutationOutcome::NotReady, EControlIpcTerminalStatus::ServerStopping,
				L"control-platform cache is not ready");
		}
		generation = m_pinnedGeneration;
		m_mutationInFlight = true;
	}

	std::shared_ptr<IControlPlatformClientChannel> channel;
	const auto finish = [this, &channel](ControlPlatformMutationResult result) {
		bool stopped = false;
		bool resnapshotRequested = false;
		{
			std::scoped_lock lock(m_mutex);
			stopped = m_stopping || m_state == EControlPlatformClientState::Stopped ||
				m_state == EControlPlatformClientState::Stopping;
			resnapshotRequested = !stopped && (m_resnapshotRequested ||
				m_state != EControlPlatformClientState::Ready || m_pinnedGeneration == 0);
			if (m_activeChannel == channel) m_activeChannel.reset();
			m_mutationInFlight = false;
		}
		if (channel) channel->Close();
		if (stopped) return MutationResult(EControlPlatformMutationOutcome::Stopped,
			EControlIpcTerminalStatus::Cancelled);
		if (resnapshotRequested && result.outcome != EControlPlatformMutationOutcome::Stopped &&
			result.outcome != EControlPlatformMutationOutcome::Failed &&
			result.outcome != EControlPlatformMutationOutcome::RetryWithSameOperationId) {
			result.outcome = EControlPlatformMutationOutcome::ResnapshotRequired;
		}
		return result;
	};

	try {
		ControlPlatformEndpointReadRequirements requirements;
		requirements.minimumGeneration = generation;
		const auto discovered = m_endpointReader.ReadDetailed(requirements);
		if (!discovered.IsDiscovered()) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ServerStopping,
				discovered.diagnostic, discovered.disposition));
		}
		const auto& endpoint = *discovered.snapshot;
		if (!profiles::IsCanonicalProfileAuthorityId(endpoint.profileId) || endpoint.profileId != m_options.profileId) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProfileMismatch,
				L"control-platform endpoint profile mismatch"));
		}
		const auto disposition = CControlPlatformEndpoint::ClassifySnapshot(endpoint, m_options.profileHash, requirements);
		if (disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
				L"control-platform endpoint snapshot is not usable", disposition));
		}
		if (endpoint.generation != generation) {
			return finish(MutationResult(EControlPlatformMutationOutcome::ResnapshotRequired,
				EControlIpcTerminalStatus::GenerationMismatch, L"control-platform generation changed before mutation"));
		}

		auto created = m_options.channelFactory();
		if (!created) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InternalError,
				L"control-platform channel factory returned null"));
		}
		channel = std::shared_ptr<IControlPlatformClientChannel>(std::move(created));
		bool interruptedBeforeConnect = false;
		bool stoppingBeforeConnect = false;
		{
			std::scoped_lock lock(m_mutex);
			interruptedBeforeConnect = m_stopping || m_resnapshotRequested || m_state != EControlPlatformClientState::Ready ||
				m_pinnedGeneration != generation;
			stoppingBeforeConnect = m_stopping;
			if (!interruptedBeforeConnect) m_activeChannel = channel;
		}
		if (interruptedBeforeConnect) {
			return finish(MutationResult(stoppingBeforeConnect ? EControlPlatformMutationOutcome::Stopped :
				EControlPlatformMutationOutcome::ResnapshotRequired,
				stoppingBeforeConnect ? EControlIpcTerminalStatus::Cancelled : EControlIpcTerminalStatus::GenerationMismatch));
		}

		const auto connected = channel->Connect(endpoint, m_options.exchangeDeadline);
		if (!connected.success) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::DeadlineExceeded,
				connected.diagnostic, EControlPlatformEndpointDiscoveryDisposition::Discovered, connected.reason));
		}

		const auto validateResponse = [&finish](const ControlIpcFrame& requestFrame,
			const std::vector<ControlIpcFrame>& responses, EControlIpcKind expectedKind,
			std::uint64_t expectedGeneration, ControlIpcFrame& response) -> std::optional<ControlPlatformMutationResult> {
			if (responses.size() != 1) {
				return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
					L"control-platform exchange returned other than one terminal response"));
			}
			const auto& candidate = responses.front();
			if (!IsExactTerminalResponse(candidate) || candidate.header.requestId != requestFrame.header.requestId ||
				candidate.header.majorVersion != kControlIpcMajorVersion || candidate.header.generation == 0) {
				return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
					L"malformed control-platform terminal response"));
			}
			if (candidate.header.kind == EControlIpcKind::Error) {
				const auto error = DecodeControlIpcError(candidate.payload);
				if (!error) {
					return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
						L"malformed control-platform error response"));
				}
				const auto outcome = error->status == EControlIpcTerminalStatus::GenerationMismatch
					? EControlPlatformMutationOutcome::ResnapshotRequired : EControlPlatformMutationOutcome::Failed;
				return finish(MutationResult(outcome, error->status, L"control-platform returned terminal error"));
			}
			if (candidate.header.kind != expectedKind) {
				return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
					L"unexpected control-platform response"));
			}
			if (candidate.header.generation != expectedGeneration) {
				return finish(MutationResult(EControlPlatformMutationOutcome::ResnapshotRequired,
					EControlIpcTerminalStatus::GenerationMismatch, L"control-platform response generation mismatch"));
			}
			response = candidate;
			return std::nullopt;
		};

		const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
		const auto helloId = NextRequestId();
		if (!helloPayload || !helloId) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
				L"invalid mutation hello request identity"));
		}
		const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
			EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
		std::vector<ControlIpcFrame> helloResponses;
		const auto helloExchange = channel->Exchange(hello, helloResponses, m_options.exchangeDeadline);
		if (!helloExchange.success) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::DeadlineExceeded,
				helloExchange.diagnostic, EControlPlatformEndpointDiscoveryDisposition::Discovered, helloExchange.reason));
		}
		ControlIpcFrame helloAck;
		if (const auto invalid = validateResponse(hello, helloResponses, EControlIpcKind::HelloAck, generation, helloAck)) return *invalid;
		const auto acknowledgedProfile = DecodeControlStorageHello(helloAck.payload);
		if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProfileMismatch,
				L"control-platform mutation hello profile mismatch"));
		}

		const auto applyPayload = EncodeControlStorageApplyRequest(request);
		const auto applyId = NextRequestId();
		if (!applyPayload || !applyId) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
				L"invalid storage apply request"));
		}
		const ControlIpcFrame apply{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageApplyRequest,
			EControlIpcFlags::Request, *applyId, generation }, *applyPayload };
		std::vector<ControlIpcFrame> applyResponses;
		const auto applyExchange = channel->Exchange(apply, applyResponses, m_options.exchangeDeadline);
		if (!applyExchange.success) {
			return finish(MutationResult(EControlPlatformMutationOutcome::RetryWithSameOperationId,
				EControlIpcTerminalStatus::DeadlineExceeded, applyExchange.diagnostic,
				EControlPlatformEndpointDiscoveryDisposition::Discovered, applyExchange.reason));
		}
		ControlIpcFrame applyResponse;
		if (const auto invalid = validateResponse(apply, applyResponses, EControlIpcKind::StorageApplyResponse, generation, applyResponse)) return *invalid;
		const auto storageResult = DecodeControlStorageApplyResponse(applyResponse.payload);
		if (!storageResult) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
				L"malformed storage apply response"));
		}

		if (storageResult->status == storage::EStorageMutationStatus::Conflict) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Conflict, EControlIpcTerminalStatus::Succeeded,
				L"control-platform storage revision conflict", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				EControlIpcTransportDisconnectReason::None, *storageResult));
		}
		if (storageResult->status == storage::EStorageMutationStatus::Failed) {
			return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::Succeeded,
				L"control-platform storage mutation failed", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				EControlIpcTransportDisconnectReason::None, *storageResult));
		}

		bool cacheGap = false;
		bool fencedBeforePublication = false;
		bool stoppedBeforePublication = false;
		{
			std::scoped_lock lock(m_mutex);
			if (m_stopping || m_resnapshotRequested || m_state != EControlPlatformClientState::Ready ||
				m_pinnedGeneration != generation) {
				fencedBeforePublication = true;
				stoppedBeforePublication = m_stopping;
			}
			else if (storageResult->status == storage::EStorageMutationStatus::Succeeded) {
				if (!storageResult->changeBatch) cacheGap = true;
				else {
					const auto cacheResult = m_cache.Apply(*storageResult->changeBatch);
					cacheGap = cacheResult == storage::EStorageChangeApplyStatus::ResyncRequired ||
						(cacheResult == storage::EStorageChangeApplyStatus::IgnoredStale &&
							!m_cache.Matches(generation, storageResult->revision));
				}
			}
			else if (!m_cache.Matches(generation, storageResult->revision)) {
				cacheGap = true;
			}
		}
		if (fencedBeforePublication) {
			return finish(MutationResult(stoppedBeforePublication ? EControlPlatformMutationOutcome::Stopped :
				EControlPlatformMutationOutcome::ResnapshotRequired,
				stoppedBeforePublication ? EControlIpcTerminalStatus::Cancelled : EControlIpcTerminalStatus::GenerationMismatch,
				L"control-platform mutation was fenced before cache publication",
				EControlPlatformEndpointDiscoveryDisposition::Discovered,
				EControlIpcTransportDisconnectReason::None, *storageResult));
		}
		if (cacheGap) {
			return finish(MutationResult(EControlPlatformMutationOutcome::ResnapshotRequired,
				EControlIpcTerminalStatus::Succeeded, L"storage mutation committed but cache requires a full snapshot",
				EControlPlatformEndpointDiscoveryDisposition::Discovered, EControlIpcTransportDisconnectReason::None, *storageResult));
		}
		return finish(MutationResult(storageResult->status == storage::EStorageMutationStatus::Succeeded
			? EControlPlatformMutationOutcome::Succeeded : EControlPlatformMutationOutcome::NotApplicable,
			EControlIpcTerminalStatus::Succeeded, {}, EControlPlatformEndpointDiscoveryDisposition::Discovered,
			EControlIpcTransportDisconnectReason::None, *storageResult));
	}
	catch (...) {
		return finish(MutationResult(EControlPlatformMutationOutcome::Failed, EControlIpcTerminalStatus::InternalError,
			L"control-platform storage mutation raised an unexpected exception"));
	}
}

ControlPlatformProfileResult CControlPlatformClient::ExecuteProfile(const ControlProfileRpcRequest& request)
{
	const bool mutation = IsProfileMutation(request.operation);
	if (!IsValidProfileMutationId(request)) {
		return ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
			L"invalid control-platform profile mutation request");
	}

	std::uint64_t generation = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopping || m_state == EControlPlatformClientState::Stopped || m_state == EControlPlatformClientState::Stopping) {
			return ProfileResult(EControlPlatformProfileOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		}
		if (m_attemptInFlight || m_mutationInFlight) {
			return ProfileResult(EControlPlatformProfileOutcome::OperationInFlight, EControlIpcTerminalStatus::DeadlineExceeded,
				L"another control-platform operation is in flight");
		}
		if (m_state != EControlPlatformClientState::Ready || m_pinnedGeneration == 0 ||
			m_cache.GetGeneration() != m_pinnedGeneration) {
			return ProfileResult(EControlPlatformProfileOutcome::NotReady, EControlIpcTerminalStatus::ServerStopping,
				L"control-platform cache is not ready");
		}
		generation = m_pinnedGeneration;
		m_mutationInFlight = true;
	}

	std::shared_ptr<IControlPlatformClientChannel> channel;
	const auto finish = [this, &channel](ControlPlatformProfileResult result) {
		bool stopped = false;
		bool resnapshotRequested = false;
		{
			std::scoped_lock lock(m_mutex);
			stopped = m_stopping || m_state == EControlPlatformClientState::Stopped ||
				m_state == EControlPlatformClientState::Stopping;
			resnapshotRequested = !stopped && (m_resnapshotRequested || m_state != EControlPlatformClientState::Ready ||
				m_pinnedGeneration == 0);
			if (m_activeChannel == channel) m_activeChannel.reset();
			m_mutationInFlight = false;
		}
		if (channel) channel->Close();
		if (stopped) return ProfileResult(EControlPlatformProfileOutcome::Stopped, EControlIpcTerminalStatus::Cancelled);
		if (resnapshotRequested && result.outcome != EControlPlatformProfileOutcome::RetryWithSameOperationId &&
			result.outcome != EControlPlatformProfileOutcome::Failed && result.outcome != EControlPlatformProfileOutcome::Stopped) {
			result.outcome = EControlPlatformProfileOutcome::ResnapshotRequired;
		}
		return result;
	};

	const auto failureForTransport = [&finish, mutation](EControlIpcTransportDisconnectReason reason,
		std::wstring diagnostic, bool profileRequestDispatched) {
		return finish(ProfileResult(mutation && profileRequestDispatched
			? EControlPlatformProfileOutcome::RetryWithSameOperationId : EControlPlatformProfileOutcome::Failed,
			reason == EControlIpcTransportDisconnectReason::DeadlineExceeded
				? EControlIpcTerminalStatus::DeadlineExceeded : EControlIpcTerminalStatus::InternalError,
			std::move(diagnostic), EControlPlatformEndpointDiscoveryDisposition::Discovered, reason));
	};

	try {
		ControlPlatformEndpointReadRequirements requirements;
		requirements.minimumGeneration = generation;
		const auto discovered = m_endpointReader.ReadDetailed(requirements);
		if (!discovered.IsDiscovered()) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ServerStopping,
				L"control-platform endpoint is not available", discovered.disposition));
		}
		const auto& endpoint = *discovered.snapshot;
		if (!profiles::IsCanonicalProfileAuthorityId(endpoint.profileId) || endpoint.profileId != m_options.profileId) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProfileMismatch,
				L"control-platform endpoint profile mismatch"));
		}
		const auto disposition = CControlPlatformEndpoint::ClassifySnapshot(endpoint, m_options.profileHash, requirements);
		if (disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::InvalidRequest,
				L"control-platform endpoint snapshot is not usable", disposition));
		}
		if (endpoint.generation != generation) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::ResnapshotRequired,
				EControlIpcTerminalStatus::GenerationMismatch, L"control-platform generation changed before profile request"));
		}

		auto created = m_options.channelFactory();
		if (!created) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::InternalError,
			L"control-platform channel factory returned null"));
		channel = std::shared_ptr<IControlPlatformClientChannel>(std::move(created));
		bool interrupted = false;
		bool stopping = false;
		{
			std::scoped_lock lock(m_mutex);
			interrupted = m_stopping || m_resnapshotRequested || m_state != EControlPlatformClientState::Ready ||
				m_pinnedGeneration != generation;
			stopping = m_stopping;
			if (!interrupted) m_activeChannel = channel;
		}
		if (interrupted) return finish(ProfileResult(stopping ? EControlPlatformProfileOutcome::Stopped :
			EControlPlatformProfileOutcome::ResnapshotRequired,
			stopping ? EControlIpcTerminalStatus::Cancelled : EControlIpcTerminalStatus::GenerationMismatch));

		const auto connected = channel->Connect(endpoint, m_options.exchangeDeadline);
		if (!connected.success) return failureForTransport(connected.reason, L"control-platform profile connection failed", false);
		const auto checkInterrupted = [this, generation, &finish]() -> std::optional<ControlPlatformProfileResult> {
			bool stopping = false;
			bool cancelled = false;
			{
				std::scoped_lock lock(m_mutex);
				stopping = m_stopping || m_state == EControlPlatformClientState::Stopped ||
					m_state == EControlPlatformClientState::Stopping;
				cancelled = stopping || m_resnapshotRequested || m_state != EControlPlatformClientState::Ready ||
					m_pinnedGeneration != generation;
			}
			if (!cancelled) return std::nullopt;
			return finish(ProfileResult(stopping ? EControlPlatformProfileOutcome::Stopped :
				EControlPlatformProfileOutcome::ResnapshotRequired,
				stopping ? EControlIpcTerminalStatus::Cancelled : EControlIpcTerminalStatus::GenerationMismatch));
		};
		if (const auto cancelled = checkInterrupted()) return *cancelled;

		const auto validate = [&finish, generation](const ControlIpcFrame& sent, const std::vector<ControlIpcFrame>& responses,
			EControlIpcKind expected, ControlIpcFrame& response) -> std::optional<ControlPlatformProfileResult> {
			if (responses.size() != 1) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed,
				EControlIpcTerminalStatus::ProtocolError, L"control-platform exchange returned other than one terminal response"));
			const auto& candidate = responses.front();
			if (!IsExactTerminalResponse(candidate) || candidate.header.requestId != sent.header.requestId ||
				candidate.header.majorVersion != kControlIpcMajorVersion || candidate.header.minorVersion > kControlIpcMinorVersion ||
				candidate.header.generation == 0) {
				return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
					L"malformed control-platform terminal response"));
			}
			if (candidate.header.kind == EControlIpcKind::Error) {
				auto error = DecodeControlIpcError(candidate.payload);
				if (!error) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
					L"malformed control-platform error response"));
				return finish(ProfileResult(error->status == EControlIpcTerminalStatus::GenerationMismatch
					? EControlPlatformProfileOutcome::ResnapshotRequired : EControlPlatformProfileOutcome::Failed,
					error->status, L"control-platform returned terminal error"));
			}
			if (candidate.header.kind != expected) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed,
				EControlIpcTerminalStatus::ProtocolError, L"unexpected control-platform response"));
			if (candidate.header.generation != generation) return finish(ProfileResult(EControlPlatformProfileOutcome::ResnapshotRequired,
				EControlIpcTerminalStatus::GenerationMismatch, L"control-platform response generation mismatch"));
			response = candidate;
			return std::nullopt;
		};

		const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
		const auto helloId = NextRequestId();
		if (!helloPayload || !helloId) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed,
			EControlIpcTerminalStatus::InvalidRequest, L"invalid profile hello request"));
		const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
			EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
		std::vector<ControlIpcFrame> helloResponses;
		const auto helloExchange = channel->Exchange(hello, helloResponses, m_options.exchangeDeadline);
		if (!helloExchange.success) return failureForTransport(helloExchange.reason, L"control-platform profile hello failed", false);
		ControlIpcFrame helloAck;
		if (const auto invalid = validate(hello, helloResponses, EControlIpcKind::HelloAck, helloAck)) return *invalid;
		const auto acknowledgedProfile = DecodeControlStorageHello(helloAck.payload);
		if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProfileMismatch,
				L"control-platform profile hello mismatch"));
		}
		if (const auto cancelled = checkInterrupted()) return *cancelled;

		auto encoded = EncodeControlProfileRpcRequest(request);
		auto requestId = NextRequestId();
		auto fields = encoded ? EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload),
			std::move(*encoded) } }) : std::nullopt;
		if (!requestId || !fields) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed,
			EControlIpcTerminalStatus::InvalidRequest, L"invalid profile request"));
		const ControlIpcFrame profileRequest{ { kControlIpcMajorVersion, kControlIpcMinorVersion,
			EControlIpcKind::ProfileRequest, EControlIpcFlags::Request, *requestId, generation }, std::move(*fields) };
		std::vector<ControlIpcFrame> profileResponses;
		const auto exchange = channel->Exchange(profileRequest, profileResponses, m_options.exchangeDeadline);
		if (!exchange.success) return failureForTransport(exchange.reason, L"control-platform profile request transport loss", true);
		ControlIpcFrame profileResponse;
		if (const auto invalid = validate(profileRequest, profileResponses, EControlIpcKind::ProfileResponse, profileResponse)) return *invalid;
		auto decodedFields = DecodeControlIpcFields(profileResponse.payload);
		if (decodedFields.outcome != EControlIpcFieldDecodeOutcome::Decoded || decodedFields.fields.size() != 1 ||
			decodedFields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload)) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
				L"malformed control-platform profile response"));
		}
		auto decoded = DecodeControlProfileRpcResponse(decodedFields.fields.front().value);
		if (!decoded) return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::ProtocolError,
			L"malformed control-platform profile payload"));
		if (decoded->terminalStatus == EControlIpcTerminalStatus::GenerationMismatch) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::ResnapshotRequired, decoded->terminalStatus,
				L"control-platform profile generation mismatch", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				EControlIpcTransportDisconnectReason::None, std::move(decoded)));
		}
		if (decoded->result.status == profiles::ControlUserDataProfileRegistryStatus::PersistConflict) {
			return finish(ProfileResult(EControlPlatformProfileOutcome::Conflict, decoded->terminalStatus,
				L"control-platform profile revision conflict", EControlPlatformEndpointDiscoveryDisposition::Discovered,
				EControlIpcTransportDisconnectReason::None, std::move(decoded)));
		}
		const auto outcome = decoded->terminalStatus == EControlIpcTerminalStatus::Succeeded && decoded->result.Succeeded()
			? EControlPlatformProfileOutcome::Succeeded : EControlPlatformProfileOutcome::Failed;
		return finish(ProfileResult(outcome, decoded->terminalStatus,
			outcome == EControlPlatformProfileOutcome::Succeeded ? L"" : L"control-platform profile operation failed",
			EControlPlatformEndpointDiscoveryDisposition::Discovered, EControlIpcTransportDisconnectReason::None, std::move(decoded)));
	}
	catch (...) {
		return finish(ProfileResult(EControlPlatformProfileOutcome::Failed, EControlIpcTerminalStatus::InternalError,
			L"control-platform profile operation raised an unexpected exception"));
	}
}

EControlPlatformClientState CControlPlatformClient::GetState() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_state;
}

std::uint64_t CControlPlatformClient::GetPinnedGeneration() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_pinnedGeneration;
}

std::uint32_t CControlPlatformClient::GetRetryAttemptCount() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_retryAttempts;
}

std::optional<std::chrono::steady_clock::time_point> CControlPlatformClient::GetNextRetryTime() const noexcept
{
	std::scoped_lock lock(m_mutex);
	return m_nextRetry;
}

EControlPlatformRetryClass CControlPlatformClient::ClassifyMutationResult(
	const storage::StorageMutationRequest& request, const storage::StorageMutationResult& result) noexcept
{
	if (!IsMutationRequestReplaySafe(request)) return kNoRetry;
	if (result.status == storage::EStorageMutationStatus::Conflict) return EControlPlatformRetryClass::MutationConflictDoNotRetry;
	return kNoRetry;
}

EControlPlatformRetryClass CControlPlatformClient::ClassifyAmbiguousMutationTransportFailure(
	const storage::StorageMutationRequest& request) noexcept
{
	return IsMutationRequestReplaySafe(request)
		? EControlPlatformRetryClass::MutationRetryWithSameOperationId
		: kNoRetry;
}

bool CControlPlatformClient::IsMutationRequestReplaySafe(const storage::StorageMutationRequest& request) noexcept
{
	if (request.operationId.empty() || request.operationId.size() > storage::kMaximumStorageOperationIdBytes
		|| !storage::IsValidStorageUtf8(request.operationId, false) || request.mutations.empty()
		|| request.mutations.size() > storage::kMaximumStorageItems) {
		return false;
	}
	std::set<storage::StorageAddress> addresses;
	std::size_t encodedBytes = sizeof(std::uint32_t) * 2 + request.operationId.size()
		+ (request.expectedRevision ? sizeof(std::uint64_t) : 0);
	for (const auto& mutation : request.mutations) {
		if (!mutation.address.IsValid() || !storage::IsValidStorageTarget(mutation.target)
			|| !addresses.emplace(mutation.address).second
			|| (mutation.value && (mutation.value->size() > storage::kMaximumStorageStringBytes
				|| !storage::IsValidStorageUtf8(*mutation.value)))) {
			return false;
		}
		const std::size_t stringBytes = mutation.address.scopeId.size() + mutation.address.owner.size()
			+ mutation.address.key.size() + (mutation.value ? mutation.value->size() : 0);
		if (!CheckedAdd(encodedBytes, kEncodedMutationFixedBytes, storage::kMaximumStorageMutationPayloadBytes)
			|| !CheckedAdd(encodedBytes, stringBytes, storage::kMaximumStorageMutationPayloadBytes)) {
			return false;
		}
	}
	return true;
}

ControlPlatformClientResult CControlPlatformClient::RunAttempt(std::shared_ptr<IControlPlatformClientChannel> channel)
{
	const auto transitionOrAbort = [this](EControlPlatformClientState state,
		std::uint64_t minimumGeneration = 0) -> std::optional<ControlPlatformClientResult> {
		bool interrupted = false;
		{
			std::scoped_lock lock(m_mutex);
			interrupted = m_stopping || m_resnapshotRequested;
			if (!interrupted) m_state = state;
		}
		if (!interrupted) return std::nullopt;
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
			L"control-platform bootstrap was cancelled by shutdown or resynchronization", minimumGeneration);
	};

	ControlPlatformEndpointReadRequirements requirements;
	{
		std::scoped_lock lock(m_mutex);
		requirements.minimumGeneration = m_minimumGeneration;
	}
	const auto discovered = m_endpointReader.ReadDetailed(requirements);
	if (!discovered.IsDiscovered()) {
		if (IsTransientDiscoveryDisposition(discovered.disposition)) {
			return FinishTransientFailure(EControlIpcTerminalStatus::ServerStopping, discovered.diagnostic,
				discovered.disposition);
		}
		return FinishUnavailable(EControlIpcTerminalStatus::InvalidRequest, discovered.diagnostic,
			discovered.disposition);
	}
	const auto& endpoint = discovered.snapshot;
	//! Do not connect to an endpoint that advertises another immutable profile,
	//! even if a caller accidentally supplied a matching pipe hash.
	if (!profiles::IsCanonicalProfileAuthorityId(endpoint->profileId) || endpoint->profileId != m_options.profileId) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProfileMismatch,
			L"control-platform endpoint profile mismatch");
	}
	const auto endpointDisposition = CControlPlatformEndpoint::ClassifySnapshot(*endpoint, m_options.profileHash, requirements);
	if (endpointDisposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) {
		if (IsTransientDiscoveryDisposition(endpointDisposition)) {
			return FinishTransientFailure(EControlIpcTerminalStatus::ServerStopping, L"Control IPC endpoint snapshot is not usable",
				endpointDisposition);
		}
		return FinishUnavailable(EControlIpcTerminalStatus::InvalidRequest, L"Control IPC endpoint snapshot is malformed",
			endpointDisposition);
	}
	if (auto interrupted = transitionOrAbort(EControlPlatformClientState::Connecting, endpoint->generation)) return *interrupted;
	const auto connected = channel->Connect(*endpoint, m_options.exchangeDeadline);
	if (!connected.success) {
		if (IsTransientTransportReason(connected.reason)) return FinishTransientFailure(EControlIpcTerminalStatus::DeadlineExceeded,
			connected.diagnostic.empty() ? L"control-platform connection failed" : connected.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, connected.reason);
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			connected.diagnostic.empty() ? L"control-platform connection was rejected" : connected.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, connected.reason);
	}
	if (auto interrupted = transitionOrAbort(EControlPlatformClientState::Hello, endpoint->generation)) return *interrupted;

	const auto helloPayload = EncodeControlStorageHello(m_options.profileId);
	const auto helloId = NextRequestId();
	if (!helloPayload || !helloId) return FinishUnavailable(EControlIpcTerminalStatus::InvalidRequest, L"invalid hello request identity");
	const ControlIpcFrame hello{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::Hello,
		EControlIpcFlags::Request, *helloId, 0 }, *helloPayload };
	std::vector<ControlIpcFrame> helloResponses;
	const auto helloExchange = channel->Exchange(hello, helloResponses, m_options.exchangeDeadline);
	if (!helloExchange.success) {
		if (IsTransientTransportReason(helloExchange.reason)) return FinishTransientFailure(EControlIpcTerminalStatus::DeadlineExceeded,
			helloExchange.diagnostic.empty() ? L"control-platform hello failed" : helloExchange.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, helloExchange.reason);
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			helloExchange.diagnostic.empty() ? L"control-platform hello exchange was rejected" : helloExchange.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, helloExchange.reason);
	}
	ControlIpcFrame helloAck;
	auto validated = ValidateSingleTerminalResponse(hello, helloResponses, EControlIpcKind::HelloAck, helloAck);
	if (validated.outcome != EControlPlatformClientOutcome::Ready) return validated;
	const auto acknowledgedProfile = DecodeControlStorageHello(helloAck.payload);
	if (!acknowledgedProfile || *acknowledgedProfile != m_options.profileId) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProfileMismatch, L"control-platform hello profile mismatch");
	}
	if (helloAck.header.generation == 0 || helloAck.header.generation != endpoint->generation) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch, L"control-platform generation changed during hello",
			std::max(helloAck.header.generation, endpoint->generation));
	}
	bool helloInterrupted = false;
	{
		std::scoped_lock lock(m_mutex);
		helloInterrupted = m_stopping || m_resnapshotRequested;
		if (!helloInterrupted) {
			m_pinnedGeneration = helloAck.header.generation;
			m_state = EControlPlatformClientState::Snapshotting;
		}
	}
	if (helloInterrupted) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
			L"control-platform bootstrap was cancelled after hello", helloAck.header.generation);
	}
	const auto snapshotId = NextRequestId();
	if (!snapshotId) return FinishUnavailable(EControlIpcTerminalStatus::InvalidRequest, L"control-platform request id overflow");
	const ControlIpcFrame snapshotRequest{ { kControlIpcMajorVersion, kControlIpcMinorVersion,
		EControlIpcKind::StorageSnapshotRequest, EControlIpcFlags::Request, *snapshotId, helloAck.header.generation }, {} };
	std::vector<ControlIpcFrame> snapshotResponses;
	const auto snapshotExchange = channel->Exchange(snapshotRequest, snapshotResponses, m_options.exchangeDeadline);
	if (!snapshotExchange.success) {
		if (IsTransientTransportReason(snapshotExchange.reason)) return FinishTransientFailure(EControlIpcTerminalStatus::DeadlineExceeded,
			snapshotExchange.diagnostic.empty() ? L"control-platform snapshot failed" : snapshotExchange.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, snapshotExchange.reason);
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			snapshotExchange.diagnostic.empty() ? L"control-platform snapshot exchange was rejected" : snapshotExchange.diagnostic,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, snapshotExchange.reason);
	}
	ControlIpcFrame snapshotResponse;
	validated = ValidateSingleTerminalResponse(snapshotRequest, snapshotResponses, EControlIpcKind::StorageSnapshotResponse, snapshotResponse);
	if (validated.outcome != EControlPlatformClientOutcome::Ready) return validated;
	const auto snapshot = DecodeControlStorageSnapshotResponse(snapshotResponse.payload);
	if (!snapshot) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			L"control-platform snapshot payload is malformed");
	}
	if (snapshot->generation != helloAck.header.generation) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch, L"control-platform snapshot generation mismatch",
			std::max(snapshot->generation, helloAck.header.generation));
	}
	if (!IsUsableSnapshot(*snapshot, helloAck.header.generation)) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			L"control-platform snapshot violates storage invariants");
	}
	bool snapshotInterrupted = false;
	bool cacheAccepted = false;
	{
		std::scoped_lock lock(m_mutex);
		snapshotInterrupted = m_stopping || m_resnapshotRequested;
		if (!snapshotInterrupted) {
			m_cache.Replace(*snapshot);
			cacheAccepted = m_cache.Matches(snapshot->generation, snapshot->revision);
			if (cacheAccepted) {
				m_retryAttempts = 0;
				m_nextRetry.reset();
				m_minimumGeneration = std::max(m_minimumGeneration, snapshot->generation);
				m_state = EControlPlatformClientState::Ready;
				m_attemptInFlight = false;
				m_activeChannel.reset();
			}
		}
	}
	if (snapshotInterrupted) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
			L"control-platform bootstrap was cancelled before snapshot publication", helloAck.header.generation);
	}
	if (!cacheAccepted) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
			L"control-platform snapshot was rejected by the local cache");
	}
	channel->Close();
	return Result(EControlPlatformClientOutcome::Ready, kNoRetry, EControlIpcTerminalStatus::Succeeded);
}

ControlPlatformClientResult CControlPlatformClient::FinishTransientFailure(EControlIpcTerminalStatus status, std::wstring diagnostic,
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition, EControlIpcTransportDisconnectReason transportReason)
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	EControlPlatformClientOutcome outcome = EControlPlatformClientOutcome::RetryScheduled;
	EControlPlatformRetryClass retryClass = EControlPlatformRetryClass::ReadOnlyRetry;
	EControlIpcTerminalStatus resultStatus = status;
	std::chrono::steady_clock::time_point now;
	bool clockAvailable = true;
	try {
		now = Now();
	}
	catch (...) {
		clockAvailable = false;
	}
	{
		std::scoped_lock lock(m_mutex);
		channel = std::move(m_activeChannel);
		m_attemptInFlight = false;
		m_pinnedGeneration = 0;
		bool cacheInvalidated = true;
		try {
			InvalidateCache();
		}
		catch (...) {
			cacheInvalidated = false;
		}
		if (!cacheInvalidated) {
			m_resnapshotRequested = false;
			m_state = EControlPlatformClientState::Unavailable;
			m_nextRetry.reset();
			outcome = EControlPlatformClientOutcome::Unavailable;
			retryClass = kNoRetry;
			resultStatus = EControlIpcTerminalStatus::InternalError;
		} else if (m_stopping) {
			m_state = EControlPlatformClientState::Stopped;
			m_resnapshotRequested = false;
			m_nextRetry.reset();
			outcome = EControlPlatformClientOutcome::Stopped;
			retryClass = kNoRetry;
			resultStatus = EControlIpcTerminalStatus::Cancelled;
		} else if (m_resnapshotRequested) {
			m_resnapshotRequested = false;
			m_state = EControlPlatformClientState::ReconnectRequired;
			outcome = EControlPlatformClientOutcome::ReconnectRequired;
			resultStatus = EControlIpcTerminalStatus::GenerationMismatch;
		} else if (!clockAvailable) {
			m_state = EControlPlatformClientState::Unavailable;
			m_nextRetry.reset();
			outcome = EControlPlatformClientOutcome::Unavailable;
			retryClass = kNoRetry;
			resultStatus = EControlIpcTerminalStatus::InternalError;
		} else if (m_retryAttempts >= m_options.maximumRetryAttempts) {
			m_state = EControlPlatformClientState::Unavailable;
			m_nextRetry.reset();
			outcome = EControlPlatformClientOutcome::Unavailable;
			retryClass = kNoRetry;
		} else {
			++m_retryAttempts;
			m_nextRetry = now + RetryDelayFor(m_retryAttempts);
			m_state = EControlPlatformClientState::RetryScheduled;
		}
	}
	if (channel) channel->Close();
	return Result(outcome, retryClass, resultStatus, std::move(diagnostic), discoveryDisposition, transportReason);
}

ControlPlatformClientResult CControlPlatformClient::FinishUnavailable(EControlIpcTerminalStatus status, std::wstring diagnostic,
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition, EControlIpcTransportDisconnectReason transportReason)
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	bool stopped = false;
	{
		std::scoped_lock lock(m_mutex);
		channel = std::move(m_activeChannel);
		m_attemptInFlight = false;
		m_resnapshotRequested = false;
		m_nextRetry.reset();
		m_pinnedGeneration = 0;
		try {
			InvalidateCache();
		}
		catch (...) {
			// The transport has already been fenced; preserve a terminal state.
		}
		m_state = m_stopping ? EControlPlatformClientState::Stopped : EControlPlatformClientState::Unavailable;
		stopped = m_stopping;
	}
	if (channel) channel->Close();
	return Result(stopped ? EControlPlatformClientOutcome::Stopped : EControlPlatformClientOutcome::Unavailable, kNoRetry,
		stopped ? EControlIpcTerminalStatus::Cancelled : status, std::move(diagnostic), discoveryDisposition, transportReason);
}

ControlPlatformClientResult CControlPlatformClient::FinishReconnectRequired(EControlIpcTerminalStatus status, std::wstring diagnostic,
	std::uint64_t minimumGeneration, EControlPlatformEndpointDiscoveryDisposition discoveryDisposition,
	EControlIpcTransportDisconnectReason transportReason)
{
	std::shared_ptr<IControlPlatformClientChannel> channel;
	bool stopped = false;
	bool clockAvailable = true;
	std::chrono::steady_clock::time_point now;
	try {
		now = Now();
	}
	catch (...) {
		clockAvailable = false;
	}
	EControlPlatformClientOutcome outcome = EControlPlatformClientOutcome::ReconnectRequired;
	EControlPlatformRetryClass retryClass = EControlPlatformRetryClass::ReadOnlyRetry;
	EControlIpcTerminalStatus resultStatus = status;
	{
		std::scoped_lock lock(m_mutex);
		channel = std::move(m_activeChannel);
		m_attemptInFlight = false;
		if (m_stopping) {
			m_state = EControlPlatformClientState::Stopped;
			m_nextRetry.reset();
			stopped = true;
		} else {
			bool cacheInvalidated = true;
			try {
				InvalidateCache();
			}
			catch (...) {
				cacheInvalidated = false;
			}
			m_pinnedGeneration = 0;
			m_minimumGeneration = std::max(m_minimumGeneration, minimumGeneration);
			const bool retryAlreadyScheduled = m_resnapshotRequested && m_nextRetry.has_value();
			m_resnapshotRequested = false;
			if (!cacheInvalidated) {
				m_nextRetry.reset();
				m_state = EControlPlatformClientState::Unavailable;
				outcome = EControlPlatformClientOutcome::Unavailable;
				retryClass = kNoRetry;
				resultStatus = EControlIpcTerminalStatus::InternalError;
			} else if (retryAlreadyScheduled) {
				m_state = EControlPlatformClientState::ReconnectRequired;
			} else if (!clockAvailable || m_retryAttempts >= m_options.maximumRetryAttempts) {
				m_nextRetry.reset();
				m_state = EControlPlatformClientState::Unavailable;
				outcome = EControlPlatformClientOutcome::Unavailable;
				retryClass = kNoRetry;
				if (!clockAvailable) resultStatus = EControlIpcTerminalStatus::InternalError;
			} else {
				++m_retryAttempts;
				m_nextRetry = now + RetryDelayFor(m_retryAttempts);
				m_state = EControlPlatformClientState::ReconnectRequired;
			}
		}
	}
	if (channel) channel->Close();
	return Result(stopped ? EControlPlatformClientOutcome::Stopped : outcome,
		stopped ? kNoRetry : retryClass,
		stopped ? EControlIpcTerminalStatus::Cancelled : resultStatus, std::move(diagnostic), discoveryDisposition, transportReason);
}

ControlPlatformClientResult CControlPlatformClient::ValidateSingleTerminalResponse(const ControlIpcFrame& request,
	const std::vector<ControlIpcFrame>& responses, EControlIpcKind expectedKind, ControlIpcFrame& response)
{
	if (responses.size() != 1) return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError,
		L"control-platform exchange returned other than one terminal response");
	const auto& candidate = responses.front();
	if (!IsExactTerminalResponse(candidate) || candidate.header.requestId != request.header.requestId ||
		candidate.header.majorVersion != kControlIpcMajorVersion || candidate.header.generation == 0) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError, L"malformed control-platform terminal response");
	}
	if (candidate.header.kind == EControlIpcKind::Error) {
		const auto error = DecodeControlIpcError(candidate.payload);
		if (!error) return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError, L"malformed control-platform error response");
		if (error->status == EControlIpcTerminalStatus::ProfileMismatch ||
			error->status == EControlIpcTerminalStatus::AccessDenied ||
			error->status == EControlIpcTerminalStatus::UnsupportedVersion ||
			error->status == EControlIpcTerminalStatus::InvalidRequest ||
			error->status == EControlIpcTerminalStatus::ProtocolError) {
			return FinishUnavailable(error->status, L"control-platform rejected immutable profile identity");
		}
		if (error->status == EControlIpcTerminalStatus::GenerationMismatch) {
			return FinishReconnectRequired(error->status, L"control-platform generation mismatch", candidate.header.generation);
		}
		return FinishTransientFailure(error->status, L"control-platform returned terminal error");
	}
	if (candidate.header.kind != expectedKind) {
		return FinishUnavailable(EControlIpcTerminalStatus::ProtocolError, L"unexpected control-platform response");
	}
	if (request.header.kind != EControlIpcKind::Hello && candidate.header.generation != request.header.generation) {
		return FinishReconnectRequired(EControlIpcTerminalStatus::GenerationMismatch,
			L"control-platform response generation mismatch", std::max(candidate.header.generation, request.header.generation));
	}
	response = candidate;
	return Result(EControlPlatformClientOutcome::Ready, kNoRetry, EControlIpcTerminalStatus::Succeeded);
}

bool CControlPlatformClient::IsTransientDiscoveryDisposition(
	EControlPlatformEndpointDiscoveryDisposition disposition) noexcept
{
	switch (disposition) {
	case EControlPlatformEndpointDiscoveryDisposition::NotPublished:
	case EControlPlatformEndpointDiscoveryDisposition::NotAccepting:
	case EControlPlatformEndpointDiscoveryDisposition::DeadOrStale:
	case EControlPlatformEndpointDiscoveryDisposition::Busy:
	case EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure:
		return true;
	case EControlPlatformEndpointDiscoveryDisposition::Discovered:
	case EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor:
	case EControlPlatformEndpointDiscoveryDisposition::Closed:
	case EControlPlatformEndpointDiscoveryDisposition::AccessDenied:
	case EControlPlatformEndpointDiscoveryDisposition::SecurityRejected:
	case EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi:
		return false;
	}
	return false;
}

bool CControlPlatformClient::IsTransientTransportReason(EControlIpcTransportDisconnectReason reason) noexcept
{
	switch (reason) {
	case EControlIpcTransportDisconnectReason::Stopped:
	case EControlIpcTransportDisconnectReason::PeerClosed:
	case EControlIpcTransportDisconnectReason::ConnectFailed:
	case EControlIpcTransportDisconnectReason::DeadlineExceeded:
	case EControlIpcTransportDisconnectReason::ResourceExhausted:
	case EControlIpcTransportDisconnectReason::IoError:
		return true;
	case EControlIpcTransportDisconnectReason::None:
	case EControlIpcTransportDisconnectReason::AccessDenied:
	case EControlIpcTransportDisconnectReason::ProtocolError:
	case EControlIpcTransportDisconnectReason::CallbackFailed:
		return false;
	}
	return false;
}

std::optional<std::uint64_t> CControlPlatformClient::NextRequestId() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_nextRequestId == 0) return std::nullopt;
	const auto id = m_nextRequestId;
	if (id == std::numeric_limits<std::uint64_t>::max()) m_nextRequestId = 0;
	else ++m_nextRequestId;
	return id;
}

std::chrono::steady_clock::time_point CControlPlatformClient::Now() const
{
	return m_options.now();
}

std::chrono::milliseconds CControlPlatformClient::RetryDelayFor(std::uint32_t attempt) const noexcept
{
	std::uint64_t delay = static_cast<std::uint64_t>(m_options.retryBaseDelay.count());
	const auto maximum = static_cast<std::uint64_t>(m_options.retryMaximumDelay.count());
	for (std::uint32_t index = 1; index < attempt && delay < maximum; ++index) {
		delay = std::min(maximum, delay > maximum / 2 ? maximum : delay * 2);
	}
	const auto jitterBound = std::max<std::uint64_t>(1, delay / 4);
	const auto jitter = (static_cast<std::uint64_t>(m_options.retryJitterSalt) + attempt * 1103515245ULL) % jitterBound;
	return std::chrono::milliseconds(std::min(maximum, delay > maximum - jitter ? maximum : delay + jitter));
}

void CControlPlatformClient::InvalidateCache()
{
	m_cache.Replace({});
}

} // namespace platform::controlipc
