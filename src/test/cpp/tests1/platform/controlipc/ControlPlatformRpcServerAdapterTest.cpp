/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlPlatformRpcServerAdapter.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "platform/secrets/CSecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/ISecretVaultCapabilityService.h"
#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"
#include "platform/secrets/ISecretVaultService.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kGeneration = 9;

ControlIpcFrame Request(EControlIpcKind kind, std::uint64_t requestId, std::uint64_t generation,
	std::vector<std::uint8_t> payload = {})
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, EControlIpcFlags::Request,
		requestId, generation }, std::move(payload) };
}

class CFakeVault final : public secrets::ISecretVaultService {
public:
	std::string_view GetProfileId() const noexcept override { return kProfileId; }
	secrets::SecretGetResult Get(std::string_view, std::string_view) const override
	{
		return { secrets::ESecretGetStatus::NotFound, 0, std::nullopt };
	}
	secrets::SecretMutationResult Apply(const secrets::SecretMutationRequest&) override { return {}; }
	std::unique_ptr<secrets::ISecretVaultChangeSubscription> Subscribe(secrets::SecretChangeCallback) override { return nullptr; }
	secrets::ESecretVaultStopStatus Stop() noexcept override { return secrets::ESecretVaultStopStatus::Stopped; }
};

class CFakeCapabilities final : public secrets::ISecretVaultCapabilityService {
public:
	std::string_view GetProfileId() const noexcept override { return kProfileId; }
	secrets::SecretVaultCapabilityIssueResult Issue(const secrets::SecretVaultCapabilityIssueRequest& request) override
	{
		++issueCount;
		lastIssue = request;
		if (!issueSucceeds) return {};
		secrets::SecretVaultCapabilityToken capability{};
		capability.fill(0x5a);
		return { secrets::ESecretVaultCapabilityIssueStatus::Issued, capability,
			std::chrono::steady_clock::now() + std::chrono::minutes(1) };
	}
	secrets::SecretVaultCapabilityValidationResult Validate(const secrets::SecretVaultCapabilityValidationRequest& request) override
	{
		++validateCount;
		lastRequest = request;
		return { secrets::ESecretVaultCapabilityValidationStatus::Valid };
	}
	secrets::SecretVaultCapabilityRevokeResult RevokeExtension(const secrets::SecretVaultCapabilityBinding&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeSession(const secrets::SecretVaultCapabilitySessionIdentity&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeHostSession(
		const secrets::SecretVaultCapabilityHostSessionIdentity&) override { return {}; }
	secrets::ESecretVaultCapabilityStopStatus Stop() noexcept override { return secrets::ESecretVaultCapabilityStopStatus::Stopped; }

	int validateCount = 0;
	int issueCount = 0;
	bool issueSucceeds = false;
	secrets::SecretVaultCapabilityValidationRequest lastRequest{};
	secrets::SecretVaultCapabilityIssueRequest lastIssue{};
};

class CFakeMigration final : public secrets::ISecretVaultLegacyMigrationCoordinator {
public:
	secrets::SecretVaultLegacyMigrationResult EnsureMigrated(std::string_view extensionId) override
	{
		++ensureCount;
		lastExtensionId = extensionId;
		return result;
	}
	secrets::ESecretVaultLegacyMigrationStopStatus Stop() noexcept override
	{
		return secrets::ESecretVaultLegacyMigrationStopStatus::Stopped;
	}

	int ensureCount = 0;
	std::string lastExtensionId;
	secrets::SecretVaultLegacyMigrationResult result{ secrets::ESecretVaultLegacyMigrationStatus::Migrated };
};

std::shared_ptr<secrets::ISecretVaultExtensionGrantAuthority> GrantAuthority()
{
	return std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>(kProfileId, kGeneration);
}

std::shared_ptr<CFakeMigration> Migration()
{
	return std::make_shared<CFakeMigration>();
}

std::shared_ptr<profiles::ControlUserDataProfileRegistry> ProfileRegistry(
	const std::shared_ptr<storage::IStorageService>& storage)
{
	auto registry = std::make_shared<profiles::ControlUserDataProfileRegistry>(storage);
	if (!registry->Start().Succeeded()) throw std::runtime_error("profile registry did not start");
	return registry;
}

ControlIpcFrameDispatchResult Dispatch(IControlIpcSessionHandler& session, const ControlIpcFrame& request,
	std::uint32_t pid)
{
	auto result = session.HandleFrame({ 1, pid }, request);
	EXPECT_EQ(1u, result.responseFrames.size());
	if (!result.responseFrames.empty()) {
		EXPECT_EQ(request.header.requestId, result.responseFrames.front().header.requestId);
		EXPECT_TRUE(HasFlag(result.responseFrames.front().header.flags, EControlIpcFlags::Terminal));
	}
	return result;
}

void Hello(IControlIpcSessionHandler& session, std::uint32_t pid, std::uint64_t requestId)
{
	const auto payload = EncodeControlStorageHello(kProfileId);
	ASSERT_TRUE(payload);
	const auto result = Dispatch(session, Request(EControlIpcKind::Hello, requestId, 0, *payload), pid);
	ASSERT_EQ(1u, result.responseFrames.size());
	EXPECT_EQ(EControlIpcKind::HelloAck, result.responseFrames.front().header.kind);
}

TEST(ControlPlatformRpcServerAdapter, RoutesStorageAndGatesSecretRequestsOnTheSameTransportHello)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	auto vault = std::make_shared<CFakeVault>();
	auto capabilities = std::make_shared<CFakeCapabilities>();
	auto migration = Migration();
	CControlPlatformRpcServerAdapter adapter({ kProfileId, kGeneration }, storage, vault, capabilities, GrantAuthority(), migration, ProfileRegistry(storage));
	auto session = adapter.CreateSession({ 77, 4242 });
	ASSERT_NE(nullptr, session);

	ControlSecretVaultGetRequest get{ {}, "extension-host-a", { "publisher.extension", "token" } };
	const auto getPayload = EncodeControlSecretVaultGetRequest(get);
	ASSERT_TRUE(getPayload);
	const auto beforeHello = Dispatch(*session, Request(EControlIpcKind::SecretGetRequest, 1, kGeneration, *getPayload), 4242);
	ASSERT_EQ(EControlIpcKind::Error, beforeHello.responseFrames.front().header.kind);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest,
		DecodeControlIpcError(beforeHello.responseFrames.front().payload)->status);
	EXPECT_EQ(0, capabilities->validateCount);

	Hello(*session, 4242, 2);
	const auto secret = Dispatch(*session, Request(EControlIpcKind::SecretGetRequest, 3, kGeneration, *getPayload), 4242);
	ASSERT_EQ(EControlIpcKind::SecretGetResponse, secret.responseFrames.front().header.kind);
	EXPECT_EQ(1, capabilities->validateCount);
	EXPECT_EQ(kProfileId, capabilities->lastRequest.session.profileId);
	EXPECT_EQ(4242u, capabilities->lastRequest.session.clientProcessId);
	EXPECT_EQ(kGeneration, capabilities->lastRequest.session.connectionGeneration);
	EXPECT_EQ(1, migration->ensureCount);
	EXPECT_EQ("publisher.extension", migration->lastExtensionId);
}

TEST(ControlPlatformRpcServerAdapter, RoutesGenerationPinnedProfileCommandsAfterHello)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	CControlPlatformRpcServerAdapter adapter({ kProfileId, kGeneration }, storage, std::make_shared<CFakeVault>(),
		std::make_shared<CFakeCapabilities>(), GrantAuthority(), Migration(), ProfileRegistry(storage));
	auto session = adapter.CreateSession({ 88, 4242 });
	ASSERT_NE(nullptr, session);
	Hello(*session, 4242, 1);

	ControlProfileRpcRequest create;
	create.operation = EControlProfileRpcOperation::CreateNamed;
	create.mutation = { "profile-create-1", 0 };
	create.create = { L"opaque-profile-1", L"Profile one", profiles::UserDataProfileKind::Normal, {}, {} };
	auto command = EncodeControlProfileRpcRequest(create);
	ASSERT_TRUE(command);
	auto payload = EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload), std::move(*command) } });
	ASSERT_TRUE(payload);
	auto created = Dispatch(*session, Request(EControlIpcKind::ProfileRequest, 2, kGeneration, std::move(*payload)), 4242);
	ASSERT_EQ(EControlIpcKind::ProfileResponse, created.responseFrames.front().header.kind);
	auto fields = DecodeControlIpcFields(created.responseFrames.front().payload);
	ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, fields.outcome);
	ASSERT_EQ(1u, fields.fields.size());
	auto result = DecodeControlProfileRpcResponse(fields.fields.front().value);
	ASSERT_TRUE(result);
	EXPECT_EQ(EControlIpcTerminalStatus::Succeeded, result->terminalStatus);
	EXPECT_EQ(profiles::ControlUserDataProfileRegistryStatus::Applied, result->result.status);

	ControlProfileRpcRequest list;
	list.operation = EControlProfileRpcOperation::List;
	command = EncodeControlProfileRpcRequest(list);
	ASSERT_TRUE(command);
	payload = EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload), std::move(*command) } });
	ASSERT_TRUE(payload);
	auto listed = Dispatch(*session, Request(EControlIpcKind::ProfileRequest, 3, kGeneration, std::move(*payload)), 4242);
	fields = DecodeControlIpcFields(listed.responseFrames.front().payload);
	ASSERT_EQ(EControlIpcFieldDecodeOutcome::Decoded, fields.outcome);
	result = DecodeControlProfileRpcResponse(fields.fields.front().value);
	ASSERT_TRUE(result);
	EXPECT_EQ(EControlIpcTerminalStatus::Succeeded, result->terminalStatus);
	EXPECT_FALSE(result->snapshotDocument.empty());
}

TEST(ControlPlatformRpcServerAdapter, BindsCapabilityValidationToEachVerifiedTransportPidAndGeneration)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	auto vault = std::make_shared<CFakeVault>();
	auto capabilities = std::make_shared<CFakeCapabilities>();
	CControlPlatformRpcServerAdapter adapter({ kProfileId, kGeneration }, storage, vault, capabilities, GrantAuthority(), Migration(), ProfileRegistry(storage));
	ControlSecretVaultGetRequest get{ {}, "extension-host-a", { "publisher.extension", "token" } };
	const auto payload = EncodeControlSecretVaultGetRequest(get);
	ASSERT_TRUE(payload);

	auto first = adapter.CreateSession({ 1, 1001 });
	ASSERT_NE(nullptr, first);
	Hello(*first, 1001, 1);
	(void)Dispatch(*first, Request(EControlIpcKind::SecretGetRequest, 2, kGeneration, *payload), 1001);
	EXPECT_EQ(1001u, capabilities->lastRequest.session.clientProcessId);

	auto second = adapter.CreateSession({ 2, 2002 });
	ASSERT_NE(nullptr, second);
	Hello(*second, 2002, 3);
	(void)Dispatch(*second, Request(EControlIpcKind::SecretGetRequest, 4, kGeneration, *payload), 2002);
	EXPECT_EQ(2002u, capabilities->lastRequest.session.clientProcessId);
	EXPECT_EQ(kGeneration, capabilities->lastRequest.session.connectionGeneration);
}

TEST(ControlPlatformRpcServerAdapter, ClosingTheGateReturnsExactlyOneTerminalStoppingResponseAndCloses)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	auto vault = std::make_shared<CFakeVault>();
	auto capabilities = std::make_shared<CFakeCapabilities>();
	CControlPlatformRpcServerAdapter adapter({ kProfileId, kGeneration }, storage, vault, capabilities, GrantAuthority(), Migration(), ProfileRegistry(storage));
	auto session = adapter.CreateSession({ 1, 1001 });
	ASSERT_NE(nullptr, session);
	Hello(*session, 1001, 1);
	EXPECT_TRUE(adapter.BeginStopping());
	EXPECT_EQ(nullptr, adapter.CreateSession({ 2, 2002 }));
	const auto stopped = Dispatch(*session, Request(EControlIpcKind::StorageSnapshotRequest, 2, kGeneration), 1001);
	EXPECT_EQ(EControlIpcSessionDecision::Close, stopped.decision);
	ASSERT_EQ(1u, stopped.responseFrames.size());
	const auto error = DecodeControlIpcError(stopped.responseFrames.front().payload);
	ASSERT_TRUE(error);
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping, error->status);
	adapter.Stop();
	EXPECT_EQ(EControlPlatformRpcServerAdapterState::Stopped, adapter.State());
}

TEST(ControlPlatformRpcServerAdapter, ProductionSessionRoutesIssueThroughTheBoundGrantAuthority)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	auto vault = std::make_shared<CFakeVault>();
	auto capabilities = std::make_shared<CFakeCapabilities>();
	capabilities->issueSucceeds = true;
	auto authority = std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>(kProfileId, kGeneration);
	ASSERT_EQ(secrets::ESecretVaultExtensionGrantAuthorityStatus::Applied,
		authority->ActivateOrReplace({ 0, "host-session", 3, { "publisher.extension" }, { 4242 } }).status);
	CControlPlatformRpcServerAdapter adapter({ kProfileId, kGeneration }, storage, vault, capabilities, authority, Migration(), ProfileRegistry(storage));
	auto session = adapter.CreateSession({ 77, 4242 });
	ASSERT_NE(nullptr, session);
	Hello(*session, 4242, 1);

	ControlSecretVaultCapabilityIssueRequest issue{ "host-session", 3, "publisher.extension", std::chrono::minutes(1) };
	const auto payload = EncodeControlSecretVaultCapabilityIssueRequest(issue);
	ASSERT_TRUE(payload);
	const auto response = Dispatch(*session,
		Request(EControlIpcKind::SecretCapabilityIssueRequest, 2, kGeneration, *payload), 4242);
	ASSERT_EQ(EControlIpcKind::SecretCapabilityIssueResponse, response.responseFrames.front().header.kind);
	EXPECT_EQ(1, capabilities->issueCount);
	EXPECT_EQ(kProfileId, capabilities->lastIssue.binding.session.profileId);
	EXPECT_EQ(4242u, capabilities->lastIssue.binding.session.clientProcessId);
	EXPECT_EQ(kGeneration, capabilities->lastIssue.binding.session.connectionGeneration);
	EXPECT_EQ("publisher.extension", capabilities->lastIssue.binding.extensionId);
}

TEST(ControlPlatformRpcServerAdapter, RejectsGrantAuthoritiesWithMismatchedProfileOrGeneration)
{
	auto storage = std::make_shared<storage::CInMemoryStorageService>(kGeneration);
	auto vault = std::make_shared<CFakeVault>();
	auto capabilities = std::make_shared<CFakeCapabilities>();
	EXPECT_THROW((CControlPlatformRpcServerAdapter({ kProfileId, kGeneration }, storage, vault, capabilities,
		std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>("fedcba9876543210fedcba9876543210", kGeneration), Migration(), ProfileRegistry(storage))), std::invalid_argument);
	EXPECT_THROW((CControlPlatformRpcServerAdapter({ kProfileId, kGeneration }, storage, vault, capabilities,
		std::make_shared<secrets::CSecretVaultExtensionGrantAuthority>(kProfileId, kGeneration + 1), Migration(), ProfileRegistry(storage))), std::invalid_argument);
}

} // namespace
} // namespace platform::controlipc
