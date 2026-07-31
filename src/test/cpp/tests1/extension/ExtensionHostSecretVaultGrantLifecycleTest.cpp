/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionHostSecretVaultGrantLifecycle.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct GrantCall {
	std::string_view name;
	SExtensionHostSecretVaultGrantSession session;
	std::uint32_t processId = 0;
	std::uint64_t expectedRevision = 0;
	std::vector<std::string> extensionIds;
};

class FakeGrantLifecycle final : public IExtensionHostSecretVaultGrantLifecycle {
public:
	bool failActivation = false;
	bool conflictRegister = false;
	std::vector<GrantCall> calls;

	SExtensionHostSecretVaultGrantResult Activate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "activate", session, 0, expectedRevision, {} });
		return Result(expectedRevision, failActivation);
	}

	SExtensionHostSecretVaultGrantResult RegisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t processId,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "register", session, processId, expectedRevision, {} });
		return Result(expectedRevision, conflictRegister);
	}

	SExtensionHostSecretVaultGrantResult UnregisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t processId,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "unregister", session, processId, expectedRevision, {} });
		return Result(expectedRevision);
	}

	SExtensionHostSecretVaultGrantResult ReplaceInstalledExtensionInventory(
		const SExtensionHostSecretVaultGrantSession& session,
		const std::vector<std::string>& extensionIds,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "replace", session, 0, expectedRevision, extensionIds });
		return Result(expectedRevision);
	}

	SExtensionHostSecretVaultGrantResult RevokeIssuedCapabilities(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "revoke", session, 0, expectedRevision, {} });
		return Result(expectedRevision);
	}

	SExtensionHostSecretVaultGrantResult Deactivate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override
	{
		calls.push_back({ "deactivate", session, 0, expectedRevision, {} });
		return Result(expectedRevision);
	}

private:
	static SExtensionHostSecretVaultGrantResult Result(std::uint64_t expectedRevision, bool conflict = false)
	{
		return {
			conflict ? EExtensionHostSecretVaultGrantStatus::Conflict : EExtensionHostSecretVaultGrantStatus::Succeeded,
			conflict ? expectedRevision : expectedRevision + 1,
		};
	}
};

SExtensionHostSecretVaultGrantSession Session(std::uint64_t generation, std::wstring id)
{
	return { generation, std::move(id) };
}

TEST(ExtensionHostSecretVaultGrantCoordinator, RegistersDistinctProcessOnlyOnceAndRevokesBeforeDeactivation)
{
	auto lifecycle = std::make_shared<FakeGrantLifecycle>();
	CExtensionHostSecretVaultGrantCoordinator coordinator(lifecycle);

	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Deferred, coordinator.AcquireEditorLease(401));
	EXPECT_TRUE(coordinator.Activate(Session(11, L"session-11")));
	EXPECT_TRUE(coordinator.ReplaceInstalledExtensionInventory({ "Publisher.Extension", "publisher.extension" }));
	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Registered, coordinator.AcquireEditorLease(401));
	EXPECT_TRUE(coordinator.ReleaseEditorLease(401));
	EXPECT_TRUE(coordinator.ReleaseEditorLease(401));
	coordinator.RevokeAndDeactivate();

	ASSERT_EQ(6u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[0].name);
	EXPECT_EQ("register", lifecycle->calls[1].name);
	EXPECT_EQ(401u, lifecycle->calls[1].processId);
	EXPECT_EQ("replace", lifecycle->calls[2].name);
	EXPECT_EQ(std::vector<std::string>({ "publisher.extension" }), lifecycle->calls[2].extensionIds);
	EXPECT_EQ("unregister", lifecycle->calls[3].name);
	EXPECT_EQ(401u, lifecycle->calls[3].processId);
	EXPECT_EQ("revoke", lifecycle->calls[4].name);
	EXPECT_EQ("deactivate", lifecycle->calls[5].name);
	EXPECT_EQ(5u, lifecycle->calls[5].expectedRevision);
}

TEST(ExtensionHostSecretVaultGrantCoordinator, GenerationRolloverFencesOldSessionBeforeReissuingPidLease)
{
	auto lifecycle = std::make_shared<FakeGrantLifecycle>();
	CExtensionHostSecretVaultGrantCoordinator coordinator(lifecycle);

	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Deferred, coordinator.AcquireEditorLease(404));
	EXPECT_TRUE(coordinator.Activate(Session(21, L"session-21")));
	EXPECT_TRUE(coordinator.Activate(Session(22, L"session-22")));

	ASSERT_EQ(6u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[0].name);
	EXPECT_EQ("register", lifecycle->calls[1].name);
	EXPECT_EQ("revoke", lifecycle->calls[2].name);
	EXPECT_EQ(21u, lifecycle->calls[2].session.generation);
	EXPECT_EQ("deactivate", lifecycle->calls[3].name);
	EXPECT_EQ(21u, lifecycle->calls[3].session.generation);
	EXPECT_EQ("activate", lifecycle->calls[4].name);
	EXPECT_EQ(22u, lifecycle->calls[4].session.generation);
	EXPECT_EQ("register", lifecycle->calls[5].name);
	EXPECT_EQ(22u, lifecycle->calls[5].session.generation);
}

TEST(ExtensionHostSecretVaultGrantCoordinator, ActivationFailureDoesNotIssuePidCapabilityAndIsFenced)
{
	auto lifecycle = std::make_shared<FakeGrantLifecycle>();
	lifecycle->failActivation = true;
	CExtensionHostSecretVaultGrantCoordinator coordinator(lifecycle);

	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Deferred, coordinator.AcquireEditorLease(405));
	EXPECT_FALSE(coordinator.Activate(Session(31, L"session-31")));
	EXPECT_FALSE(coordinator.IsActiveForGeneration(31));

	ASSERT_EQ(3u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[0].name);
	EXPECT_EQ("revoke", lifecycle->calls[1].name);
	EXPECT_EQ("deactivate", lifecycle->calls[2].name);
	for (const auto& call : lifecycle->calls) {
		EXPECT_NE("register", call.name);
	}
}

TEST(ExtensionHostSecretVaultGrantCoordinator, RevisionConflictFailsClosedAndPreventsSubsequentGrantUse)
{
	auto lifecycle = std::make_shared<FakeGrantLifecycle>();
	CExtensionHostSecretVaultGrantCoordinator coordinator(lifecycle);

	EXPECT_TRUE(coordinator.Activate(Session(41, L"session-41")));
	lifecycle->conflictRegister = true;
	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Rejected, coordinator.AcquireEditorLease(406));
	EXPECT_FALSE(coordinator.IsActiveForGeneration(41));

	ASSERT_EQ(4u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[0].name);
	EXPECT_EQ("register", lifecycle->calls[1].name);
	EXPECT_EQ("revoke", lifecycle->calls[2].name);
	EXPECT_EQ("deactivate", lifecycle->calls[3].name);
}

TEST(ExtensionHostSecretVaultGrantCoordinator, ShutdownIsIdempotentAndDropsLeasesForTheNextControllerLifetime)
{
	auto lifecycle = std::make_shared<FakeGrantLifecycle>();
	CExtensionHostSecretVaultGrantCoordinator coordinator(lifecycle);

	EXPECT_EQ(EExtensionHostSecretVaultLeaseAcquireResult::Deferred, coordinator.AcquireEditorLease(407));
	EXPECT_TRUE(coordinator.Activate(Session(51, L"session-51")));
	coordinator.Shutdown();
	coordinator.Shutdown();

	ASSERT_EQ(4u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[0].name);
	EXPECT_EQ("register", lifecycle->calls[1].name);
	EXPECT_EQ("revoke", lifecycle->calls[2].name);
	EXPECT_EQ("deactivate", lifecycle->calls[3].name);

	EXPECT_TRUE(coordinator.Activate(Session(52, L"session-52")));
	ASSERT_EQ(5u, lifecycle->calls.size());
	EXPECT_EQ("activate", lifecycle->calls[4].name);
	EXPECT_EQ(52u, lifecycle->calls[4].session.generation);
}

} // namespace
