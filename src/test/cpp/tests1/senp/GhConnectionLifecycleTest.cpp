/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhConnectionLifecycle.h"

#include <atomic>
#include <memory>
#include <thread>

namespace {
using namespace senp;
using namespace senp::github;
using platform::process::EBoundedProcessStatus;

std::vector<std::uint8_t> Bytes(const std::string_view value)
{
	return { value.begin(), value.end() };
}

class Authority final : public ISenpToolGrantAuthority {
public:
	std::optional<SenpApprovedToolOwner> Resolve(std::wstring_view profileId,
		std::wstring_view extensionId) const override
	{
		return SenpApprovedToolOwner(std::wstring(profileId), std::wstring(extensionId),
			std::wstring(64, L'a'), 7, SenpToolCapability::GitHubRepositoryRead, true);
	}
};

class FakeCredential final : public IGhAccountCredential {
public:
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>&,
		std::uint32_t, std::size_t, std::size_t, HANDLE) override
	{
		if (m_revoked) return { EBoundedProcessStatus::InvalidRequest, -1, {}, {} };
		return { EBoundedProcessStatus::Succeeded, 0, Bytes("{}"), {} };
	}
	void Revoke() noexcept override { m_revoked = true; }
private:
	bool m_revoked{};
};

class FakeConnectionPlatform final : public IGhConnectionPlatform {
public:
	void Set(GhConnectionTerminal terminal, std::optional<GhAccountIdentity> identity = std::nullopt,
		std::shared_ptr<IGhAccountCredential> credential = {})
	{
		m_terminal = terminal;
		m_identity = std::move(identity);
		m_credential = std::move(credential);
	}
	void Block(HANDLE entered, HANDLE release) noexcept
	{
		m_entered = entered;
		m_release = release;
	}
	GhConnectionCheckResult Check(const GhToolProbe&, std::wstring_view configurationDirectory,
		std::wstring_view hostname, std::optional<std::wstring_view> requestedLogin, HANDLE) override
	{
		m_profiles.push_back(std::wstring(configurationDirectory));
		m_hosts.push_back(std::wstring(hostname));
		m_logins.push_back(requestedLogin ? std::optional<std::wstring>(*requestedLogin) : std::nullopt);
		if (m_entered) ::SetEvent(m_entered);
		if (m_release) (void)::WaitForSingleObject(m_release, 3000);
		return { m_terminal, m_identity, m_credential };
	}
	[[nodiscard]] std::size_t Calls() const noexcept { return m_hosts.size(); }
private:
	GhConnectionTerminal m_terminal{ GhConnectionTerminal::Failed };
	std::optional<GhAccountIdentity> m_identity;
	std::shared_ptr<IGhAccountCredential> m_credential;
	std::vector<std::wstring> m_profiles, m_hosts;
	std::vector<std::optional<std::wstring>> m_logins;
	HANDLE m_entered{}, m_release{};
};

class FakeToolPlatform final : public IGhToolPlatform {
public:
	std::optional<std::wstring> ResolveExecutable() const override { return L"C:\\Tools\\gh.exe"; }
	GhProcessOutcome Run(const GhProcessInvocation& invocation, HANDLE) const override
	{
		m_state->invocations.push_back(invocation);
		if (m_state->outcomes.empty()) {
			return { EBoundedProcessStatus::Succeeded, 0,
				Bytes("gh version 2.93.0 (2026-05-27)\n"), {} };
		}
		auto value = std::move(m_state->outcomes.front());
		m_state->outcomes.erase(m_state->outcomes.begin());
		return value;
	}
	void Queue(GhProcessOutcome value) { m_state->outcomes.push_back(std::move(value)); }
	[[nodiscard]] const std::vector<GhProcessInvocation>& Invocations() const noexcept { return m_state->invocations; }
private:
	class State final {
	private:
		std::vector<GhProcessOutcome> outcomes;
		std::vector<GhProcessInvocation> invocations;
		friend class FakeToolPlatform;
	};
	std::shared_ptr<State> m_state{ std::make_shared<State>() };
};

GhToolProbe AvailableProbe()
{
	auto tool = std::make_shared<FakeToolPlatform>();
	return CGhToolPolicy(tool, L"C:\\Sakura").Probe(nullptr);
}

GhAccountIdentity Account(std::wstring login)
{
	return { L"github.com", std::move(login), L"keyring", L"C:\\Profiles\\gh" };
}

ContributionOwnerIdentity Owner(const std::int64_t accountGeneration)
{
	return { L"sample.github", std::wstring(64, L'a'), 7, 11, accountGeneration };
}

std::shared_ptr<FakeConnectionPlatform> ConnectedPlatform(std::wstring login)
{
	auto platform = std::make_shared<FakeConnectionPlatform>();
	platform->Set(GhConnectionTerminal::Succeeded, Account(std::move(login)),
		std::make_shared<FakeCredential>());
	return platform;
}

bool HasOverride(const GhProcessInvocation& invocation, const std::wstring_view name,
	const std::wstring_view value)
{
	return std::ranges::any_of(invocation.EnvironmentOverrides(), [&](const auto& entry) {
		return entry.first == name && entry.second == value;
	});
}

} // namespace

TEST(GhConnectionLifecycle, KeepsUnknownUntilACompletedCheckProvesDisconnected)
{
	auto authority = std::make_shared<Authority>();
	CSenpToolGrants grants(authority);
	auto platform = std::make_shared<FakeConnectionPlatform>();
	platform->Set(GhConnectionTerminal::AuthenticationRequired);
	CGhConnectionLifecycle lifecycle(platform, grants, L"profile-1", L"C:\\Profiles\\gh");
	EXPECT_EQ(GhConnectionState::Unknown, lifecycle.Snapshot().State());
	auto attempt = lifecycle.Begin(L"github.com", std::nullopt);
	ASSERT_TRUE(attempt);
	EXPECT_EQ(GhConnectionState::Checking, lifecycle.Snapshot().State());
	const auto result = lifecycle.Complete(std::move(*attempt), AvailableProbe(), nullptr);
	EXPECT_EQ(GhConnectionTerminal::AuthenticationRequired, result.Terminal());
	EXPECT_EQ(GhConnectionState::Disconnected, result.Snapshot().State());
	EXPECT_FALSE(result.Snapshot().Account());
	EXPECT_EQ(0, result.Snapshot().AccountGeneration());
}

TEST(GhConnectionLifecycle, PublishesOnlyVerifiedIdentityAndRevokesOldAccountGrantsOnReplacement)
{
	auto authority = std::make_shared<Authority>();
	CSenpToolGrants grants(authority);
	auto session = grants.OpenSession({ 31, 1701 });
	ASSERT_NE(nullptr, session);
	auto platform = ConnectedPlatform(L"account-a");
	CGhConnectionLifecycle lifecycle(platform, grants, L"profile-1", L"C:\\Profiles\\gh");
	auto first = lifecycle.Begin(L"github.com", L"account-a");
	ASSERT_TRUE(first);
	const auto firstResult = lifecycle.Complete(std::move(*first), AvailableProbe(), nullptr);
	ASSERT_EQ(GhConnectionTerminal::Succeeded, firstResult.Terminal());
	ASSERT_EQ(1, firstResult.Snapshot().AccountGeneration());
	ASSERT_EQ(L"account-a", firstResult.Snapshot().Account()->Login());
	auto oldLease = lifecycle.Acquire(1);
	ASSERT_TRUE(oldLease);
	EXPECT_FALSE(lifecycle.Acquire(0));

	const SenpToolGrantRequest request(L"profile-1", Owner(1), SenpToolCapability::GitHubRepositoryRead);
	const auto issued = session->Issue(request, std::chrono::steady_clock::time_point{});
	ASSERT_EQ(SenpToolGrantIssueStatus::Granted, issued.Status());
	platform->Set(GhConnectionTerminal::Succeeded, Account(L"account-b"),
		std::make_shared<FakeCredential>());
	auto second = lifecycle.Begin(L"github.com", L"account-b");
	ASSERT_TRUE(second);
	EXPECT_TRUE(lifecycle.Acquire(1));
	const auto secondResult = lifecycle.Complete(std::move(*second), AvailableProbe(), nullptr);
	EXPECT_EQ(GhConnectionTerminal::Succeeded, secondResult.Terminal());
	EXPECT_EQ(2, secondResult.Snapshot().AccountGeneration());
	EXPECT_EQ(L"account-b", secondResult.Snapshot().Account()->Login());
	EXPECT_EQ(EBoundedProcessStatus::InvalidRequest,
		oldLease->Credential().RunAuthenticated({ L"api", L"user" }, 1000, 1024, 1024, nullptr).Status());
	EXPECT_EQ(SenpToolGrantCheck::Invalid,
		session->Validate(issued.GrantId(), request, std::chrono::steady_clock::time_point{}));
}

TEST(GhConnectionLifecycle, CandidateFailurePreservesTheUsableAccountButCurrentRejectionRequiresReauthentication)
{
	auto authority = std::make_shared<Authority>();
	CSenpToolGrants grants(authority);
	auto platform = ConnectedPlatform(L"account-a");
	CGhConnectionLifecycle lifecycle(platform, grants, L"profile-1", L"C:\\Profiles\\gh");
	auto first = lifecycle.Begin(L"github.com", L"account-a");
	ASSERT_TRUE(first);
	ASSERT_EQ(GhConnectionTerminal::Succeeded,
		lifecycle.Complete(std::move(*first), AvailableProbe(), nullptr).Terminal());

	platform->Set(GhConnectionTerminal::TimedOut);
	auto failedSwitch = lifecycle.Begin(L"github.com", L"account-b");
	ASSERT_TRUE(failedSwitch);
	const auto timeout = lifecycle.Complete(std::move(*failedSwitch), AvailableProbe(), nullptr);
	EXPECT_EQ(GhConnectionTerminal::TimedOut, timeout.Terminal());
	EXPECT_EQ(GhConnectionState::Connected, timeout.Snapshot().State());
	ASSERT_TRUE(timeout.Snapshot().Account());
	EXPECT_EQ(L"account-a", timeout.Snapshot().Account()->Login());
	EXPECT_EQ(1, timeout.Snapshot().AccountGeneration());

	platform->Set(GhConnectionTerminal::AuthenticationRequired);
	auto rejectedCurrent = lifecycle.Begin(L"github.com", L"account-a");
	ASSERT_TRUE(rejectedCurrent);
	const auto rejected = lifecycle.Complete(std::move(*rejectedCurrent), AvailableProbe(), nullptr);
	EXPECT_EQ(GhConnectionState::ReauthenticationRequired, rejected.Snapshot().State());
	EXPECT_FALSE(rejected.Snapshot().Account());
	EXPECT_EQ(0, rejected.Snapshot().AccountGeneration());
	EXPECT_FALSE(lifecycle.Acquire(1));
}

TEST(GhConnectionLifecycle, DisconnectInvalidatesAnInFlightCandidateBeforeItCanPublish)
{
	auto authority = std::make_shared<Authority>();
	CSenpToolGrants grants(authority);
	auto platform = ConnectedPlatform(L"late-account");
	const HANDLE entered = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	const HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(nullptr, entered);
	ASSERT_NE(nullptr, release);
	platform->Block(entered, release);
	CGhConnectionLifecycle lifecycle(platform, grants, L"profile-1", L"C:\\Profiles\\gh");
	auto attempt = lifecycle.Begin(L"github.com", L"late-account");
	ASSERT_TRUE(attempt);
	std::optional<GhConnectionOperationResult> result;
	std::jthread worker([&] {
		result = lifecycle.Complete(std::move(*attempt), AvailableProbe(), nullptr);
	});
	ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(entered, 3000));
	lifecycle.Disconnect();
	ASSERT_TRUE(::SetEvent(release));
	worker.join();
	ASSERT_TRUE(result);
	EXPECT_EQ(GhConnectionTerminal::Stale, result->Terminal());
	EXPECT_EQ(GhConnectionState::Disconnected, result->Snapshot().State());
	EXPECT_FALSE(result->Snapshot().Account());
	::CloseHandle(release);
	::CloseHandle(entered);
}

TEST(GhConnectionLifecycle, RejectsBusyForgedIdentityAndClosedOperationsWithExplicitTerminals)
{
	auto authority = std::make_shared<Authority>();
	CSenpToolGrants grants(authority);
	auto platform = ConnectedPlatform(L"different-account");
	CGhConnectionLifecycle lifecycle(platform, grants, L"profile-1", L"C:\\Profiles\\gh");
	EXPECT_FALSE(lifecycle.Begin(L"GitHub.com", std::nullopt));
	auto attempt = lifecycle.Begin(L"github.com", L"requested-account");
	ASSERT_TRUE(attempt);
	EXPECT_FALSE(lifecycle.Begin(L"github.com", std::nullopt));
	const auto mismatch = lifecycle.Complete(std::move(*attempt), AvailableProbe(), nullptr);
	EXPECT_EQ(GhConnectionTerminal::IdentityMismatch, mismatch.Terminal());
	EXPECT_EQ(GhConnectionState::Unavailable, mismatch.Snapshot().State());
	lifecycle.Close();
	EXPECT_FALSE(lifecycle.Begin(L"github.com", std::nullopt));
	EXPECT_EQ(GhConnectionState::Unavailable, lifecycle.Snapshot().State());
}

TEST(GhConnectionLifecycle, NativePlatformPinsConfigAccountAndVerifiesIdentityBeforeReturningCredential)
{
	auto tool = std::make_shared<FakeToolPlatform>();
	const auto probe = CGhToolPolicy(tool, L"C:\\Sakura").Probe(nullptr);
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes(
		R"({"hosts":{"github.com":[{"active":false,"gitProtocol":"https","host":"github.com","login":"account-a","scopes":"repo","state":"success","tokenSource":"keyring"},{"active":true,"gitProtocol":"https","host":"github.com","login":"account-b","scopes":"repo","state":"success","tokenSource":"keyring"}]}})"), {} });
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes("secret-token\n"), {} });
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes(R"({"login":"account-a","id":42})"), {} });
	CWindowsGhConnectionPlatform platform(tool, L"C:\\Sakura");
	const auto checked = platform.Check(probe, L"C:\\Profiles\\gh", L"github.com", L"account-a", nullptr);
	ASSERT_EQ(GhConnectionTerminal::Succeeded, checked.Terminal());
	ASSERT_TRUE(checked.Identity());
	ASSERT_TRUE(checked.Credential());
	EXPECT_EQ(L"account-a", checked.Identity()->Login());
	EXPECT_EQ(L"keyring", checked.Identity()->TokenSource());
	ASSERT_EQ(4U, tool->Invocations().size());
	EXPECT_EQ((std::vector<std::wstring>{ L"auth", L"status", L"--hostname", L"github.com", L"--json", L"hosts" }),
		tool->Invocations()[1].Arguments());
	EXPECT_EQ((std::vector<std::wstring>{ L"auth", L"token", L"--hostname", L"github.com", L"--user", L"account-a" }),
		tool->Invocations()[2].Arguments());
	EXPECT_EQ(L"user", tool->Invocations()[3].Arguments().back());
	EXPECT_TRUE(HasOverride(tool->Invocations()[1], L"GH_CONFIG_DIR", L"C:\\Profiles\\gh"));
	EXPECT_FALSE(HasOverride(tool->Invocations()[1], L"GH_TOKEN", L"secret-token"));
	EXPECT_TRUE(HasOverride(tool->Invocations()[3], L"GH_TOKEN", L"secret-token"));
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes("{}"), {} });
	EXPECT_EQ(EBoundedProcessStatus::Succeeded,
		checked.Credential()->RunAuthenticated({ L"api", L"user" }, 1000, 1024, 1024, nullptr).Status());
	ASSERT_EQ(5U, tool->Invocations().size());
	EXPECT_TRUE(HasOverride(tool->Invocations()[4], L"GH_TOKEN", L"secret-token"));
}

TEST(GhConnectionLifecycle, NativePlatformSeparatesConfirmedAuthenticationAbsenceFromMalformedAndMismatchedIdentity)
{
	auto tool = std::make_shared<FakeToolPlatform>();
	const auto probe = CGhToolPolicy(tool, L"C:\\Sakura").Probe(nullptr);
	CWindowsGhConnectionPlatform platform(tool, L"C:\\Sakura");
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes(
		R"({"hosts":{"github.com":[{"active":true,"host":"github.com","login":"account-a","state":"failure","tokenSource":"keyring"}]}})"), {} });
	EXPECT_EQ(GhConnectionTerminal::AuthenticationRequired,
		platform.Check(probe, L"C:\\Profiles\\gh", L"github.com", std::nullopt, nullptr).Terminal());
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes("not-json"), {} });
	EXPECT_EQ(GhConnectionTerminal::Failed,
		platform.Check(probe, L"C:\\Profiles\\gh", L"github.com", std::nullopt, nullptr).Terminal());
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes(
		R"({"hosts":{"github.com":[{"active":true,"host":"github.com","login":"account-a","state":"success","tokenSource":"keyring"}]}})"), {} });
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes("secret-token\n"), {} });
	tool->Queue({ EBoundedProcessStatus::Succeeded, 0, Bytes(R"({"login":"account-b"})"), {} });
	EXPECT_EQ(GhConnectionTerminal::IdentityMismatch,
		platform.Check(probe, L"C:\\Profiles\\gh", L"github.com", std::nullopt, nullptr).Terminal());
}
