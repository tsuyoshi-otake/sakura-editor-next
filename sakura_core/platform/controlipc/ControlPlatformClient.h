/*! @file
	@brief Editor-process bootstrap client for the control platform service.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcNamedPipeTransport.h"
#include "platform/controlipc/ControlProfileRpc.h"
#include "platform/controlipc/ControlStorageRpc.h"
#include <sakura/storage/StorageSnapshotCache.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace platform::controlipc {

//! Observable lifecycle of one editor-side control-platform connection.
enum class EControlPlatformClientState : std::uint8_t {
	Stopped,
	Discovering,
	Connecting,
	Hello,
	Snapshotting,
	Ready,
	RetryScheduled,
	Unavailable,
	ReconnectRequired,
	Stopping,
};

//! Every EnsureReady call has one of these terminal outcomes; no intermediate state is returned as success.
enum class EControlPlatformClientOutcome : std::uint8_t {
	Ready,
	AlreadyReady,
	ConnectionInFlight,
	RetryScheduled,
	Unavailable,
	ReconnectRequired,
	Stopped,
};

enum class EControlPlatformRetryClass : std::uint8_t {
	None,
	ReadOnlyRetry,
	MutationRetryWithSameOperationId,
	MutationConflictDoNotRetry,
};

struct ControlPlatformClientResult {
	EControlPlatformClientOutcome outcome = EControlPlatformClientOutcome::Stopped;
	EControlPlatformRetryClass retryClass = EControlPlatformRetryClass::None;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	std::wstring diagnostic;

	[[nodiscard]] bool IsReady() const noexcept
	{
		return outcome == EControlPlatformClientOutcome::Ready || outcome == EControlPlatformClientOutcome::AlreadyReady;
	}
};

//! Terminal result of one editor-originated control-storage mutation.  This is
//! deliberately narrower than IStorageService: editor processes never become
//! authoritative storage writers and may only issue one revisioned command.
enum class EControlPlatformMutationOutcome : std::uint8_t {
	Succeeded,
	NotApplicable,
	Conflict,
	RetryWithSameOperationId,
	ResnapshotRequired,
	NotReady,
	OperationInFlight,
	Stopped,
	Failed,
};

struct ControlPlatformMutationResult {
	EControlPlatformMutationOutcome outcome = EControlPlatformMutationOutcome::Failed;
	storage::StorageMutationResult storageResult;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	std::wstring diagnostic;
};

//! Terminal result of one editor-originated user-data profile operation.  The
//! response is copied only after the authenticated, generation-pinned channel
//! has returned exactly one valid ProfileResponse.
enum class EControlPlatformProfileOutcome : std::uint8_t {
	Succeeded,
	Conflict,
	RetryWithSameOperationId,
	ResnapshotRequired,
	NotReady,
	OperationInFlight,
	Stopped,
	Failed,
};

struct ControlPlatformProfileResult {
	EControlPlatformProfileOutcome outcome = EControlPlatformProfileOutcome::Failed;
	std::optional<ControlProfileRpcResponse> response;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	//! Deliberately generic transport/protocol text; never a profile identity,
	//! profile document, endpoint name, or filesystem path.
	std::wstring diagnostic;
};

//! Endpoint discovery seam. Implementations return only a copied, untrusted snapshot.
class IControlPlatformEndpointReader {
public:
	virtual ~IControlPlatformEndpointReader() = default;
	[[nodiscard]] virtual std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements) = 0;
	//! Compatibility default for existing fake readers.  Production readers
	//! override this to preserve the typed endpoint disposition.
	[[nodiscard]] virtual ControlPlatformEndpointDiscoveryResult ReadDetailed(
		const ControlPlatformEndpointReadRequirements& requirements)
	{
		auto snapshot = Read(requirements);
		return snapshot
			? ControlPlatformEndpointDiscoveryResult{ EControlPlatformEndpointDiscoveryDisposition::Discovered,
				std::move(snapshot), ERROR_SUCCESS, {} }
			: ControlPlatformEndpointDiscoveryResult{ EControlPlatformEndpointDiscoveryDisposition::NotPublished,
				std::nullopt, ERROR_FILE_NOT_FOUND, L"Control IPC endpoint was not published" };
	}
	//! Editor runtime shutdown is terminal. Stateless test readers need no cleanup.
	virtual void Close() noexcept {}
};

//! Production reader over the protected shared-memory endpoint ABI.
class CControlPlatformEndpointReader final : public IControlPlatformEndpointReader {
public:
	explicit CControlPlatformEndpointReader(CControlPlatformEndpoint& endpoint) noexcept : m_endpoint(endpoint) {}
	[[nodiscard]] std::optional<ControlPlatformEndpointSnapshot> Read(
		const ControlPlatformEndpointReadRequirements& requirements) override;
	[[nodiscard]] ControlPlatformEndpointDiscoveryResult ReadDetailed(
		const ControlPlatformEndpointReadRequirements& requirements) override;

private:
	CControlPlatformEndpoint& m_endpoint;
};

//! Exchange seam. A call returns every frame observed by one bounded request exchange.
class IControlPlatformClientChannel {
public:
	virtual ~IControlPlatformClientChannel() = default;
	[[nodiscard]] virtual ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot& endpoint,
		std::chrono::milliseconds deadline) = 0;
	[[nodiscard]] virtual ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline) = 0;
	virtual void Close() noexcept = 0;
};

//! Production adapter. Tests should inject IControlPlatformClientChannel instead of creating pipes.
class CControlPlatformNamedPipeChannel final : public IControlPlatformClientChannel {
public:
	[[nodiscard]] ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot& endpoint,
		std::chrono::milliseconds deadline) override;
	[[nodiscard]] ControlIpcTransportResult Exchange(const ControlIpcFrame& request,
		std::vector<ControlIpcFrame>& responses, std::chrono::milliseconds deadline) override;
	void Close() noexcept override;

private:
	CControlIpcNamedPipeClient m_client;
};

struct ControlPlatformClientOptions {
	//! Both identifiers are immutable inputs supplied by process composition.
	std::string profileId;
	std::wstring profileHash;
	//! Anti-rollback floor copied from the first trusted endpoint descriptor.
	//! Zero is reserved for tests or composition paths without a prior descriptor.
	std::uint64_t minimumGeneration = 0;
	std::chrono::milliseconds exchangeDeadline = std::chrono::seconds(5);
	std::chrono::milliseconds retryBaseDelay = std::chrono::milliseconds(50);
	std::chrono::milliseconds retryMaximumDelay = std::chrono::seconds(5);
	std::uint32_t maximumRetryAttempts = 5;
	//! Deterministic salt used to spread retries without a sleep or ambient RNG dependency.
	std::uint32_t retryJitterSalt = 0;
	std::function<std::chrono::steady_clock::time_point()> now;
	std::function<std::unique_ptr<IControlPlatformClientChannel>()> channelFactory;
};

/*! 
	@brief UI-independent editor bootstrap state machine.

	EnsureReady owns a complete read-only discovery/connect/hello/snapshot attempt. A
	second caller while that attempt is in progress observes ConnectionInFlight;
	it cannot create a second transport. Stop closes the active channel and wins
	over every later completion, so scheduled retries never survive shutdown.
	RequireResnapshot cancels an active attempt and fences its eventual completion:
	no stale attempt may publish Ready or repopulate the cache afterward.

	The owner must keep this object alive until every concurrent EnsureReady call
	has returned. Stop is the cancellation boundary; destruction is not a join.
*/
class CControlPlatformClient final {
public:
	CControlPlatformClient(ControlPlatformClientOptions options,
		IControlPlatformEndpointReader& endpointReader, storage::CStorageSnapshotCache& cache);
	~CControlPlatformClient();
	CControlPlatformClient(const CControlPlatformClient&) = delete;
	CControlPlatformClient& operator=(const CControlPlatformClient&) = delete;

	[[nodiscard]] ControlPlatformClientResult EnsureReady();
	void Stop() noexcept;
	//! Invalidates the cache and requires a new full snapshot before reads may be treated as ready.
	[[nodiscard]] ControlPlatformClientResult RequireResnapshot(std::uint64_t observedGeneration);
	/*! 
		@brief Sends one ready-only revisioned storage mutation over a fresh,
		authenticated connection.

		This method never performs hidden bootstrap/retry work.  An Exchange loss
		after the Apply request is the only result that asks its caller to retry,
		and that retry must retain request.operationId exactly.
	*/
	[[nodiscard]] ControlPlatformMutationResult Apply(const storage::StorageMutationRequest& request);
	/*! 
		@brief Sends one profile read or CAS mutation over a fresh authenticated channel.

		The client must already be Ready with a generation-matched storage cache.
		Read failures are terminal and never start a hidden bootstrap.  Only an
		ambiguous loss after a mutation ProfileRequest yields
		RetryWithSameOperationId, and the caller-owned operationId is untouched.
	*/
	[[nodiscard]] ControlPlatformProfileResult ExecuteProfile(const ControlProfileRpcRequest& request);

	[[nodiscard]] EControlPlatformClientState GetState() const noexcept;
	[[nodiscard]] std::uint64_t GetPinnedGeneration() const noexcept;
	[[nodiscard]] std::uint32_t GetRetryAttemptCount() const noexcept;
	[[nodiscard]] std::optional<std::chrono::steady_clock::time_point> GetNextRetryTime() const noexcept;

	//! These helpers define the replay contract used by Apply and its callers.
	[[nodiscard]] static EControlPlatformRetryClass ClassifyMutationResult(
		const storage::StorageMutationRequest& request, const storage::StorageMutationResult& result) noexcept;
	//! Only an ambiguous transport loss may replay, and it must retain request.operationId exactly.
	[[nodiscard]] static EControlPlatformRetryClass ClassifyAmbiguousMutationTransportFailure(
		const storage::StorageMutationRequest& request) noexcept;
	[[nodiscard]] static bool IsMutationRequestReplaySafe(const storage::StorageMutationRequest& request) noexcept;

private:
	[[nodiscard]] ControlPlatformClientResult RunAttempt(std::shared_ptr<IControlPlatformClientChannel> channel);
	[[nodiscard]] ControlPlatformClientResult FinishTransientFailure(EControlIpcTerminalStatus status, std::wstring diagnostic,
		EControlPlatformEndpointDiscoveryDisposition discoveryDisposition = EControlPlatformEndpointDiscoveryDisposition::Discovered,
		EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None);
	[[nodiscard]] ControlPlatformClientResult FinishUnavailable(EControlIpcTerminalStatus status, std::wstring diagnostic,
		EControlPlatformEndpointDiscoveryDisposition discoveryDisposition = EControlPlatformEndpointDiscoveryDisposition::Discovered,
		EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None);
	[[nodiscard]] ControlPlatformClientResult FinishReconnectRequired(EControlIpcTerminalStatus status, std::wstring diagnostic,
		std::uint64_t minimumGeneration,
		EControlPlatformEndpointDiscoveryDisposition discoveryDisposition = EControlPlatformEndpointDiscoveryDisposition::Discovered,
		EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None);
	[[nodiscard]] static bool IsTransientDiscoveryDisposition(EControlPlatformEndpointDiscoveryDisposition disposition) noexcept;
	[[nodiscard]] static bool IsTransientTransportReason(EControlIpcTransportDisconnectReason reason) noexcept;
	[[nodiscard]] ControlPlatformClientResult ValidateSingleTerminalResponse(const ControlIpcFrame& request,
		const std::vector<ControlIpcFrame>& responses, EControlIpcKind expectedKind, ControlIpcFrame& response);
	[[nodiscard]] std::optional<std::uint64_t> NextRequestId() noexcept;
	[[nodiscard]] std::chrono::steady_clock::time_point Now() const;
	[[nodiscard]] std::chrono::milliseconds RetryDelayFor(std::uint32_t attempt) const noexcept;
	void InvalidateCache();

	ControlPlatformClientOptions m_options;
	IControlPlatformEndpointReader& m_endpointReader;
	storage::CStorageSnapshotCache& m_cache;
	mutable std::mutex m_mutex;
	EControlPlatformClientState m_state = EControlPlatformClientState::ReconnectRequired;
	bool m_stopping = false;
	bool m_attemptInFlight = false;
	//! Shares m_activeChannel with bootstrap/profile/storage work so
	//! Stop/RequireResnapshot can cancel exactly one in-flight external operation.
	bool m_mutationInFlight = false;
	bool m_resnapshotRequested = false;
	std::uint64_t m_pinnedGeneration = 0;
	std::uint64_t m_minimumGeneration = 0;
	std::uint64_t m_nextRequestId = 1;
	std::uint32_t m_retryAttempts = 0;
	std::optional<std::chrono::steady_clock::time_point> m_nextRetry;
	std::shared_ptr<IControlPlatformClientChannel> m_activeChannel;
};

} // namespace platform::controlipc
