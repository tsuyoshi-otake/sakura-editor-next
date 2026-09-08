/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/SenpControlPackageAuthority.h"

#include <memory>
#include <string>
#include <vector>

namespace {
using namespace senp;
using platform::controlipc::ControlIpcSessionContext;
using Status = ESenpPackageAuthorityPublishStatus;

constexpr auto kNow = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
constexpr std::uint64_t kRevision = 5;

std::wstring Digest(const wchar_t fill = L'a') { return std::wstring(64, fill); }

ExtensionDescriptor Extension(std::wstring id = L"sakura.github-pull-requests",
	std::wstring digest = Digest())
{
	ExtensionDescriptor value;
	value.id = std::move(id);
	value.archiveSha256 = std::move(digest);
	value.installed = true;
	value.enabled = true;
	value.runtime.schemaVersion = 2;
	value.runtime.abi = L"sakura:senp/extension@2.0.0";
	value.runtime.capabilities = { L"workbench.views.tree", L"tools.github.repository.read" };
	return value;
}

ManagementSnapshot Snapshot(std::vector<ExtensionDescriptor> extensions,
	const EManagementState state = EManagementState::Ready, const std::uint64_t revision = kRevision)
{
	ManagementSnapshot value;
	value.state = state;
	value.revision = revision;
	value.extensions = std::move(extensions);
	return value;
}

TEST(SenpControlPackageAuthority, ApprovesOnlyOwnersDeclaringAKnownToolCapability)
{
	auto onlyWorkbench = Extension(L"sakura.markdown");
	onlyWorkbench.runtime.capabilities = { L"workbench.views.tree", L"workbench.documents.readonly" };
	auto unknownTool = Extension(L"sakura.other");
	unknownTool.runtime.capabilities = { L"tools.github.repository.write" };

	CSenpControlPackageAuthority authority;
	const auto published = authority.Publish(L"profile1",
		Snapshot({ Extension(), onlyWorkbench, unknownTool }));
	EXPECT_EQ(Status::Published, published.status);
	EXPECT_EQ(1U, published.approved);

	const auto resolved = authority.Resolve(L"profile1", L"sakura.github-pull-requests");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_TRUE(resolved->Enabled());
	EXPECT_EQ(Digest(), resolved->PackageDigest());
	EXPECT_EQ(kRevision, resolved->ManagementRevision());
	EXPECT_EQ(SenpToolCapability::GitHubRepositoryRead, resolved->Capabilities());
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.markdown").has_value());
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.other").has_value());
}

TEST(SenpControlPackageAuthority, RefusesAPackageTheRuntimeCouldNotActivate)
{
	for (int variant = 0; variant < 4; ++variant) {
		auto extension = Extension();
		switch (variant) {
		case 0: extension.installed = false; break;
		case 1: extension.enabled = false; break;
		case 2: extension.runtime.schemaVersion = 1; break;
		case 3: extension.runtime.abi = L"sakura:senp/extension@1.0.0"; break;
		}
		CSenpControlPackageAuthority authority;
		const auto published = authority.Publish(L"profile1", Snapshot({ extension }));
		EXPECT_EQ(Status::Published, published.status);
		EXPECT_EQ(0U, published.approved);
		EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
	}
}

TEST(SenpControlPackageAuthority, RefusesADigestNoOwnerRequestCanCarry)
{
	const std::wstring rejected[] = { std::wstring(63, L'a'), std::wstring(64, L'A'),
		std::wstring(64, L'g'), std::wstring{} };
	for (const auto& digest : rejected) {
		CSenpControlPackageAuthority authority;
		const auto published = authority.Publish(L"profile1",
			Snapshot({ Extension(L"sakura.github-pull-requests", digest) }));
		EXPECT_EQ(Status::Published, published.status);
		EXPECT_EQ(0U, published.approved);
		EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
	}
}

TEST(SenpControlPackageAuthority, ADiagnosticReloadWithdrawsInsteadOfProvingEnablement)
{
	CSenpControlPackageAuthority authority;
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	ASSERT_TRUE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());

	// The service keeps the previously discovered extensions after a failed
	// reload, so a newer revision in this state still proves nothing.
	const auto diagnostics = authority.Publish(L"profile1",
		Snapshot({ Extension() }, EManagementState::ReadyWithDiagnostics, kRevision + 1));
	EXPECT_EQ(Status::Unverified, diagnostics.status);
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
	EXPECT_FALSE(authority.Revision(L"profile1").has_value());

	for (const auto state : { EManagementState::Created, EManagementState::Failed,
		EManagementState::Stopped }) {
		ASSERT_EQ(Status::Published, authority.Publish(L"profile1",
			Snapshot({ Extension() }, EManagementState::Ready, kRevision + 2)).status);
		EXPECT_EQ(Status::Unverified, authority.Publish(L"profile1",
			Snapshot({ Extension() }, state, kRevision + 3)).status);
		EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
	}
	EXPECT_EQ(Status::Unverified, authority.Publish(L"profile1",
		Snapshot({ Extension() }, EManagementState::Ready, 0)).status);
}

TEST(SenpControlPackageAuthority, AnOlderSnapshotNeverReplacesThePublishedTable)
{
	CSenpControlPackageAuthority authority;
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	const auto stale = authority.Publish(L"profile1",
		Snapshot({}, EManagementState::Ready, kRevision - 1));
	EXPECT_EQ(Status::Stale, stale.status);
	EXPECT_EQ(kRevision, stale.revision);
	const auto resolved = authority.Resolve(L"profile1", L"sakura.github-pull-requests");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(kRevision, resolved->ManagementRevision());
}

TEST(SenpControlPackageAuthority, EveryPublishedRevisionReachesEveryResolution)
{
	CSenpControlPackageAuthority authority;
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	ASSERT_EQ(kRevision, authority.Revision(L"profile1").value_or(0));
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1",
		Snapshot({ Extension() }, EManagementState::Ready, kRevision + 4)).status);
	EXPECT_EQ(kRevision + 4, authority.Revision(L"profile1").value_or(0));
	const auto resolved = authority.Resolve(L"profile1", L"sakura.github-pull-requests");
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(kRevision + 4, resolved->ManagementRevision());
}

TEST(SenpControlPackageAuthority, AnAmbiguousIdentityRefusesTheWholePublish)
{
	CSenpControlPackageAuthority authority;
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	const auto duplicated = authority.Publish(L"profile1",
		Snapshot({ Extension(), Extension(L"sakura.github-pull-requests", Digest(L'b')) },
			EManagementState::Ready, kRevision + 1));
	EXPECT_EQ(Status::Unverified, duplicated.status);
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
}

TEST(SenpControlPackageAuthority, OneProfileNeverAnswersForAnother)
{
	CSenpControlPackageAuthority authority;
	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	EXPECT_FALSE(authority.Resolve(L"profile2", L"sakura.github-pull-requests").has_value());
	EXPECT_EQ(1U, authority.Size());

	authority.Withdraw(L"profile1");
	EXPECT_EQ(0U, authority.Size());
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());

	ASSERT_EQ(Status::Published, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
	authority.Close();
	EXPECT_FALSE(authority.Resolve(L"profile1", L"sakura.github-pull-requests").has_value());
	EXPECT_EQ(Status::Closed, authority.Publish(L"profile1", Snapshot({ Extension() })).status);
}

TEST(SenpControlPackageAuthority, RefusesAProfileIdOutsideTheOpaqueIdentitySpace)
{
	CSenpControlPackageAuthority authority;
	for (const auto* profileId : { L"", L"profile 1", L"profile.1", L"c:/profiles/one" }) {
		EXPECT_EQ(Status::InvalidRequest, authority.Publish(profileId, Snapshot({ Extension() })).status);
	}
	EXPECT_EQ(0U, authority.Size());
}

TEST(SenpControlPackageAuthority, AdmitsABoundedNumberOfProfilesAndOwners)
{
	CSenpControlPackageAuthority authority;
	for (std::size_t index = 0; index < CSenpControlPackageAuthority::MaximumProfiles(); ++index) {
		const auto profileId = L"profile" + std::to_wstring(index);
		ASSERT_EQ(Status::Published, authority.Publish(profileId, Snapshot({ Extension() })).status);
	}
	EXPECT_EQ(CSenpControlPackageAuthority::MaximumProfiles(), authority.Size());
	EXPECT_EQ(Status::ResourceExhausted,
		authority.Publish(L"profileExtra", Snapshot({ Extension() })).status);
	// An already admitted profile keeps publishing once the table is full.
	EXPECT_EQ(Status::Published, authority.Publish(L"profile0",
		Snapshot({ Extension() }, EManagementState::Ready, kRevision + 1)).status);

	std::vector<ExtensionDescriptor> owners;
	for (std::size_t index = 0; index <= CSenpControlPackageAuthority::MaximumOwners(); ++index) {
		owners.push_back(Extension(L"sakura.owner-" + std::to_wstring(index)));
	}
	EXPECT_EQ(Status::ResourceExhausted, authority.Publish(L"profile0",
		Snapshot(owners, EManagementState::Ready, kRevision + 2)).status);
	EXPECT_FALSE(authority.Resolve(L"profile0", L"sakura.owner-0").has_value());
}

TEST(SenpControlPackageAuthority, GrantsFollowTheControlOwnedTableAndNotTheRequest)
{
	auto authority = std::make_shared<CSenpControlPackageAuthority>();
	ASSERT_EQ(Status::Published, authority->Publish(L"profile1", Snapshot({ Extension() })).status);

	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = grants.OpenSession(connection);
	ASSERT_NE(nullptr, session);
	const ContributionOwnerIdentity owner{ L"sakura.github-pull-requests", Digest(), 3, 2, 1 };
	const SenpToolGrantRequest request(L"profile1", owner, SenpToolCapability::GitHubRepositoryRead);
	const auto issued = session->Issue(request, kNow);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, issued.Status());
	EXPECT_EQ(SenpToolGrantCheck::Granted, session->Validate(issued.GrantId(), request, kNow));

	// A capability the table never approved is refused even for the same owner.
	const SenpToolGrantRequest widened(L"profile1", owner, SenpToolCapability::OpenConnectionUi);
	EXPECT_EQ(SenpToolGrantIssueStatus::Unauthorized, session->Issue(widened, kNow).Status());

	// A request that carries a digest of its own choosing proves nothing.
	const ContributionOwnerIdentity forged{ L"sakura.github-pull-requests", Digest(L'b'), 3, 2, 1 };
	const SenpToolGrantRequest forgedRequest(L"profile1", forged, SenpToolCapability::GitHubRepositoryRead);
	EXPECT_EQ(SenpToolGrantIssueStatus::Unauthorized, session->Issue(forgedRequest, kNow).Status());

	// Losing proof of the enablement state strands the live grant.
	ASSERT_EQ(Status::Unverified, authority->Publish(L"profile1",
		Snapshot({ Extension() }, EManagementState::ReadyWithDiagnostics, kRevision + 1)).status);
	EXPECT_EQ(SenpToolGrantCheck::StaleAuthority, session->Validate(issued.GrantId(), request, kNow));
}

} // namespace
