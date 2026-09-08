/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhRepositoryRead.h"

#include <memory>
#include <tuple>

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
			std::wstring(64, L'a'), 1, SenpToolCapability::GitHubRepositoryRead, true);
	}
};

class Credential final : public IGhAccountCredential {
public:
	GhProcessOutcome RunAuthenticated(const std::vector<std::wstring>& arguments,
		std::uint32_t, std::size_t, std::size_t, HANDLE) override
	{
		m_arguments = arguments;
		return m_outcome;
	}
	GhProcessOutcome RunAuthenticatedStreaming(const std::vector<std::wstring>& arguments,
		std::uint32_t timeout, std::size_t output, std::size_t error,
		std::shared_ptr<platform::process::IBoundedProcessOutputObserver>, HANDLE stop) override
	{
		return RunAuthenticated(arguments, timeout, output, error, stop);
	}
	void Revoke() noexcept override {}
	void Set(EBoundedProcessStatus status, std::string output)
	{
		m_outcome = { status, status == EBoundedProcessStatus::Succeeded ? 0 : 1,
			Bytes(output), {} };
	}
	[[nodiscard]] const std::vector<std::wstring>& Arguments() const noexcept { return m_arguments; }
private:
	GhProcessOutcome m_outcome{ EBoundedProcessStatus::Failed, 1, {}, {} };
	std::vector<std::wstring> m_arguments;
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

class Fixture final {
public:
	Fixture() : m_connection(std::make_shared<ConnectionPlatform>(m_credential)),
		m_grants(std::make_shared<Authority>()), m_lifecycle(m_connection, m_grants,
			L"profile-1", L"C:\\Profiles\\gh"), m_policy(m_tool, L"C:\\Sakura"),
		m_probe(m_policy.Probe(nullptr)), m_reader(m_policy)
	{
		auto attempt = m_lifecycle.Begin(L"github.com", std::nullopt);
		if (attempt) (void)m_lifecycle.Complete(std::move(*attempt), m_probe, nullptr);
	}
	[[nodiscard]] Credential& CredentialValue() noexcept { return *m_credential; }
	[[nodiscard]] CGhRepositoryReader& Reader() noexcept { return m_reader; }
	[[nodiscard]] GhAuthenticatedAccount Account()
	{
		return std::move(*m_lifecycle.Acquire(1));
	}
	[[nodiscard]] const GhToolProbe& Probe() const noexcept { return m_probe; }
	[[nodiscard]] const CGhToolPolicy& Policy() const noexcept { return m_policy; }
private:
	std::shared_ptr<Credential> m_credential{ std::make_shared<Credential>() };
	std::shared_ptr<ConnectionPlatform> m_connection;
	CSenpToolGrants m_grants;
	CGhConnectionLifecycle m_lifecycle;
	std::shared_ptr<ToolPlatform> m_tool{ std::make_shared<ToolPlatform>() };
	CGhToolPolicy m_policy;
	GhToolProbe m_probe;
	CGhRepositoryReader m_reader;
};

GhRepositoryReadRequest Request(std::vector<std::pair<std::wstring, std::wstring>> query = {},
	std::optional<std::wstring> etag = std::nullopt)
{
	return { L"github.com", L"owner", L"repo", { L"issues" }, std::move(query), std::move(etag) };
}

} // namespace

TEST(GhRepositoryRead, BuildsValidatedQueryAndConditionalHeaderThroughThePolicy)
{
	Fixture fixture;
	const auto prepared = fixture.Policy().PrepareRepositoryRead(fixture.Probe(),
		Request({ { L"state", L"open" }, { L"page", L"2" }, { L"branch", L"feature/x" } }, L"\"etag-1\""));
	ASSERT_EQ(GhRepositoryReadStatus::Succeeded, prepared.Status());
	EXPECT_EQ((std::vector<std::wstring>{ L"api", L"--hostname", L"github.com", L"--method", L"GET",
		L"--include", L"--header", L"X-GitHub-Api-Version: 2022-11-28", L"--header",
		L"If-None-Match: \"etag-1\"", L"repos/owner/repo/issues?state=open&page=2&branch=feature%2Fx" }),
		prepared.Arguments());
	EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
		fixture.Policy().PrepareRepositoryRead(fixture.Probe(),
			Request({ { L"page", L"2" }, { L"page", L"3" } })).Status());
	EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
		fixture.Policy().PrepareRepositoryRead(fixture.Probe(), Request({ { L"unknown", L"x" } })).Status());
	EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
		fixture.Policy().PrepareRepositoryRead(fixture.Probe(), Request({ { L"page", L"0" } })).Status());
	EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
		fixture.Policy().PrepareRepositoryRead(fixture.Probe(), Request({ { L"per_page", L"101" } })).Status());
	EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
		fixture.Policy().PrepareRepositoryRead(fixture.Probe(), Request({}, L"etag\r\ninjected")).Status());
}

TEST(GhRepositoryRead, ReturnsOneJsonPageWithEtagAndOnlyTheNextPageNumber)
{
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
		"HTTP/2.0 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nETag: W/\"abc\"\r\n"
		"Link: <https://api.github.com/repositories/1/issues?per_page=50&page=3>; rel=\"next\", "
		"<https://api.github.com/repositories/1/issues?page=8>; rel=\"last\"\r\n\r\n[{\"id\":1}]");
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request({ { L"page", L"2" } }), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::Succeeded, result.Status());
	EXPECT_EQ(200, result.HttpStatus());
	EXPECT_EQ(2U, result.CurrentPage());
	ASSERT_TRUE(result.NextPage());
	EXPECT_EQ(3U, *result.NextPage());
	EXPECT_EQ("W/\"abc\"", *result.ETag());
	EXPECT_EQ("[{\"id\":1}]", std::string(result.Body().begin(), result.Body().end()));
}

TEST(GhRepositoryRead, NormalizesNotModifiedEvenWhenTheCliReturnsNonzero)
{
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Failed,
		"HTTP/2.0 304 Not Modified\r\nETag: \"same\"\r\n\r\n");
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request({}, L"\"same\""), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::NotModified, result.Status());
	EXPECT_TRUE(result.Body().empty());
}

TEST(GhRepositoryRead, KeepsUnauthorizedForbiddenAndNotFoundDistinct)
{
	for (const auto [code, expected] : {
		std::pair{ 401, GhRepositoryResponseStatus::Unauthorized },
		std::pair{ 403, GhRepositoryResponseStatus::Forbidden },
		std::pair{ 404, GhRepositoryResponseStatus::NotFound } }) {
		Fixture fixture;
		fixture.CredentialValue().Set(EBoundedProcessStatus::Failed,
			"HTTP/2.0 " + std::to_string(code) + " Error\r\nContent-Type: application/json\r\n\r\n{\"message\":\"x\"}");
		auto account = fixture.Account();
		const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
		EXPECT_EQ(expected, result.Status());
		EXPECT_EQ(code, result.HttpStatus());
	}
}

TEST(GhRepositoryRead, SeparatesRateLimitedResponsesAndValidatesCooldownHeaders)
{
	for (const auto& [output, expected, retryAfter] :
		std::vector<std::tuple<std::string, GhRepositoryResponseStatus, std::optional<std::uint32_t>>>{
			{ "HTTP/2.0 429 Too Many Requests\r\nRetry-After: 120\r\nX-RateLimit-Reset: 200\r\n\r\n{}",
				GhRepositoryResponseStatus::RateLimited, 120 },
			{ "HTTP/2.0 403 Forbidden\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset: 200\r\n\r\n{}",
				GhRepositoryResponseStatus::RateLimited, std::nullopt },
			{ "HTTP/2.0 403 Forbidden\r\nX-RateLimit-Remaining: 1\r\n\r\n{}",
				GhRepositoryResponseStatus::Forbidden, std::nullopt },
		}) {
		Fixture fixture;
		fixture.CredentialValue().Set(EBoundedProcessStatus::Failed, output);
		auto account = fixture.Account();
		const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
		EXPECT_EQ(expected, result.Status());
		EXPECT_EQ(retryAfter, result.RetryAfterSeconds());
		if (expected == GhRepositoryResponseStatus::RateLimited) {
			ASSERT_TRUE(result.RateLimitResetUnixSeconds());
			EXPECT_EQ(200U, *result.RateLimitResetUnixSeconds());
		}
	}
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Failed,
		"HTTP/2.0 429 Too Many Requests\r\nRetry-After: tomorrow\r\n\r\n{}");
	auto account = fixture.Account();
	EXPECT_EQ(GhRepositoryResponseStatus::InvalidEnvelope,
		fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr).Status());
}

TEST(GhRepositoryRead, RejectsMalformedEnvelopeContentTypeAndJsonRoot)
{
	for (const auto& [output, expected] : std::vector<std::pair<std::string, GhRepositoryResponseStatus>>{
		{ "not-http", GhRepositoryResponseStatus::InvalidEnvelope },
		{ "HTTP/2.0 200 OK\r\nContent-Type: text/plain\r\n\r\n{}", GhRepositoryResponseStatus::InvalidEnvelope },
		{ "HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\nnot-json", GhRepositoryResponseStatus::InvalidJson },
		{ "HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\ntrue", GhRepositoryResponseStatus::InvalidJson },
	}) {
		Fixture fixture;
		fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded, output);
		auto account = fixture.Account();
		EXPECT_EQ(expected, fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr).Status());
	}
}

TEST(GhRepositoryRead, PreservesTimeoutCancelAndOutputLimitWithoutPublishingBody)
{
	for (const auto [status, expected] : {
		std::pair{ EBoundedProcessStatus::TimedOut, GhRepositoryResponseStatus::TimedOut },
		std::pair{ EBoundedProcessStatus::Cancelled, GhRepositoryResponseStatus::Cancelled },
		std::pair{ EBoundedProcessStatus::OutputLimitExceeded, GhRepositoryResponseStatus::OutputLimitExceeded } }) {
		Fixture fixture;
		fixture.CredentialValue().Set(status, "");
		auto account = fixture.Account();
		const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
		EXPECT_EQ(expected, result.Status());
		EXPECT_TRUE(result.Body().empty());
	}
}
