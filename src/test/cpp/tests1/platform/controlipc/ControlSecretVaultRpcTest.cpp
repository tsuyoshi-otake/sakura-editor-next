/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlSecretVaultRpc.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

namespace platform::controlipc {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";
constexpr std::uint64_t kGeneration = 31;

ControlIpcFrame Request(EControlIpcKind kind, std::uint64_t requestId, std::vector<std::uint8_t> payload = {})
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, EControlIpcFlags::Request,
		requestId, kGeneration }, std::move(payload) };
}

secrets::SecretAddress Address(std::string key = "token")
{
	return { "publisher.one", std::move(key) };
}

ControlSecretVaultGetRequest GetRequest()
{
	ControlSecretVaultGetRequest request{ {}, "host-session", Address() };
	request.capability.fill(0xa5);
	return request;
}

ControlSecretVaultApplyRequest ApplyRequest()
{
	ControlSecretVaultApplyRequest request{ {}, "host-session",
		{ secrets::ESecretMutationKind::Set, "publisher.one", "token", "super-secret", "operation-1", 0 } };
	request.capability.fill(0xa5);
	return request;
}

ControlSecretVaultCapabilityIssueRequest CapabilityIssueRequest()
{
	return { "host-session", 5, "publisher.one", std::chrono::minutes(10) };
}

class CFakeCapabilityService final : public secrets::ISecretVaultCapabilityService {
public:
	std::string_view GetProfileId() const noexcept override { return kProfileId; }
	secrets::SecretVaultCapabilityIssueResult Issue(const secrets::SecretVaultCapabilityIssueRequest& request) override
	{
		lastIssue = request;
		++issueCount;
		return issue;
	}
	secrets::SecretVaultCapabilityValidationResult Validate(const secrets::SecretVaultCapabilityValidationRequest& request) override
	{
		if (throwOnValidate) throw std::runtime_error("validate");
		if (callOrder) callOrder->emplace_back("validate");
		lastValidation = request;
		++validateCount;
		return { status };
	}
	secrets::SecretVaultCapabilityRevokeResult RevokeExtension(const secrets::SecretVaultCapabilityBinding&) override { return {}; }
	secrets::SecretVaultCapabilityRevokeResult RevokeSession(const secrets::SecretVaultCapabilitySessionIdentity& session) override
	{
		lastRevokedSession = session;
		++revokeCount;
		return revoke;
	}
	secrets::SecretVaultCapabilityRevokeResult RevokeHostSession(
		const secrets::SecretVaultCapabilityHostSessionIdentity&) override { return {}; }
	secrets::ESecretVaultCapabilityStopStatus Stop() noexcept override { return secrets::ESecretVaultCapabilityStopStatus::Stopped; }

	secrets::ESecretVaultCapabilityValidationStatus status = secrets::ESecretVaultCapabilityValidationStatus::Valid;
	secrets::SecretVaultCapabilityIssueResult issue{ secrets::ESecretVaultCapabilityIssueStatus::InvalidConfiguration, std::nullopt, std::nullopt };
	secrets::SecretVaultCapabilityRevokeResult revoke{ secrets::ESecretVaultCapabilityRevokeStatus::Revoked, 1 };
	secrets::SecretVaultCapabilityValidationRequest lastValidation{};
	secrets::SecretVaultCapabilityIssueRequest lastIssue{};
	secrets::SecretVaultCapabilitySessionIdentity lastRevokedSession{};
	int validateCount = 0;
	int issueCount = 0;
	int revokeCount = 0;
	bool throwOnValidate = false;
	std::vector<std::string>* callOrder = nullptr;
};

class CFakeGrantAuthority final : public secrets::ISecretVaultExtensionGrantAuthority {
public:
	std::string_view GetProfileId() const noexcept override { return kProfileId; }
	std::uint64_t GetControlConnectionGeneration() const noexcept override { return kGeneration; }
	secrets::SecretVaultExtensionGrantAuthorityResult ActivateOrReplace(const secrets::SecretVaultExtensionGrantAuthorityActivateRequest&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult RegisterEditorProcess(const secrets::SecretVaultExtensionGrantAuthorityEditorLeaseMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult UnregisterEditorProcess(const secrets::SecretVaultExtensionGrantAuthorityEditorLeaseMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult ReplaceApprovedExtensions(const secrets::SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult DisableExtension(const secrets::SecretVaultExtensionGrantAuthorityDisableExtensionMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorityResult Deactivate(const secrets::SecretVaultExtensionGrantAuthoritySessionMutation&) override { return {}; }
	secrets::SecretVaultExtensionGrantAuthorizationResult AuthorizeIssue(const secrets::SecretVaultExtensionGrantAuthorityIssueRequest& request) const override
	{
		lastIssueAuthorization = request;
		++issueAuthorizationCount;
		return issueAuthorization;
	}
	secrets::SecretVaultExtensionGrantRevokeAuthorizationResult AuthorizeRevokeSession(
		const secrets::SecretVaultExtensionGrantRevokeAuthorizationRequest& request) const override
	{
		lastRevokeAuthorization = request;
		++revokeAuthorizationCount;
		return revokeAuthorization;
	}
	secrets::ESecretVaultExtensionGrantAuthorityStatus Stop() noexcept override { return secrets::ESecretVaultExtensionGrantAuthorityStatus::Stopped; }

	mutable secrets::SecretVaultExtensionGrantAuthorityIssueRequest lastIssueAuthorization{};
	mutable secrets::SecretVaultExtensionGrantRevokeAuthorizationRequest lastRevokeAuthorization{};
	secrets::SecretVaultExtensionGrantAuthorizationResult issueAuthorization{
		secrets::ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, {}, 1 };
	secrets::SecretVaultExtensionGrantRevokeAuthorizationResult revokeAuthorization{
		secrets::ESecretVaultExtensionGrantAuthorizationStatus::AccessDenied, {}, 1, 0 };
	mutable int issueAuthorizationCount = 0;
	mutable int revokeAuthorizationCount = 0;
};

class CFakeVault final : public secrets::ISecretVaultService {
public:
	std::string_view GetProfileId() const noexcept override { return kProfileId; }
	secrets::SecretGetResult Get(std::string_view extensionId, std::string_view key) const override
	{
		if (throwOnGet) throw std::runtime_error("get");
		if (callOrder) callOrder->emplace_back("get");
		lastGet = { std::string(extensionId), std::string(key) };
		return get;
	}
	secrets::SecretMutationResult Apply(const secrets::SecretMutationRequest& request) override
	{
		if (throwOnApply) throw std::runtime_error("apply");
		if (callOrder) callOrder->emplace_back("apply");
		lastApply = request;
		return apply;
	}
	std::unique_ptr<secrets::ISecretVaultChangeSubscription> Subscribe(secrets::SecretChangeCallback) override { return nullptr; }
	secrets::ESecretVaultStopStatus Stop() noexcept override { return secrets::ESecretVaultStopStatus::Stopped; }

	mutable secrets::SecretAddress lastGet;
	secrets::SecretMutationRequest lastApply;
	secrets::SecretGetResult get{ secrets::ESecretGetStatus::Found, 7, std::string("super-secret") };
	secrets::SecretMutationResult apply{ secrets::ESecretMutationStatus::Succeeded, 8, false,
		secrets::SecretChange{ std::string(kProfileId), Address(), secrets::ESecretChangeKind::Set, 8 }, "do not serialize" };
	bool throwOnGet = false;
	bool throwOnApply = false;
	std::vector<std::string>* callOrder = nullptr;
};

class CFakeLegacyMigrationCoordinator final : public secrets::ISecretVaultLegacyMigrationCoordinator {
public:
	secrets::SecretVaultLegacyMigrationResult EnsureMigrated(std::string_view extensionId) override
	{
		lastExtensionId.assign(extensionId);
		++ensureCount;
		if (callOrder) callOrder->emplace_back("migrate");
		return result;
	}
	secrets::ESecretVaultLegacyMigrationStopStatus Stop() noexcept override
	{
		return secrets::ESecretVaultLegacyMigrationStopStatus::Stopped;
	}

	secrets::SecretVaultLegacyMigrationResult result{ secrets::ESecretVaultLegacyMigrationStatus::Migrated };
	std::string lastExtensionId;
	int ensureCount = 0;
	std::vector<std::string>* callOrder = nullptr;
};

CControlSecretVaultRpcSession Session(CFakeVault& vault, CFakeCapabilityService& capability)
{
	return { { std::string(kProfileId), 99, kGeneration }, vault, capability };
}

CControlSecretVaultRpcSession AuthorizedSession(CFakeVault& vault, CFakeCapabilityService& capability,
	CFakeGrantAuthority& authority)
{
	return { { std::string(kProfileId), 99, kGeneration }, vault, capability, authority };
}

CControlSecretVaultRpcSession MigratingSession(CFakeVault& vault, CFakeCapabilityService& capability,
	CFakeLegacyMigrationCoordinator& migration)
{
	return { { std::string(kProfileId), 99, kGeneration }, vault, capability, migration };
}

EControlIpcTerminalStatus ErrorStatus(const ControlIpcFrame& response)
{
	EXPECT_EQ(EControlIpcKind::Error, response.header.kind);
	EXPECT_EQ(EControlIpcFlags::Response | EControlIpcFlags::Terminal, response.header.flags);
	auto error = DecodeControlIpcError(response.payload);
	EXPECT_TRUE(error);
	return error ? error->status : EControlIpcTerminalStatus::InternalError;
}

void AppendField(std::vector<std::uint8_t>& payload, std::uint16_t tag, std::span<const std::uint8_t> value)
{
	payload.push_back(static_cast<std::uint8_t>(tag)); payload.push_back(static_cast<std::uint8_t>(tag >> 8));
	const auto size = static_cast<std::uint32_t>(value.size());
	for (std::size_t i = 0; i != sizeof(size); ++i) payload.push_back(static_cast<std::uint8_t>(size >> (i * 8)));
	payload.insert(payload.end(), value.begin(), value.end());
}

bool Contains(std::span<const std::uint8_t> bytes, std::string_view value)
{
	return std::search(bytes.begin(), bytes.end(), value.begin(), value.end()) != bytes.end();
}

TEST(ControlSecretVaultRpc, RoundTripsTypedPayloadsAndBindsTheAuthenticatedSession)
{
	const auto getPayload = EncodeControlSecretVaultGetRequest(GetRequest());
	ASSERT_TRUE(getPayload);
	const auto decodedGet = DecodeControlSecretVaultGetRequest(*getPayload);
	ASSERT_TRUE(decodedGet);
	EXPECT_EQ("host-session", decodedGet->extensionHostSessionId);
	EXPECT_EQ(Address(), decodedGet->address);

	const auto applyPayload = EncodeControlSecretVaultApplyRequest(ApplyRequest());
	ASSERT_TRUE(applyPayload);
	const auto decodedApply = DecodeControlSecretVaultApplyRequest(*applyPayload);
	ASSERT_TRUE(decodedApply);
	EXPECT_EQ("super-secret", decodedApply->mutation.value);

	CFakeVault vault; CFakeCapabilityService capability; auto session = Session(vault, capability);
	const auto getResponse = session.Process(Request(EControlIpcKind::SecretGetRequest, 101, *getPayload));
	ASSERT_EQ(EControlIpcKind::SecretGetResponse, getResponse.header.kind);
	EXPECT_EQ(101u, getResponse.header.requestId);
	const auto result = DecodeControlSecretVaultGetResponse(getResponse.payload);
	ASSERT_TRUE(result);
	EXPECT_EQ(secrets::ESecretGetStatus::Found, result->status);
	ASSERT_TRUE(result->value);
	EXPECT_EQ("super-secret", *result->value);
	EXPECT_EQ(std::string(kProfileId), capability.lastValidation.session.profileId);
	EXPECT_EQ("host-session", capability.lastValidation.session.extensionHostSessionId);
	EXPECT_EQ(99u, capability.lastValidation.session.clientProcessId);
	EXPECT_EQ(kGeneration, capability.lastValidation.session.connectionGeneration);
	EXPECT_EQ(Address(), capability.lastValidation.address);
}

TEST(ControlSecretVaultRpc, RejectsCrossNamespaceCapabilityWithoutCallingVaultAndKeepsErrorGeneric)
{
	CFakeVault vault; CFakeCapabilityService capability; capability.status = secrets::ESecretVaultCapabilityValidationStatus::ExtensionMismatch;
	auto session = Session(vault, capability);
	const auto payload = EncodeControlSecretVaultGetRequest(GetRequest());
	ASSERT_TRUE(payload);
	const auto response = session.Process(Request(EControlIpcKind::SecretGetRequest, 102, *payload));
	EXPECT_EQ(EControlIpcTerminalStatus::AccessDenied, ErrorStatus(response));
	EXPECT_EQ(1, capability.validateCount);
	EXPECT_TRUE(vault.lastGet.extensionId.empty());
	const auto error = DecodeControlIpcError(response.payload);
	ASSERT_TRUE(error);
	EXPECT_EQ("access denied", error->diagnostic);
	EXPECT_FALSE(Contains(response.payload, "publisher.one"));
}

TEST(ControlSecretVaultRpc, MigratesOnlyAfterCapabilityValidationAndBeforeVaultAccess)
{
	CFakeVault vault; CFakeCapabilityService capability; CFakeLegacyMigrationCoordinator migration;
	std::vector<std::string> calls;
	vault.callOrder = &calls;
	capability.callOrder = &calls;
	migration.callOrder = &calls;
	auto session = MigratingSession(vault, capability, migration);
	const auto getPayload = EncodeControlSecretVaultGetRequest(GetRequest());
	const auto applyPayload = EncodeControlSecretVaultApplyRequest(ApplyRequest());
	ASSERT_TRUE(getPayload);
	ASSERT_TRUE(applyPayload);

	capability.status = secrets::ESecretVaultCapabilityValidationStatus::ExtensionMismatch;
	EXPECT_EQ(EControlIpcTerminalStatus::AccessDenied,
		ErrorStatus(session.Process(Request(EControlIpcKind::SecretGetRequest, 150, *getPayload))));
	EXPECT_EQ((std::vector<std::string>{ "validate" }), calls);
	EXPECT_EQ(0, migration.ensureCount);

	capability.status = secrets::ESecretVaultCapabilityValidationStatus::Valid;
	EXPECT_EQ(EControlIpcKind::SecretGetResponse,
		session.Process(Request(EControlIpcKind::SecretGetRequest, 151, *getPayload)).header.kind);
	EXPECT_EQ((std::vector<std::string>{ "validate", "validate", "migrate", "get" }), calls);
	EXPECT_EQ("publisher.one", migration.lastExtensionId);

	calls.clear();
	EXPECT_EQ(EControlIpcKind::SecretApplyResponse,
		session.Process(Request(EControlIpcKind::SecretApplyRequest, 152, *applyPayload)).header.kind);
	EXPECT_EQ((std::vector<std::string>{ "validate", "migrate", "apply" }), calls);
}

TEST(ControlSecretVaultRpc, MigrationFailureFailsClosedBeforeGetAndApply)
{
	CFakeVault vault; CFakeCapabilityService capability; CFakeLegacyMigrationCoordinator migration;
	migration.result = { secrets::ESecretVaultLegacyMigrationStatus::LegacyCorruptData };
	auto session = MigratingSession(vault, capability, migration);
	const auto getPayload = EncodeControlSecretVaultGetRequest(GetRequest());
	const auto applyPayload = EncodeControlSecretVaultApplyRequest(ApplyRequest());
	ASSERT_TRUE(getPayload);
	ASSERT_TRUE(applyPayload);

	const auto get = session.Process(Request(EControlIpcKind::SecretGetRequest, 153, *getPayload));
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, ErrorStatus(get));
	EXPECT_TRUE(vault.lastGet.extensionId.empty());
	EXPECT_FALSE(Contains(get.payload, "publisher.one"));
	const auto apply = session.Process(Request(EControlIpcKind::SecretApplyRequest, 154, *applyPayload));
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, ErrorStatus(apply));
	EXPECT_TRUE(vault.lastApply.extensionId.empty());
	EXPECT_EQ(2, migration.ensureCount);
}

TEST(ControlSecretVaultRpc, RejectsDuplicateMalformedUtf8EnumAndOversizePayloadsBeforeValidation)
{
	const auto good = EncodeControlSecretVaultGetRequest(GetRequest());
	ASSERT_TRUE(good);
	auto duplicate = *good;
	const std::array<std::uint8_t, secrets::kSecretVaultCapabilityTokenBytes> token{};
	AppendField(duplicate, static_cast<std::uint16_t>(EControlIpcFieldTag::Capability), token);
	EXPECT_FALSE(DecodeControlSecretVaultGetRequest(duplicate));

	std::vector<std::uint8_t> invalidUtf8;
	AppendField(invalidUtf8, static_cast<std::uint16_t>(EControlIpcFieldTag::Capability), token);
	const std::array<std::uint8_t, 1> bad{ 0xff };
	AppendField(invalidUtf8, static_cast<std::uint16_t>(EControlIpcFieldTag::ExtensionHostSessionId), bad);
	EXPECT_FALSE(DecodeControlSecretVaultGetRequest(invalidUtf8));

	auto mutation = ApplyRequest(); mutation.mutation.kind = static_cast<secrets::ESecretMutationKind>(99);
	EXPECT_FALSE(EncodeControlSecretVaultApplyRequest(mutation));
	std::vector<std::uint8_t> huge(kControlSecretVaultRpcMaximumPayloadBytes + 1);
	EXPECT_FALSE(DecodeControlSecretVaultGetRequest(huge));

	CFakeVault vault; CFakeCapabilityService capability; auto session = Session(vault, capability);
	const auto response = session.Process(Request(EControlIpcKind::SecretGetRequest, 103, duplicate));
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, ErrorStatus(response));
	EXPECT_EQ(0, capability.validateCount);
}

TEST(ControlSecretVaultRpc, AppliesWithoutLeakingSecretCapabilityOrDiagnostic)
{
	CFakeVault vault; CFakeCapabilityService capability; auto session = Session(vault, capability);
	const auto payload = EncodeControlSecretVaultApplyRequest(ApplyRequest());
	ASSERT_TRUE(payload);
	const auto response = session.Process(Request(EControlIpcKind::SecretApplyRequest, 104, *payload));
	ASSERT_EQ(EControlIpcKind::SecretApplyResponse, response.header.kind);
	const auto result = DecodeControlSecretVaultApplyResponse(response.payload);
	ASSERT_TRUE(result);
	EXPECT_EQ(secrets::ESecretMutationStatus::Succeeded, result->status);
	EXPECT_EQ("", result->diagnostic);
	EXPECT_TRUE(result->change);
	EXPECT_EQ("", result->change->profileId);
	EXPECT_FALSE(Contains(response.payload, "super-secret"));
	EXPECT_FALSE(Contains(response.payload, "do not serialize"));
	EXPECT_FALSE(Contains(response.payload, "host-session"));
	EXPECT_FALSE(std::search_n(response.payload.begin(), response.payload.end(), 32, static_cast<std::uint8_t>(0xa5)) != response.payload.end());
}

TEST(ControlSecretVaultRpc, RejectsGenerationAndHeaderFailuresAndMapsStoppedAndThrownAuthorities)
{
	CFakeVault vault; CFakeCapabilityService capability; auto session = Session(vault, capability);
	const auto payload = EncodeControlSecretVaultGetRequest(GetRequest());
	ASSERT_TRUE(payload);
	auto stale = Request(EControlIpcKind::SecretGetRequest, 105, *payload); stale.header.generation = kGeneration - 1;
	EXPECT_EQ(EControlIpcTerminalStatus::GenerationMismatch, ErrorStatus(session.Process(stale)));
	auto wrongFlags = Request(EControlIpcKind::SecretGetRequest, 106, *payload); wrongFlags.header.flags = EControlIpcFlags::Response;
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest, ErrorStatus(session.Process(wrongFlags)));
	auto oldMajor = Request(EControlIpcKind::SecretGetRequest, 107, *payload); oldMajor.header.majorVersion = 2;
	EXPECT_EQ(EControlIpcTerminalStatus::UnsupportedVersion, ErrorStatus(session.Process(oldMajor)));

	capability.status = secrets::ESecretVaultCapabilityValidationStatus::Stopped;
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping, ErrorStatus(session.Process(Request(EControlIpcKind::SecretGetRequest, 108, *payload))));
	capability.status = secrets::ESecretVaultCapabilityValidationStatus::Valid;
	vault.get = { secrets::ESecretGetStatus::Stopped, 0, std::nullopt };
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping, ErrorStatus(session.Process(Request(EControlIpcKind::SecretGetRequest, 109, *payload))));
	vault.throwOnGet = true;
	EXPECT_EQ(EControlIpcTerminalStatus::InternalError, ErrorStatus(session.Process(Request(EControlIpcKind::SecretGetRequest, 110, *payload))));
}

TEST(ControlSecretVaultRpc, IssuesOnlyAfterExactGrantAuthorizationAndDoesNotLeakBinding)
{
	CFakeVault vault; CFakeCapabilityService capability; CFakeGrantAuthority authority;
	authority.issueAuthorization = { secrets::ESecretVaultExtensionGrantAuthorizationStatus::Authorized,
		{ { std::string(kProfileId), "host-session", 99, kGeneration }, "publisher.one" }, 4 };
	secrets::SecretVaultCapabilityToken issuedToken{}; issuedToken.fill(0x5a);
	capability.issue = { secrets::ESecretVaultCapabilityIssueStatus::Issued, issuedToken,
		std::chrono::steady_clock::now() + std::chrono::minutes(10) };
	auto session = AuthorizedSession(vault, capability, authority);
	const auto request = CapabilityIssueRequest();
	const auto payload = EncodeControlSecretVaultCapabilityIssueRequest(request);
	ASSERT_TRUE(payload);
	const auto decodedRequest = DecodeControlSecretVaultCapabilityIssueRequest(*payload);
	ASSERT_TRUE(decodedRequest);
	EXPECT_EQ(request.hostSessionId, decodedRequest->hostSessionId);
	EXPECT_EQ(request.hostGeneration, decodedRequest->hostGeneration);
	EXPECT_EQ(request.extensionId, decodedRequest->extensionId);
	EXPECT_EQ(request.lifetime, decodedRequest->lifetime);
	const auto response = session.Process(Request(EControlIpcKind::SecretCapabilityIssueRequest, 111, *payload));
	ASSERT_EQ(EControlIpcKind::SecretCapabilityIssueResponse, response.header.kind);
	const auto decoded = DecodeControlSecretVaultCapabilityIssueResponse(response.payload);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(issuedToken, decoded->capability);
	EXPECT_EQ(request.lifetime, decoded->lifetime);
	EXPECT_EQ(1, authority.issueAuthorizationCount);
	EXPECT_EQ(1, capability.issueCount);
	EXPECT_EQ(std::string(kProfileId), capability.lastIssue.binding.session.profileId);
	EXPECT_EQ(99u, capability.lastIssue.binding.session.clientProcessId);
	EXPECT_EQ(kGeneration, capability.lastIssue.binding.session.connectionGeneration);
	EXPECT_EQ("publisher.one", capability.lastIssue.binding.extensionId);
	EXPECT_FALSE(Contains(response.payload, "host-session"));
	EXPECT_FALSE(Contains(response.payload, "publisher.one"));
}

TEST(ControlSecretVaultRpc, DeniedMalformedAndCompatibilityIssueRequestsNeverCallCapabilityIssuer)
{
	CFakeVault vault; CFakeCapabilityService capability; CFakeGrantAuthority authority;
	auto session = AuthorizedSession(vault, capability, authority);
	const auto payload = EncodeControlSecretVaultCapabilityIssueRequest(CapabilityIssueRequest());
	ASSERT_TRUE(payload);
	const auto denied = session.Process(Request(EControlIpcKind::SecretCapabilityIssueRequest, 112, *payload));
	EXPECT_EQ(EControlIpcTerminalStatus::AccessDenied, ErrorStatus(denied));
	EXPECT_EQ(1, authority.issueAuthorizationCount);
	EXPECT_EQ(0, capability.issueCount);
	EXPECT_FALSE(Contains(denied.payload, "host-session"));

	auto malformed = *payload;
	const std::array<std::uint8_t, 8> extra{};
	AppendField(malformed, static_cast<std::uint16_t>(EControlIpcFieldTag::ExtensionHostGeneration), extra);
	EXPECT_EQ(EControlIpcTerminalStatus::InvalidRequest,
		ErrorStatus(session.Process(Request(EControlIpcKind::SecretCapabilityIssueRequest, 113, malformed))));
	EXPECT_EQ(1, authority.issueAuthorizationCount);
	EXPECT_EQ(0, capability.issueCount);

	auto compatibility = Session(vault, capability);
	EXPECT_EQ(EControlIpcTerminalStatus::AccessDenied,
		ErrorStatus(compatibility.Process(Request(EControlIpcKind::SecretCapabilityIssueRequest, 114, *payload))));
	EXPECT_EQ(0, capability.issueCount);
}

TEST(ControlSecretVaultRpc, RevokesServerDerivedLeaseWithoutChangingTheBrokerOwnedLease)
{
	CFakeVault vault; CFakeCapabilityService capability; CFakeGrantAuthority authority;
	authority.revokeAuthorization = { secrets::ESecretVaultExtensionGrantAuthorizationStatus::Authorized,
		{ std::string(kProfileId), "host-session", 99, kGeneration }, 9, 5 };
	auto session = AuthorizedSession(vault, capability, authority);
	const auto payload = EncodeControlSecretVaultCapabilityRevokeSessionRequest();
	ASSERT_TRUE(payload);
	const auto response = session.Process(Request(EControlIpcKind::SecretCapabilityRevokeSessionRequest, 115, *payload));
	EXPECT_EQ(EControlIpcKind::SecretCapabilityRevokeSessionResponse, response.header.kind);
	EXPECT_EQ(EControlIpcFlags::Response | EControlIpcFlags::Terminal, response.header.flags);
	EXPECT_TRUE(DecodeControlSecretVaultCapabilityRevokeSessionResponse(response.payload));
	EXPECT_EQ(1, authority.revokeAuthorizationCount);
	EXPECT_EQ(1, capability.revokeCount);
	EXPECT_EQ(99u, capability.lastRevokedSession.clientProcessId);
	EXPECT_EQ(std::string(kProfileId), capability.lastRevokedSession.profileId);
	EXPECT_FALSE(Contains(response.payload, "host-session"));

	capability.revoke = { secrets::ESecretVaultCapabilityRevokeStatus::Stopped, 0 };
	EXPECT_EQ(EControlIpcTerminalStatus::ServerStopping,
		ErrorStatus(session.Process(Request(EControlIpcKind::SecretCapabilityRevokeSessionRequest, 116, *payload))));
}

} // namespace
} // namespace platform::controlipc
