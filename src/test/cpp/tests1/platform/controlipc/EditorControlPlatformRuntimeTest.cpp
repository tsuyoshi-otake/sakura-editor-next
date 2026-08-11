/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/controlipc/EditorControlPlatformRuntime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr char kProfileAuthorityId[] = "0123456789abcdef0123456789abcdef";

std::filesystem::path UniqueProfilePath()
{
	static std::atomic_uint64_t sequence{ 0 };
	return std::filesystem::temp_directory_path() /
		("sakura-editor-control-runtime-" + std::to_string(::GetCurrentProcessId()) + "-" +
			std::to_string(::GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
}

ControlPlatformEndpointSnapshot Endpoint(const std::wstring& hash, std::uint64_t generation)
{
	return { ::GetCurrentProcessId(), generation, ControlPlatformEndpointLifecycle::Accepting,
		hash, BuildControlPipeName(hash), kProfileAuthorityId };
}

ControlIpcFrame Response(const ControlIpcFrame& request, EControlIpcKind kind, std::uint64_t generation,
	std::vector<std::uint8_t> payload)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation }, std::move(payload) };
}

std::vector<std::uint8_t> ProfileResponsePayload(const ControlProfileRpcResponse& response)
{
	const auto encoded = EncodeControlProfileRpcResponse(response);
	EXPECT_TRUE(encoded.has_value());
	if (!encoded) return {};
	const auto fields = EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload), *encoded } });
	EXPECT_TRUE(fields.has_value());
	return fields.value_or(std::vector<std::uint8_t>{});
}

ControlProfileRpcRequest ProfileResolveRequest()
{
	ControlProfileRpcRequest request;
	request.operation = EControlProfileRpcOperation::Resolve;
	request.profileId = L"profile-a";
	return request;
}

ControlProfileRpcRequest ProfileRenameRequest()
{
	ControlProfileRpcRequest request;
	request.operation = EControlProfileRpcOperation::Rename;
	request.mutation.operationId = "runtime-profile-operation";
	request.mutation.expectedStorageRevision = 3;
	request.profileId = L"profile-a";
	request.displayName = L"Renamed";
	return request;
}

storage::StorageSnapshot Snapshot(std::uint64_t generation, std::uint64_t revision = 3,
	std::uint64_t entryRevision = 3)
{
	storage::StorageSnapshot snapshot;
	snapshot.generation = generation;
	snapshot.revision = revision;
	snapshot.entries.push_back({ { storage::EStorageScope::Profile, "profile-a", "editor", "theme" },
		storage::EStorageTarget::User, "dark", entryRevision });
	return snapshot;
}

struct Script {
	std::mutex mutex;
	std::deque<ControlIpcTransportResult> connectResults;
	std::deque<std::vector<ControlIpcFrame>> responses;
	int channelCreations = 0;
	int connectCalls = 0;
	int exchangeCalls = 0;
	int closeCalls = 0;
	bool blockConnect = false;
	bool connectEntered = false;
	bool releaseConnect = false;
	std::condition_variable condition;
};

class CScriptedChannel final : public IControlPlatformClientChannel {
public:
	explicit CScriptedChannel(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		std::unique_lock lock(m_script->mutex);
		++m_script->connectCalls;
		if (m_script->blockConnect) {
			m_script->connectEntered = true;
			m_script->condition.notify_all();
			m_script->condition.wait(lock, [this] { return m_closed || m_script->releaseConnect; });
			if (m_closed) {
				return { false, EControlIpcTransportDisconnectReason::Stopped, ERROR_OPERATION_ABORTED, L"cancelled" };
			}
		}
		if (m_script->connectResults.empty()) return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
		auto result = m_script->connectResults.front();
		m_script->connectResults.pop_front();
		return result;
	}

	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		std::scoped_lock lock(m_script->mutex);
		++m_script->exchangeCalls;
		m_script->condition.notify_all();
		if (m_script->responses.empty()) return { false, EControlIpcTransportDisconnectReason::PeerClosed, 0, L"script exhausted" };
		responses = std::move(m_script->responses.front());
		m_script->responses.pop_front();
		for (auto& response : responses) response.header.requestId = request.header.requestId;
		return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
	}

	void Close() noexcept override
	{
		std::scoped_lock lock(m_script->mutex);
		++m_script->closeCalls;
		m_closed = true;
		m_script->condition.notify_all();
	}

private:
	std::shared_ptr<Script> m_script;
	//! Close is channel-local; closing a completed bootstrap channel must not
	//! pre-cancel a later resnapshot channel created from the same test script.
	bool m_closed = false;
};

class CScriptedReader final : public IControlPlatformEndpointReader {
public:
	ControlPlatformEndpointDiscoveryResult ReadDetailed(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		std::scoped_lock lock(m_mutex);
		requirementsSeen.push_back(requirements);
		if (results.empty()) return { EControlPlatformEndpointDiscoveryDisposition::NotPublished, std::nullopt,
			ERROR_FILE_NOT_FOUND, L"not published" };
		auto result = results.front();
		if (results.size() > 1) results.pop_front();
		return result;
	}

	std::optional<ControlPlatformEndpointSnapshot> Read(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		return ReadDetailed(requirements).snapshot;
	}

	std::mutex m_mutex;
	std::deque<ControlPlatformEndpointDiscoveryResult> results;
	std::vector<ControlPlatformEndpointReadRequirements> requirementsSeen;
};

void QueueHealthyBootstrap(const std::shared_ptr<Script>& script, std::uint64_t generation,
	std::uint64_t revision = 3, std::uint64_t entryRevision = 3)
{
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	const auto snapshot = EncodeControlStorageSnapshotResponse(Snapshot(generation, revision, entryRevision));
	ASSERT_TRUE(hello);
	ASSERT_TRUE(snapshot);
	std::scoped_lock lock(script->mutex);
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, generation }, *hello } });
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotResponse,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, generation }, *snapshot } });
}

EditorControlPlatformRuntimeOptions Options(const std::filesystem::path& profile, const std::shared_ptr<Script>& script)
{
	EditorControlPlatformRuntimeOptions options;
	options.profileDirectory = profile;
	options.startupBudget = std::chrono::milliseconds(100);
	options.clientOptions.exchangeDeadline = std::chrono::milliseconds(10);
	options.clientOptions.retryBaseDelay = std::chrono::milliseconds(2);
	options.clientOptions.retryMaximumDelay = std::chrono::milliseconds(10);
	options.clientOptions.maximumRetryAttempts = 2;
	options.clientOptions.retryJitterSalt = 0;
	options.clientOptions.channelFactory = [script] {
		std::scoped_lock lock(script->mutex);
		++script->channelCreations;
		return std::make_unique<CScriptedChannel>(script);
	};
	return options;
}

// The runtime takes ownership of the reader. Share only immutable scripted state between
// the factory copy and the assertions below by using this small forwarding reader instead.
class CSharedScriptedReader final : public IControlPlatformEndpointReader {
public:
	explicit CSharedScriptedReader(std::shared_ptr<CScriptedReader> reader) : m_reader(std::move(reader)) {}
	std::optional<ControlPlatformEndpointSnapshot> Read(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		return m_reader->Read(requirements);
	}
	ControlPlatformEndpointDiscoveryResult ReadDetailed(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		return m_reader->ReadDetailed(requirements);
	}
private:
	std::shared_ptr<CScriptedReader> m_reader;
};

EditorControlPlatformRuntimeDependencies SharedDependencies(const std::shared_ptr<CScriptedReader>& reader)
{
	EditorControlPlatformRuntimeDependencies dependencies;
	dependencies.endpointReaderFactory = [reader](const std::filesystem::path&, const std::wstring&) {
		return std::make_unique<CSharedScriptedReader>(reader);
	};
	return dependencies;
}

TEST(EditorControlPlatformRuntime, FreezesInitialIdentityAndPassesItsGenerationAsAntiRollbackFloor)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 7), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 7);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));

	const auto result = runtime.Start();
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, result.code);
	ASSERT_TRUE(result.identity.has_value());
	EXPECT_EQ(kProfileAuthorityId, result.identity->profileId);
	EXPECT_EQ(hash, result.identity->profileHash);
	EXPECT_EQ(7u, result.identity->minimumGeneration);
	ASSERT_GE(reader->requirementsSeen.size(), 2u);
	EXPECT_EQ(0u, reader->requirementsSeen.front().minimumGeneration);
	EXPECT_EQ(7u, reader->requirementsSeen.back().minimumGeneration);
	EXPECT_TRUE(runtime.Stop().code == EEditorControlPlatformRuntimeResultCode::Stopped);
}

TEST(EditorControlPlatformRuntime, StorageCacheCoordinatesUseGlobalRevisionAndExposeNothingUntilReady)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 7), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 7), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 8), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 7, 47, 11);
	QueueHealthyBootstrap(script, 8, 48, 12);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);

	const auto ready = runtime.StorageCacheCoordinates();
	ASSERT_EQ(EEditorControlStorageCacheCoordinateCode::Ready, ready.code);
	ASSERT_TRUE(ready.coordinates.has_value());
	EXPECT_EQ(kProfileAuthorityId, ready.coordinates->profileId);
	EXPECT_EQ(7u, ready.coordinates->generation);
	EXPECT_EQ(47u, ready.coordinates->storageRevision);
	EXPECT_NE(11u, ready.coordinates->storageRevision);

	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
	}
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled, runtime.RequestResnapshot(8).code);
	bool connectEntered = false;
	{
		std::unique_lock lock(script->mutex);
		connectEntered = script->condition.wait_for(lock, std::chrono::seconds(1), [&] { return script->connectEntered; });
	}
	if (!connectEntered) (void)runtime.Stop();
	ASSERT_TRUE(connectEntered);
	const auto resynchronizing = runtime.StorageCacheCoordinates();
	EXPECT_EQ(EEditorControlStorageCacheCoordinateCode::Resynchronizing, resynchronizing.code);
	EXPECT_FALSE(resynchronizing.coordinates.has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	const auto stopped = runtime.StorageCacheCoordinates();
	EXPECT_EQ(EEditorControlStorageCacheCoordinateCode::Stopped, stopped.code);
	EXPECT_FALSE(stopped.coordinates.has_value());
}

TEST(EditorControlPlatformRuntime, NonStartingStorageCacheWaitReachesExistingResnapshotCompletion)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 8), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 5, 17, 4);
	QueueHealthyBootstrap(script, 8, 29, 5);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
	}
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled, runtime.RequestResnapshot(8).code);
	bool connectEntered = false;
	{
		std::unique_lock lock(script->mutex);
		connectEntered = script->condition.wait_for(lock, std::chrono::seconds(1), [&] { return script->connectEntered; });
	}
	if (!connectEntered) (void)runtime.Stop();
	ASSERT_TRUE(connectEntered);

	EditorControlStorageCacheWaitResult result;
	std::thread waiter([&] { result = runtime.WaitForStorageCacheReady(std::chrono::seconds(1)); });
	{
		std::scoped_lock lock(script->mutex);
		script->releaseConnect = true;
		script->condition.notify_all();
	}
	waiter.join();
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::Ready, result.code);
	ASSERT_TRUE(result.coordinates.has_value());
	EXPECT_EQ(8u, result.coordinates->generation);
	EXPECT_EQ(29u, result.coordinates->storageRevision);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, NonStartingStorageCacheWaitTimeoutAndCancellationDoNotCreateWork)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 8), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 5);
	QueueHealthyBootstrap(script, 8);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
	}
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled, runtime.RequestResnapshot(8).code);
	bool connectEntered = false;
	int channelCreationsBeforeWait = 0;
	int connectCallsBeforeWait = 0;
	{
		std::unique_lock lock(script->mutex);
		connectEntered = script->condition.wait_for(lock, std::chrono::seconds(1), [&] { return script->connectEntered; });
		channelCreationsBeforeWait = script->channelCreations;
		connectCallsBeforeWait = script->connectCalls;
	}
	if (!connectEntered) (void)runtime.Stop();
	ASSERT_TRUE(connectEntered);
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::TimedOut,
		runtime.WaitForStorageCacheReady(std::chrono::milliseconds(5)).code);
	std::stop_source cancellation;
	cancellation.request_stop();
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::Cancelled,
		runtime.WaitForStorageCacheReady(std::chrono::seconds(1), cancellation.get_token()).code);
	EXPECT_EQ(EEditorControlStorageCacheCoordinateCode::Resynchronizing, runtime.StorageCacheCoordinates().code);
	{
		std::scoped_lock lock(script->mutex);
		EXPECT_EQ(channelCreationsBeforeWait, script->channelCreations);
		EXPECT_EQ(connectCallsBeforeWait, script->connectCalls);
	}
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, NonStartingStorageCacheWaitMapsTerminalStatesAndStopWakes)
{
	const auto failedProfile = UniqueProfilePath();
	auto failedReader = std::make_shared<CScriptedReader>();
	failedReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::AccessDenied, std::nullopt,
		ERROR_ACCESS_DENIED, L"denied" });
	auto failedScript = std::make_shared<Script>();
	CEditorControlPlatformRuntime failed(Options(failedProfile, failedScript), SharedDependencies(failedReader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, failed.Start().code);
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::Failed,
		failed.WaitForStorageCacheReady(std::chrono::seconds(1)).code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, failed.Stop().code);

	const auto degradedProfile = UniqueProfilePath();
	const auto degradedHash = ComputeCanonicalProfileHash(degradedProfile);
	auto degradedReader = std::make_shared<CScriptedReader>();
	degradedReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered,
		Endpoint(degradedHash, 3), ERROR_SUCCESS, {} });
	auto degradedScript = std::make_shared<Script>();
	{
		std::scoped_lock lock(degradedScript->mutex);
		degradedScript->connectResults.push_back({ false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_PIPE_BUSY, L"busy" });
	}
	auto degradedOptions = Options(degradedProfile, degradedScript);
	degradedOptions.startupBudget = std::chrono::milliseconds(1);
	degradedOptions.clientOptions.retryBaseDelay = std::chrono::milliseconds(50);
	degradedOptions.clientOptions.retryMaximumDelay = std::chrono::milliseconds(50);
	degradedOptions.allowDegradedUnavailable = true;
	CEditorControlPlatformRuntime degraded(std::move(degradedOptions), SharedDependencies(degradedReader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::DegradedUnavailable, degraded.Start().code);
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::DegradedUnavailable,
		degraded.WaitForStorageCacheReady(std::chrono::seconds(1)).code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, degraded.Stop().code);

	const auto stopProfile = UniqueProfilePath();
	const auto stopHash = ComputeCanonicalProfileHash(stopProfile);
	auto stopReader = std::make_shared<CScriptedReader>();
	stopReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(stopHash, 5), ERROR_SUCCESS, {} });
	stopReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(stopHash, 5), ERROR_SUCCESS, {} });
	stopReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(stopHash, 8), ERROR_SUCCESS, {} });
	auto stopScript = std::make_shared<Script>();
	QueueHealthyBootstrap(stopScript, 5);
	QueueHealthyBootstrap(stopScript, 8);
	CEditorControlPlatformRuntime stopping(Options(stopProfile, stopScript), SharedDependencies(stopReader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, stopping.Start().code);
	{
		std::scoped_lock lock(stopScript->mutex);
		stopScript->blockConnect = true;
	}
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled, stopping.RequestResnapshot(8).code);
	bool connectEntered = false;
	{
		std::unique_lock lock(stopScript->mutex);
		connectEntered = stopScript->condition.wait_for(lock, std::chrono::seconds(1), [&] { return stopScript->connectEntered; });
	}
	if (!connectEntered) (void)stopping.Stop();
	ASSERT_TRUE(connectEntered);
	EditorControlStorageCacheWaitResult stoppedWait;
	std::thread waiter([&] { stoppedWait = stopping.WaitForStorageCacheReady(std::chrono::seconds(1)); });
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, stopping.Stop().code);
	waiter.join();
	EXPECT_EQ(EEditorControlStorageCacheWaitCode::Stopped, stoppedWait.code);
}

TEST(EditorControlPlatformRuntime, RequiresAFullSnapshotBeforePublishingReady)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 4), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	{
		std::scoped_lock lock(script->mutex);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 4 }, *hello } });
	}
	auto options = Options(profile, script);
	options.clientOptions.maximumRetryAttempts = 0;
	CEditorControlPlatformRuntime runtime(std::move(options), SharedDependencies(reader));
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, runtime.Start().code);
	EXPECT_FALSE(runtime.Identity().has_value());
	EXPECT_FALSE(runtime.Find({ storage::EStorageScope::Profile, "profile-a", "editor", "theme" }).has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, KeepsOneWorkerAndDeduplicatesNonAdvancingResnapshot)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 6);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.RequestResnapshot(6).code);
	std::scoped_lock lock(script->mutex);
	EXPECT_EQ(1, script->channelCreations);
	EXPECT_EQ(1, script->connectCalls);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, MutationConflictSchedulesSameGenerationFullResnapshot)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 6);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	storage::StorageMutationResult conflict;
	conflict.status = storage::EStorageMutationStatus::Conflict;
	conflict.revision = 4;
	conflict.diagnostic = "storage revision conflict";
	const auto conflictPayload = EncodeControlStorageApplyResponse(conflict);
	ASSERT_TRUE(conflictPayload);
	{
		std::scoped_lock lock(script->mutex);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, *hello } });
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageApplyResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, *conflictPayload } });
	}
	QueueHealthyBootstrap(script, 6);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	storage::StorageMutationRequest request{ "layout-conflict", 3,
		{ { { storage::EStorageScope::Profile, "profile-a", "workbench.layout", "state" },
			storage::EStorageTarget::User, "{\"version\":1}" } } };
	EXPECT_EQ(EEditorControlStorageApplyCode::ConflictResnapshotScheduled, runtime.Apply(request).code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, ProfileFacadeIsReadyOnlyAndKeepsDecodedResponseSeparateFromCacheState)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	EXPECT_EQ(EEditorControlProfileExecuteCode::Stopped, runtime.ExecuteProfile(ProfileResolveRequest()).code);
	QueueHealthyBootstrap(script, 6);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	ControlProfileRpcResponse profileResponse;
	profileResponse.terminalStatus = EControlIpcTerminalStatus::Succeeded;
	profileResponse.result.status = profiles::ControlUserDataProfileRegistryStatus::Resolved;
	profileResponse.result.storageRevision = 3;
	const auto payload = ProfileResponsePayload(profileResponse);
	ASSERT_FALSE(payload.empty());
	{
		std::scoped_lock lock(script->mutex);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, *hello } });
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, payload } });
	}
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	const auto result = runtime.ExecuteProfile(ProfileResolveRequest());
	EXPECT_EQ(EEditorControlProfileExecuteCode::Succeeded, result.code);
	EXPECT_EQ(EControlIpcTerminalStatus::Succeeded, result.terminalStatus);
	ASSERT_TRUE(result.response.has_value());
	EXPECT_EQ(profiles::ControlUserDataProfileRegistryStatus::Resolved, result.response->result.status);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, ProfileConflictSchedulesResnapshotWithoutDiscardingProfileResponse)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 6);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	ControlProfileRpcResponse conflict;
	conflict.terminalStatus = EControlIpcTerminalStatus::InvalidRequest;
	conflict.result.status = profiles::ControlUserDataProfileRegistryStatus::PersistConflict;
	conflict.result.storageRevision = 4;
	const auto payload = ProfileResponsePayload(conflict);
	ASSERT_FALSE(payload.empty());
	{
		std::scoped_lock lock(script->mutex);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, *hello } });
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 0, 6 }, payload } });
	}
	QueueHealthyBootstrap(script, 6);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	const auto result = runtime.ExecuteProfile(ProfileRenameRequest());
	EXPECT_EQ(EEditorControlProfileExecuteCode::ConflictResnapshotScheduled, result.code);
	ASSERT_TRUE(result.response.has_value());
	EXPECT_EQ(profiles::ControlUserDataProfileRegistryStatus::PersistConflict, result.response->result.status);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, StopClosesAndDrainsAnInFlightProfileFacadeCall)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 6), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 6);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
		script->connectEntered = false;
	}
	EditorControlProfileExecuteResult result;
	std::thread caller([&] { result = runtime.ExecuteProfile(ProfileResolveRequest()); });
	bool entered = false;
	{
		std::unique_lock lock(script->mutex);
		entered = script->condition.wait_for(lock, std::chrono::seconds(1), [&] { return script->connectEntered; });
	}
	if (!entered) {
		(void)runtime.Stop();
		caller.join();
	}
	ASSERT_TRUE(entered);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	caller.join();
	EXPECT_EQ(EEditorControlProfileExecuteCode::Stopped, result.code);
	std::scoped_lock lock(script->mutex);
	EXPECT_GE(script->closeCalls, 1);
}

TEST(EditorControlPlatformRuntime, StartupBudgetAllowsDegradedOnlyWhenExplicitlyEnabled)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 3), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	{
		std::scoped_lock lock(script->mutex);
		script->connectResults.push_back({ false, EControlIpcTransportDisconnectReason::ConnectFailed, ERROR_PIPE_BUSY, L"busy" });
	}
	auto options = Options(profile, script);
	options.startupBudget = std::chrono::milliseconds(1);
	options.clientOptions.retryBaseDelay = std::chrono::milliseconds(50);
	options.clientOptions.retryMaximumDelay = std::chrono::milliseconds(50);
	options.allowDegradedUnavailable = true;
	CEditorControlPlatformRuntime degraded(std::move(options), SharedDependencies(reader));
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::DegradedUnavailable, degraded.Start().code);
	EXPECT_FALSE(degraded.Find({ storage::EStorageScope::Profile, "profile-a", "editor", "theme" }).has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, degraded.Stop().code);
}

TEST(EditorControlPlatformRuntime, SecurityAndProtocolFailuresFailClosed)
{
	const auto profile = UniqueProfilePath();
	auto deniedReader = std::make_shared<CScriptedReader>();
	deniedReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::AccessDenied, std::nullopt, ERROR_ACCESS_DENIED, L"denied" });
	auto script = std::make_shared<Script>();
	CEditorControlPlatformRuntime denied(Options(profile, script), SharedDependencies(deniedReader));
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, denied.Start().code);
	EXPECT_FALSE(denied.Identity().has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, denied.Stop().code);

	const auto protocolProfile = UniqueProfilePath();
	const auto protocolHash = ComputeCanonicalProfileHash(protocolProfile);
	auto protocolReader = std::make_shared<CScriptedReader>();
	protocolReader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered,
		Endpoint(protocolHash, 12), ERROR_SUCCESS, {} });
	auto protocolScript = std::make_shared<Script>();
	{
		std::scoped_lock lock(protocolScript->mutex);
		protocolScript->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion,
			EControlIpcKind::StorageSnapshotResponse, EControlIpcFlags::Response | EControlIpcFlags::Terminal,
			0, 12 }, {} } });
	}
	CEditorControlPlatformRuntime protocol(Options(protocolProfile, protocolScript), SharedDependencies(protocolReader));
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, protocol.Start().code);
	EXPECT_FALSE(protocol.Identity().has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, protocol.Stop().code);
}

TEST(EditorControlPlatformRuntime, InitialDiscoveryUsesTheWorkerRetryBudgetBeforeReady)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::NotPublished, std::nullopt,
		ERROR_FILE_NOT_FOUND, L"not published" });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 13), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 13);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	ASSERT_GE(reader->requirementsSeen.size(), 3u);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, HigherGenerationResnapshotUsesTheExistingWorkerAndRefreshesCache)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 5), ERROR_SUCCESS, {} });
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 8), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 5);
	QueueHealthyBootstrap(script, 8);
	CEditorControlPlatformRuntime runtime(Options(profile, script), SharedDependencies(reader));
	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::ResnapshotScheduled, runtime.RequestResnapshot(8).code);
	// The request fences immediately; Start waits for the existing worker's terminal ready publication.
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Ready, runtime.Start().code);
	const auto value = runtime.Find({ storage::EStorageScope::Profile, "profile-a", "editor", "theme" });
	ASSERT_TRUE(value.has_value());
	EXPECT_EQ("dark", value->value);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(EditorControlPlatformRuntime, StopCancelsInFlightClientJoinsWorkerAndIsIdempotent)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 11), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
	}
	auto options = Options(profile, script);
	options.startupBudget = std::chrono::seconds(1);
	CEditorControlPlatformRuntime runtime(std::move(options), SharedDependencies(reader));
	EditorControlPlatformRuntimeResult startup;
	std::thread starter([&] { startup = runtime.Start(); });
	bool connected = false;
	{
		std::unique_lock lock(script->mutex);
		connected = script->condition.wait_for(lock, std::chrono::seconds(1), [&] { return script->connectEntered; });
	}
	if (!connected) {
		(void)runtime.Stop();
		starter.join();
	}
	ASSERT_TRUE(connected);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	starter.join();
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, startup.code);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	std::scoped_lock lock(script->mutex);
	EXPECT_GE(script->closeCalls, 1);
}

TEST(EditorControlPlatformRuntime, StartupTimeoutCannotBeOverwrittenByLateReadyCompletion)
{
	const auto profile = UniqueProfilePath();
	const auto hash = ComputeCanonicalProfileHash(profile);
	auto reader = std::make_shared<CScriptedReader>();
	reader->results.push_back({ EControlPlatformEndpointDiscoveryDisposition::Discovered, Endpoint(hash, 17), ERROR_SUCCESS, {} });
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 17);
	{
		std::scoped_lock lock(script->mutex);
		script->blockConnect = true;
	}
	auto options = Options(profile, script);
	options.startupBudget = std::chrono::milliseconds(10);
	CEditorControlPlatformRuntime runtime(std::move(options), SharedDependencies(reader));

	ASSERT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, runtime.Start().code);
	{
		std::scoped_lock lock(script->mutex);
		script->releaseConnect = true;
		script->condition.notify_all();
	}
	bool lateAttemptFinished = false;
	{
		std::unique_lock lock(script->mutex);
		lateAttemptFinished = script->condition.wait_for(lock, std::chrono::seconds(1), [&] {
			return script->exchangeCalls >= 2;
		});
	}
	if (!lateAttemptFinished) {
		(void)runtime.Stop();
	}
	ASSERT_TRUE(lateAttemptFinished);
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::HardFailure, runtime.Start().code);
	EXPECT_FALSE(runtime.Identity().has_value());
	EXPECT_EQ(EEditorControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

} // namespace
} // namespace platform::controlipc
