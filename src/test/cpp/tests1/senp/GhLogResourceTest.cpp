/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhLogResource.h"

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
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>& arguments,
		std::uint32_t, std::size_t, std::size_t, HANDLE) override
	{
		m_arguments = arguments;
		return { EBoundedProcessStatus::InvalidRequest, -1, {}, {} };
	}
	GhProcessOutcome RunAuthenticatedStreaming(const std::vector<std::wstring>& arguments,
		const std::uint32_t timeout, const std::size_t output, const std::size_t error,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver> observer, HANDLE) override
	{
		m_arguments = arguments;
		m_timeout = timeout;
		m_outputLimit = output;
		m_errorLimit = error;
		for (const auto& chunk : m_chunks) {
			if (!observer->OnOutput(EBoundedProcessStream::StandardOutput, chunk)) {
				return { EBoundedProcessStatus::ObserverRejected, -1, {}, {} };
			}
		}
		if (!m_error.empty()) {
			(void)observer->OnOutput(EBoundedProcessStream::StandardError, m_error);
		}
		return { m_status, m_status == EBoundedProcessStatus::Succeeded ? 0 : 1, {}, m_error };
	}
	void Revoke() noexcept override {}
	void Set(const EBoundedProcessStatus status, std::vector<std::string> chunks,
		std::string error = {})
	{
		m_status = status;
		m_chunks.clear();
		for (const auto& chunk : chunks) m_chunks.push_back(Bytes(chunk));
		m_error = Bytes(error);
	}
	void SetRepeated(const std::size_t count, std::string chunk)
	{
		m_status = EBoundedProcessStatus::Succeeded;
		m_chunks.assign(count, Bytes(chunk));
	}
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
	[[nodiscard]] std::uint32_t Timeout() const noexcept { return m_timeout; }
	[[nodiscard]] std::size_t OutputLimit() const noexcept { return m_outputLimit; }
	[[nodiscard]] std::size_t ErrorLimit() const noexcept { return m_errorLimit; }
private:
	std::vector<std::wstring> m_arguments;
	std::vector<std::vector<std::uint8_t>> m_chunks;
	std::vector<std::uint8_t> m_error;
	EBoundedProcessStatus m_status{ EBoundedProcessStatus::Succeeded };
	std::uint32_t m_timeout{};
	std::size_t m_outputLimit{}, m_errorLimit{};
};

class ConnectionPlatform final : public IGhConnectionPlatform {
public:
	explicit ConnectionPlatform(std::shared_ptr<Credential> credential) : m_credential(std::move(credential)) {}
	GhConnectionCheckResult Check(const GhToolProbe&, std::wstring_view config,
		std::wstring_view host, std::optional<std::wstring_view>, HANDLE) override
	{
		return { GhConnectionTerminal::Succeeded,
			GhAccountIdentity(std::wstring(host), L"octocat", L"keyring", std::wstring(config)), m_credential };
	}
private:
	std::shared_ptr<Credential> m_credential;
};

class ToolPlatform final : public IGhToolPlatform {
public:
	std::optional<std::wstring> ResolveExecutable() const override { return L"C:\\Tools\\gh.exe"; }
	GhProcessOutcome Run(const GhProcessInvocation&, HANDLE) const override
	{
		return { EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.93.0 (2026-05-27)\n"), {} };
	}
};

TextResourceScope Scope()
{
	return { "profile-1", "publisher.extension", std::string(64, 'a'), "grant-1", 1, 1, 1, 1 };
}

class Fixture final {
public:
	Fixture() : m_connection(std::make_shared<ConnectionPlatform>(m_credential)),
		m_grants(std::make_shared<Authority>()), m_lifecycle(m_connection, m_grants,
			L"profile-1", L"C:\\Profiles\\gh"), m_policy(m_tool, L"C:\\Sakura"),
		m_probe(m_policy.Probe(nullptr)), m_logs(m_policy, m_store)
	{
		auto attempt = m_lifecycle.Begin(L"github.com", std::nullopt);
		if (attempt) (void)m_lifecycle.Complete(std::move(*attempt), m_probe, nullptr);
	}
	[[nodiscard]] Credential& CredentialValue() noexcept { return *m_credential; }
	[[nodiscard]] GhAuthenticatedAccount Account() { return std::move(*m_lifecycle.Acquire(1)); }
	[[nodiscard]] CGhLogResource& Logs() noexcept { return m_logs; }
	[[nodiscard]] SenpTextResourceStore& Store() noexcept { return m_store; }
	[[nodiscard]] const GhToolProbe& Probe() const noexcept { return m_probe; }
private:
	std::shared_ptr<Credential> m_credential{ std::make_shared<Credential>() };
	std::shared_ptr<ConnectionPlatform> m_connection;
	CSenpToolGrants m_grants;
	CGhConnectionLifecycle m_lifecycle;
	std::shared_ptr<ToolPlatform> m_tool{ std::make_shared<ToolPlatform>() };
	CGhToolPolicy m_policy;
	GhToolProbe m_probe;
	SenpTextResourceStore m_store;
	CGhLogResource m_logs;
};

GhJobLogRequest Request(const std::uint64_t jobId = 42)
{
	return { L"github.com", L"owner", L"repo", jobId };
}

} // namespace

TEST(GhLogResource, StreamsOnlyStdoutIntoACompleteResourceThroughTheFixedCommand)
{
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
		{ "first line\n", "second line\n" },
		"https://objects.githubusercontent.com/signed?token=secret");
	auto account = fixture.Account();
	const auto result = fixture.Logs().Download(account, fixture.Probe(), Scope(), Request(), nullptr);
	ASSERT_EQ(GhLogResourceStatus::Succeeded, result.Status());
	EXPECT_EQ(23U, result.AcceptedBytes());
	const auto read = fixture.Store().Read(Scope(), result.Handle(), 0, 1024);
	EXPECT_EQ(TextResourceState::Complete, read.state);
	EXPECT_EQ(TextResourceEnd::Complete, read.end);
	EXPECT_EQ("first line\nsecond line\n", read.bytes);
	EXPECT_EQ((std::vector<std::wstring>{ L"api", L"--hostname", L"github.com", L"--method", L"GET",
		L"--header", L"X-GitHub-Api-Version: 2022-11-28",
		L"repos/owner/repo/actions/jobs/42/logs" }), fixture.CredentialValue().Arguments());
	EXPECT_EQ(120000U, fixture.CredentialValue().Timeout());
	EXPECT_EQ(SenpTextResourceStore::kResourceBytes, fixture.CredentialValue().OutputLimit());
	EXPECT_EQ(64U * 1024U, fixture.CredentialValue().ErrorLimit());
}

TEST(GhLogResource, AFailedTransferKeepsItsAcceptedPrefixAsExplicitPartialContent)
{
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Failed, { "accepted prefix\n" });
	auto account = fixture.Account();
	const auto result = fixture.Logs().Download(account, fixture.Probe(), Scope(), Request(), nullptr);
	EXPECT_EQ(GhLogResourceStatus::UnavailableOrNotFound, result.Status());
	const auto read = fixture.Store().Read(Scope(), result.Handle(), 0, 1024);
	EXPECT_EQ(TextResourceState::Partial, read.state);
	EXPECT_EQ(TextResourceEnd::Failed, read.end);
	EXPECT_EQ("accepted prefix\n", read.bytes);
}

TEST(GhLogResource, CancellationAndTimeoutAreDistinctTerminalResources)
{
	for (const auto [process, expected, end] : {
		std::tuple{ EBoundedProcessStatus::Cancelled, GhLogResourceStatus::Cancelled, TextResourceEnd::Cancelled },
		std::tuple{ EBoundedProcessStatus::TimedOut, GhLogResourceStatus::TimedOut, TextResourceEnd::Failed } }) {
		Fixture fixture;
		fixture.CredentialValue().Set(process, { "prefix" });
		auto account = fixture.Account();
		const auto result = fixture.Logs().Download(account, fixture.Probe(), Scope(), Request(), nullptr);
		EXPECT_EQ(expected, result.Status());
		const auto read = fixture.Store().Read(Scope(), result.Handle(), 0, 1024);
		EXPECT_EQ(TextResourceState::Partial, read.state);
		EXPECT_EQ(end, read.end);
	}
}

TEST(GhLogResource, ResourceLimitStopsTheProducerAndPreservesExactlyTheBoundedPrefix)
{
	Fixture fixture;
	fixture.CredentialValue().SetRepeated(SenpTextResourceStore::kResourceBytes
		/ SenpTextResourceStore::kChunkBytes + 1, std::string(SenpTextResourceStore::kChunkBytes, 'x'));
	auto account = fixture.Account();
	const auto result = fixture.Logs().Download(account, fixture.Probe(), Scope(), Request(), nullptr);
	EXPECT_EQ(GhLogResourceStatus::LimitExceeded, result.Status());
	EXPECT_EQ(SenpTextResourceStore::kResourceBytes, result.AcceptedBytes());
	const auto read = fixture.Store().Read(Scope(), result.Handle(), SenpTextResourceStore::kResourceBytes, 1);
	EXPECT_EQ(TextResourceState::Partial, read.state);
	EXPECT_EQ(TextResourceEnd::LimitExceeded, read.end);
	EXPECT_EQ(SenpTextResourceStore::kResourceBytes, read.length);
}

TEST(GhLogResource, InvalidIdentityGenerationAndJobInputsCreateNoResourceOrProcess)
{
	Fixture fixture;
	auto account = fixture.Account();
	auto scope = Scope();
	scope.accountGeneration = 2;
	EXPECT_EQ(GhLogResourceStatus::InvalidRequest,
		fixture.Logs().Download(account, fixture.Probe(), scope, Request(), nullptr).Status());
	scope.accountGeneration = 1;
	EXPECT_EQ(GhLogResourceStatus::InvalidRequest,
		fixture.Logs().Download(account, fixture.Probe(), scope,
			GhJobLogRequest(L"other.example", L"owner", L"repo", 42), nullptr).Status());
	EXPECT_EQ(GhLogResourceStatus::InvalidRequest,
		fixture.Logs().Download(account, fixture.Probe(), scope, Request(0), nullptr).Status());
	EXPECT_TRUE(fixture.CredentialValue().Arguments().empty());
	EXPECT_EQ(0U, fixture.Store().Usage().resources);
}

TEST(GhLogResource, ClosedStoreRejectsBeforeCredentialUse)
{
	Fixture fixture;
	fixture.Store().Close();
	auto account = fixture.Account();
	const auto result = fixture.Logs().Download(account, fixture.Probe(), Scope(), Request(), nullptr);
	EXPECT_EQ(GhLogResourceStatus::ResourceUnavailable, result.Status());
	EXPECT_TRUE(result.Handle().empty());
	EXPECT_TRUE(fixture.CredentialValue().Arguments().empty());
}
