/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/SenpToolGrants.h"

#include <memory>

namespace {
using namespace senp;
using platform::controlipc::ControlIpcSessionContext;

constexpr auto kNow = std::chrono::steady_clock::time_point(std::chrono::seconds(100));

ContributionOwnerIdentity Owner(std::wstring extensionId = L"sample.github")
{
	return { std::move(extensionId), std::wstring(64, L'a'), 7, 11, 13 };
}

SenpApprovedToolOwner Approved(std::wstring profileId = L"profile-1",
	std::wstring extensionId = L"sample.github", std::wstring digest = std::wstring(64, L'a'),
	std::uint64_t revision = 5, SenpToolCapability capabilities = SenpToolCapability::GitHubRepositoryRead,
	bool enabled = true)
{
	return { std::move(profileId), std::move(extensionId), std::move(digest),
		revision, capabilities, enabled };
}

class Authority final : public ISenpToolGrantAuthority {
public:
	explicit Authority(SenpApprovedToolOwner value) : m_value(std::move(value)) {}
	std::optional<SenpApprovedToolOwner> Resolve(
		std::wstring_view, std::wstring_view) const override
	{
		if (m_throw) throw std::runtime_error("authority unavailable");
		return m_value;
	}
	void Set(SenpApprovedToolOwner value) { m_value = std::move(value); }
	void SetThrow(bool value) noexcept { m_throw = value; }
private:
	SenpApprovedToolOwner m_value;
	bool m_throw{};
};

TEST(SenpToolGrants, TrustedAuthorityMustMatchProfileExtensionDigestEnablementAndCapability)
{
	auto authority = std::make_shared<Authority>(Approved());
	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = grants.OpenSession(connection);
	ASSERT_NE(nullptr, session);
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	const auto accepted = session->Issue(request, kNow);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, accepted.Status());
	EXPECT_EQ(32U, accepted.GrantId().size());
	EXPECT_EQ(kNow + CSenpToolGrants::GrantLifetime(), accepted.ExpiresAt());

	for (int mismatch = 0; mismatch < 6; ++mismatch) {
		switch (mismatch) {
		case 0: authority->Set(Approved(L"profile-2")); break;
		case 1: authority->Set(Approved(L"profile-1", L"other.github")); break;
		case 2: authority->Set(Approved(L"profile-1", L"sample.github", std::wstring(64, L'b'))); break;
		case 3: authority->Set(Approved(L"profile-1", L"sample.github", std::wstring(64, L'a'), 5,
			SenpToolCapability::OpenConnectionUi)); break;
		case 4: authority->Set(Approved(L"profile-1", L"sample.github", std::wstring(64, L'a'), 5,
			SenpToolCapability::GitHubRepositoryRead, false)); break;
		case 5: authority->Set(Approved(L"profile-1", L"sample.github", std::wstring(64, L'a'), 0)); break;
		}
		EXPECT_EQ(SenpToolGrantIssueStatus::Unauthorized, session->Issue(request, kNow).Status());
	}
	authority->SetThrow(true);
	EXPECT_EQ(SenpToolGrantIssueStatus::Unavailable, session->Issue(request, kNow).Status());
}

TEST(SenpToolGrants, OpaqueHandleNeverAuthorizesAnotherConnectionOrOwnerScope)
{
	auto authority = std::make_shared<Authority>(Approved());
	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = grants.OpenSession(connection);
	auto foreignSession = grants.OpenSession({ 32, 1701 });
	auto foreignProcess = grants.OpenSession({ 31, 1702 });
	ASSERT_NE(nullptr, session);
	ASSERT_NE(nullptr, foreignSession);
	ASSERT_NE(nullptr, foreignProcess);
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	const auto issued = session->Issue(request, kNow);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, issued.Status());
	EXPECT_EQ(SenpToolGrantCheck::Granted,
		session->Validate(issued.GrantId(), request, kNow));
	EXPECT_EQ(SenpToolGrantCheck::WrongConnection,
		foreignSession->Validate(issued.GrantId(), request, kNow));
	EXPECT_EQ(SenpToolGrantCheck::WrongConnection,
		foreignProcess->Validate(issued.GrantId(), request, kNow));

	for (int mismatch = 0; mismatch < 6; ++mismatch) {
		auto owner = Owner();
		std::wstring profile = L"profile-1";
		auto capability = SenpToolCapability::GitHubRepositoryRead;
		switch (mismatch) {
		case 0: profile = L"profile-2"; break;
		case 1: owner.extensionId = L"other.github"; break;
		case 2: owner.packageDigest.assign(64, L'b'); break;
		case 3: ++owner.generation; break;
		case 4: ++owner.workspaceRevision; break;
		case 5: ++owner.accountGeneration; break;
		}
		EXPECT_EQ(SenpToolGrantCheck::WrongScope, session->Validate(issued.GrantId(),
			SenpToolGrantRequest(std::move(profile), std::move(owner), capability), kNow));
	}
	EXPECT_EQ(SenpToolGrantCheck::Invalid,
		session->Validate("forged", request, kNow));
}

TEST(SenpToolGrants, ExpiryAndTrustedAuthorityRevisionInvalidateExistingHandles)
{
	auto authority = std::make_shared<Authority>(Approved());
	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = grants.OpenSession(connection);
	ASSERT_NE(nullptr, session);
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	const auto first = session->Issue(request, kNow);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, first.Status());
	authority->Set(Approved(L"profile-1", L"sample.github", std::wstring(64, L'a'), 6));
	EXPECT_EQ(SenpToolGrantCheck::StaleAuthority,
		session->Validate(first.GrantId(), request, kNow));
	authority->Set(Approved());
	EXPECT_EQ(SenpToolGrantCheck::Expired,
		session->Validate(first.GrantId(), request, first.ExpiresAt()));
	EXPECT_EQ(SenpToolGrantCheck::Invalid,
		session->Validate(first.GrantId(), request, first.ExpiresAt()));
}

TEST(SenpToolGrants, RevocationTargetsExactConnectionOwnerAndProfileBeforeClose)
{
	auto authority = std::make_shared<Authority>(Approved());
	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext firstConnection{ 31, 1701 };
	const ControlIpcSessionContext secondConnection{ 32, 1702 };
	auto firstSession = grants.OpenSession(firstConnection);
	auto secondSession = grants.OpenSession(secondConnection);
	ASSERT_NE(nullptr, firstSession);
	ASSERT_NE(nullptr, secondSession);
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	const auto first = firstSession->Issue(request, kNow);
	const auto second = secondSession->Issue(request, kNow);
	ASSERT_EQ(2U, grants.Size());
	firstSession.reset();
	EXPECT_EQ(SenpToolGrantCheck::Granted, secondSession->Validate(second.GrantId(), request, kNow));
	EXPECT_EQ(1U, grants.Size());
	grants.RevokeOwner(L"profile-1", Owner(L"other.github"));
	EXPECT_EQ(1U, grants.Size());
	grants.RevokeOwner(L"profile-1", Owner());
	EXPECT_EQ(0U, grants.Size());
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, secondSession->Issue(request, kNow).Status());
	grants.RevokeProfile(L"profile-2");
	EXPECT_EQ(1U, grants.Size());
	grants.RevokeProfile(L"profile-1");
	EXPECT_EQ(0U, grants.Size());
	grants.Close();
	grants.Close();
	EXPECT_EQ(SenpToolGrantIssueStatus::Closed, secondSession->Issue(request, kNow).Status());
	EXPECT_EQ(SenpToolGrantCheck::Closed, secondSession->Validate(std::string(32, '0'), request, kNow));
	EXPECT_EQ(nullptr, grants.OpenSession(firstConnection));
}

TEST(SenpToolGrants, InvalidRequestsAndBoundedCapacityFailClosed)
{
	auto authority = std::make_shared<Authority>(Approved());
	CSenpToolGrants grants(authority);
	const ControlIpcSessionContext connection{ 31, 1701 };
	auto session = grants.OpenSession(connection);
	ASSERT_NE(nullptr, session);
	for (int mismatch = 0; mismatch < 7; ++mismatch) {
		auto owner = Owner();
		std::wstring profile = L"profile-1";
		auto capability = SenpToolCapability::GitHubRepositoryRead;
		switch (mismatch) {
		case 0: profile = L"../profile"; break;
		case 1: owner.extensionId = L"UPPER"; break;
		case 2: owner.packageDigest[0] = L'X'; break;
		case 3: owner.generation = 0; break;
		case 4: owner.workspaceRevision = -1; break;
		case 5: owner.accountGeneration = -1; break;
		case 6: capability = SenpToolCapability::None; break;
		}
		EXPECT_EQ(SenpToolGrantIssueStatus::InvalidRequest,
			session->Issue(SenpToolGrantRequest(std::move(profile), std::move(owner), capability), kNow).Status());
	}
	EXPECT_EQ(SenpToolGrantIssueStatus::InvalidRequest,
		grants.OpenSession({ 0, 1701 }) == nullptr ? SenpToolGrantIssueStatus::InvalidRequest
			: SenpToolGrantIssueStatus::Granted);
	std::vector<std::unique_ptr<CSenpToolGrantSession>> sessions;
	for (std::size_t index = 0; index < CSenpToolGrants::MaximumGrants(); ++index) {
		sessions.push_back(grants.OpenSession({ index + 100, static_cast<std::uint32_t>(1701 + index) }));
		ASSERT_NE(nullptr, sessions.back());
		ASSERT_EQ(SenpToolGrantIssueStatus::Granted,
			sessions.back()->Issue(SenpToolGrantRequest(L"profile-1", Owner(),
				SenpToolCapability::GitHubRepositoryRead), kNow).Status());
	}
	EXPECT_EQ(SenpToolGrantIssueStatus::ResourceExhausted,
		session->Issue(SenpToolGrantRequest(L"profile-1", Owner(),
			SenpToolCapability::GitHubRepositoryRead), kNow).Status());
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	const auto expiresAt = kNow + CSenpToolGrants::GrantLifetime();
	EXPECT_EQ(SenpToolGrantIssueStatus::ResourceExhausted,
		session->Issue(request, expiresAt - std::chrono::milliseconds(1)).Status());
	// None of the old clients need to Validate or disconnect to reclaim expiry.
	const auto renewed = session->Issue(request, expiresAt);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, renewed.Status());
	EXPECT_EQ(1U, grants.Size());
	EXPECT_EQ(SenpToolGrantCheck::Granted, session->Validate(renewed.GrantId(), request, expiresAt));
	const auto later = expiresAt + std::chrono::seconds(1);
	const auto surviving = sessions.front()->Issue(request, later);
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, surviving.Status());
	const auto next = session->Issue(request, renewed.ExpiresAt());
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, next.Status());
	EXPECT_EQ(2U, grants.Size());
	EXPECT_EQ(SenpToolGrantCheck::Invalid,
		session->Validate(renewed.GrantId(), request, renewed.ExpiresAt()));
	EXPECT_EQ(SenpToolGrantCheck::Granted,
		sessions.front()->Validate(surviving.GrantId(), request, renewed.ExpiresAt()));
	session->Close();
	session->Close();
	EXPECT_EQ(SenpToolGrantIssueStatus::Closed, session->Issue(request, renewed.ExpiresAt()).Status());
	EXPECT_EQ(1U, grants.Size());
}

TEST(SenpToolGrants, RetainedSessionObservesClosedAfterBrokerDestruction)
{
	auto authority = std::make_shared<Authority>(Approved());
	std::unique_ptr<CSenpToolGrantSession> session;
	const SenpToolGrantRequest request(L"profile-1", Owner(), SenpToolCapability::GitHubRepositoryRead);
	std::string grantId;
	{
		CSenpToolGrants grants(authority);
		session = grants.OpenSession({ 31, 1701 });
		ASSERT_NE(nullptr, session);
		const auto issued = session->Issue(request, kNow);
		ASSERT_EQ(SenpToolGrantIssueStatus::Granted, issued.Status());
		grantId = issued.GrantId();
	}
	EXPECT_EQ(SenpToolGrantIssueStatus::Closed, session->Issue(request, kNow).Status());
	EXPECT_EQ(SenpToolGrantCheck::Closed, session->Validate(grantId, request, kNow));
	session.reset();
}

} // namespace
