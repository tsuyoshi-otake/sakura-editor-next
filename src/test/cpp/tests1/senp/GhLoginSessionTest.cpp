/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhLoginSession.h"

#include <memory>

namespace {
using namespace senp;
using namespace senp::github;
using platform::process::EBoundedProcessStatus;
using platform::process::EBoundedProcessStream;

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
			std::wstring(64, L'a'), 1, SenpToolCapability::GitHubRepositoryRead, true);
	}
};

class Credential final : public IGhAccountCredential {
public:
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>&,
		std::uint32_t, std::size_t, std::size_t, HANDLE) override
	{
		return { m_revoked ? EBoundedProcessStatus::InvalidRequest : EBoundedProcessStatus::Succeeded,
			m_revoked ? -1 : 0, {}, {} };
	}
	void Revoke() noexcept override { m_revoked = true; }
	[[nodiscard]] bool Revoked() const noexcept { return m_revoked; }
private:
	bool m_revoked{};
};

class ConnectionPlatform final : public IGhConnectionPlatform {
public:
	GhConnectionCheckResult Check(const GhToolProbe&, std::wstring_view config,
		std::wstring_view hostname, std::optional<std::wstring_view>, HANDLE) override
	{
		++m_calls;
		return { m_terminal,
			m_terminal == GhConnectionTerminal::Succeeded
				? std::optional<GhAccountIdentity>(GhAccountIdentity(std::wstring(hostname), L"octocat",
					L"keyring", std::wstring(config))) : std::nullopt,
			m_terminal == GhConnectionTerminal::Succeeded ? m_credential : nullptr };
	}
	void SetTerminal(const GhConnectionTerminal terminal) noexcept { m_terminal = terminal; }
	[[nodiscard]] std::size_t Calls() const noexcept { return m_calls; }
	[[nodiscard]] const std::shared_ptr<Credential>& CredentialValue() const noexcept { return m_credential; }
private:
	GhConnectionTerminal m_terminal{ GhConnectionTerminal::Succeeded };
	std::shared_ptr<Credential> m_credential{ std::make_shared<Credential>() };
	std::size_t m_calls{};
};

class ToolPlatform final : public IGhToolPlatform {
public:
	std::optional<std::wstring> ResolveExecutable() const override { return L"C:\\Tools\\gh.exe"; }
	GhProcessOutcome Run(const GhProcessInvocation& invocation, HANDLE) const override
	{
		m_state->invocations.push_back(invocation);
		if (invocation.Arguments() == std::vector<std::wstring>{ L"--version" }) {
			return { EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.93.0 (2026-05-27)\n"), {} };
		}
		if (invocation.OutputObserver() && !m_state->output.empty()) {
			if (!invocation.OutputObserver()->OnOutput(EBoundedProcessStream::StandardError, m_state->output)) {
				return { EBoundedProcessStatus::ObserverRejected, -1, {}, {} };
			}
		}
		return { m_state->status, m_state->status == EBoundedProcessStatus::Succeeded ? 0 : -1, {}, {} };
	}
	void Set(EBoundedProcessStatus status, std::string output)
	{
		m_state->status = status;
		m_state->output = Bytes(output);
	}
	[[nodiscard]] const std::vector<GhProcessInvocation>& Invocations() const noexcept { return m_state->invocations; }
private:
	class State final {
	private:
		std::vector<GhProcessInvocation> invocations;
		EBoundedProcessStatus status{ EBoundedProcessStatus::Succeeded };
		std::vector<std::uint8_t> output;
		friend class ToolPlatform;
	};
	std::shared_ptr<State> m_state{ std::make_shared<State>() };
};

class Fixture final {
public:
	Fixture() : m_grants(std::make_shared<Authority>()), m_lifecycle(m_connection, m_grants,
		L"profile-1", L"C:\\Profiles\\gh"), m_login(m_tool, m_lifecycle,
		L"C:\\Sakura", L"C:\\Profiles\\gh"),
		m_probe(CGhToolPolicy(m_tool, L"C:\\Sakura").Probe(nullptr)) {}
	[[nodiscard]] ToolPlatform& Tool() noexcept { return *m_tool; }
	[[nodiscard]] ConnectionPlatform& Connection() noexcept { return *m_connection; }
	[[nodiscard]] CGhConnectionLifecycle& Lifecycle() noexcept { return m_lifecycle; }
	[[nodiscard]] CGhLoginSession& Login() noexcept { return m_login; }
	[[nodiscard]] const GhToolProbe& Probe() const noexcept { return m_probe; }
private:
	std::shared_ptr<ToolPlatform> m_tool{ std::make_shared<ToolPlatform>() };
	std::shared_ptr<ConnectionPlatform> m_connection{ std::make_shared<ConnectionPlatform>() };
	CSenpToolGrants m_grants;
	CGhConnectionLifecycle m_lifecycle;
	CGhLoginSession m_login;
	GhToolProbe m_probe;
};

bool HasOverride(const GhProcessInvocation& invocation, const std::wstring_view name,
	const std::wstring_view value)
{
	return std::ranges::any_of(invocation.EnvironmentOverrides(), [&](const auto& entry) {
		return entry.first == name && entry.second == value;
	});
}

} // namespace

TEST(GhLoginSession, RunsOnlyTheFixedWebFlowAndPublishesAfterIdentityVerification)
{
	Fixture fixture;
	fixture.Tool().Set(EBoundedProcessStatus::Succeeded,
		"First copy your one-time code: ABCD-1234\nOpen https://github.com/login/device\n");
	EXPECT_EQ(GhLoginTerminal::Succeeded, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	const auto snapshot = fixture.Login().Snapshot();
	EXPECT_EQ(GhLoginPhase::Succeeded, snapshot.Phase());
	EXPECT_EQ(GhLoginTerminal::Succeeded, snapshot.Terminal());
	EXPECT_NE(std::string::npos, snapshot.OutputUtf8().find("ABCD-1234"));
	EXPECT_EQ(L"Credential storage reported by GitHub CLI: keyring", snapshot.StorageNotice());
	ASSERT_EQ(2U, fixture.Tool().Invocations().size());
	const auto& login = fixture.Tool().Invocations()[1];
	EXPECT_EQ((std::vector<std::wstring>{ L"auth", L"login", L"--hostname", L"github.com",
		L"--web", L"--skip-ssh-key" }), login.Arguments());
	EXPECT_TRUE(HasOverride(login, L"GH_CONFIG_DIR", L"C:\\Profiles\\gh"));
	EXPECT_EQ(1U, fixture.Connection().Calls());
}

TEST(GhLoginSession, RejectsUnexpectedControlOutputAsAnUnsupportedInteractiveFlow)
{
	Fixture fixture;
	fixture.Tool().Set(EBoundedProcessStatus::Succeeded, std::string("prompt:\0secret", 14));
	EXPECT_EQ(GhLoginTerminal::UnsupportedInteractiveFlow,
		fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	EXPECT_EQ(GhLoginPhase::Failed, fixture.Login().Snapshot().Phase());
	EXPECT_EQ(0U, fixture.Connection().Calls());
}

TEST(GhLoginSession, MapsCancellationTimeoutAndOutputLimitToDistinctTerminals)
{
	for (const auto [status, terminal] : {
		std::pair{ EBoundedProcessStatus::Cancelled, GhLoginTerminal::Cancelled },
		std::pair{ EBoundedProcessStatus::TimedOut, GhLoginTerminal::TimedOut },
		std::pair{ EBoundedProcessStatus::OutputLimitExceeded, GhLoginTerminal::OutputLimitExceeded } }) {
		Fixture fixture;
		fixture.Tool().Set(status, "bounded output\n");
		EXPECT_EQ(terminal, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
		EXPECT_TRUE(fixture.Login().Snapshot().Terminal());
	}
}

TEST(GhLoginSession, FailedCandidatePreservesAnExistingVerifiedAccount)
{
	Fixture fixture;
	EXPECT_EQ(GhLoginTerminal::Succeeded, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	ASSERT_TRUE(fixture.Lifecycle().Acquire(1));
	fixture.Tool().Set(EBoundedProcessStatus::Failed, "login failed\n");
	EXPECT_EQ(GhLoginTerminal::Failed, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	EXPECT_TRUE(fixture.Lifecycle().Acquire(1));
}

TEST(GhLoginSession, DisconnectRevokesOnlyTheLocalLeaseAndExplainsSharedAuthentication)
{
	Fixture fixture;
	ASSERT_EQ(GhLoginTerminal::Succeeded, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	fixture.Login().Disconnect();
	const auto snapshot = fixture.Login().Snapshot();
	EXPECT_EQ(GhLoginPhase::Disconnected, snapshot.Phase());
	EXPECT_FALSE(snapshot.SharedAuthenticationNotice().empty());
	EXPECT_FALSE(fixture.Lifecycle().Acquire(1));
	EXPECT_TRUE(fixture.Connection().CredentialValue()->Revoked());
	for (const auto& invocation : fixture.Tool().Invocations()) {
		EXPECT_NE((std::vector<std::wstring>{ L"auth", L"logout" }), invocation.Arguments());
	}
}

TEST(GhLoginSession, InvalidBusyAndClosedCallsReachExplicitTerminals)
{
	Fixture fixture;
	EXPECT_EQ(GhLoginTerminal::InvalidRequest, fixture.Login().Run(fixture.Probe(), L"GitHub.com", nullptr));
	fixture.Login().Close();
	EXPECT_EQ(GhLoginTerminal::Closed, fixture.Login().Run(fixture.Probe(), L"github.com", nullptr));
	EXPECT_EQ(GhLoginPhase::Closed, fixture.Login().Snapshot().Phase());
}
