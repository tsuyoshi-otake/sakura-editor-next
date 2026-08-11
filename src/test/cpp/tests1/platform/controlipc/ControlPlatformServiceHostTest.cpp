/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/controlipc/ControlPlatformServiceHost.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "platform/secrets/CInMemorySecretVaultService.h"
#include "platform/secrets/CSecretVaultCapabilityService.h"
#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char kProfileAuthorityId[] = "0123456789abcdef0123456789abcdef";

std::wstring LifecycleName(ControlPlatformEndpointLifecycle lifecycle)
{
	switch (lifecycle) {
	case ControlPlatformEndpointLifecycle::Starting: return L"Starting";
	case ControlPlatformEndpointLifecycle::Accepting: return L"Accepting";
	case ControlPlatformEndpointLifecycle::Stopping: return L"Stopping";
	case ControlPlatformEndpointLifecycle::Stopped: return L"Stopped";
	}
	return L"Unknown";
}

struct Trace {
	std::vector<std::wstring> events;
	std::vector<ControlPlatformEndpointSnapshot> snapshots;
	ControlIpcNamedPipeOptions pipeOptions;
	bool adapterGateClosedAtPipeStop = false;
};

class CRecordingEndpoint final : public IControlPlatformServiceEndpoint {
public:
	explicit CRecordingEndpoint(std::shared_ptr<Trace> trace) : m_trace(std::move(trace)) {}

	bool CreateForControl(const std::filesystem::path& profileDirectory, std::wstring& diagnostic) override
	{
		m_trace->events.push_back(L"endpoint.create:" + profileDirectory.wstring());
		diagnostic = m_createDiagnostic;
		return m_createSucceeds;
	}

	void Close() noexcept override { m_trace->events.push_back(L"endpoint.close"); }

	bool Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic) override
	{
		m_trace->snapshots.push_back(snapshot);
		m_trace->events.push_back(L"endpoint.publish:" + LifecycleName(snapshot.lifecycle));
		diagnostic = m_publishDiagnostic;
		if (snapshot.lifecycle == ControlPlatformEndpointLifecycle::Starting) return m_startingPublishSucceeds;
		if (snapshot.lifecycle == ControlPlatformEndpointLifecycle::Accepting) return m_acceptingPublishSucceeds;
		return true;
	}

	const std::wstring& ProfileHash() const noexcept override { return m_profileHash; }

	bool m_createSucceeds = true;
	bool m_startingPublishSucceeds = true;
	bool m_acceptingPublishSucceeds = true;
	std::wstring m_createDiagnostic = L"endpoint create failed";
	std::wstring m_publishDiagnostic = L"endpoint publish failed";
	std::wstring m_profileHash = kProfileHash;

private:
	std::shared_ptr<Trace> m_trace;
};

class CRecordingPipeServer final : public IControlPlatformServicePipeServer {
public:
	CRecordingPipeServer(std::shared_ptr<Trace> trace, std::shared_ptr<IControlIpcFrameHandler> handler) :
		m_trace(std::move(trace)), m_handler(std::move(handler)) {}

	ControlIpcTransportResult Start(const ControlIpcNamedPipeOptions& options) override
	{
		m_trace->events.push_back(L"pipe.start:" + options.pipeName);
		m_trace->pipeOptions = options;
		return m_startResult;
	}

	void Stop() noexcept override
	{
		m_trace->adapterGateClosedAtPipeStop = !m_handler || !m_handler->CreateSession({ 1, 1 });
		m_trace->events.push_back(L"pipe.stop");
	}

	ControlIpcTransportResult m_startResult{ true, EControlIpcTransportDisconnectReason::None, 0, L"" };

private:
	std::shared_ptr<Trace> m_trace;
	std::shared_ptr<IControlIpcFrameHandler> m_handler;
};

struct TestComposition {
	std::shared_ptr<Trace> trace = std::make_shared<Trace>();
	CRecordingEndpoint* endpoint = nullptr;
	CRecordingPipeServer* pipeServer = nullptr;
	ControlPlatformServiceHostDependencies Dependencies()
	{
		return {
			[this] {
				auto endpoint = std::make_unique<CRecordingEndpoint>(trace);
				this->endpoint = endpoint.get();
				return endpoint;
			},
			[this](std::shared_ptr<IControlIpcFrameHandler> handler) {
				auto server = std::make_unique<CRecordingPipeServer>(trace, std::move(handler));
				this->pipeServer = server.get();
				return server;
			},
		};
	}
};

ControlPlatformServiceHostOptions ValidOptions()
{
	ControlPlatformServiceHostOptions options;
	options.profileDirectory = L"C:\\test-profile";
	options.profileId = kProfileAuthorityId;
	options.authorityGeneration = 9;
	options.pipeOptions.pipeName = L"\\\\.\\pipe\\caller-must-not-control-identity";
	options.pipeOptions.maximumSessions = 3;
	options.pipeOptions.maximumQueuedBytes = 4096;
	options.pipeOptions.readBufferBytes = 1024;
	options.pipeOptions.ioTimeout = std::chrono::milliseconds(250);
	return options;
}

std::shared_ptr<storage::IStorageAuthority> Storage()
{
	return std::make_shared<storage::CInMemoryStorageService>(9);
}

std::shared_ptr<secrets::ISecretVaultService> Vault()
{
	return std::make_shared<secrets::CInMemorySecretVaultService>(kProfileAuthorityId);
}

std::shared_ptr<secrets::ISecretVaultCapabilityService> Capabilities()
{
	return std::make_shared<secrets::CSecretVaultCapabilityService>(kProfileAuthorityId);
}

std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> GrantAuthority(
	std::string profileId = kProfileAuthorityId, std::uint64_t generation = 9)
{
	return std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>(std::move(profileId), generation);
}

class CNoopMigration final : public secrets::ISecretVaultLegacyMigrationCoordinator {
public:
	secrets::SecretVaultLegacyMigrationResult EnsureMigrated(std::string_view) override
	{
		return { secrets::ESecretVaultLegacyMigrationStatus::Migrated };
	}
	secrets::ESecretVaultLegacyMigrationStopStatus Stop() noexcept override
	{
		return secrets::ESecretVaultLegacyMigrationStopStatus::Stopped;
	}
};

std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> Migration()
{
	return std::make_shared<CNoopMigration>();
}

std::shared_ptr<profiles::ControlUserDataProfileRegistry> ProfileRegistry(
	const std::shared_ptr<storage::IStorageService>& storage)
{
	auto profiles = std::make_shared<profiles::ControlUserDataProfileRegistry>(storage);
	if (!profiles->Start().Succeeded()) throw std::runtime_error("profile registry did not start");
	return profiles;
}

CControlPlatformServiceHost MakeHost(TestComposition& composition, ControlPlatformServiceHostOptions options = ValidOptions())
{
	auto storage = Storage();
	auto profiles = ProfileRegistry(storage);
	return CControlPlatformServiceHost(std::move(options), storage, Vault(), Capabilities(), GrantAuthority(), Migration(), profiles, composition.Dependencies());
}

TEST(ControlPlatformServiceHost, StartsAndStopsInTheRequiredOrderWithDerivedIdentity)
{
	TestComposition composition;
	auto host = MakeHost(composition);

	const auto start = host.Start();
	ASSERT_EQ(EControlPlatformServiceHostResultCode::Started, start.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Started, start.state);
	EXPECT_EQ(EControlPlatformServiceHostState::Started, host.State());

	const auto expectedPipeName = BuildControlPipeName(kProfileHash);
	ASSERT_EQ(2u, composition.trace->snapshots.size());
	for (const auto& snapshot : composition.trace->snapshots) {
		EXPECT_EQ(kProfileAuthorityId, snapshot.profileId);
		EXPECT_EQ(kProfileHash, snapshot.profileHash);
		EXPECT_EQ(expectedPipeName, snapshot.pipeName);
		EXPECT_EQ(9u, snapshot.generation);
	}
	EXPECT_EQ(expectedPipeName, composition.trace->pipeOptions.pipeName);
	EXPECT_EQ(3u, composition.trace->pipeOptions.maximumSessions);
	EXPECT_EQ(4096u, composition.trace->pipeOptions.maximumQueuedBytes);
	EXPECT_EQ(1024u, composition.trace->pipeOptions.readBufferBytes);
	EXPECT_EQ(std::chrono::milliseconds(250), composition.trace->pipeOptions.ioTimeout);
	EXPECT_EQ((std::vector<std::wstring>{
		L"endpoint.create:C:\\test-profile", L"endpoint.publish:Starting",
		L"pipe.start:" + expectedPipeName, L"endpoint.publish:Accepting" }), composition.trace->events);

	const auto stop = host.Stop();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::Stopped, stop.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, stop.state);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, host.State());
	EXPECT_EQ((std::vector<std::wstring>{
		L"endpoint.create:C:\\test-profile", L"endpoint.publish:Starting",
		L"pipe.start:" + expectedPipeName, L"endpoint.publish:Accepting",
		L"endpoint.publish:Stopping", L"pipe.stop", L"endpoint.publish:Stopped", L"endpoint.close" }), composition.trace->events);
	EXPECT_TRUE(composition.trace->adapterGateClosedAtPipeStop);
}

TEST(ControlPlatformServiceHost, PublishesTheSameImmutableAuthorityIdentityForEveryLifecycleSnapshot)
{
	TestComposition composition;
	auto host = MakeHost(composition);
	ASSERT_EQ(EControlPlatformServiceHostResultCode::Started, host.Start().code);
	ASSERT_EQ(EControlPlatformServiceHostResultCode::Stopped, host.Stop().code);
	ASSERT_EQ(4u, composition.trace->snapshots.size());
	for (const auto& snapshot : composition.trace->snapshots) {
		EXPECT_EQ(kProfileAuthorityId, snapshot.profileId);
		EXPECT_EQ(9u, snapshot.generation);
	}
}

TEST(ControlPlatformServiceHost, RepeatedTerminalCallsAreIdempotent)
{
	TestComposition composition;
	auto host = MakeHost(composition);
	EXPECT_EQ(EControlPlatformServiceHostResultCode::Started, host.Start().code);
	const auto afterFirstStart = composition.trace->events;
	EXPECT_EQ(EControlPlatformServiceHostResultCode::AlreadyStarted, host.Start().code);
	EXPECT_EQ(afterFirstStart, composition.trace->events);
	EXPECT_EQ(EControlPlatformServiceHostResultCode::Stopped, host.Stop().code);
	const auto afterFirstStop = composition.trace->events;
	EXPECT_EQ(EControlPlatformServiceHostResultCode::AlreadyStopped, host.Stop().code);
	EXPECT_EQ(afterFirstStop, composition.trace->events);
}

TEST(ControlPlatformServiceHost, InvalidInputHasNoEndpointOrTransportSideEffects)
{
	TestComposition composition;
	auto options = ValidOptions();
	options.profileId.assign(1, static_cast<char>(0xff));
	auto host = MakeHost(composition, std::move(options));

	const auto result = host.Start();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::InvalidOptions, result.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, host.State());
	EXPECT_TRUE(composition.trace->events.empty());
	EXPECT_EQ(nullptr, composition.endpoint);
	EXPECT_EQ(nullptr, composition.pipeServer);
}

TEST(ControlPlatformServiceHost, RejectsSecretAuthoritiesThatDoNotMatchThePublishedCanonicalProfile)
{
	TestComposition composition;
	auto storage = Storage();
	auto vault = std::make_shared<secrets::CInMemorySecretVaultService>("fedcba9876543210fedcba9876543210");
	auto capabilities = Capabilities();
	CControlPlatformServiceHost host(ValidOptions(), storage, std::move(vault), std::move(capabilities), GrantAuthority(), Migration(), ProfileRegistry(storage),
		composition.Dependencies());

	const auto result = host.Start();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::InvalidOptions, result.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
	EXPECT_TRUE(composition.trace->events.empty());
}

TEST(ControlPlatformServiceHost, RequiresTheControlOwnedMigrationCoordinatorBeforeEndpointCreation)
{
	TestComposition composition;
	auto storage = Storage();
	CControlPlatformServiceHost host(ValidOptions(), storage, Vault(), Capabilities(), GrantAuthority(), nullptr, ProfileRegistry(storage),
		composition.Dependencies());

	const auto result = host.Start();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::InvalidOptions, result.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
	EXPECT_TRUE(composition.trace->events.empty());
}

TEST(ControlPlatformServiceHost, RejectsGrantAuthoritiesThatDoNotMatchThePublishedControlIdentity)
{
	for (const auto& grantAuthority : {
		GrantAuthority("fedcba9876543210fedcba9876543210"),
		GrantAuthority(kProfileAuthorityId, 10),
	}) {
		TestComposition composition;
		auto storage = Storage();
		CControlPlatformServiceHost host(ValidOptions(), storage, Vault(), Capabilities(), grantAuthority, Migration(), ProfileRegistry(storage),
			composition.Dependencies());
		const auto result = host.Start();
		EXPECT_EQ(EControlPlatformServiceHostResultCode::InvalidOptions, result.code);
		EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
		EXPECT_TRUE(composition.trace->events.empty());
	}
}

TEST(ControlPlatformServiceHost, RejectsEmptyMalformedAndOversizedAuthorityIdentityBeforeEndpointCreation)
{
	for (const std::string invalidProfileId : {
		std::string{},
		std::string("0123456789abcdef0123456789abcdeF"),
		std::string("0123456789abcdef0123456789abcdef0"),
	}) {
		TestComposition composition;
		auto options = ValidOptions();
		options.profileId = invalidProfileId;
		auto host = MakeHost(composition, std::move(options));
		const auto result = host.Start();
		EXPECT_EQ(EControlPlatformServiceHostResultCode::InvalidOptions, result.code);
		EXPECT_TRUE(composition.trace->events.empty());
	}
}

TEST(ControlPlatformServiceHost, EndpointCreateFailureRollsBackToStopped)
{
	TestComposition composition;
	ControlPlatformServiceHostDependencies dependencies{
		[&composition] {
			auto endpoint = std::make_unique<CRecordingEndpoint>(composition.trace);
			endpoint->m_createSucceeds = false;
			composition.endpoint = endpoint.get();
			return endpoint;
		},
		[&composition](std::shared_ptr<IControlIpcFrameHandler> handler) {
			auto server = std::make_unique<CRecordingPipeServer>(composition.trace, std::move(handler));
			composition.pipeServer = server.get();
			return server;
		},
	};
	auto storage = Storage();
	CControlPlatformServiceHost failing(ValidOptions(), storage, Vault(), Capabilities(), GrantAuthority(), Migration(), ProfileRegistry(storage), std::move(dependencies));

	const auto result = failing.Start();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::EndpointCreateFailed, result.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, failing.State());
	EXPECT_EQ((std::vector<std::wstring>{ L"endpoint.create:C:\\test-profile", L"endpoint.close" }), composition.trace->events);
	EXPECT_EQ(nullptr, composition.pipeServer);
}

TEST(ControlPlatformServiceHost, PipeStartAndAcceptingPublishFailuresStopThePipeBeforeWithdrawingEndpoint)
{
	for (const bool failAcceptingPublish : { false, true }) {
		TestComposition composition;
		ControlPlatformServiceHostDependencies dependencies{
			[&composition, failAcceptingPublish] {
				auto endpoint = std::make_unique<CRecordingEndpoint>(composition.trace);
				endpoint->m_acceptingPublishSucceeds = !failAcceptingPublish;
				composition.endpoint = endpoint.get();
				return endpoint;
			},
			[&composition, failAcceptingPublish](std::shared_ptr<IControlIpcFrameHandler> handler) {
				auto server = std::make_unique<CRecordingPipeServer>(composition.trace, std::move(handler));
				if (!failAcceptingPublish) {
					server->m_startResult = { false, EControlIpcTransportDisconnectReason::IoError, 5, L"bind failed" };
				}
				composition.pipeServer = server.get();
				return server;
			},
		};
		auto storage = Storage();
		CControlPlatformServiceHost host(ValidOptions(), storage, Vault(), Capabilities(), GrantAuthority(), Migration(), ProfileRegistry(storage), std::move(dependencies));

		const auto result = host.Start();
		EXPECT_EQ(failAcceptingPublish ? EControlPlatformServiceHostResultCode::AcceptingPublishFailed :
			EControlPlatformServiceHostResultCode::PipeStartFailed, result.code);
		EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
		EXPECT_EQ(EControlPlatformServiceHostState::Stopped, host.State());
		ASSERT_GE(composition.trace->events.size(), 2u);
		const auto stopped = std::find(composition.trace->events.begin(), composition.trace->events.end(), L"endpoint.publish:Stopped");
		const auto pipeStop = std::find(composition.trace->events.begin(), composition.trace->events.end(), L"pipe.stop");
		ASSERT_NE(composition.trace->events.end(), stopped);
		ASSERT_NE(composition.trace->events.end(), pipeStop);
		EXPECT_LT(std::distance(composition.trace->events.begin(), pipeStop),
			std::distance(composition.trace->events.begin(), stopped));
		EXPECT_EQ(L"endpoint.close", composition.trace->events.back());
		EXPECT_TRUE(composition.trace->adapterGateClosedAtPipeStop);
	}
}

TEST(ControlPlatformServiceHost, StartingPublishFailureWithdrawsEndpointWithoutCreatingTransport)
{
	TestComposition composition;
	ControlPlatformServiceHostDependencies dependencies{
		[&composition] {
			auto endpoint = std::make_unique<CRecordingEndpoint>(composition.trace);
			endpoint->m_startingPublishSucceeds = false;
			composition.endpoint = endpoint.get();
			return endpoint;
		},
		[&composition](std::shared_ptr<IControlIpcFrameHandler> handler) {
			auto server = std::make_unique<CRecordingPipeServer>(composition.trace, std::move(handler));
			composition.pipeServer = server.get();
			return server;
		},
	};
	auto storage = Storage();
	CControlPlatformServiceHost host(ValidOptions(), storage, Vault(), Capabilities(), GrantAuthority(), Migration(), ProfileRegistry(storage), std::move(dependencies));

	const auto result = host.Start();
	EXPECT_EQ(EControlPlatformServiceHostResultCode::StartingPublishFailed, result.code);
	EXPECT_EQ(EControlPlatformServiceHostState::Stopped, result.state);
	EXPECT_EQ(nullptr, composition.pipeServer);
	EXPECT_EQ((std::vector<std::wstring>{
		L"endpoint.create:C:\\test-profile", L"endpoint.publish:Starting",
		L"endpoint.publish:Stopped", L"endpoint.close" }), composition.trace->events);
}

} // namespace
} // namespace platform::controlipc
