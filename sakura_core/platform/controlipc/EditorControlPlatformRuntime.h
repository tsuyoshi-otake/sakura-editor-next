/*! @file
	@brief Long-lived editor-process composition owner for the control platform client.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlPlatformClient.h"
#include "platform/controlipc/ControlPlatformEndpointDiscoveryReader.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace platform::controlipc {

//! Transitional states are private to the retry worker; public calls return a terminal result.
enum class EEditorControlPlatformRuntimeState : std::uint8_t {
	Stopped,
	Starting,
	Ready,
	DegradedUnavailable,
	Failed,
	Stopping,
};

enum class EEditorControlPlatformRuntimeResultCode : std::uint8_t {
	Ready,
	AlreadyReady,
	DegradedUnavailable,
	ResnapshotScheduled,
	Stopped,
	InvalidOptions,
	HardFailure,
};

//! The immutable descriptor identity frozen before the client is constructed.
struct EditorControlPlatformRuntimeIdentity {
	std::string profileId;
	std::wstring profileHash;
	std::uint64_t minimumGeneration = 0;

	friend bool operator==(const EditorControlPlatformRuntimeIdentity&,
		const EditorControlPlatformRuntimeIdentity&) = default;
};

//! All input paths have already been selected by process composition, never by this owner.
struct EditorControlPlatformRuntimeOptions {
	std::filesystem::path profileDirectory;
	std::chrono::milliseconds startupBudget = std::chrono::seconds(5);
	//! A transient startup timeout may be exposed only when this opt-in is true.
	bool allowDegradedUnavailable = false;
	ControlPlatformClientOptions clientOptions;
};

//! Tests normally inject a reader plus the client channel factory in clientOptions.
//! Production uses CControlPlatformEndpointDiscoveryReader when this factory is absent.
struct EditorControlPlatformRuntimeDependencies {
	std::function<std::unique_ptr<IControlPlatformEndpointReader>(const std::filesystem::path&, const std::wstring&)>
		endpointReaderFactory;
};

struct EditorControlPlatformRuntimeResult {
	EEditorControlPlatformRuntimeResultCode code = EEditorControlPlatformRuntimeResultCode::HardFailure;
	EEditorControlPlatformRuntimeState state = EEditorControlPlatformRuntimeState::Stopped;
	std::optional<EditorControlPlatformRuntimeIdentity> identity;
	std::optional<ControlPlatformClientResult> clientResult;
	std::wstring diagnostic;
};

//! Editor-facing terminal outcome for one control-owned storage mutation.
enum class EEditorControlStorageApplyCode : std::uint8_t {
	Succeeded,
	NotApplicable,
	ConflictResnapshotScheduled,
	RetryWithSameOperationId,
	ResnapshotScheduled,
	NotReady,
	OperationInFlight,
	Stopped,
	Failed,
};

struct EditorControlStorageApplyResult {
	EEditorControlStorageApplyCode code = EEditorControlStorageApplyCode::Failed;
	std::optional<storage::StorageMutationResult> storageResult;
	std::wstring diagnostic;
};

//! Editor-facing terminal outcome for one control-owned profile operation.
//! A scheduled storage-cache resnapshot never replaces the independently
//! decoded profile response.
enum class EEditorControlProfileExecuteCode : std::uint8_t {
	Succeeded,
	ConflictResnapshotScheduled,
	ResnapshotScheduled,
	RetryWithSameOperationId,
	NotReady,
	OperationInFlight,
	Stopped,
	Failed,
};

struct EditorControlProfileExecuteResult {
	EEditorControlProfileExecuteCode code = EEditorControlProfileExecuteCode::Failed;
	std::optional<ControlProfileRpcResponse> response;
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	EControlPlatformEndpointDiscoveryDisposition discoveryDisposition =
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure;
	EControlIpcTransportDisconnectReason transportReason = EControlIpcTransportDisconnectReason::None;
	std::wstring diagnostic;
};

//! A CAS-safe coordinate copied from the synchronized, control-owned storage cache.
//! `storageRevision` is the global snapshot revision, never an individual entry revision.
struct EditorControlStorageCacheCoordinates {
	std::string profileId;
	std::uint64_t generation = 0;
	std::uint64_t storageRevision = 0;

	friend bool operator==(const EditorControlStorageCacheCoordinates&,
		const EditorControlStorageCacheCoordinates&) = default;
};

//! Read-only state of the synchronized cache. Only Ready carries coordinates.
enum class EEditorControlStorageCacheCoordinateCode : std::uint8_t {
	Ready,
	Resynchronizing,
	DegradedUnavailable,
	Failed,
	Stopping,
	Stopped,
};

struct EditorControlStorageCacheCoordinateResult {
	EEditorControlStorageCacheCoordinateCode code = EEditorControlStorageCacheCoordinateCode::Failed;
	EEditorControlPlatformRuntimeState state = EEditorControlPlatformRuntimeState::Stopped;
	std::optional<EditorControlStorageCacheCoordinates> coordinates;
	std::wstring diagnostic;
};

//! Terminal result of waiting for work already scheduled by another runtime API.
enum class EEditorControlStorageCacheWaitCode : std::uint8_t {
	Ready,
	TimedOut,
	Cancelled,
	DegradedUnavailable,
	Failed,
	Stopped,
};

struct EditorControlStorageCacheWaitResult {
	EEditorControlStorageCacheWaitCode code = EEditorControlStorageCacheWaitCode::Failed;
	EEditorControlPlatformRuntimeState state = EEditorControlPlatformRuntimeState::Stopped;
	std::optional<EditorControlStorageCacheCoordinates> coordinates;
	std::wstring diagnostic;
};

/*! 
	@brief Editor-side owner of endpoint discovery, a synchronized snapshot cache, one
	client, and exactly one retry worker.

	The first acceptable endpoint descriptor freezes the opaque profile authority ID
	and anti-rollback generation.  The worker is the only caller of EnsureReady().
	Readers see value copies through Find(), never the cache or client itself.
*/
class CEditorControlPlatformRuntime final {
public:
	explicit CEditorControlPlatformRuntime(EditorControlPlatformRuntimeOptions options);
	CEditorControlPlatformRuntime(EditorControlPlatformRuntimeOptions options,
		EditorControlPlatformRuntimeDependencies dependencies);
	~CEditorControlPlatformRuntime();
	CEditorControlPlatformRuntime(const CEditorControlPlatformRuntime&) = delete;
	CEditorControlPlatformRuntime& operator=(const CEditorControlPlatformRuntime&) = delete;

	[[nodiscard]] EditorControlPlatformRuntimeResult Start();
	[[nodiscard]] EditorControlPlatformRuntimeResult Stop();
	//! Raises the resynchronization floor monotonically and wakes the existing worker.
	[[nodiscard]] EditorControlPlatformRuntimeResult RequestResnapshot(std::uint64_t observedGeneration);
	[[nodiscard]] std::optional<EditorControlPlatformRuntimeIdentity> Identity() const;
	[[nodiscard]] std::optional<storage::StorageEntry> Find(const storage::StorageAddress& address) const;
	/*! 
		@brief Returns a CAS-safe coordinate only while the existing cache is Ready.

		This is strictly observational: it never reads an endpoint, creates a
		channel, starts the runtime, schedules a retry, or changes runtime state.
	*/
	[[nodiscard]] EditorControlStorageCacheCoordinateResult StorageCacheCoordinates() const;
	/*! 
		@brief Waits for already-scheduled bootstrap/resynchronization work.

		The wait uses a bounded monotonic deadline and caller cancellation. It
		never starts the runtime or schedules I/O/retry work; callers must invoke
		Start() or RequestResnapshot() separately when that is desired.
	*/
	[[nodiscard]] EditorControlStorageCacheWaitResult WaitForStorageCacheReady(
		std::chrono::milliseconds timeout, std::stop_token cancellation = {}) const;
	//! Ready-only facade for editor consumers such as the workbench Memento.
	//! It never exposes the authoritative storage service or creates a local writer.
	[[nodiscard]] EditorControlStorageApplyResult Apply(const storage::StorageMutationRequest& request);
	//! Ready-only profile façade. It borrows one active client call so Stop()
	//! closes the channel and drains before tearing down the reader/cache owner.
	[[nodiscard]] EditorControlProfileExecuteResult ExecuteProfile(const ControlProfileRpcRequest& request);

private:
	[[nodiscard]] bool HasValidOptions(std::wstring& diagnostic) const;
	[[nodiscard]] EditorControlPlatformRuntimeResult ResultLocked(
		EEditorControlPlatformRuntimeResultCode code, std::wstring diagnostic = {}) const;
	[[nodiscard]] EditorControlPlatformRuntimeResult StartupTerminalResultLocked() const;
	[[nodiscard]] EditorControlStorageCacheCoordinateResult StorageCacheCoordinatesLocked() const;
	[[nodiscard]] bool FreezeFirstIdentity();
	void BeginReaderCallLocked();
	void EndReaderCallLocked() noexcept;
	void BeginClientCallLocked();
	void EndClientCallLocked() noexcept;
	void RetryWorker() noexcept;
	//! Forces one same-generation full snapshot after a mutation conflict or cache
	//! gap.  Caller must not hold m_mutex and must already own one active client call.
	[[nodiscard]] ControlPlatformClientResult ForceResnapshotAfterMutation(CControlPlatformClient& client);
	void DestroyStoppedResourcesLocked() noexcept;

	const EditorControlPlatformRuntimeOptions m_options;
	const EditorControlPlatformRuntimeDependencies m_dependencies;
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_condition;
	EEditorControlPlatformRuntimeState m_state = EEditorControlPlatformRuntimeState::Stopped;
	std::optional<EditorControlPlatformRuntimeIdentity> m_identity;
	std::optional<std::chrono::steady_clock::time_point> m_startupDeadline;
	std::uint64_t m_requestedResnapshotGeneration = 0;
	//! Changes only when an external resnapshot/stop request must interrupt a
	//! retry wait.  Condition-variable notifications alone are not retry work.
	std::uint64_t m_retryWakeRevision = 0;
	std::uint32_t m_initialDiscoveryAttempts = 0;
	std::size_t m_activeReaderCalls = 0;
	std::size_t m_activeClientCalls = 0;
	std::optional<ControlPlatformClientResult> m_lastClientResult;
	std::unique_ptr<IControlPlatformEndpointReader> m_endpointReader;
	std::unique_ptr<storage::CStorageSnapshotCache> m_cache;
	std::unique_ptr<CControlPlatformClient> m_client;
	std::thread m_worker;
};

} // namespace platform::controlipc
