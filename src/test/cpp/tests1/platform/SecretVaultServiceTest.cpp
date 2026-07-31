/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/secrets/CInMemorySecretVaultService.h"

#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

namespace platform::secrets {
namespace {

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";

SecretMutationRequest Set(std::string extensionId, std::string key, std::string value,
	std::string operationId, std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .kind = ESecretMutationKind::Set, .extensionId = std::move(extensionId),
		.key = std::move(key), .value = std::move(value), .operationId = std::move(operationId),
		.expectedRevision = expectedRevision };
}

SecretMutationRequest Delete(std::string extensionId, std::string key, std::string operationId,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .kind = ESecretMutationKind::Delete, .extensionId = std::move(extensionId),
		.key = std::move(key), .operationId = std::move(operationId),
		.expectedRevision = expectedRevision };
}

TEST(SecretVaultTypes, ValidatesUtf8IdentifiersAndCanonicalExtensionIdentity)
{
	std::string canonical;
	EXPECT_TRUE(IsValidSecretVaultIdentifier(kProfileId, kMaximumSecretVaultProfileIdBytes));
	EXPECT_FALSE(IsValidSecretVaultIdentifier("", kMaximumSecretVaultProfileIdBytes));
	EXPECT_FALSE(IsValidSecretVaultIdentifier("bad\nvalue", kMaximumSecretVaultKeyBytes));
	EXPECT_FALSE(IsValidSecretVaultIdentifier(std::string("\xc0\xaf", 2), kMaximumSecretVaultKeyBytes));
	EXPECT_TRUE(CanonicalizeSecretVaultExtensionId("Publisher.Extension", canonical));
	EXPECT_EQ("publisher.extension", canonical);
	EXPECT_FALSE(CanonicalizeSecretVaultExtensionId("publisher", canonical));
	EXPECT_FALSE(CanonicalizeSecretVaultExtensionId("publisher..extension", canonical));
	EXPECT_FALSE(CanonicalizeSecretVaultExtensionId("publisher.extension.extra", canonical));
	EXPECT_FALSE(CanonicalizeSecretVaultExtensionId("publisher.-extension", canonical));
	EXPECT_TRUE(IsValidSecretVaultUtf8("line one\nline two"));
}

TEST(SecretVaultService, CanonicalizesExtensionIdentityAndReturnsTypedGetResults)
{
	CInMemorySecretVaultService vault{ std::string(kProfileId) };

	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Set("Publisher.Extension", "token", "secret", "set-1", 0)).status);
	const auto found = vault.Get("PUBLISHER.EXTENSION", "token");
	EXPECT_EQ(ESecretGetStatus::Found, found.status);
	ASSERT_TRUE(found.value);
	EXPECT_EQ("secret", *found.value);
	EXPECT_EQ(ESecretGetStatus::NotFound, vault.Get("publisher.extension", "missing").status);
	EXPECT_EQ(ESecretGetStatus::Invalid, vault.Get("not-an-extension", "token").status);
}

TEST(SecretVaultService, RejectsANonCanonicalProfileAuthorityIdentity)
{
	CInMemorySecretVaultService vault("profile-01");

	EXPECT_EQ(ESecretGetStatus::Invalid, vault.Get("publisher.extension", "token").status);
	EXPECT_EQ(ESecretMutationStatus::Invalid,
		vault.Apply(Set("publisher.extension", "token", "secret", "set-1", 0)).status);
	EXPECT_FALSE(vault.Subscribe([](const SecretChange&) {}));
}

TEST(SecretVaultService, CompareAndSetAndExactReplayPreventLostUpdates)
{
	CInMemorySecretVaultService vault{ std::string(kProfileId) };
	const auto firstRequest = Set("publisher.extension", "token", "one", "operation-1", 0);
	const auto first = vault.Apply(firstRequest);
	const auto replay = vault.Apply(firstRequest);
	const auto stale = vault.Apply(Set("publisher.extension", "token", "two", "operation-2", 0));
	const auto collision = vault.Apply(Set("publisher.extension", "token", "two", "operation-1", 1));

	EXPECT_EQ(ESecretMutationStatus::Succeeded, first.status);
	EXPECT_EQ(1u, first.revision);
	EXPECT_EQ(ESecretMutationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(ESecretMutationStatus::Conflict, stale.status);
	EXPECT_EQ(ESecretMutationStatus::Invalid, collision.status);
	EXPECT_EQ("one", *vault.Get("publisher.extension", "token").value);
}

TEST(SecretVaultService, IdempotentMutationsDoNotAdvanceOrNotify)
{
	CInMemorySecretVaultService vault{ std::string(kProfileId) };
	std::vector<SecretChange> changes;
	auto subscription = vault.Subscribe([&](const SecretChange& change) { changes.push_back(change); });
	ASSERT_TRUE(subscription);

	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Set("publisher.extension", "token", "one", "set-1", 0)).status);
	EXPECT_EQ(ESecretMutationStatus::NotApplicable,
		vault.Apply(Set("publisher.extension", "token", "one", "set-2", 1)).status);
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Delete("publisher.extension", "token", "delete-1", 1)).status);
	EXPECT_EQ(ESecretMutationStatus::NotApplicable,
		vault.Apply(Delete("publisher.extension", "token", "delete-2", 2)).status);

	ASSERT_EQ(2u, changes.size());
	EXPECT_EQ(ESecretChangeKind::Set, changes[0].kind);
	EXPECT_EQ(ESecretChangeKind::Delete, changes[1].kind);
	EXPECT_EQ(1u, changes[0].revision);
	EXPECT_EQ(2u, changes[1].revision);
	EXPECT_EQ(kProfileId, changes[0].profileId);
}

TEST(SecretVaultService, EventsFollowCommitNeverExposeValueAndCallbackFailureDoesNotStrandDelivery)
{
	CInMemorySecretVaultService vault{ std::string(kProfileId) };
	bool firstObservedCommittedValue = false;
	int secondCallbackCount = 0;
	auto throwing = vault.Subscribe([&](const SecretChange& change) {
		const auto result = vault.Get(change.address.extensionId, change.address.key);
		firstObservedCommittedValue = result.status == ESecretGetStatus::Found
			&& result.value && *result.value == "secret";
		throw std::runtime_error("expected listener failure");
	});
	auto remaining = vault.Subscribe([&](const SecretChange& change) {
		EXPECT_EQ(kProfileId, change.profileId);
		EXPECT_EQ("publisher.extension", change.address.extensionId);
		EXPECT_EQ("token", change.address.key);
		++secondCallbackCount;
	});
	ASSERT_TRUE(throwing);
	ASSERT_TRUE(remaining);

	const auto result = vault.Apply(Set("Publisher.Extension", "token", "secret", "set-1", 0));
	EXPECT_EQ(ESecretMutationStatus::Succeeded, result.status);
	ASSERT_TRUE(result.change);
	EXPECT_EQ("publisher.extension", result.change->address.extensionId);
	EXPECT_TRUE(firstObservedCommittedValue);
	EXPECT_EQ(1, secondCallbackCount);
}

TEST(SecretVaultService, EnforcesRequestReplayAndSubscriptionBounds)
{
	CInMemorySecretVaultService vault(std::string(kProfileId), 1, 1);
	EXPECT_EQ(ESecretMutationStatus::Invalid,
		vault.Apply(Set("publisher.extension", "token", std::string(kMaximumSecretVaultValueBytes + 1, 'x'), "set-1")).status);
	EXPECT_EQ(ESecretMutationStatus::Invalid,
		vault.Apply(Set("publisher.extension", "token", "secret", std::string(kMaximumSecretVaultOperationIdBytes + 1, 'o'))).status);
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Set("publisher.extension", "one", "1", "old", 0)).status);
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Set("publisher.extension", "two", "2", "new", 1)).status);
	// The bounded replay ledger retains only the latest operation.
	EXPECT_EQ(ESecretMutationStatus::Succeeded,
		vault.Apply(Set("publisher.extension", "one", "3", "old", 2)).status);
	auto first = vault.Subscribe([](const SecretChange&) {});
	auto second = vault.Subscribe([](const SecretChange&) {});
	EXPECT_TRUE(first);
	EXPECT_FALSE(second);
}

TEST(SecretVaultService, StopIsTerminalAndIdempotent)
{
	CInMemorySecretVaultService vault{ std::string(kProfileId) };
	auto subscription = vault.Subscribe([](const SecretChange&) {});
	ASSERT_TRUE(subscription);
	EXPECT_EQ(ESecretVaultStopStatus::Stopped, vault.Stop());
	EXPECT_EQ(ESecretVaultStopStatus::AlreadyStopped, vault.Stop());
	EXPECT_FALSE(subscription->IsSubscribed());
	EXPECT_EQ(ESecretGetStatus::Stopped, vault.Get("publisher.extension", "token").status);
	EXPECT_EQ(ESecretMutationStatus::Stopped,
		vault.Apply(Set("publisher.extension", "token", "secret", "set-1")).status);
	EXPECT_FALSE(vault.Subscribe([](const SecretChange&) {}));
}

} // namespace
} // namespace platform::secrets
