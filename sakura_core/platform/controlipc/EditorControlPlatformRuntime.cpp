/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <algorithm>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr auto kMaximumStartupBudget = std::chrono::seconds(60);
constexpr auto kMaximumStorageCacheWait = std::chrono::seconds(60);

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

bool IsHardDiscoveryFailure(EControlPlatformEndpointDiscoveryDisposition disposition) noexcept
{
	switch (disposition) {
	case EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor:
	case EControlPlatformEndpointDiscoveryDisposition::Closed:
	case EControlPlatformEndpointDiscoveryDisposition::AccessDenied:
	case EControlPlatformEndpointDiscoveryDisposition::SecurityRejected:
	case EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi:
		return true;
	case EControlPlatformEndpointDiscoveryDisposition::Discovered:
	case EControlPlatformEndpointDiscoveryDisposition::NotPublished:
	case EControlPlatformEndpointDiscoveryDisposition::NotAccepting:
	case EControlPlatformEndpointDiscoveryDisposition::DeadOrStale:
	case EControlPlatformEndpointDiscoveryDisposition::Busy:
	case EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure:
		return false;
	}
	return true;
}

std::chrono::milliseconds DiscoveryRetryDelay(const ControlPlatformClientOptions& options, std::uint32_t attempt) noexcept
{
	std::uint64_t delay = static_cast<std::uint64_t>(options.retryBaseDelay.count());
	const auto maximum = static_cast<std::uint64_t>(options.retryMaximumDelay.count());
	for (std::uint32_t index = 1; index < attempt && delay < maximum; ++index) {
		delay = std::min(maximum, delay > maximum / 2 ? maximum : delay * 2);
	}
	const auto jitterBound = std::max<std::uint64_t>(1, delay / 4);
	const auto jitter = (static_cast<std::uint64_t>(options.retryJitterSalt) + attempt * 1103515245ULL) % jitterBound;
	return std::chrono::milliseconds(std::min(maximum, delay > maximum - jitter ? maximum : delay + jitter));
}

} // namespace

CEditorControlPlatformRuntime::CEditorControlPlatformRuntime(EditorControlPlatformRuntimeOptions options) :
	CEditorControlPlatformRuntime(std::move(options), {})
{
}

CEditorControlPlatformRuntime::CEditorControlPlatformRuntime(EditorControlPlatformRuntimeOptions options,
	EditorControlPlatformRuntimeDependencies dependencies) :
	m_options(std::move(options)),
	m_dependencies(std::move(dependencies))
{
}

CEditorControlPlatformRuntime::~CEditorControlPlatformRuntime()
{
	try {
		(void)Stop();
	}
	catch (...) {
		// Destruction cannot surface a shutdown exception.
	}
}

bool CEditorControlPlatformRuntime::HasValidOptions(std::wstring& diagnostic) const
{
	if (!IsResolvedPath(m_options.profileDirectory)) diagnostic = L"profileDirectory must be an absolute resolved path";
	else if (m_options.startupBudget <= std::chrono::milliseconds::zero() || m_options.startupBudget > kMaximumStartupBudget) {
		diagnostic = L"startupBudget is out of range";
	}
	else if (m_options.clientOptions.exchangeDeadline <= std::chrono::milliseconds::zero() ||
		m_options.clientOptions.retryBaseDelay <= std::chrono::milliseconds::zero() ||
		m_options.clientOptions.retryMaximumDelay < m_options.clientOptions.retryBaseDelay) {
		diagnostic = L"control-platform client retry limits are invalid";
	}
	else {
		return true;
	}
	return false;
}

EditorControlPlatformRuntimeResult CEditorControlPlatformRuntime::ResultLocked(
	EEditorControlPlatformRuntimeResultCode code, std::wstring diagnostic) const
{
	return {
		.code = code,
		.state = m_state,
		.identity = m_state == EEditorControlPlatformRuntimeState::Ready ||
			m_state == EEditorControlPlatformRuntimeState::DegradedUnavailable ? m_identity : std::nullopt,
		.clientResult = m_lastClientResult,
		.diagnostic = std::move(diagnostic),
	};
}

EditorControlPlatformRuntimeResult CEditorControlPlatformRuntime::StartupTerminalResultLocked() const
{
	switch (m_state) {
	case EEditorControlPlatformRuntimeState::Ready:
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::Ready);
	case EEditorControlPlatformRuntimeState::DegradedUnavailable:
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::DegradedUnavailable);
	case EEditorControlPlatformRuntimeState::Stopped:
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
	case EEditorControlPlatformRuntimeState::Stopping:
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
	case EEditorControlPlatformRuntimeState::Failed:
	case EEditorControlPlatformRuntimeState::Starting:
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure,
			m_state == EEditorControlPlatformRuntimeState::Starting ? L"control-platform startup budget expired" :
			L"control-platform startup failed");
	}
	return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure);
}

EditorControlStorageCacheCoordinateResult CEditorControlPlatformRuntime::StorageCacheCoordinatesLocked() const
{
	const auto unavailable = [this](EEditorControlStorageCacheCoordinateCode code, std::wstring diagnostic = {}) {
		return EditorControlStorageCacheCoordinateResult{ code, m_state, std::nullopt, std::move(diagnostic) };
	};

	switch (m_state) {
	case EEditorControlPlatformRuntimeState::Starting:
		return unavailable(EEditorControlStorageCacheCoordinateCode::Resynchronizing);
	case EEditorControlPlatformRuntimeState::DegradedUnavailable:
		return unavailable(EEditorControlStorageCacheCoordinateCode::DegradedUnavailable);
	case EEditorControlPlatformRuntimeState::Failed:
		return unavailable(EEditorControlStorageCacheCoordinateCode::Failed);
	case EEditorControlPlatformRuntimeState::Stopping:
		return unavailable(EEditorControlStorageCacheCoordinateCode::Stopping);
	case EEditorControlPlatformRuntimeState::Stopped:
		return unavailable(EEditorControlStorageCacheCoordinateCode::Stopped);
	case EEditorControlPlatformRuntimeState::Ready:
		break;
	}

	if (!m_identity || !m_client || !m_cache) {
		return unavailable(EEditorControlStorageCacheCoordinateCode::Failed,
			L"ready control-platform runtime has incomplete cache ownership");
	}

	const auto pinnedGeneration = m_client->GetPinnedGeneration();
	if (pinnedGeneration == 0) {
		return unavailable(EEditorControlStorageCacheCoordinateCode::Failed,
			L"ready control-platform runtime has no pinned generation");
	}

	// A concurrent Apply can advance only the global cache revision. Never return
	// a mixed generation/revision pair: retry the copied cache observation once,
	// otherwise let the persistence caller wait for the next stable publication.
	for (int attempt = 0; attempt != 2; ++attempt) {
		const auto cacheGeneration = m_cache->GetGeneration();
		const auto storageRevision = m_cache->GetRevision();
		if (cacheGeneration == pinnedGeneration && m_cache->Matches(cacheGeneration, storageRevision)) {
			return { EEditorControlStorageCacheCoordinateCode::Ready, m_state,
				EditorControlStorageCacheCoordinates{ m_identity->profileId, cacheGeneration, storageRevision }, {} };
		}
	}
	return unavailable(EEditorControlStorageCacheCoordinateCode::Resynchronizing,
		L"control-platform cache changed during coordinate observation");
}

bool CEditorControlPlatformRuntime::FreezeFirstIdentity()
{
	IControlPlatformEndpointReader* reader = nullptr;
	{
		std::scoped_lock lock(m_mutex);
		if ((m_state != EEditorControlPlatformRuntimeState::Starting &&
			m_state != EEditorControlPlatformRuntimeState::DegradedUnavailable) || !m_endpointReader) return false;
		BeginReaderCallLocked();
		reader = m_endpointReader.get();
	}
	ControlPlatformEndpointDiscoveryResult discovery;
	try {
		discovery = reader->ReadDetailed({ .minimumGeneration = 0, .requireLiveControlProcess = true });
	}
	catch (...) {
		std::scoped_lock lock(m_mutex);
		EndReaderCallLocked();
		m_state = EEditorControlPlatformRuntimeState::Failed;
		m_lastClientResult = ControlPlatformClientResult{ EControlPlatformClientOutcome::Unavailable,
			EControlPlatformRetryClass::None, EControlIpcTerminalStatus::InternalError,
			EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"endpoint discovery raised an exception" };
		m_condition.notify_all();
		return false;
	}

	if (!discovery.IsDiscovered()) {
		std::scoped_lock lock(m_mutex);
		EndReaderCallLocked();
		m_lastClientResult = ControlPlatformClientResult{ EControlPlatformClientOutcome::RetryScheduled,
			EControlPlatformRetryClass::ReadOnlyRetry, EControlIpcTerminalStatus::ServerStopping,
			discovery.disposition, EControlIpcTransportDisconnectReason::None, discovery.diagnostic };
		if (IsHardDiscoveryFailure(discovery.disposition)) m_state = EEditorControlPlatformRuntimeState::Failed;
		m_condition.notify_all();
		return false;
	}

	const auto& endpoint = *discovery.snapshot;
	const auto profileHash = ComputeCanonicalProfileHash(m_options.profileDirectory);
	const auto disposition = CControlPlatformEndpoint::ClassifySnapshot(endpoint, profileHash,
		{ .minimumGeneration = 0, .requireLiveControlProcess = true });
	if (disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered || endpoint.generation == 0 ||
		!profiles::IsCanonicalProfileAuthorityId(endpoint.profileId)) {
		std::scoped_lock lock(m_mutex);
		EndReaderCallLocked();
		m_state = EEditorControlPlatformRuntimeState::Failed;
		m_lastClientResult = ControlPlatformClientResult{ EControlPlatformClientOutcome::Unavailable,
			EControlPlatformRetryClass::None, EControlIpcTerminalStatus::InvalidRequest, disposition,
			EControlIpcTransportDisconnectReason::None, L"first control endpoint identity was invalid" };
		m_condition.notify_all();
		return false;
	}

	try {
		ControlPlatformClientOptions clientOptions = m_options.clientOptions;
		clientOptions.profileId = endpoint.profileId;
		clientOptions.profileHash = profileHash;
		clientOptions.minimumGeneration = endpoint.generation;
		auto cache = std::make_unique<storage::CStorageSnapshotCache>();
		auto client = std::make_unique<CControlPlatformClient>(std::move(clientOptions), *reader, *cache);
		std::scoped_lock lock(m_mutex);
		if (m_state != EEditorControlPlatformRuntimeState::Starting &&
			m_state != EEditorControlPlatformRuntimeState::DegradedUnavailable) {
			EndReaderCallLocked();
			return false;
		}
		m_identity = EditorControlPlatformRuntimeIdentity{ endpoint.profileId, profileHash, endpoint.generation };
		m_requestedResnapshotGeneration = endpoint.generation;
		m_cache = std::move(cache);
		m_client = std::move(client);
		EndReaderCallLocked();
		return true;
	}
	catch (...) {
		std::scoped_lock lock(m_mutex);
		EndReaderCallLocked();
		m_state = EEditorControlPlatformRuntimeState::Failed;
		m_lastClientResult = ControlPlatformClientResult{ EControlPlatformClientOutcome::Unavailable,
			EControlPlatformRetryClass::None, EControlIpcTerminalStatus::InternalError,
			EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"control-platform client creation failed" };
		m_condition.notify_all();
		return false;
	}
}

void CEditorControlPlatformRuntime::BeginReaderCallLocked()
{
	++m_activeReaderCalls;
}

void CEditorControlPlatformRuntime::EndReaderCallLocked() noexcept
{
	if (m_activeReaderCalls != 0) --m_activeReaderCalls;
	m_condition.notify_all();
}

void CEditorControlPlatformRuntime::BeginClientCallLocked()
{
	++m_activeClientCalls;
}

void CEditorControlPlatformRuntime::EndClientCallLocked() noexcept
{
	if (m_activeClientCalls != 0) --m_activeClientCalls;
	m_condition.notify_all();
}

EditorControlPlatformRuntimeResult CEditorControlPlatformRuntime::Start()
{
	std::unique_lock lock(m_mutex);
	if (m_state == EEditorControlPlatformRuntimeState::Ready) return ResultLocked(EEditorControlPlatformRuntimeResultCode::AlreadyReady);
	if (m_state == EEditorControlPlatformRuntimeState::DegradedUnavailable) return ResultLocked(EEditorControlPlatformRuntimeResultCode::DegradedUnavailable);
	if (m_state == EEditorControlPlatformRuntimeState::Failed) return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure);
	if (m_state == EEditorControlPlatformRuntimeState::Stopped) {
		std::wstring diagnostic;
		if (!HasValidOptions(diagnostic)) return ResultLocked(EEditorControlPlatformRuntimeResultCode::InvalidOptions, std::move(diagnostic));
		m_state = EEditorControlPlatformRuntimeState::Starting;
		m_lastClientResult.reset();
		m_startupDeadline = std::chrono::steady_clock::now() + m_options.startupBudget;
		lock.unlock();

		const auto profileHash = ComputeCanonicalProfileHash(m_options.profileDirectory);
		try {
			auto reader = m_dependencies.endpointReaderFactory
				? m_dependencies.endpointReaderFactory(m_options.profileDirectory, profileHash)
				: std::make_unique<CControlPlatformEndpointDiscoveryReader>(m_options.profileDirectory, profileHash);
			lock.lock();
			if (!reader || m_state != EEditorControlPlatformRuntimeState::Starting) {
				if (m_state != EEditorControlPlatformRuntimeState::Starting) return StartupTerminalResultLocked();
				m_state = EEditorControlPlatformRuntimeState::Failed;
				m_condition.notify_all();
				return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure, L"control-platform endpoint reader creation failed");
			}
			m_endpointReader = std::move(reader);
			m_worker = std::thread(&CEditorControlPlatformRuntime::RetryWorker, this);
		}
		catch (...) {
			if (!lock.owns_lock()) lock.lock();
			if (m_state == EEditorControlPlatformRuntimeState::Starting) {
				m_state = EEditorControlPlatformRuntimeState::Failed;
				m_condition.notify_all();
			}
			return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure, L"control-platform startup initialization failed");
		}
	}

	const auto deadline = m_startupDeadline.value_or(std::chrono::steady_clock::now());
	while (m_state == EEditorControlPlatformRuntimeState::Starting) {
		if (m_condition.wait_until(lock, deadline) == std::cv_status::timeout &&
			m_state == EEditorControlPlatformRuntimeState::Starting) {
			if (m_options.allowDegradedUnavailable) m_state = EEditorControlPlatformRuntimeState::DegradedUnavailable;
			else m_state = EEditorControlPlatformRuntimeState::Failed;
			m_condition.notify_all();
			break;
		}
	}
	return StartupTerminalResultLocked();
}

void CEditorControlPlatformRuntime::RetryWorker() noexcept
{
	for (;;) {
		CControlPlatformClient* client = nullptr;
		{
			std::unique_lock lock(m_mutex);
			if (m_state == EEditorControlPlatformRuntimeState::Stopping || m_state == EEditorControlPlatformRuntimeState::Stopped ||
				m_state == EEditorControlPlatformRuntimeState::Failed) return;
			if (!m_client) {
				lock.unlock();
				if (FreezeFirstIdentity()) {
					std::scoped_lock resetLock(m_mutex);
					m_initialDiscoveryAttempts = 0;
					continue;
				}
				lock.lock();
				if (m_state == EEditorControlPlatformRuntimeState::Stopping || m_state == EEditorControlPlatformRuntimeState::Stopped ||
					m_state == EEditorControlPlatformRuntimeState::Failed) return;
				if (m_initialDiscoveryAttempts >= m_options.clientOptions.maximumRetryAttempts) {
					m_state = EEditorControlPlatformRuntimeState::Failed;
					m_lastClientResult = { EControlPlatformClientOutcome::Unavailable, EControlPlatformRetryClass::None,
						EControlIpcTerminalStatus::ResourceExhausted,
						EControlPlatformEndpointDiscoveryDisposition::NotPublished,
						EControlIpcTransportDisconnectReason::None, L"control-platform endpoint discovery retry budget exhausted" };
					m_condition.notify_all();
					return;
				}
				const auto retryAt = std::chrono::steady_clock::now() + DiscoveryRetryDelay(
					m_options.clientOptions, ++m_initialDiscoveryAttempts);
				m_condition.notify_all();
				m_condition.wait_until(lock, retryAt, [this] {
					return m_state == EEditorControlPlatformRuntimeState::Stopping ||
						m_state == EEditorControlPlatformRuntimeState::Stopped ||
						m_state == EEditorControlPlatformRuntimeState::Failed;
				});
				continue;
			}
			BeginClientCallLocked();
			client = m_client.get();
		}

		ControlPlatformClientResult result;
		try {
			result = client->EnsureReady();
		}
		catch (...) {
			result = { EControlPlatformClientOutcome::Unavailable, EControlPlatformRetryClass::None,
				EControlIpcTerminalStatus::InternalError, EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
				EControlIpcTransportDisconnectReason::None, L"control-platform retry worker caught an exception" };
		}

		std::unique_lock lock(m_mutex);
		EndClientCallLocked();
		if (m_state == EEditorControlPlatformRuntimeState::Stopping ||
			m_state == EEditorControlPlatformRuntimeState::Stopped ||
			m_state == EEditorControlPlatformRuntimeState::Failed) return;
		m_lastClientResult = result;
		if (result.IsReady()) {
			const auto generation = client->GetPinnedGeneration();
			const auto revision = m_cache->GetRevision();
			if (generation != 0 && m_cache->Matches(generation, revision)) {
				m_requestedResnapshotGeneration = std::max(m_requestedResnapshotGeneration, generation);
				m_state = EEditorControlPlatformRuntimeState::Ready;
				m_condition.notify_all();
				m_condition.wait(lock, [this] {
					return m_state != EEditorControlPlatformRuntimeState::Ready;
				});
				if (m_state == EEditorControlPlatformRuntimeState::Stopping ||
					m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Failed) return;
				continue;
			}
			m_state = EEditorControlPlatformRuntimeState::Failed;
			m_condition.notify_all();
			return;
		}
		if (result.outcome == EControlPlatformClientOutcome::Unavailable ||
			IsHardDiscoveryFailure(result.discoveryDisposition) ||
			result.terminalStatus == EControlIpcTerminalStatus::InternalError) {
			m_state = EEditorControlPlatformRuntimeState::Failed;
			m_condition.notify_all();
			return;
		}
		if (result.outcome == EControlPlatformClientOutcome::Stopped) return;

		const auto nextRetry = client->GetNextRetryTime();
		const auto retryWakeRevision = m_retryWakeRevision;
		m_condition.notify_all();
		if (nextRetry) {
			m_condition.wait_until(lock, *nextRetry, [this, retryWakeRevision] {
				return m_state == EEditorControlPlatformRuntimeState::Stopping ||
					m_state == EEditorControlPlatformRuntimeState::Stopped ||
					m_state == EEditorControlPlatformRuntimeState::Failed ||
					m_retryWakeRevision != retryWakeRevision;
			});
		}
		else {
			m_condition.wait(lock, [this, retryWakeRevision] {
				return m_state == EEditorControlPlatformRuntimeState::Stopping ||
					m_state == EEditorControlPlatformRuntimeState::Stopped ||
					m_state == EEditorControlPlatformRuntimeState::Failed ||
					m_retryWakeRevision != retryWakeRevision;
			});
		}
		if (m_state == EEditorControlPlatformRuntimeState::Stopping || m_state == EEditorControlPlatformRuntimeState::Stopped ||
			m_state == EEditorControlPlatformRuntimeState::Failed) return;
	}
}

EditorControlPlatformRuntimeResult CEditorControlPlatformRuntime::RequestResnapshot(std::uint64_t observedGeneration)
{
	CControlPlatformClient* client = nullptr;
	{
		std::scoped_lock lock(m_mutex);
		if (m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Stopping) {
			return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
		}
		if (m_state == EEditorControlPlatformRuntimeState::Failed || !m_client || observedGeneration == 0) {
			return ResultLocked(EEditorControlPlatformRuntimeResultCode::HardFailure,
				L"control-platform client is not available for resynchronization");
		}
		if (observedGeneration <= m_requestedResnapshotGeneration) {
			return ResultLocked(m_state == EEditorControlPlatformRuntimeState::Ready ? EEditorControlPlatformRuntimeResultCode::Ready :
				EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled);
		}
		m_requestedResnapshotGeneration = observedGeneration;
		++m_retryWakeRevision;
		BeginClientCallLocked();
		client = m_client.get();
	}

	ControlPlatformClientResult result;
	try {
		// Stop is intentionally not under m_mutex; RequireResnapshot has the same cancellation boundary.
		result = client->RequireResnapshot(observedGeneration);
	}
	catch (...) {
		result = { EControlPlatformClientOutcome::Unavailable, EControlPlatformRetryClass::None,
			EControlIpcTerminalStatus::InternalError, EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"control-platform resnapshot raised an exception" };
	}

	std::scoped_lock lock(m_mutex);
	EndClientCallLocked();
	if (m_state == EEditorControlPlatformRuntimeState::Stopping || m_state == EEditorControlPlatformRuntimeState::Stopped) {
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
	}
	m_lastClientResult = result;
	if (result.outcome == EControlPlatformClientOutcome::Unavailable) m_state = EEditorControlPlatformRuntimeState::Failed;
	else if (m_state == EEditorControlPlatformRuntimeState::Ready) {
		m_state = EEditorControlPlatformRuntimeState::Starting;
		m_startupDeadline = std::chrono::steady_clock::now() + m_options.startupBudget;
	}
	m_condition.notify_all();
	return m_state == EEditorControlPlatformRuntimeState::Failed
		? StartupTerminalResultLocked()
		: ResultLocked(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled);
}

ControlPlatformClientResult CEditorControlPlatformRuntime::ForceResnapshotAfterMutation(CControlPlatformClient& client)
{
	const auto generation = client.GetPinnedGeneration();
	if (generation == 0) {
		return { EControlPlatformClientOutcome::Unavailable, EControlPlatformRetryClass::None,
			EControlIpcTerminalStatus::GenerationMismatch,
			EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"control-platform mutation has no pinned generation" };
	}
	// Unlike RequestResnapshot(), a storage conflict/cache gap can occur at the
	// same control generation but a newer storage revision.  RequireResnapshot
	// performs the required cache fence and schedules the existing worker.
	return client.RequireResnapshot(generation);
}

void CEditorControlPlatformRuntime::DestroyStoppedResourcesLocked() noexcept
{
	m_client.reset();
	m_endpointReader.reset();
	m_cache.reset();
	m_identity.reset();
	m_lastClientResult.reset();
	m_requestedResnapshotGeneration = 0;
	m_retryWakeRevision = 0;
	m_initialDiscoveryAttempts = 0;
	m_startupDeadline.reset();
}

EditorControlPlatformRuntimeResult CEditorControlPlatformRuntime::Stop()
{
	CControlPlatformClient* client = nullptr;
	IControlPlatformEndpointReader* reader = nullptr;
	std::thread worker;
	{
		std::unique_lock lock(m_mutex);
		if (m_state == EEditorControlPlatformRuntimeState::Stopped) return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
		if (m_state == EEditorControlPlatformRuntimeState::Stopping) {
			m_condition.wait(lock, [this] { return m_state != EEditorControlPlatformRuntimeState::Stopping; });
			return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
		}
		m_state = EEditorControlPlatformRuntimeState::Stopping;
		++m_retryWakeRevision;
		client = m_client.get();
		m_condition.notify_all();
	}
	// This cancellation must not wait for the owner mutex held by an I/O completion.
	if (client) client->Stop();
	{
		std::scoped_lock lock(m_mutex);
		worker = std::move(m_worker);
		m_condition.notify_all();
	}
	if (worker.joinable()) worker.join();
	{
		std::unique_lock lock(m_mutex);
		m_condition.wait(lock, [this] { return m_activeReaderCalls == 0 && m_activeClientCalls == 0; });
		reader = m_endpointReader.get();
		lock.unlock();
		if (reader) reader->Close();
		lock.lock();
		DestroyStoppedResourcesLocked();
		m_state = EEditorControlPlatformRuntimeState::Stopped;
		m_condition.notify_all();
		return ResultLocked(EEditorControlPlatformRuntimeResultCode::Stopped);
	}
}

std::optional<EditorControlPlatformRuntimeIdentity> CEditorControlPlatformRuntime::Identity() const
{
	std::scoped_lock lock(m_mutex);
	return m_state == EEditorControlPlatformRuntimeState::Ready || m_state == EEditorControlPlatformRuntimeState::DegradedUnavailable
		? m_identity : std::nullopt;
}

std::optional<storage::StorageEntry> CEditorControlPlatformRuntime::Find(const storage::StorageAddress& address) const
{
	std::scoped_lock lock(m_mutex);
	if (m_state != EEditorControlPlatformRuntimeState::Ready || !m_cache) return std::nullopt;
	return m_cache->Find(address);
}

EditorControlStorageCacheCoordinateResult CEditorControlPlatformRuntime::StorageCacheCoordinates() const
{
	std::scoped_lock lock(m_mutex);
	return StorageCacheCoordinatesLocked();
}

EditorControlStorageCacheWaitResult CEditorControlPlatformRuntime::WaitForStorageCacheReady(
	std::chrono::milliseconds timeout, std::stop_token cancellation) const
{
	const auto boundedTimeout = std::clamp(timeout, std::chrono::milliseconds::zero(),
		std::chrono::duration_cast<std::chrono::milliseconds>(kMaximumStorageCacheWait));
	const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;
	// A stop request can arrive while the condition variable has no runtime state
	// transition to observe. The callback only wakes this read-only wait; it never
	// requests a resnapshot or touches the transport/client.
	std::stop_callback cancelled(cancellation, [this] { m_condition.notify_all(); });

	std::unique_lock lock(m_mutex);
	for (;;) {
		const auto coordinates = StorageCacheCoordinatesLocked();
		switch (coordinates.code) {
		case EEditorControlStorageCacheCoordinateCode::Ready:
			return { EEditorControlStorageCacheWaitCode::Ready, coordinates.state,
				coordinates.coordinates, std::move(coordinates.diagnostic) };
		case EEditorControlStorageCacheCoordinateCode::DegradedUnavailable:
			return { EEditorControlStorageCacheWaitCode::DegradedUnavailable, coordinates.state,
				std::nullopt, std::move(coordinates.diagnostic) };
		case EEditorControlStorageCacheCoordinateCode::Failed:
			return { EEditorControlStorageCacheWaitCode::Failed, coordinates.state,
				std::nullopt, std::move(coordinates.diagnostic) };
		case EEditorControlStorageCacheCoordinateCode::Stopping:
		case EEditorControlStorageCacheCoordinateCode::Stopped:
			return { EEditorControlStorageCacheWaitCode::Stopped, coordinates.state,
				std::nullopt, std::move(coordinates.diagnostic) };
		case EEditorControlStorageCacheCoordinateCode::Resynchronizing:
			break;
		}

		if (cancellation.stop_requested()) {
			return { EEditorControlStorageCacheWaitCode::Cancelled, m_state, std::nullopt,
				L"control-platform cache readiness wait was cancelled" };
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return { EEditorControlStorageCacheWaitCode::TimedOut, m_state, std::nullopt,
				L"control-platform cache readiness wait timed out" };
		}
		m_condition.wait_until(lock, deadline, [&] {
			return cancellation.stop_requested() ||
				StorageCacheCoordinatesLocked().code != EEditorControlStorageCacheCoordinateCode::Resynchronizing;
		});
	}
}

EditorControlStorageApplyResult CEditorControlPlatformRuntime::Apply(const storage::StorageMutationRequest& request)
{
	CControlPlatformClient* client = nullptr;
	{
		std::scoped_lock lock(m_mutex);
		if (m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Stopping) {
			return { EEditorControlStorageApplyCode::Stopped, std::nullopt, L"control-platform runtime is stopped" };
		}
		if (m_state != EEditorControlPlatformRuntimeState::Ready || !m_client || !m_cache) {
			return { EEditorControlStorageApplyCode::NotReady, std::nullopt, L"control-platform runtime is not ready" };
		}
		BeginClientCallLocked();
		client = m_client.get();
	}

	ControlPlatformMutationResult mutation;
	try {
		mutation = client->Apply(request);
	}
	catch (...) {
		mutation = { EControlPlatformMutationOutcome::Failed, {}, EControlIpcTerminalStatus::InternalError,
			EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"control-platform storage mutation raised an exception" };
	}

	const bool requiresResnapshot = mutation.outcome == EControlPlatformMutationOutcome::Conflict ||
		mutation.outcome == EControlPlatformMutationOutcome::ResnapshotRequired;
	std::optional<ControlPlatformClientResult> resnapshot;
	if (requiresResnapshot && mutation.outcome != EControlPlatformMutationOutcome::Stopped) {
		try {
			resnapshot = ForceResnapshotAfterMutation(*client);
		}
		catch (...) {
			resnapshot = ControlPlatformClientResult{ EControlPlatformClientOutcome::Unavailable,
				EControlPlatformRetryClass::None, EControlIpcTerminalStatus::InternalError,
				EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
				EControlIpcTransportDisconnectReason::None, L"control-platform mutation resnapshot raised an exception" };
		}
	}

	std::scoped_lock lock(m_mutex);
	EndClientCallLocked();
	if (m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Stopping) {
		return { EEditorControlStorageApplyCode::Stopped, std::nullopt, L"control-platform runtime stopped during mutation" };
	}
	if (resnapshot) {
		m_lastClientResult = *resnapshot;
		if (resnapshot->outcome == EControlPlatformClientOutcome::Unavailable ||
			resnapshot->outcome == EControlPlatformClientOutcome::Stopped) {
			// Stop() owns the Stopping -> Stopped transition. If the client reports
			// Stopped without a concurrent runtime Stop (handled above), publish a
			// terminal failure instead of stranding this owner in Stopping.
			m_state = EEditorControlPlatformRuntimeState::Failed;
			m_condition.notify_all();
			return { resnapshot->outcome == EControlPlatformClientOutcome::Stopped
				? EEditorControlStorageApplyCode::Stopped : EEditorControlStorageApplyCode::Failed,
				mutation.storageResult.status == storage::EStorageMutationStatus::Failed ? std::nullopt :
					std::optional<storage::StorageMutationResult>(mutation.storageResult),
				resnapshot->diagnostic };
		}
		if (m_state == EEditorControlPlatformRuntimeState::Ready) {
			m_state = EEditorControlPlatformRuntimeState::Starting;
			m_startupDeadline = std::chrono::steady_clock::now() + m_options.startupBudget;
		}
		++m_retryWakeRevision;
		m_condition.notify_all();
		return { mutation.outcome == EControlPlatformMutationOutcome::Conflict
			? EEditorControlStorageApplyCode::ConflictResnapshotScheduled : EEditorControlStorageApplyCode::ResnapshotScheduled,
			mutation.storageResult.status == storage::EStorageMutationStatus::Failed ? std::nullopt :
				std::optional<storage::StorageMutationResult>(mutation.storageResult), mutation.diagnostic };
	}

	switch (mutation.outcome) {
	case EControlPlatformMutationOutcome::Succeeded:
		return { EEditorControlStorageApplyCode::Succeeded, mutation.storageResult, mutation.diagnostic };
	case EControlPlatformMutationOutcome::NotApplicable:
		return { EEditorControlStorageApplyCode::NotApplicable, mutation.storageResult, mutation.diagnostic };
	case EControlPlatformMutationOutcome::RetryWithSameOperationId:
		return { EEditorControlStorageApplyCode::RetryWithSameOperationId, std::nullopt, mutation.diagnostic };
	case EControlPlatformMutationOutcome::NotReady:
		return { EEditorControlStorageApplyCode::NotReady, std::nullopt, mutation.diagnostic };
	case EControlPlatformMutationOutcome::OperationInFlight:
		return { EEditorControlStorageApplyCode::OperationInFlight, std::nullopt, mutation.diagnostic };
	case EControlPlatformMutationOutcome::Stopped:
		return { EEditorControlStorageApplyCode::Stopped, std::nullopt, mutation.diagnostic };
	case EControlPlatformMutationOutcome::Conflict:
	case EControlPlatformMutationOutcome::ResnapshotRequired:
	case EControlPlatformMutationOutcome::Failed:
		return { EEditorControlStorageApplyCode::Failed, std::nullopt, mutation.diagnostic };
	}
	return { EEditorControlStorageApplyCode::Failed, std::nullopt, L"unknown control-platform mutation outcome" };
}

EditorControlProfileExecuteResult CEditorControlPlatformRuntime::ExecuteProfile(const ControlProfileRpcRequest& request)
{
	CControlPlatformClient* client = nullptr;
	{
		std::scoped_lock lock(m_mutex);
		if (m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Stopping) {
			return { EEditorControlProfileExecuteCode::Stopped, std::nullopt, EControlIpcTerminalStatus::Cancelled,
				EControlPlatformEndpointDiscoveryDisposition::Discovered, EControlIpcTransportDisconnectReason::Stopped,
				L"control-platform runtime is stopped" };
		}
		if (m_state != EEditorControlPlatformRuntimeState::Ready || !m_client || !m_cache) {
			return { EEditorControlProfileExecuteCode::NotReady, std::nullopt, EControlIpcTerminalStatus::ServerStopping,
				EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure, EControlIpcTransportDisconnectReason::None,
				L"control-platform runtime is not ready" };
		}
		BeginClientCallLocked();
		client = m_client.get();
	}

	ControlPlatformProfileResult profile;
	try {
		profile = client->ExecuteProfile(request);
	}
	catch (...) {
		profile = { EControlPlatformProfileOutcome::Failed, std::nullopt, EControlIpcTerminalStatus::InternalError,
			EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
			EControlIpcTransportDisconnectReason::None, L"control-platform profile operation raised an exception" };
	}

	// Profile mutations are control-owned storage mutations too. The profile
	// response remains the operation result; cache resynchronization is a separate
	// lifecycle effect which cannot overwrite its decoded response or operation ID.
	const bool mutation = request.operation >= EControlProfileRpcOperation::CreateNamed &&
		request.operation <= EControlProfileRpcOperation::Import;
	const bool requiresResnapshot = profile.outcome == EControlPlatformProfileOutcome::Conflict ||
		profile.outcome == EControlPlatformProfileOutcome::ResnapshotRequired ||
		(mutation && profile.outcome == EControlPlatformProfileOutcome::Succeeded && profile.response &&
			(profile.response->result.status == profiles::ControlUserDataProfileRegistryStatus::Applied ||
				profile.response->result.status == profiles::ControlUserDataProfileRegistryStatus::Imported));
	std::optional<ControlPlatformClientResult> resnapshot;
	if (requiresResnapshot && profile.outcome != EControlPlatformProfileOutcome::Stopped) {
		try {
			resnapshot = ForceResnapshotAfterMutation(*client);
		}
		catch (...) {
			resnapshot = ControlPlatformClientResult{ EControlPlatformClientOutcome::Unavailable,
				EControlPlatformRetryClass::None, EControlIpcTerminalStatus::InternalError,
				EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
				EControlIpcTransportDisconnectReason::None, L"control-platform profile resnapshot raised an exception" };
		}
	}

	std::scoped_lock lock(m_mutex);
	EndClientCallLocked();
	if (m_state == EEditorControlPlatformRuntimeState::Stopped || m_state == EEditorControlPlatformRuntimeState::Stopping) {
		return { EEditorControlProfileExecuteCode::Stopped, std::nullopt, EControlIpcTerminalStatus::Cancelled,
			EControlPlatformEndpointDiscoveryDisposition::Discovered, EControlIpcTransportDisconnectReason::Stopped,
			L"control-platform runtime stopped during profile operation" };
	}
	if (resnapshot) {
		m_lastClientResult = *resnapshot;
		if (resnapshot->outcome == EControlPlatformClientOutcome::Unavailable ||
			resnapshot->outcome == EControlPlatformClientOutcome::Stopped) {
			m_state = EEditorControlPlatformRuntimeState::Failed;
			m_condition.notify_all();
			return { resnapshot->outcome == EControlPlatformClientOutcome::Stopped
				? EEditorControlProfileExecuteCode::Stopped : EEditorControlProfileExecuteCode::Failed,
				std::move(profile.response), profile.terminalStatus, profile.discoveryDisposition,
				profile.transportReason, resnapshot->diagnostic };
		}
		if (m_state == EEditorControlPlatformRuntimeState::Ready) {
			m_state = EEditorControlPlatformRuntimeState::Starting;
			m_startupDeadline = std::chrono::steady_clock::now() + m_options.startupBudget;
		}
		++m_retryWakeRevision;
		m_condition.notify_all();
		return { profile.outcome == EControlPlatformProfileOutcome::Conflict
			? EEditorControlProfileExecuteCode::ConflictResnapshotScheduled : EEditorControlProfileExecuteCode::ResnapshotScheduled,
			std::move(profile.response), profile.terminalStatus, profile.discoveryDisposition,
			profile.transportReason, profile.diagnostic };
	}

	switch (profile.outcome) {
	case EControlPlatformProfileOutcome::Succeeded:
		return { EEditorControlProfileExecuteCode::Succeeded, std::move(profile.response), profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	case EControlPlatformProfileOutcome::RetryWithSameOperationId:
		return { EEditorControlProfileExecuteCode::RetryWithSameOperationId, std::nullopt, profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	case EControlPlatformProfileOutcome::NotReady:
		return { EEditorControlProfileExecuteCode::NotReady, std::nullopt, profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	case EControlPlatformProfileOutcome::OperationInFlight:
		return { EEditorControlProfileExecuteCode::OperationInFlight, std::nullopt, profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	case EControlPlatformProfileOutcome::Stopped:
		return { EEditorControlProfileExecuteCode::Stopped, std::nullopt, profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	case EControlPlatformProfileOutcome::Conflict:
	case EControlPlatformProfileOutcome::ResnapshotRequired:
	case EControlPlatformProfileOutcome::Failed:
		return { EEditorControlProfileExecuteCode::Failed, std::move(profile.response), profile.terminalStatus,
			profile.discoveryDisposition, profile.transportReason, profile.diagnostic };
	}
	return { EEditorControlProfileExecuteCode::Failed, std::nullopt, EControlIpcTerminalStatus::InternalError,
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure, EControlIpcTransportDisconnectReason::None,
		L"unknown control-platform profile outcome" };
}

} // namespace platform::controlipc
