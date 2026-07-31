/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/controlipc/ControlPlatformRuntime.h"
#include "platform/secrets/CInMemorySecretVaultService.h"
#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";
constexpr wchar_t kProfileHash[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct Trace final {
	std::vector<std::wstring> events;
	std::vector<ControlPlatformEndpointSnapshot> snapshots;
	ControlIpcNamedPipeOptions startedPipeOptions;
	bool pipeStartSucceeds = true;
	bool stoppingPublishSucceeds = true;
};

class RecordingAuthorityLock final : public profiles::IProfileAuthorityStoreLock {
public:
	explicit RecordingAuthorityLock(std::shared_ptr<Trace> trace) : m_trace(std::move(trace)) {}
	~RecordingAuthorityLock() override { m_trace->events.emplace_back(L"authority.lock.release"); }

private:
	std::shared_ptr<Trace> m_trace;
};

class RecordingAuthorityBackend final : public profiles::IProfileAuthorityStoreBackend {
public:
	explicit RecordingAuthorityBackend(std::shared_ptr<Trace> trace) : m_trace(std::move(trace)) {}

	profiles::ProfileAuthorityStoreStatus EnsureMetadataDirectory(const std::filesystem::path&) override
	{
		m_trace->events.emplace_back(L"authority.ensure");
		return ensureStatus;
	}

	profiles::ProfileAuthorityStoreStatus AcquireExclusiveLock(const std::filesystem::path&,
		std::unique_ptr<profiles::IProfileAuthorityStoreLock>& lock) override
	{
		m_trace->events.emplace_back(L"authority.lock.acquire");
		if (lockStatus == profiles::ProfileAuthorityStoreStatus::Succeeded) {
			lock = std::make_unique<RecordingAuthorityLock>(m_trace);
		}
		return lockStatus;
	}

	profiles::ProfileAuthorityStoreStatus ReadRecord(const std::filesystem::path&, std::string& bytes,
		bool& exists) override
	{
		m_trace->events.emplace_back(L"authority.read");
		if (throwOnRead) throw std::runtime_error("injected authority read exception");
		if (readStatus != profiles::ProfileAuthorityStoreStatus::Succeeded) return readStatus;
		exists = durableRecord.has_value();
		bytes = durableRecord.value_or(std::string{});
		return profiles::ProfileAuthorityStoreStatus::Succeeded;
	}

	profiles::ProfileAuthorityStoreStatus GenerateOpaqueProfileId(
		profiles::ProfileAuthorityProfileId& profileId) override
	{
		m_trace->events.emplace_back(L"authority.random");
		if (randomStatus == profiles::ProfileAuthorityStoreStatus::Succeeded) profileId = kProfileId;
		return randomStatus;
	}

	profiles::ProfileAuthorityStoreStatus WriteRecordAtomically(const std::filesystem::path&,
		std::string_view bytes) override
	{
		m_trace->events.emplace_back(L"authority.write");
		if (writeStatus == profiles::ProfileAuthorityStoreStatus::Succeeded) durableRecord = std::string(bytes);
		return writeStatus;
	}

	profiles::ProfileAuthorityStoreStatus ensureStatus = profiles::ProfileAuthorityStoreStatus::Succeeded;
	profiles::ProfileAuthorityStoreStatus lockStatus = profiles::ProfileAuthorityStoreStatus::Succeeded;
	profiles::ProfileAuthorityStoreStatus readStatus = profiles::ProfileAuthorityStoreStatus::Succeeded;
	profiles::ProfileAuthorityStoreStatus randomStatus = profiles::ProfileAuthorityStoreStatus::Succeeded;
	profiles::ProfileAuthorityStoreStatus writeStatus = profiles::ProfileAuthorityStoreStatus::Succeeded;
	bool throwOnRead = false;
	std::optional<std::string> durableRecord;

private:
	std::shared_ptr<Trace> m_trace;
};

enum class StorageOpenMode {
	Opened,
	PrepareIoError,
	WriterBusy,
	WriterIoError,
	ReadIoError,
	CorruptData,
	UnsupportedFormat,
	ThrowOnRead,
};

struct RecordingStorageState final {
	bool writerLocked = false;
};

class RecordingStorageLock final : public storage::IAtomicFileStorageWriterLock {
public:
	RecordingStorageLock(std::shared_ptr<Trace> trace, std::shared_ptr<RecordingStorageState> state) :
		m_trace(std::move(trace)), m_state(std::move(state)) {}
	~RecordingStorageLock() override
	{
		m_state->writerLocked = false;
		m_trace->events.emplace_back(L"storage.lock.release");
	}

private:
	std::shared_ptr<Trace> m_trace;
	std::shared_ptr<RecordingStorageState> m_state;
};

class RecordingStorageFileOperations final : public storage::IAtomicFileStorageFileOperations {
public:
	explicit RecordingStorageFileOperations(std::shared_ptr<Trace> trace) :
		m_trace(std::move(trace)), state(std::make_shared<RecordingStorageState>()) {}

	bool PrepareDirectory(const std::filesystem::path&, std::string& diagnostic) override
	{
		m_trace->events.emplace_back(L"storage.prepare");
		if (mode != StorageOpenMode::PrepareIoError) return true;
		diagnostic = "injected prepare failure";
		return false;
	}

	storage::AtomicFileStorageWriterLockResult AcquireWriterLock(const std::filesystem::path&) override
	{
		m_trace->events.emplace_back(L"storage.lock.acquire");
		if (mode == StorageOpenMode::WriterBusy) {
			return { storage::EAtomicFileStorageWriterLockStatus::Busy, nullptr, "injected busy writer" };
		}
		if (mode == StorageOpenMode::WriterIoError) {
			return { storage::EAtomicFileStorageWriterLockStatus::IoError, nullptr, "injected lock failure" };
		}
		state->writerLocked = true;
		return { storage::EAtomicFileStorageWriterLockStatus::Acquired,
			std::make_unique<RecordingStorageLock>(m_trace, state), {} };
	}

	bool ReadFile(const std::filesystem::path&, std::vector<std::uint8_t>& bytes, bool& found,
		std::string& diagnostic) override
	{
		m_trace->events.emplace_back(L"storage.read");
		if (mode == StorageOpenMode::ThrowOnRead) throw std::runtime_error("injected storage read exception");
		if (mode == StorageOpenMode::ReadIoError) {
			diagnostic = "injected read failure";
			return false;
		}
		if (mode == StorageOpenMode::CorruptData) {
			found = true;
			bytes = { 0 };
			return true;
		}
		if (mode == StorageOpenMode::UnsupportedFormat) {
			found = true;
			bytes.assign(48, 0);
			const std::uint8_t magic[] = { 'S', 'A', 'K', 'S', 'T', 'O', 'R', '1' };
			std::copy(std::begin(magic), std::end(magic), bytes.begin());
			bytes[8] = 2;
			return true;
		}
		found = false;
		bytes.clear();
		return true;
	}

	bool WriteFileAtomically(const std::filesystem::path&, std::span<const std::uint8_t>,
		std::string&) override
	{
		m_trace->events.emplace_back(L"storage.write");
		return true;
	}

	StorageOpenMode mode = StorageOpenMode::Opened;
	std::shared_ptr<RecordingStorageState> state;

private:
	std::shared_ptr<Trace> m_trace;
};

std::wstring LifecycleEvent(ControlPlatformEndpointLifecycle lifecycle)
{
	switch (lifecycle) {
	case ControlPlatformEndpointLifecycle::Starting: return L"host.endpoint.publish.starting";
	case ControlPlatformEndpointLifecycle::Accepting: return L"host.endpoint.publish.accepting";
	case ControlPlatformEndpointLifecycle::Stopping: return L"host.endpoint.publish.stopping";
	case ControlPlatformEndpointLifecycle::Stopped: return L"host.endpoint.publish.stopped";
	}
	return L"host.endpoint.publish.unknown";
}

class RecordingEndpoint final : public IControlPlatformServiceEndpoint {
public:
	explicit RecordingEndpoint(std::shared_ptr<Trace> trace) : m_trace(std::move(trace)) {}

	bool CreateForControl(const std::filesystem::path&, std::wstring&) override
	{
		m_trace->events.emplace_back(L"host.endpoint.create");
		return true;
	}

	void Close() noexcept override { m_trace->events.emplace_back(L"host.endpoint.close"); }

	bool Publish(const ControlPlatformEndpointSnapshot& snapshot, std::wstring& diagnostic) override
	{
		m_trace->events.emplace_back(LifecycleEvent(snapshot.lifecycle));
		m_trace->snapshots.emplace_back(snapshot);
		if (snapshot.lifecycle != ControlPlatformEndpointLifecycle::Stopping || m_trace->stoppingPublishSucceeds) {
			return true;
		}
		diagnostic = L"injected stopping publication failure";
		return false;
	}

	const std::wstring& ProfileHash() const noexcept override { return m_profileHash; }

private:
	std::shared_ptr<Trace> m_trace;
	std::wstring m_profileHash = kProfileHash;
};

class RecordingPipeServer final : public IControlPlatformServicePipeServer {
public:
	RecordingPipeServer(std::shared_ptr<Trace> trace, std::shared_ptr<IControlIpcFrameHandler> handler) :
		m_trace(std::move(trace)), m_handler(std::move(handler)) {}

	ControlIpcTransportResult Start(const ControlIpcNamedPipeOptions& options) override
	{
		m_trace->events.emplace_back(L"host.pipe.start");
		m_trace->startedPipeOptions = options;
		if (m_trace->pipeStartSucceeds) return { true, EControlIpcTransportDisconnectReason::None, 0, {} };
		return { false, EControlIpcTransportDisconnectReason::IoError, 5, L"injected pipe failure" };
	}

	void Stop() noexcept override
	{
		m_trace->events.emplace_back(L"host.pipe.stop");
		m_handler.reset();
	}

private:
	std::shared_ptr<Trace> m_trace;
	std::shared_ptr<IControlIpcFrameHandler> m_handler;
};

class RecordingVault final : public secrets::ISecretVaultService {
public:
	RecordingVault(std::shared_ptr<Trace> trace, std::string profileId) : m_trace(std::move(trace)), m_profileId(std::move(profileId)) {}
	std::string_view GetProfileId() const noexcept override { return m_profileId; }
	secrets::SecretGetResult Get(std::string_view, std::string_view) const override { return {}; }
	secrets::SecretMutationResult Apply(const secrets::SecretMutationRequest&) override { return {}; }
	std::unique_ptr<secrets::ISecretVaultChangeSubscription> Subscribe(secrets::SecretChangeCallback) override { return nullptr; }
	secrets::ESecretVaultStopStatus Stop() noexcept override
	{
		m_trace->events.emplace_back(L"vault.stop");
		return m_stopped ? secrets::ESecretVaultStopStatus::AlreadyStopped : (m_stopped = true, secrets::ESecretVaultStopStatus::Stopped);
	}

private:
	std::shared_ptr<Trace> m_trace;
	std::string m_profileId;
	bool m_stopped = false;
};

class RecordingCapabilities final : public secrets::ISecretVaultCapabilityService {
public:
	RecordingCapabilities(std::shared_ptr<Trace> trace, std::string profileId) : m_trace(std::move(trace)), m_profileId(std::move(profileId)) {}
	std::string_view GetProfileId() const noexcept override { return m_profileId; }
	secrets::SecretVaultCapabilityIssueResult Issue(const secrets::SecretVaultCapabilityIssueRequest&) override { return {}; }
	secrets::SecretVaultCapabilityValidationResult Validate(const secrets::SecretVaultCapabilityValidationRequest&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeExtension(const secrets::SecretVaultCapabilityBinding&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeSession(const secrets::SecretVaultCapabilitySessionIdentity&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeHostSession(
		const secrets::SecretVaultCapabilityHostSessionIdentity&) override { return {}; }
	secrets::ESecretVaultCapabilityStopStatus Stop() noexcept override
	{
		m_trace->events.emplace_back(L"capability.stop");
		return m_stopped ? secrets::ESecretVaultCapabilityStopStatus::AlreadyStopped :
			(m_stopped = true, secrets::ESecretVaultCapabilityStopStatus::Stopped);
	}

private:
	std::shared_ptr<Trace> m_trace;
	std::string m_profileId;
	bool m_stopped = false;
};

class RecordingMigration final : public secrets::ISecretVaultLegacyMigrationCoordinator {
public:
	explicit RecordingMigration(std::shared_ptr<Trace> trace) : m_trace(std::move(trace)) {}

	secrets::SecretVaultLegacyMigrationResult EnsureMigrated(std::string_view) override
	{
		return { secrets::ESecretVaultLegacyMigrationStatus::Migrated };
	}
	secrets::ESecretVaultLegacyMigrationStopStatus Stop() noexcept override
	{
		if (!m_stopped) m_trace->events.emplace_back(L"migration.stop");
		m_stopped = true;
		return secrets::ESecretVaultLegacyMigrationStopStatus::Stopped;
	}

private:
	std::shared_ptr<Trace> m_trace;
	bool m_stopped = false;
};

class RecordingGrantAuthority final : public secrets::ISecretVaultExtensionGrantAuthority {
public:
	RecordingGrantAuthority(std::shared_ptr<Trace> trace, std::string profileId, std::uint64_t generation) :
		m_trace(std::move(trace)), m_profileId(std::move(profileId)), m_generation(generation) {}

	std::string_view GetProfileId() const noexcept override { return m_profileId; }
	std::uint64_t GetControlConnectionGeneration() const noexcept override { return m_generation; }
	secrets::SecretVaultExtensionGrantAuthorityResult ActivateOrReplace(
		const secrets::SecretVaultExtensionGrantAuthorityActivateRequest&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult RegisterEditorProcess(
		const secrets::SecretVaultExtensionGrantAuthorityEditorLeaseMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult UnregisterEditorProcess(
		const secrets::SecretVaultExtensionGrantAuthorityEditorLeaseMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult ReplaceApprovedExtensions(
		const secrets::SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult DisableExtension(
		const secrets::SecretVaultExtensionGrantAuthorityDisableExtensionMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult Deactivate(
		const secrets::SecretVaultExtensionGrantAuthoritySessionMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorizationResult AuthorizeIssue(
		const secrets::SecretVaultExtensionGrantAuthorityIssueRequest&) const override { return {}; }
	secrets::SecretVaultExtensionGrantRevokeAuthorizationResult AuthorizeRevokeSession(
		const secrets::SecretVaultExtensionGrantRevokeAuthorizationRequest&) const override { return {}; }
	secrets::ESecretVaultExtensionGrantAuthorityStatus Stop() noexcept override
	{
		if (!m_stopped) m_trace->events.emplace_back(L"grant.stop");
		m_stopped = true;
		return secrets::ESecretVaultExtensionGrantAuthorityStatus::Stopped;
	}

private:
	std::shared_ptr<Trace> m_trace;
	std::string m_profileId;
	std::uint64_t m_generation;
	bool m_stopped = false;
};

ControlPlatformRuntimeOptions ValidOptions()
{
	ControlPlatformRuntimeOptions options;
	options.profileDirectory = L"C:\\profiles\\default";
	options.storageDirectory = L"C:\\profiles\\default\\.sakura-platform\\storage";
	options.legacyProfileAlias = L"legacy-default";
	options.maximumCompletedOperations = 64;
	options.pipeOptions.pipeName = L"caller-supplied-name-must-be-overridden";
	options.pipeOptions.maximumSessions = 7;
	options.pipeOptions.maximumQueuedBytes = 32 * 1024;
	options.pipeOptions.readBufferBytes = 8 * 1024;
	options.pipeOptions.ioTimeout = std::chrono::milliseconds(2222);
	return options;
}

struct TestComposition final {
	std::shared_ptr<Trace> trace = std::make_shared<Trace>();
	std::shared_ptr<RecordingAuthorityBackend> authority = std::make_shared<RecordingAuthorityBackend>(trace);
	std::shared_ptr<RecordingStorageFileOperations> storage = std::make_shared<RecordingStorageFileOperations>(trace);

	ControlPlatformRuntimeDependencies Dependencies() const
	{
		ControlPlatformServiceHostDependencies hostDependencies;
		hostDependencies.endpointFactory = [trace = trace] {
			trace->events.emplace_back(L"host.endpoint.factory");
			return std::make_unique<RecordingEndpoint>(trace);
		};
		hostDependencies.pipeServerFactory = [trace = trace](std::shared_ptr<IControlIpcFrameHandler> handler) {
			trace->events.emplace_back(L"host.pipe.factory");
			return std::make_unique<RecordingPipeServer>(trace, std::move(handler));
		};
		return {
			.profileAuthorityBackend = authority,
			.storageFileOperations = storage,
			.vaultFactory = [trace = trace](const std::filesystem::path&, std::string profileId) {
				trace->events.emplace_back(L"vault.open");
				std::shared_ptr<secrets::ISecretVaultService> vault =
					std::make_shared<secrets::CInMemorySecretVaultService>(std::move(profileId));
				return ControlPlatformRuntimeVaultCreateResult{
					EControlPlatformRuntimeVaultCreateStatus::Created,
					secrets::WindowsDpapiSecretVaultOpenResult{ secrets::EWindowsDpapiSecretVaultOpenStatus::Opened, {} },
					std::move(vault) };
			},
			.migrationFactory = [trace = trace](std::shared_ptr<secrets::ISecretVaultService>,
				const std::filesystem::path& legacyRoot) {
				trace->events.emplace_back(L"migration.create:" + legacyRoot.wstring());
				std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration =
					std::make_shared<RecordingMigration>(trace);
				return ControlPlatformRuntimeMigrationCreateResult{
					EControlPlatformRuntimeMigrationCreateStatus::Created, std::move(migration), {} };
			},
			.capabilityFactory = [trace = trace](std::string profileId) {
				trace->events.emplace_back(L"capability.create");
				std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities =
					std::make_shared<RecordingCapabilities>(trace, std::move(profileId));
				return ControlPlatformRuntimeCapabilityCreateResult{
					EControlPlatformRuntimeCapabilityCreateStatus::Created, std::move(capabilities), {} };
			},
			.extensionGrantAuthorityFactory = [trace = trace](std::string profileId, std::uint64_t generation) {
				trace->events.emplace_back(L"grant.create");
				return std::make_shared<RecordingGrantAuthority>(trace, std::move(profileId), generation);
			},
			.hostDependencies = std::move(hostDependencies),
		};
	}
};

std::size_t EventIndex(const std::vector<std::wstring>& events, std::wstring_view event)
{
	const auto found = std::find(events.begin(), events.end(), event);
	return found == events.end() ? events.size() : static_cast<std::size_t>(found - events.begin());
}

TEST(ControlPlatformRuntime, StartsInStrictDurableOrderAndStopsInExactReverseOrder)
{
	TestComposition composition;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());

	const auto started = runtime.Start();
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, started.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Running, started.state);
	ASSERT_TRUE(started.authorityResult);
	EXPECT_EQ(profiles::ProfileAuthorityStoreStatus::Succeeded, started.authorityResult->status);
	ASSERT_TRUE(started.storageOpenResult);
	EXPECT_EQ(storage::EAtomicFileStorageOpenStatus::Opened, started.storageOpenResult->status);
	ASSERT_TRUE(started.migrationCreateResult);
	EXPECT_EQ(EControlPlatformRuntimeMigrationCreateStatus::Created, started.migrationCreateResult->status);
	ASSERT_TRUE(started.hostResult);
	EXPECT_EQ(EControlPlatformServiceHostResultCode::Started, started.hostResult->code);
	ASSERT_TRUE(started.identity);
	const ControlPlatformRuntimeIdentity expectedIdentity{ kProfileId, 1 };
	EXPECT_EQ(expectedIdentity, *started.identity);
	EXPECT_EQ(started.identity, runtime.Identity());
	EXPECT_TRUE(composition.storage->state->writerLocked);

	const std::vector<std::wstring> startupEvents = {
		L"authority.ensure", L"authority.lock.acquire", L"authority.read", L"authority.random",
		L"authority.write", L"authority.lock.release", L"storage.prepare", L"storage.lock.acquire",
		L"storage.read", L"vault.open", L"migration.create:C:\\profiles\\default\\extensionData\\secrets",
		L"capability.create", L"grant.create", L"host.endpoint.factory", L"host.endpoint.create",
		L"host.endpoint.publish.starting", L"host.pipe.factory", L"host.pipe.start",
		L"host.endpoint.publish.accepting",
	};
	EXPECT_EQ(startupEvents, composition.trace->events);
	EXPECT_EQ(7u, composition.trace->startedPipeOptions.maximumSessions);
	EXPECT_EQ(32u * 1024u, composition.trace->startedPipeOptions.maximumQueuedBytes);
	EXPECT_EQ(8u * 1024u, composition.trace->startedPipeOptions.readBufferBytes);
	EXPECT_EQ(std::chrono::milliseconds(2222), composition.trace->startedPipeOptions.ioTimeout);
	EXPECT_EQ(BuildControlPipeName(kProfileHash), composition.trace->startedPipeOptions.pipeName);
	ASSERT_EQ(2u, composition.trace->snapshots.size());
	for (const auto& snapshot : composition.trace->snapshots) {
		EXPECT_EQ(kProfileId, snapshot.profileId);
		EXPECT_EQ(1u, snapshot.generation);
	}

	const auto stopped = runtime.Stop();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::Stopped, stopped.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, stopped.state);
	EXPECT_FALSE(runtime.Identity());
	EXPECT_FALSE(composition.storage->state->writerLocked);
	const auto& events = composition.trace->events;
	EXPECT_LT(EventIndex(events, L"host.endpoint.publish.stopping"), EventIndex(events, L"host.pipe.stop"));
	EXPECT_LT(EventIndex(events, L"host.pipe.stop"), EventIndex(events, L"host.endpoint.publish.stopped"));
	EXPECT_LT(EventIndex(events, L"host.endpoint.publish.stopped"), EventIndex(events, L"host.endpoint.close"));
	EXPECT_LT(EventIndex(events, L"host.endpoint.close"), EventIndex(events, L"storage.lock.release"));
	EXPECT_LT(EventIndex(events, L"grant.stop"), EventIndex(events, L"capability.stop"));
	EXPECT_LT(EventIndex(events, L"capability.stop"), EventIndex(events, L"migration.stop"));
	EXPECT_LT(EventIndex(events, L"migration.stop"), EventIndex(events, L"vault.stop"));
}

TEST(ControlPlatformRuntime, InvalidResolvedPathsAndBoundsFailBeforeAuthorityAcquisition)
{
	TestComposition composition;
	auto options = ValidOptions();
	options.profileDirectory = L"relative-profile";
	CControlPlatformRuntime invalidPath(options, composition.Dependencies());
	const auto pathResult = invalidPath.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::InvalidOptions, pathResult.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, pathResult.state);
	EXPECT_TRUE(composition.trace->events.empty());

	options = ValidOptions();
	options.pipeOptions.maximumSessions = 0;
	CControlPlatformRuntime invalidPipe(options, composition.Dependencies());
	const auto pipeResult = invalidPipe.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::InvalidOptions, pipeResult.code);
	EXPECT_TRUE(composition.trace->events.empty());

	auto dependencies = composition.Dependencies();
	dependencies.hostDependencies.emplace();
	CControlPlatformRuntime invalidDependencies(ValidOptions(), std::move(dependencies));
	const auto dependencyResult = invalidDependencies.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::InvalidOptions, dependencyResult.code);
	EXPECT_TRUE(composition.trace->events.empty());
}

TEST(ControlPlatformRuntime, AuthorityFailureRemainsTypedAndNeverOpensStorageOrHost)
{
	TestComposition composition;
	composition.authority->ensureStatus = profiles::ProfileAuthorityStoreStatus::SecurityFailed;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AuthorityFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.authorityResult);
	EXPECT_EQ(profiles::ProfileAuthorityStoreStatus::SecurityFailed, result.authorityResult->status);
	EXPECT_FALSE(result.storageOpenResult);
	EXPECT_FALSE(result.hostResult);
	EXPECT_FALSE(result.identity);
	EXPECT_EQ(std::vector<std::wstring>{ L"authority.ensure" }, composition.trace->events);
	EXPECT_FALSE(composition.authority->durableRecord);
}

TEST(ControlPlatformRuntime, InvalidLegacyAliasIsAnExplicitAuthorityFailureBeforeDurableIo)
{
	TestComposition composition;
	auto options = ValidOptions();
	options.legacyProfileAlias.assign(1, static_cast<wchar_t>(0xd800));
	CControlPlatformRuntime runtime(std::move(options), composition.Dependencies());

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AuthorityFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.authorityResult);
	EXPECT_EQ(profiles::ProfileAuthorityStoreStatus::InvalidArgument, result.authorityResult->status);
	EXPECT_TRUE(composition.trace->events.empty());
	EXPECT_FALSE(result.storageOpenResult);
	EXPECT_FALSE(result.hostResult);
}

struct StorageFailureCase final {
	StorageOpenMode mode;
	storage::EAtomicFileStorageOpenStatus expectedStatus;
};

class ControlPlatformRuntimeStorageFailureTest :
	public testing::TestWithParam<StorageFailureCase> {};

TEST_P(ControlPlatformRuntimeStorageFailureTest, FailsClosedBeforeHostAndReleasesAnyWriterLock)
{
	TestComposition composition;
	composition.storage->mode = GetParam().mode;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::StorageOpenFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.authorityResult);
	EXPECT_EQ(profiles::ProfileAuthorityStoreStatus::Succeeded, result.authorityResult->status);
	ASSERT_TRUE(result.storageOpenResult);
	EXPECT_EQ(GetParam().expectedStatus, result.storageOpenResult->status);
	EXPECT_FALSE(result.hostResult);
	EXPECT_FALSE(result.identity);
	EXPECT_FALSE(runtime.Identity());
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_EQ(composition.trace->events.size(), EventIndex(composition.trace->events, L"host.endpoint.factory"));
}

INSTANTIATE_TEST_SUITE_P(AllDurableOpenFailures, ControlPlatformRuntimeStorageFailureTest,
	testing::Values(
		StorageFailureCase{ StorageOpenMode::PrepareIoError, storage::EAtomicFileStorageOpenStatus::IoError },
		StorageFailureCase{ StorageOpenMode::WriterBusy, storage::EAtomicFileStorageOpenStatus::WriterBusy },
		StorageFailureCase{ StorageOpenMode::WriterIoError, storage::EAtomicFileStorageOpenStatus::IoError },
		StorageFailureCase{ StorageOpenMode::ReadIoError, storage::EAtomicFileStorageOpenStatus::IoError },
		StorageFailureCase{ StorageOpenMode::CorruptData, storage::EAtomicFileStorageOpenStatus::CorruptData },
		StorageFailureCase{ StorageOpenMode::UnsupportedFormat, storage::EAtomicFileStorageOpenStatus::UnsupportedFormat }));

TEST(ControlPlatformRuntime, StorageExceptionHasExplicitTerminalResultAndReleasesWriterLock)
{
	TestComposition composition;
	composition.storage->mode = StorageOpenMode::ThrowOnRead;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::UnexpectedFailure, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.authorityResult);
	EXPECT_FALSE(result.storageOpenResult);
	EXPECT_FALSE(result.identity);
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_LT(EventIndex(composition.trace->events, L"storage.read"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
}

TEST(ControlPlatformRuntime, VaultOpenFailureRollsBackStorageBeforeCapabilityOrHostCreation)
{
	TestComposition composition;
	auto dependencies = composition.Dependencies();
	dependencies.vaultFactory = [trace = composition.trace](const std::filesystem::path&, std::string) {
		trace->events.emplace_back(L"vault.open.failed");
		return ControlPlatformRuntimeVaultCreateResult{
			EControlPlatformRuntimeVaultCreateStatus::OpenFailed,
			secrets::WindowsDpapiSecretVaultOpenResult{ secrets::EWindowsDpapiSecretVaultOpenStatus::WriterBusy, {} }, nullptr };
	};
	CControlPlatformRuntime runtime(ValidOptions(), std::move(dependencies));

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::VaultOpenFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.vaultCreateResult);
	EXPECT_EQ(EControlPlatformRuntimeVaultCreateStatus::OpenFailed, result.vaultCreateResult->status);
	ASSERT_TRUE(result.vaultCreateResult->openResult);
	EXPECT_EQ(secrets::EWindowsDpapiSecretVaultOpenStatus::WriterBusy,
		result.vaultCreateResult->openResult->status);
	EXPECT_FALSE(result.capabilityCreateResult);
	EXPECT_FALSE(result.hostResult);
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_LT(EventIndex(composition.trace->events, L"vault.open.failed"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
	EXPECT_EQ(composition.trace->events.size(), EventIndex(composition.trace->events, L"host.endpoint.factory"));
}

TEST(ControlPlatformRuntime, MigrationCreationFailureIsTypedAndStopsVaultBeforeReleasingStorage)
{
	TestComposition composition;
	auto dependencies = composition.Dependencies();
	dependencies.vaultFactory = [trace = composition.trace](const std::filesystem::path&, std::string profileId) {
		trace->events.emplace_back(L"vault.open");
		std::shared_ptr<secrets::ISecretVaultService> vault =
			std::make_shared<RecordingVault>(trace, std::move(profileId));
		return ControlPlatformRuntimeVaultCreateResult{ EControlPlatformRuntimeVaultCreateStatus::Created,
			secrets::WindowsDpapiSecretVaultOpenResult{ secrets::EWindowsDpapiSecretVaultOpenStatus::Opened, {} }, std::move(vault) };
	};
	dependencies.migrationFactory = [trace = composition.trace](std::shared_ptr<secrets::ISecretVaultService>,
		const std::filesystem::path& legacyRoot) {
		trace->events.emplace_back(L"migration.create.failed:" + legacyRoot.wstring());
		return ControlPlatformRuntimeMigrationCreateResult{
			EControlPlatformRuntimeMigrationCreateStatus::CreateFailed, nullptr, L"injected migration failure" };
	};
	CControlPlatformRuntime runtime(ValidOptions(), std::move(dependencies));

	const auto result = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::MigrationCreateFailed, result.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, result.state);
	ASSERT_TRUE(result.migrationCreateResult);
	EXPECT_EQ(EControlPlatformRuntimeMigrationCreateStatus::CreateFailed, result.migrationCreateResult->status);
	EXPECT_EQ(L"injected migration failure", result.diagnostic);
	EXPECT_FALSE(result.capabilityCreateResult);
	EXPECT_FALSE(result.hostResult);
	EXPECT_LT(EventIndex(composition.trace->events, L"migration.create.failed:C:\\profiles\\default\\extensionData\\secrets"),
		EventIndex(composition.trace->events, L"vault.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"vault.stop"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
}

TEST(ControlPlatformRuntime, StopsHostThenGrantThenCapabilityThenMigrationThenVaultThenStorageExactlyOnce)
{
	TestComposition composition;
	auto dependencies = composition.Dependencies();
	dependencies.vaultFactory = [trace = composition.trace](const std::filesystem::path&, std::string profileId) {
		trace->events.emplace_back(L"vault.open");
		std::shared_ptr<secrets::ISecretVaultService> vault = std::make_shared<RecordingVault>(trace, std::move(profileId));
		return ControlPlatformRuntimeVaultCreateResult{ EControlPlatformRuntimeVaultCreateStatus::Created,
			secrets::WindowsDpapiSecretVaultOpenResult{ secrets::EWindowsDpapiSecretVaultOpenStatus::Opened, {} }, std::move(vault) };
	};
	dependencies.capabilityFactory = [trace = composition.trace](std::string profileId) {
		trace->events.emplace_back(L"capability.create");
		std::shared_ptr<secrets::ISecretVaultCapabilityService> capabilities =
			std::make_shared<RecordingCapabilities>(trace, std::move(profileId));
		return ControlPlatformRuntimeCapabilityCreateResult{
			EControlPlatformRuntimeCapabilityCreateStatus::Created, std::move(capabilities), {} };
	};
	dependencies.migrationFactory = [trace = composition.trace](std::shared_ptr<secrets::ISecretVaultService>,
		const std::filesystem::path&) {
		trace->events.emplace_back(L"migration.create");
		std::shared_ptr<secrets::ISecretVaultLegacyMigrationCoordinator> migration =
			std::make_shared<RecordingMigration>(trace);
		return ControlPlatformRuntimeMigrationCreateResult{
			EControlPlatformRuntimeMigrationCreateStatus::Created, std::move(migration), {} };
	};
	CControlPlatformRuntime runtime(ValidOptions(), std::move(dependencies));
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, runtime.Start().code);
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	EXPECT_LT(EventIndex(composition.trace->events, L"host.endpoint.close"),
		EventIndex(composition.trace->events, L"grant.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"grant.stop"),
		EventIndex(composition.trace->events, L"capability.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"capability.stop"),
		EventIndex(composition.trace->events, L"migration.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"migration.stop"),
		EventIndex(composition.trace->events, L"vault.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"vault.stop"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
	const auto eventsAfterStop = composition.trace->events;
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AlreadyStopped, runtime.Stop().code);
	EXPECT_EQ(eventsAfterStop, composition.trace->events);
}

TEST(ControlPlatformRuntime, ProductionGrantAuthorityIsBoundToTheCanonicalRuntimeIdentityAndStoppedWithTheRuntime)
{
	TestComposition composition;
	auto dependencies = composition.Dependencies();
	dependencies.extensionGrantAuthorityFactory = {};
	CControlPlatformRuntime runtime(ValidOptions(), std::move(dependencies));
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, runtime.Start().code);
	auto authority = runtime.ExtensionGrantAuthority();
	ASSERT_NE(nullptr, authority);
	EXPECT_EQ(kProfileId, authority->GetProfileId());
	EXPECT_EQ(1u, authority->GetControlConnectionGeneration());
	EXPECT_EQ(secrets::ESecretVaultExtensionGrantAuthorityStatus::Applied,
		authority->ActivateOrReplace({ 0, "host-session", 1, { "publisher.extension" }, { 42 } }).status);

	ASSERT_EQ(EControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	EXPECT_EQ(nullptr, runtime.ExtensionGrantAuthority());
	EXPECT_EQ(secrets::ESecretVaultExtensionGrantAuthorizationStatus::Stopped,
		authority->AuthorizeIssue({ kProfileId, 1, "host-session", 1, 42, "publisher.extension" }).status);
}

TEST(ControlPlatformRuntime, HostStartFailureWithdrawsEndpointBeforeClosingStorageAndCanRetry)
{
	TestComposition composition;
	composition.trace->pipeStartSucceeds = false;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());

	const auto failed = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::HostStartFailed, failed.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, failed.state);
	ASSERT_TRUE(failed.hostResult);
	EXPECT_EQ(EControlPlatformServiceHostResultCode::PipeStartFailed, failed.hostResult->code);
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_LT(EventIndex(composition.trace->events, L"host.endpoint.close"),
		EventIndex(composition.trace->events, L"grant.stop"));
	EXPECT_LT(EventIndex(composition.trace->events, L"grant.stop"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
	const auto eventsAfterFailure = composition.trace->events.size();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AlreadyStopped, runtime.Stop().code);
	EXPECT_EQ(eventsAfterFailure, composition.trace->events.size());

	composition.trace->pipeStartSucceeds = true;
	const auto retried = runtime.Start();
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, retried.code);
	ASSERT_TRUE(retried.identity);
	EXPECT_EQ(kProfileId, retried.identity->profileId);
	EXPECT_EQ(2u, retried.identity->authorityGeneration);
	EXPECT_EQ(EControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
}

TEST(ControlPlatformRuntime, RepeatedStartAndStopAreIdempotentAndDoNotRepeatDurableWork)
{
	TestComposition composition;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, runtime.Start().code);
	const auto afterStart = composition.trace->events.size();

	const auto repeatedStart = runtime.Start();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AlreadyRunning, repeatedStart.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Running, repeatedStart.state);
	EXPECT_EQ(afterStart, composition.trace->events.size());

	ASSERT_EQ(EControlPlatformRuntimeResultCode::Stopped, runtime.Stop().code);
	const auto afterStop = composition.trace->events.size();
	const auto repeatedStop = runtime.Stop();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::AlreadyStopped, repeatedStop.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, repeatedStop.state);
	EXPECT_EQ(afterStop, composition.trace->events.size());
}

TEST(ControlPlatformRuntime, HostStopFailureIsReportedAfterEndpointAndStorageAreClosed)
{
	TestComposition composition;
	composition.trace->stoppingPublishSucceeds = false;
	CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());
	ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, runtime.Start().code);

	const auto stopped = runtime.Stop();
	EXPECT_EQ(EControlPlatformRuntimeResultCode::HostStopFailed, stopped.code);
	EXPECT_EQ(EControlPlatformRuntimeState::Stopped, stopped.state);
	ASSERT_TRUE(stopped.hostResult);
	EXPECT_EQ(EControlPlatformServiceHostResultCode::StoppingPublishFailed, stopped.hostResult->code);
	EXPECT_FALSE(runtime.Identity());
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_LT(EventIndex(composition.trace->events, L"host.endpoint.close"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
}

TEST(ControlPlatformRuntime, DestructorPerformsReverseShutdown)
{
	TestComposition composition;
	{
		CControlPlatformRuntime runtime(ValidOptions(), composition.Dependencies());
		ASSERT_EQ(EControlPlatformRuntimeResultCode::Running, runtime.Start().code);
	}
	EXPECT_FALSE(composition.storage->state->writerLocked);
	EXPECT_LT(EventIndex(composition.trace->events, L"host.endpoint.close"),
		EventIndex(composition.trace->events, L"storage.lock.release"));
}

} // namespace
} // namespace platform::controlipc
