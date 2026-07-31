/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/controlipc/ControlPlatformClient.h"

#include <chrono>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kProfileAuthorityId[] = "0123456789abcdef0123456789abcdef";
constexpr char kOtherProfileAuthorityId[] = "fedcba9876543210fedcba9876543210";

ControlPlatformEndpointSnapshot Endpoint(std::uint64_t generation = 7)
{
	return { ::GetCurrentProcessId(), generation, ControlPlatformEndpointLifecycle::Accepting,
		kProfileHash, BuildControlPipeName(kProfileHash), kProfileAuthorityId };
}

ControlIpcFrame Response(const ControlIpcFrame& request, EControlIpcKind kind, std::uint64_t generation,
	std::vector<std::uint8_t> payload = {})
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

ControlProfileRpcRequest ResolveProfileRequest()
{
	ControlProfileRpcRequest request;
	request.operation = EControlProfileRpcOperation::Resolve;
	request.profileId = L"profile-a";
	return request;
}

ControlProfileRpcRequest RenameProfileRequest(std::string operationId = "profile-operation-a")
{
	ControlProfileRpcRequest request;
	request.operation = EControlProfileRpcOperation::Rename;
	request.mutation.operationId = std::move(operationId);
	request.mutation.expectedStorageRevision = 1;
	request.profileId = L"profile-a";
	request.displayName = L"Renamed";
	return request;
}

struct Script {
	std::deque<std::vector<ControlIpcFrame>> responses;
	std::vector<ControlIpcFrame> requests;
	ControlIpcTransportResult connectResult{ true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	int connectCalls = 0;
	int exchangeCalls = 0;
	int closeCalls = 0;
	bool throwOnConnect = false;
	std::function<void()> onConnect;
};

class CScriptedEndpointReader final : public IControlPlatformEndpointReader {
public:
	std::optional<ControlPlatformEndpointSnapshot> Read(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		++readCalls;
		lastRequirements = requirements;
		if (throwOnRead) throw std::runtime_error("endpoint reader failed");
		return endpoint;
	}

	ControlPlatformEndpointDiscoveryResult ReadDetailed(const ControlPlatformEndpointReadRequirements& requirements) override
	{
		if (detailedResult) {
			++readCalls;
			lastRequirements = requirements;
			return *detailedResult;
		}
		return IControlPlatformEndpointReader::ReadDetailed(requirements);
	}

	std::optional<ControlPlatformEndpointSnapshot> endpoint = Endpoint();
	ControlPlatformEndpointReadRequirements lastRequirements;
	int readCalls = 0;
	bool throwOnRead = false;
	std::optional<ControlPlatformEndpointDiscoveryResult> detailedResult;
};

class CScriptedChannel final : public IControlPlatformClientChannel {
public:
	explicit CScriptedChannel(std::shared_ptr<Script> script) : m_script(std::move(script)) {}

	ControlIpcTransportResult Connect(const ControlPlatformEndpointSnapshot&, std::chrono::milliseconds) override
	{
		++m_script->connectCalls;
		if (m_script->onConnect) m_script->onConnect();
		if (m_script->throwOnConnect) throw std::runtime_error("connect failed unexpectedly");
		return m_script->connectResult;
	}

	ControlIpcTransportResult Exchange(const ControlIpcFrame& request, std::vector<ControlIpcFrame>& responses,
		std::chrono::milliseconds) override
	{
		++m_script->exchangeCalls;
		m_script->requests.push_back(request);
		if (m_script->responses.empty()) return { false, EControlIpcTransportDisconnectReason::IoError, 0, L"script exhausted" };
		responses = std::move(m_script->responses.front());
		m_script->responses.pop_front();
		for (auto& response : responses) {
			// Tests describe only kind/generation/payload; request correlation is owned by the fake transport.
			response.header.requestId = request.header.requestId;
		}
		return { true, EControlIpcTransportDisconnectReason::None, 0, L"" };
	}

	void Close() noexcept override { ++m_script->closeCalls; }

private:
	std::shared_ptr<Script> m_script;
};

ControlPlatformClientOptions Options(std::shared_ptr<Script> script,
	std::chrono::steady_clock::time_point& now, std::uint32_t retries = 3)
{
	ControlPlatformClientOptions options;
	options.profileId = kProfileAuthorityId;
	options.profileHash = kProfileHash;
	options.exchangeDeadline = std::chrono::milliseconds(10);
	options.retryBaseDelay = std::chrono::milliseconds(10);
	options.retryMaximumDelay = std::chrono::milliseconds(40);
	options.maximumRetryAttempts = retries;
	options.retryJitterSalt = 0;
	options.now = [&now] { return now; };
	options.channelFactory = [script] { return std::make_unique<CScriptedChannel>(script); };
	return options;
}

storage::StorageSnapshot Snapshot(std::uint64_t generation)
{
	storage::StorageSnapshot snapshot;
	snapshot.generation = generation;
	snapshot.revision = 1;
	snapshot.entries.push_back({ { storage::EStorageScope::Profile, "profile-a", "tests", "theme" },
		storage::EStorageTarget::User, "dark", 1 });
	return snapshot;
}

void QueueHealthyBootstrap(const std::shared_ptr<Script>& script, std::uint64_t generation = 7)
{
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	const auto snapshot = EncodeControlStorageSnapshotResponse(Snapshot(generation));
	ASSERT_TRUE(hello);
	ASSERT_TRUE(snapshot);
	// request IDs are filled by CScriptedChannel on exchange.
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, generation }, *hello } });
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 2, generation }, *snapshot } });
}

TEST(ControlPlatformClient, DiscoversPinsGenerationAndReplacesCacheFromOneFullSnapshot)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);

	const auto result = client.EnsureReady();
	EXPECT_EQ(EControlPlatformClientOutcome::Ready, result.outcome);
	EXPECT_EQ(EControlPlatformClientState::Ready, client.GetState());
	EXPECT_EQ(7u, client.GetPinnedGeneration());
	EXPECT_EQ(7u, cache.GetGeneration());
	EXPECT_EQ(1u, cache.GetRevision());
	EXPECT_EQ(1, script->connectCalls);
	EXPECT_EQ(2, script->exchangeCalls);
	EXPECT_EQ(EControlPlatformClientOutcome::AlreadyReady, client.EnsureReady().outcome);
}

TEST(ControlPlatformClient, AppliesFrozenDescriptorGenerationAsTheInitialAntiRollbackFloor)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script, 9);
	CScriptedEndpointReader endpointReader;
	endpointReader.endpoint = Endpoint(9);
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	auto options = Options(script, now);
	options.minimumGeneration = 9;
	CControlPlatformClient client(std::move(options), endpointReader, cache);

	EXPECT_EQ(EControlPlatformClientOutcome::Ready, client.EnsureReady().outcome);
	EXPECT_EQ(9u, endpointReader.lastRequirements.minimumGeneration);
	EXPECT_EQ(9u, client.GetPinnedGeneration());
	EXPECT_EQ(9u, cache.GetGeneration());
}

TEST(ControlPlatformClient, RejectsProfileMismatchAndInvalidatesOnGenerationMismatch)
{
	auto now = std::chrono::steady_clock::time_point{};
	{
		auto script = std::make_shared<Script>();
		const auto wrongProfile = EncodeControlStorageHello(kOtherProfileAuthorityId);
		ASSERT_TRUE(wrongProfile);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, 7 }, *wrongProfile } });
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		cache.Replace(Snapshot(6));
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
		EXPECT_EQ(0u, cache.GetGeneration());
	}
	{
		auto script = std::make_shared<Script>();
		const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
		const auto mismatchedSnapshot = EncodeControlStorageSnapshotResponse(Snapshot(8));
		ASSERT_TRUE(hello);
		ASSERT_TRUE(mismatchedSnapshot);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, 7 }, *hello } });
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 2, 7 }, *mismatchedSnapshot } });
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		cache.Replace(Snapshot(6));
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		EXPECT_EQ(EControlPlatformClientOutcome::ReconnectRequired, client.EnsureReady().outcome);
		EXPECT_EQ(EControlPlatformClientState::ReconnectRequired, client.GetState());
		EXPECT_TRUE(client.GetNextRetryTime().has_value());
		EXPECT_EQ(0u, cache.GetGeneration());
		EXPECT_EQ(0u, cache.GetRevision());
	}
}

TEST(ControlPlatformClient, RejectsEndpointProfileMismatchOrMalformedIdentityBeforePipeConnection)
{
	for (const std::string endpointProfileId : {
		std::string(kOtherProfileAuthorityId),
		std::string("0123456789abcdef0123456789abcdeF"),
		std::string{},
	}) {
		auto script = std::make_shared<Script>();
		CScriptedEndpointReader endpointReader;
		endpointReader.endpoint->profileId = endpointProfileId;
		storage::CStorageSnapshotCache cache;
		auto now = std::chrono::steady_clock::time_point{};
		CControlPlatformClient client(Options(script, now), endpointReader, cache);

		const auto result = client.EnsureReady();
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::ProfileMismatch, result.terminalStatus);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
		EXPECT_EQ(0, script->connectCalls);
		EXPECT_EQ(0, script->exchangeCalls);
	}
}

TEST(ControlPlatformClient, DeduplicatesReentrantCallersAndShutdownWinsOverInFlightWork)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	auto client = std::make_unique<CControlPlatformClient>(Options(script, now), endpointReader, cache);
	ControlPlatformClientResult second;
	script->onConnect = [&] { second = client->EnsureReady(); };
	EXPECT_EQ(EControlPlatformClientOutcome::Ready, client->EnsureReady().outcome);
	EXPECT_EQ(EControlPlatformClientOutcome::ConnectionInFlight, second.outcome);
	EXPECT_EQ(1, script->connectCalls);

	auto stoppingScript = std::make_shared<Script>();
	CScriptedEndpointReader stoppingReader;
	storage::CStorageSnapshotCache stoppingCache;
	auto stoppingClient = std::make_unique<CControlPlatformClient>(Options(stoppingScript, now), stoppingReader, stoppingCache);
	stoppingScript->onConnect = [&] { stoppingClient->Stop(); };
	EXPECT_EQ(EControlPlatformClientOutcome::Stopped, stoppingClient->EnsureReady().outcome);
	EXPECT_EQ(EControlPlatformClientState::Stopped, stoppingClient->GetState());
	EXPECT_GE(stoppingScript->closeCalls, 1);
	EXPECT_FALSE(stoppingClient->GetNextRetryTime().has_value());
	EXPECT_EQ(EControlPlatformClientOutcome::Stopped, stoppingClient->EnsureReady().outcome);
}

TEST(ControlPlatformClient, ActiveResnapshotFencesTheOldAttemptBeforeItCanPublishReady)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	cache.Replace(Snapshot(6));
	auto now = std::chrono::steady_clock::time_point{};
	auto client = std::make_unique<CControlPlatformClient>(Options(script, now), endpointReader, cache);
	ControlPlatformClientResult resnapshot;
	script->onConnect = [&] { resnapshot = client->RequireResnapshot(8); };

	const auto result = client->EnsureReady();

	EXPECT_EQ(EControlPlatformClientOutcome::ReconnectRequired, resnapshot.outcome);
	EXPECT_EQ(EControlPlatformClientOutcome::ReconnectRequired, result.outcome);
	EXPECT_EQ(EControlPlatformClientState::ReconnectRequired, client->GetState());
	EXPECT_EQ(0u, client->GetPinnedGeneration());
	EXPECT_EQ(0u, cache.GetGeneration());
	EXPECT_EQ(0, script->exchangeCalls);
	EXPECT_GE(script->closeCalls, 1);
}

TEST(ControlPlatformClient, RejectsMalformedSnapshotBeforeTheCacheCanBecomeReady)
{
	auto script = std::make_shared<Script>();
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	auto malformed = Snapshot(7);
	malformed.entries.front().revision = 0;
	const auto encoded = EncodeControlStorageSnapshotResponse(malformed);
	ASSERT_TRUE(hello);
	ASSERT_TRUE(encoded);
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, 7 }, *hello } });
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageSnapshotResponse,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 2, 7 }, *encoded } });
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);

	const auto result = client.EnsureReady();

	EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, result.outcome);
	EXPECT_EQ(EControlIpcTerminalStatus::ProtocolError, result.terminalStatus);
	EXPECT_EQ(0u, cache.GetGeneration());
	EXPECT_EQ(0u, cache.GetRevision());
}

TEST(ControlPlatformClient, PreservesTypedDiscoveryAndTransportFailuresWithoutDiagnosticParsing)
{
	auto now = std::chrono::steady_clock::time_point{};
	for (const auto disposition : {
		EControlPlatformEndpointDiscoveryDisposition::NotPublished,
		EControlPlatformEndpointDiscoveryDisposition::NotAccepting,
		EControlPlatformEndpointDiscoveryDisposition::DeadOrStale,
		EControlPlatformEndpointDiscoveryDisposition::Busy,
		EControlPlatformEndpointDiscoveryDisposition::ResourceOrIoFailure,
	}) {
		auto script = std::make_shared<Script>();
		CScriptedEndpointReader endpointReader;
		endpointReader.detailedResult = { disposition, std::nullopt, ERROR_RETRY, L"untrusted diagnostic" };
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);

		const auto result = client.EnsureReady();
		EXPECT_EQ(EControlPlatformClientOutcome::RetryScheduled, result.outcome);
		EXPECT_EQ(disposition, result.discoveryDisposition);
		EXPECT_EQ(EControlIpcTransportDisconnectReason::None, result.transportReason);
		EXPECT_TRUE(client.GetNextRetryTime().has_value());
		EXPECT_EQ(0, script->connectCalls);
	}
	for (const auto disposition : {
		EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor,
		EControlPlatformEndpointDiscoveryDisposition::Closed,
		EControlPlatformEndpointDiscoveryDisposition::AccessDenied,
		EControlPlatformEndpointDiscoveryDisposition::SecurityRejected,
		EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi,
	}) {
		auto script = std::make_shared<Script>();
		CScriptedEndpointReader endpointReader;
		endpointReader.detailedResult = { disposition, std::nullopt, ERROR_ACCESS_DENIED, L"untrusted diagnostic" };
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);

		const auto result = client.EnsureReady();
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, result.outcome);
		EXPECT_EQ(disposition, result.discoveryDisposition);
		EXPECT_FALSE(client.GetNextRetryTime().has_value());
		EXPECT_EQ(0, script->connectCalls);
	}
	for (const auto [reason, expected] : {
		std::pair{ EControlIpcTransportDisconnectReason::PeerClosed, EControlPlatformClientOutcome::RetryScheduled },
		std::pair{ EControlIpcTransportDisconnectReason::AccessDenied, EControlPlatformClientOutcome::Unavailable },
		std::pair{ EControlIpcTransportDisconnectReason::ProtocolError, EControlPlatformClientOutcome::Unavailable },
	}) {
		auto script = std::make_shared<Script>();
		script->connectResult = { false, reason, ERROR_ACCESS_DENIED, L"untrusted diagnostic" };
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);

		const auto result = client.EnsureReady();
		EXPECT_EQ(expected, result.outcome);
		EXPECT_EQ(reason, result.transportReason);
		EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::Discovered, result.discoveryDisposition);
	}
}

TEST(ControlPlatformClient, ReconnectRequiredSchedulesABoundedRetryDeadline)
{
	auto script = std::make_shared<Script>();
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 1, 8 }, *hello } });
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);

	const auto first = client.EnsureReady();
	EXPECT_EQ(EControlPlatformClientOutcome::ReconnectRequired, first.outcome);
	const auto retry = client.GetNextRetryTime();
	ASSERT_TRUE(retry);
	EXPECT_GT(*retry, now);
	EXPECT_EQ(EControlPlatformClientOutcome::RetryScheduled, client.EnsureReady().outcome);
	EXPECT_EQ(1, script->connectCalls);
}

TEST(ControlPlatformClient, InjectedExceptionsReachUnavailableAndClearInFlightState)
{
	auto now = std::chrono::steady_clock::time_point{};
	{
		auto script = std::make_shared<Script>();
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		cache.Replace(Snapshot(6));
		auto options = Options(script, now);
		options.now = []() -> std::chrono::steady_clock::time_point { throw std::runtime_error("clock"); };
		CControlPlatformClient client(std::move(options), endpointReader, cache);
		EXPECT_EQ(0u, cache.GetGeneration());
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
	}
	{
		auto script = std::make_shared<Script>();
		CScriptedEndpointReader endpointReader;
		endpointReader.throwOnRead = true;
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
	}
	{
		auto script = std::make_shared<Script>();
		script->throwOnConnect = true;
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
	}
}

TEST(ControlPlatformClient, BoundsRetryWithoutSleepingAndRejectsMultipleTerminalFrames)
{
	auto now = std::chrono::steady_clock::time_point{};
	{
		auto script = std::make_shared<Script>();
		script->connectResult = { false, EControlIpcTransportDisconnectReason::ConnectFailed, 0, L"unavailable" };
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now, 2), endpointReader, cache);
		EXPECT_EQ(EControlPlatformClientOutcome::RetryScheduled, client.EnsureReady().outcome);
		const auto firstRetry = client.GetNextRetryTime();
		ASSERT_TRUE(firstRetry);
		EXPECT_EQ(EControlPlatformClientOutcome::RetryScheduled, client.EnsureReady().outcome);
		EXPECT_EQ(1, script->connectCalls);
		now = *firstRetry;
		EXPECT_EQ(EControlPlatformClientOutcome::RetryScheduled, client.EnsureReady().outcome);
		const auto secondRetry = client.GetNextRetryTime();
		ASSERT_TRUE(secondRetry);
		EXPECT_GT(*secondRetry, *firstRetry);
		now = *secondRetry;
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, client.EnsureReady().outcome);
		EXPECT_EQ(3, script->connectCalls);
	}
	{
		auto script = std::make_shared<Script>();
		const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
		ASSERT_TRUE(hello);
		auto one = Response({ { 1, 0, EControlIpcKind::Hello, EControlIpcFlags::Request, 1, 0 }, {} }, EControlIpcKind::HelloAck, 7, *hello);
		auto two = one;
		script->responses.push_back({ one, two });
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		const auto result = client.EnsureReady();
		EXPECT_EQ(EControlPlatformClientOutcome::Unavailable, result.outcome);
		EXPECT_EQ(EControlIpcTerminalStatus::ProtocolError, result.terminalStatus);
		EXPECT_EQ(EControlPlatformClientState::Unavailable, client.GetState());
	}
}

TEST(ControlPlatformClient, ExposesMutationReplayContractWithoutTransportReplay)
{
	storage::StorageMutationRequest request{ "operation-a", 0,
		{ { { storage::EStorageScope::Profile, "profile-a", "tests", "key" }, storage::EStorageTarget::User, "value" } } };
	EXPECT_TRUE(CControlPlatformClient::IsMutationRequestReplaySafe(request));
	EXPECT_EQ(EControlPlatformRetryClass::MutationRetryWithSameOperationId,
		CControlPlatformClient::ClassifyAmbiguousMutationTransportFailure(request));
	EXPECT_EQ(EControlPlatformRetryClass::None,
		CControlPlatformClient::ClassifyMutationResult(request, { storage::EStorageMutationStatus::Failed }));
	EXPECT_EQ(EControlPlatformRetryClass::MutationConflictDoNotRetry,
		CControlPlatformClient::ClassifyMutationResult(request, { storage::EStorageMutationStatus::Conflict }));
	request.operationId.clear();
	EXPECT_FALSE(CControlPlatformClient::IsMutationRequestReplaySafe(request));
	EXPECT_EQ(EControlPlatformRetryClass::None,
		CControlPlatformClient::ClassifyAmbiguousMutationTransportFailure(request));

	request.operationId.assign(storage::kMaximumStorageOperationIdBytes + 1, 'x');
	EXPECT_FALSE(CControlPlatformClient::IsMutationRequestReplaySafe(request));
	request.operationId = "operation-b";
	request.mutations.push_back(request.mutations.front());
	EXPECT_FALSE(CControlPlatformClient::IsMutationRequestReplaySafe(request));
	request.mutations.pop_back();
	request.mutations.front().value = std::string("\xff", 1);
	EXPECT_FALSE(CControlPlatformClient::IsMutationRequestReplaySafe(request));
}

TEST(ControlPlatformClient, AppliesReadyOnlyMutationOverFreshHelloChannelAndUpdatesCache)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	storage::StorageMutationRequest request{ "layout-operation-a", 1,
		{ { { storage::EStorageScope::Profile, "profile-a", "workbench.layout", "state" },
			storage::EStorageTarget::User, "{\"version\":1}" } } };
	storage::StorageMutationResult storageResult;
	storageResult.status = storage::EStorageMutationStatus::Succeeded;
	storageResult.revision = 2;
	storageResult.changeBatch = storage::StorageChangeBatch{ 7, 1, 2,
		{ { request.mutations.front().address, storage::EStorageTarget::User,
			storage::StorageEntry{ request.mutations.front().address, storage::EStorageTarget::User,
				*request.mutations.front().value, 2 } } } };
	const auto apply = EncodeControlStorageApplyResponse(storageResult);
	ASSERT_TRUE(apply);
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::StorageApplyResponse,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 7 }, *apply } });
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);
	ASSERT_TRUE(client.EnsureReady().IsReady());

	const auto result = client.Apply(request);
	EXPECT_EQ(EControlPlatformMutationOutcome::Succeeded, result.outcome);
	EXPECT_EQ(storage::EStorageMutationStatus::Succeeded, result.storageResult.status);
	EXPECT_EQ(2u, cache.GetRevision());
	const auto stored = cache.Find(request.mutations.front().address);
	ASSERT_TRUE(stored.has_value());
	EXPECT_EQ(*request.mutations.front().value, stored->value);
	ASSERT_EQ(4u, script->requests.size());
	EXPECT_EQ(EControlIpcKind::Hello, script->requests[2].header.kind);
	EXPECT_EQ(EControlIpcKind::StorageApplyRequest, script->requests[3].header.kind);
	EXPECT_EQ(7u, script->requests[3].header.generation);
	const auto decodedRequest = DecodeControlStorageApplyRequest(script->requests[3].payload);
	ASSERT_TRUE(decodedRequest.has_value());
	EXPECT_EQ(request, *decodedRequest);
}

TEST(ControlPlatformClient, ReturnsSameOperationRetryOnlyWhenApplyExchangeIsAmbiguous)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);
	ASSERT_TRUE(client.EnsureReady().IsReady());
	storage::StorageMutationRequest request{ "layout-operation-b", 1,
		{ { { storage::EStorageScope::Profile, "profile-a", "workbench.layout", "state" },
			storage::EStorageTarget::User, "{\"version\":1}" } } };

	const auto result = client.Apply(request);
	EXPECT_EQ(EControlPlatformMutationOutcome::RetryWithSameOperationId, result.outcome);
	EXPECT_EQ(EControlIpcTransportDisconnectReason::IoError, result.transportReason);
	ASSERT_EQ(4u, script->requests.size());
	const auto decodedRequest = DecodeControlStorageApplyRequest(script->requests.back().payload);
	ASSERT_TRUE(decodedRequest.has_value());
	EXPECT_EQ(request, *decodedRequest);
}

TEST(ControlPlatformClient, ExecutesGenerationPinnedProfileReadAfterFreshHelloAndReturnsOneDecodedResponse)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
	ASSERT_TRUE(hello);
	ControlProfileRpcResponse profile;
	profile.terminalStatus = EControlIpcTerminalStatus::Succeeded;
	profile.result.status = profiles::ControlUserDataProfileRegistryStatus::Resolved;
	profile.result.storageRevision = 1;
	profile.result.resolved = profiles::UserDataProfileResolveResult{};
	profile.result.resolved->profile = profiles::UserDataProfileDescriptor{ L"profile-a", L"Profile A",
		profiles::UserDataProfileKind::Normal };
	const auto payload = ProfileResponsePayload(profile);
	ASSERT_FALSE(payload.empty());
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
	script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 7 }, payload } });
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);
	EXPECT_EQ(EControlPlatformProfileOutcome::NotReady, client.ExecuteProfile(ResolveProfileRequest()).outcome);
	ASSERT_TRUE(client.EnsureReady().IsReady());
	std::optional<EControlPlatformProfileOutcome> nestedOutcome;
	script->onConnect = [&] { nestedOutcome = client.ExecuteProfile(ResolveProfileRequest()).outcome; };

	const auto result = client.ExecuteProfile(ResolveProfileRequest());
	ASSERT_EQ(EControlPlatformProfileOutcome::Succeeded, result.outcome);
	ASSERT_TRUE(result.response.has_value());
	ASSERT_TRUE(result.response->result.resolved.has_value());
	ASSERT_TRUE(result.response->result.resolved->profile.has_value());
	EXPECT_EQ(L"profile-a", result.response->result.resolved->profile->profileId);
	ASSERT_TRUE(nestedOutcome.has_value());
	EXPECT_EQ(EControlPlatformProfileOutcome::OperationInFlight, *nestedOutcome);
	ASSERT_EQ(4u, script->requests.size());
	EXPECT_EQ(EControlIpcKind::Hello, script->requests[2].header.kind);
	EXPECT_EQ(EControlIpcKind::ProfileRequest, script->requests[3].header.kind);
	EXPECT_EQ(7u, script->requests[3].header.generation);
	const auto outer = DecodeControlIpcFields(script->requests[3].payload);
	ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, outer.outcome);
	ASSERT_EQ(1u, outer.fields.size());
	const auto sent = DecodeControlProfileRpcRequest(outer.fields.front().value);
	ASSERT_TRUE(sent.has_value());
	EXPECT_EQ(ResolveProfileRequest().operation, sent->operation);
}

TEST(ControlPlatformClient, RejectsProfileGenerationMismatchAndMultipleTerminalResponses)
{
	for (const bool multiple : { false, true }) {
		auto script = std::make_shared<Script>();
		QueueHealthyBootstrap(script);
		const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
		ASSERT_TRUE(hello);
		ControlProfileRpcResponse profile;
		profile.terminalStatus = EControlIpcTerminalStatus::Succeeded;
		profile.result.status = profiles::ControlUserDataProfileRegistryStatus::Resolved;
		const auto payload = ProfileResponsePayload(profile);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
		if (multiple) {
			script->responses.push_back({
				{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
					EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 7 }, payload },
				{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
					EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 7 }, payload } });
		}
		else {
			script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
				EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 8 }, payload } });
		}
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		auto now = std::chrono::steady_clock::time_point{};
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		ASSERT_TRUE(client.EnsureReady().IsReady());
		const auto result = client.ExecuteProfile(ResolveProfileRequest());
		EXPECT_EQ(multiple ? EControlPlatformProfileOutcome::Failed : EControlPlatformProfileOutcome::ResnapshotRequired,
			result.outcome);
		EXPECT_EQ(multiple ? EControlIpcTerminalStatus::ProtocolError : EControlIpcTerminalStatus::GenerationMismatch,
			result.terminalStatus);
	}
}

TEST(ControlPlatformClient, ResnapshotClosesAnActiveProfileChannelBeforeItCanDispatchHello)
{
	auto script = std::make_shared<Script>();
	QueueHealthyBootstrap(script);
	CScriptedEndpointReader endpointReader;
	storage::CStorageSnapshotCache cache;
	auto now = std::chrono::steady_clock::time_point{};
	CControlPlatformClient client(Options(script, now), endpointReader, cache);
	ASSERT_TRUE(client.EnsureReady().IsReady());
	std::optional<ControlPlatformClientResult> resnapshot;
	script->onConnect = [&] { resnapshot = client.RequireResnapshot(7); };

	const auto result = client.ExecuteProfile(ResolveProfileRequest());
	ASSERT_TRUE(resnapshot.has_value());
	EXPECT_EQ(EControlPlatformProfileOutcome::ResnapshotRequired, result.outcome);
	EXPECT_EQ(EControlPlatformClientOutcome::ReconnectRequired, resnapshot->outcome);
	EXPECT_EQ(2u, script->requests.size()); // no profile Hello or ProfileRequest after the fence
	EXPECT_GE(script->closeCalls, 1);
}

TEST(ControlPlatformClient, ProfileMutationConflictAndAmbiguousLossPreserveTypedReplayContract)
{
	{
		auto script = std::make_shared<Script>();
		QueueHealthyBootstrap(script);
		const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
		ASSERT_TRUE(hello);
		ControlProfileRpcResponse profile;
		profile.terminalStatus = EControlIpcTerminalStatus::InvalidRequest;
		profile.result.status = profiles::ControlUserDataProfileRegistryStatus::PersistConflict;
		profile.result.storageRevision = 2;
		const auto payload = ProfileResponsePayload(profile);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileResponse,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 4, 7 }, payload } });
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		auto now = std::chrono::steady_clock::time_point{};
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		ASSERT_TRUE(client.EnsureReady().IsReady());
		const auto result = client.ExecuteProfile(RenameProfileRequest());
		EXPECT_EQ(EControlPlatformProfileOutcome::Conflict, result.outcome);
		ASSERT_TRUE(result.response.has_value());
		EXPECT_EQ(profiles::ControlUserDataProfileRegistryStatus::PersistConflict, result.response->result.status);
	}
	{
		auto script = std::make_shared<Script>();
		QueueHealthyBootstrap(script);
		const auto hello = EncodeControlStorageHello(kProfileAuthorityId);
		ASSERT_TRUE(hello);
		script->responses.push_back({ { { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::HelloAck,
			EControlIpcFlags::Response | EControlIpcFlags::Terminal, 3, 7 }, *hello } });
		CScriptedEndpointReader endpointReader;
		storage::CStorageSnapshotCache cache;
		auto now = std::chrono::steady_clock::time_point{};
		CControlPlatformClient client(Options(script, now), endpointReader, cache);
		ASSERT_TRUE(client.EnsureReady().IsReady());
		const auto request = RenameProfileRequest("profile-operation-replay");
		const auto result = client.ExecuteProfile(request);
		EXPECT_EQ(EControlPlatformProfileOutcome::RetryWithSameOperationId, result.outcome);
		EXPECT_EQ(EControlIpcTransportDisconnectReason::IoError, result.transportReason);
		ASSERT_EQ(4u, script->requests.size());
		const auto fields = DecodeControlIpcFields(script->requests.back().payload);
		ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, fields.outcome);
		const auto sent = DecodeControlProfileRpcRequest(fields.fields.front().value);
		ASSERT_TRUE(sent.has_value());
		EXPECT_EQ(request.mutation.operationId, sent->mutation.operationId);
	}
}

} // namespace
} // namespace platform::controlipc
