/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/secrets/CSecretVaultCapabilityService.h"

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <utility>

namespace {

using namespace std::chrono_literals;
using namespace platform::secrets;

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kOtherProfileId = "fedcba9876543210fedcba9876543210";

class CDeterministicCapabilityTokenSource final : public ISecretVaultCapabilityTokenSource {
public:
	void Push(std::uint8_t firstByte)
	{
		SecretVaultCapabilityToken token{};
		for (std::size_t index = 0; index < token.size(); ++index) {
			token[index] = static_cast<std::uint8_t>(firstByte + index);
		}
		m_tokens.push_back(token);
	}

	[[nodiscard]] bool Fill(SecretVaultCapabilityToken& token) noexcept override
	{
		if (m_fail || m_tokens.empty()) {
			return false;
		}
		token = m_tokens.front();
		m_tokens.pop_front();
		return true;
	}

	bool m_fail = false;

private:
	std::deque<SecretVaultCapabilityToken> m_tokens;
};

class CDeterministicCapabilityClock final : public ISecretVaultCapabilityClock {
public:
	[[nodiscard]] std::chrono::steady_clock::time_point Now() const noexcept override { return m_now; }
	void Advance(std::chrono::milliseconds elapsed) noexcept { m_now += elapsed; }

private:
	std::chrono::steady_clock::time_point m_now{};
};

SecretVaultCapabilitySessionIdentity Session(
	std::string profileId = std::string(kProfileId),
	std::string sessionId = "host-session-a",
	std::uint32_t clientProcessId = 7001,
	std::uint64_t generation = 11)
{
	return {
		.profileId = std::move(profileId),
		.extensionHostSessionId = std::move(sessionId),
		.clientProcessId = clientProcessId,
		.connectionGeneration = generation,
	};
}

SecretVaultCapabilityHostSessionIdentity HostSession(
	std::string profileId = std::string(kProfileId),
	std::string sessionId = "host-session-a",
	std::uint64_t generation = 11)
{
	return {
		.profileId = std::move(profileId),
		.extensionHostSessionId = std::move(sessionId),
		.connectionGeneration = generation,
	};
}

SecretVaultCapabilityBinding Binding(
	std::string extensionId = "sample.publisher",
	SecretVaultCapabilitySessionIdentity session = Session())
{
	return { .session = std::move(session), .extensionId = std::move(extensionId) };
}

SecretVaultCapabilityIssueRequest IssueRequest(
	std::string extensionId = "sample.publisher",
	SecretVaultCapabilitySessionIdentity session = Session(),
	std::chrono::milliseconds lifetime = 1min)
{
	return { .binding = Binding(std::move(extensionId), std::move(session)), .lifetime = lifetime };
}

SecretVaultCapabilityValidationRequest ValidationRequest(
	const SecretVaultCapabilityToken& capability,
	SecretVaultCapabilitySessionIdentity session = Session(),
	std::string extensionId = "sample.publisher")
{
	return {
		.capability = capability,
		.session = std::move(session),
		.address = { .extensionId = std::move(extensionId), .key = "access-token" },
	};
}

struct ServiceFixture {
	ServiceFixture(std::size_t maximumGrants = 8)
		: tokenSource(std::make_shared<CDeterministicCapabilityTokenSource>())
		, clock(std::make_shared<CDeterministicCapabilityClock>())
		, service(std::string(kProfileId), tokenSource, clock,
			SecretVaultCapabilityServiceConfig{ .maximumGrants = maximumGrants, .maximumLifetime = 1min })
	{
	}

	std::shared_ptr<CDeterministicCapabilityTokenSource> tokenSource;
	std::shared_ptr<CDeterministicCapabilityClock> clock;
	CSecretVaultCapabilityService service;
};

[[nodiscard]] SecretVaultCapabilityToken IssueOne(ServiceFixture& fixture,
	const SecretVaultCapabilityIssueRequest& request = IssueRequest())
{
	const auto result = fixture.service.Issue(request);
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::Issued, result.status);
	EXPECT_TRUE(result.capability.has_value());
	return result.capability.value_or(SecretVaultCapabilityToken{});
}

TEST(SecretVaultCapabilityService, SharedHostCannotUseAnotherExtensionsNamespace)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(1);
	fixture.tokenSource->Push(2);
	const auto first = IssueOne(fixture, IssueRequest("first.extension"));
	const auto second = IssueOne(fixture, IssueRequest("second.extension"));

	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(first, Session(), "first.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::ExtensionMismatch,
		fixture.service.Validate(ValidationRequest(first, Session(), "second.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(second, Session(), "second.extension")).status);
}

TEST(SecretVaultCapabilityService, ValidationRejectsEveryWrongConnectionBindingAndBearer)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(10);
	const auto capability = IssueOne(fixture);

	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::ProfileMismatch,
		fixture.service.Validate(ValidationRequest(capability, Session(std::string(kOtherProfileId)))).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::SessionMismatch,
		fixture.service.Validate(ValidationRequest(capability, Session(std::string(kProfileId), "host-session-b"))).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::ClientProcessMismatch,
		fixture.service.Validate(ValidationRequest(capability, Session(std::string(kProfileId), "host-session-a", 7002))).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::ConnectionGenerationMismatch,
		fixture.service.Validate(ValidationRequest(capability, Session(std::string(kProfileId), "host-session-a", 7001, 12))).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::ExtensionMismatch,
		fixture.service.Validate(ValidationRequest(capability, Session(), "other.extension")).status);

	auto wrongCapability = capability;
	wrongCapability.front() ^= 0x80;
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(wrongCapability)).status);
}

TEST(SecretVaultCapabilityService, ExactCanonicalExtensionIdentityIsRequiredAtBothBoundaries)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(20);
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::InvalidBinding,
		fixture.service.Issue(IssueRequest("Sample.Publisher")).status);

	const auto capability = IssueOne(fixture);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::InvalidAddress,
		fixture.service.Validate(ValidationRequest(capability, Session(), "Sample.Publisher")).status);
}

TEST(SecretVaultCapabilityService, ExpiryIsMonotonicAndExpiredGrantIsPruned)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(30);
	const auto capability = IssueOne(fixture, IssueRequest("sample.publisher", Session(), 10ms));
	fixture.clock->Advance(10ms);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Expired,
		fixture.service.Validate(ValidationRequest(capability)).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(capability)).status);
}

TEST(SecretVaultCapabilityService, RevokeExtensionOnlyInvalidatesThatCanonicalExtensionBinding)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(40);
	fixture.tokenSource->Push(50);
	const auto first = IssueOne(fixture, IssueRequest("first.extension"));
	const auto second = IssueOne(fixture, IssueRequest("second.extension"));

	const auto revoke = fixture.service.RevokeExtension(Binding("first.extension"));
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Revoked, revoke.status);
	EXPECT_EQ(1u, revoke.revokedGrantCount);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(first, Session(), "first.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(second, Session(), "second.extension")).status);
}

TEST(SecretVaultCapabilityService, RevokeSessionInvalidatesAllExtensionsForThatConnection)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(60);
	fixture.tokenSource->Push(70);
	fixture.tokenSource->Push(80);
	const auto first = IssueOne(fixture, IssueRequest("first.extension"));
	const auto second = IssueOne(fixture, IssueRequest("second.extension"));
	const auto otherSession = Session(std::string(kProfileId), "host-session-b");
	const auto third = IssueOne(fixture, IssueRequest("third.extension", otherSession));

	const auto revoke = fixture.service.RevokeSession(Session());
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Revoked, revoke.status);
	EXPECT_EQ(2u, revoke.revokedGrantCount);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(first, Session(), "first.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(second, Session(), "second.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(third, otherSession, "third.extension")).status);
}

TEST(SecretVaultCapabilityService, RevokeHostSessionInvalidatesEveryPidForOneExactHostGeneration)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(61);
	fixture.tokenSource->Push(62);
	fixture.tokenSource->Push(63);
	fixture.tokenSource->Push(64);
	fixture.tokenSource->Push(65);
	const auto first = IssueOne(fixture, IssueRequest("first.extension", Session()));
	const auto second = IssueOne(fixture, IssueRequest("second.extension", Session()));
	const auto otherPid = Session(std::string(kProfileId), "host-session-a", 7002);
	const auto third = IssueOne(fixture, IssueRequest("third.extension", otherPid));
	const auto otherHost = Session(std::string(kProfileId), "host-session-b", 7003);
	const auto fourth = IssueOne(fixture, IssueRequest("fourth.extension", otherHost));
	const auto otherGeneration = Session(std::string(kProfileId), "host-session-a", 7004, 12);
	const auto fifth = IssueOne(fixture, IssueRequest("fifth.extension", otherGeneration));

	const auto revoke = fixture.service.RevokeHostSession(HostSession());
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Revoked, revoke.status);
	EXPECT_EQ(3u, revoke.revokedGrantCount);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(first, Session(), "first.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(second, Session(), "second.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::CapabilityMismatch,
		fixture.service.Validate(ValidationRequest(third, otherPid, "third.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(fourth, otherHost, "fourth.extension")).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Valid,
		fixture.service.Validate(ValidationRequest(fifth, otherGeneration, "fifth.extension")).status);
}

TEST(SecretVaultCapabilityService, CapacityIsBoundedAndExpiryReclaimsOneSlot)
{
	ServiceFixture fixture(1);
	fixture.tokenSource->Push(90);
	fixture.tokenSource->Push(100);
	(void)IssueOne(fixture, IssueRequest("first.extension", Session(), 10ms));
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::CapacityReached,
		fixture.service.Issue(IssueRequest("second.extension")).status);

	fixture.clock->Advance(10ms);
	const auto issuedAfterPrune = fixture.service.Issue(IssueRequest("second.extension"));
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::Issued, issuedAfterPrune.status);
}

TEST(SecretVaultCapabilityService, TokenDigestCollisionDoesNotShareAuthority)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(110);
	fixture.tokenSource->Push(110);
	(void)IssueOne(fixture, IssueRequest("first.extension"));
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::Collision,
		fixture.service.Issue(IssueRequest("second.extension")).status);
}

TEST(SecretVaultCapabilityService, EntropyFailureAndInvalidConfigurationHaveExplicitTerminalOutcomes)
{
	ServiceFixture fixture;
	fixture.tokenSource->m_fail = true;
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::EntropyFailure,
		fixture.service.Issue(IssueRequest()).status);

	auto tokenSource = std::make_shared<CDeterministicCapabilityTokenSource>();
	auto clock = std::make_shared<CDeterministicCapabilityClock>();
	CSecretVaultCapabilityService invalid(std::string(kProfileId), tokenSource, clock,
		SecretVaultCapabilityServiceConfig{ .maximumGrants = 0, .maximumLifetime = 1min });
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::InvalidConfiguration,
		invalid.Issue(IssueRequest()).status);
}

TEST(SecretVaultCapabilityService, StopIsTerminalAndIdempotent)
{
	ServiceFixture fixture;
	fixture.tokenSource->Push(120);
	const auto capability = IssueOne(fixture);
	EXPECT_EQ(ESecretVaultCapabilityStopStatus::Stopped, fixture.service.Stop());
	EXPECT_EQ(ESecretVaultCapabilityStopStatus::AlreadyStopped, fixture.service.Stop());
	EXPECT_EQ(ESecretVaultCapabilityIssueStatus::Stopped, fixture.service.Issue(IssueRequest()).status);
	EXPECT_EQ(ESecretVaultCapabilityValidationStatus::Stopped,
		fixture.service.Validate(ValidationRequest(capability)).status);
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Stopped,
		fixture.service.RevokeExtension(Binding()).status);
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Stopped,
		fixture.service.RevokeSession(Session()).status);
	EXPECT_EQ(ESecretVaultCapabilityRevokeStatus::Stopped,
		fixture.service.RevokeHostSession(HostSession()).status);
}

} // namespace
