/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhRepositoryRead.h"

#include <memory>
#include <string>
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
		std::uint32_t timeout, std::size_t output, std::size_t error, HANDLE) override
	{
		m_arguments = arguments;
		m_timeoutMilliseconds = timeout;
		m_maximumOutputBytes = output;
		m_maximumErrorBytes = error;
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
	[[nodiscard]] std::uint32_t TimeoutMilliseconds() const noexcept { return m_timeoutMilliseconds; }
	[[nodiscard]] std::size_t MaximumOutputBytes() const noexcept { return m_maximumOutputBytes; }
	[[nodiscard]] std::size_t MaximumErrorBytes() const noexcept { return m_maximumErrorBytes; }
private:
	GhProcessOutcome m_outcome{ EBoundedProcessStatus::Failed, 1, {}, {} };
	std::vector<std::wstring> m_arguments;
	std::uint32_t m_timeoutMilliseconds = 0;
	std::size_t m_maximumOutputBytes = 0;
	std::size_t m_maximumErrorBytes = 0;
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
	std::optional<GhToolResolution> ResolveExecutable() const override
	{
		return GhToolResolution{ L"C:\\Tools\\gh.exe", GhToolDiscovery::SearchPath };
	}
	bool VerifyExecutableTrust(const std::wstring&) const override { return true; }
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
	auto result = fixture.Reader().Read(account, fixture.Probe(), Request({ { L"page", L"2" } }), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::Succeeded, result.Status());
	EXPECT_EQ(200, result.HttpStatus());
	EXPECT_EQ(2U, result.CurrentPage());
	ASSERT_TRUE(result.NextPage());
	EXPECT_EQ(3U, *result.NextPage());
	EXPECT_EQ("W/\"abc\"", *result.ETag());
	EXPECT_EQ("[{\"id\":1}]", std::string(result.Body().begin(), result.Body().end()));
	// The validating parse's DOM travels with the page so projection parses
	// nothing a second time.
	auto document = result.TakeDocument();
	ASSERT_TRUE(document.has_value());
	const auto* array = std::get_if<platform::serialization::JsoncValue::Array>(&document->Value());
	ASSERT_NE(nullptr, array);
	ASSERT_EQ(1U, array->size());
	EXPECT_NE(nullptr, std::get_if<platform::serialization::JsoncValue::Object>(&array->front().Value()));
	EXPECT_FALSE(result.TakeDocument().has_value());
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

TEST(GhRepositoryRead, ParsesAHttpDateRetryAfterAsAnAbsoluteInstant)
{
	// RFC 9110 allows Retry-After to be an IMF-fixdate instead of delta
	// seconds. The envelope stores it as the absolute instant it names so the
	// scheduler can wait on it exactly as on an X-RateLimit-Reset timestamp.
	Fixture dated;
	dated.CredentialValue().Set(EBoundedProcessStatus::Failed,
		"HTTP/2.0 429 Too Many Requests\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n\r\n{}");
	{
		auto account = dated.Account();
		const auto result = dated.Reader().Read(account, dated.Probe(), Request(), nullptr);
		EXPECT_EQ(GhRepositoryResponseStatus::RateLimited, result.Status());
		EXPECT_FALSE(result.RetryAfterSeconds());
		ASSERT_TRUE(result.RetryAfterUnixSeconds());
		EXPECT_EQ(1792567680U, *result.RetryAfterUnixSeconds());
	}

	// A Retry-After date also marks a 403 as rate limited, the same way the
	// delta-seconds form does.
	Fixture forbidden;
	forbidden.CredentialValue().Set(EBoundedProcessStatus::Failed,
		"HTTP/2.0 403 Forbidden\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n\r\n{}");
	{
		auto account = forbidden.Account();
		const auto result = forbidden.Reader().Read(account, forbidden.Probe(), Request(), nullptr);
		EXPECT_EQ(GhRepositoryResponseStatus::RateLimited, result.Status());
		ASSERT_TRUE(result.RetryAfterUnixSeconds());
		EXPECT_EQ(1792567680U, *result.RetryAfterUnixSeconds());
	}

	// A value that is neither delta-seconds nor a fixdate still breaks the
	// envelope: a non-GMT zone and a malformed date are both refused.
	for (const auto* header : { "Retry-After: Wed, 21 Oct 2026 07:28:00 UTC",
		"Retry-After: Wednesday, 21 Oct 2026 07:28:00 GMT",
		"Retry-After: Wed, 32 Oct 2026 07:28:00 GMT" }) {
		Fixture fixture;
		fixture.CredentialValue().Set(EBoundedProcessStatus::Failed,
			std::string("HTTP/2.0 429 Too Many Requests\r\n") + header + "\r\n\r\n{}");
		auto account = fixture.Account();
		EXPECT_EQ(GhRepositoryResponseStatus::InvalidEnvelope,
			fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr).Status()) << header;
	}
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

TEST(GhRepositoryRead, RefusesAReadForAHostOtherThanTheAccountsOwn)
{
	// The account was verified against github.com; a request naming another
	// host must be refused before gh is ever launched for it.
	Fixture fixture;
	auto account = fixture.Account();
	const auto refused = fixture.Reader().Read(account, fixture.Probe(),
		{ L"evil.example", L"owner", L"repo", { L"issues" } }, nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::InvalidRequest, refused.Status());
	EXPECT_TRUE(fixture.CredentialValue().Arguments().empty());
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

TEST(GhRepositoryRead, ABodyOverThePageLimitIsTooLargeNotInvalidJson)
{
	// A valid JSON array roughly half again over the strict parser's input
	// limit. The body is well-formed, so the only honest answer is that it is
	// too large - reporting it as invalid JSON would misstate what failed.
	constexpr auto limit = kMaximumRepositoryPageBodyBytes;
	std::string body = "[";
	while (body.size() < limit + limit / 2) body += R"({"id":1,"title":"a"},)";
	body.back() = ']';
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
		"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n" + body);
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::OutputLimitExceeded, result.Status());
	EXPECT_TRUE(result.Body().empty());
}

TEST(GhRepositoryRead, ABodyOfExactlyTheLimitStillSucceeds)
{
	// The bound is exclusive: one array of one string sized to the byte is a
	// page the parser admits, and the reader must not refuse its own boundary.
	constexpr auto limit = kMaximumRepositoryPageBodyBytes;
	std::string body = "[\"";
	body.append(limit - 4, 'a');
	body += "\"]";
	ASSERT_EQ(limit, body.size());
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
		"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n" + body);
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::Succeeded, result.Status());
	EXPECT_EQ(body.size(), result.Body().size());
}

TEST(GhRepositoryRead, AParserResourceLimitIsTooLargeNotInvalidJson)
{
	// Each body parses far enough to hit a parser resource bound - node count,
	// depth, key length - which is the page being too large, not the page being
	// malformed. A small body that is simply broken stays InvalidJson. A string
	// over the parser's 1 Mi-unit bound cannot fit inside the 1 MiB body bound,
	// so that diagnostic has no body here.
	std::string nodes = "[";
	for (std::size_t index = 0; index < 65540; ++index) nodes += "0,";
	nodes += "0]";
	ASSERT_LT(nodes.size(), kMaximumRepositoryPageBodyBytes);
	const std::string depth(80, '[');
	for (const auto& [body, expected] : std::vector<std::pair<std::string, GhRepositoryResponseStatus>>{
		{ nodes, GhRepositoryResponseStatus::OutputLimitExceeded },
		{ depth + std::string(80, ']'), GhRepositoryResponseStatus::OutputLimitExceeded },
		{ "{\"" + std::string(70000, 'k') + "\":1}", GhRepositoryResponseStatus::OutputLimitExceeded },
		{ "{", GhRepositoryResponseStatus::InvalidJson },
	}) {
		Fixture fixture;
		fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
			"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n" + body);
		auto account = fixture.Account();
		EXPECT_EQ(expected, fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr).Status());
	}
}

TEST(GhRepositoryRead, LaunchesTheReadAtThePageBodyPlusHeaderCaptureBound)
{
	// The capture bound was review-only until the seam recorded it: the reader
	// must launch `gh api --include` bounded at the page-body limit plus the
	// fixed 64 KiB header allowance - not the 4 MiB the old policy constant
	// once implied, and not unlimited.
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::Succeeded,
		"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n[]");
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
	ASSERT_EQ(GhRepositoryResponseStatus::Succeeded, result.Status());
	EXPECT_EQ(kMaximumRepositoryPageBodyBytes + 64u * 1024u,
		fixture.CredentialValue().MaximumOutputBytes());
	EXPECT_EQ(64u * 1024u, fixture.CredentialValue().MaximumErrorBytes());
	EXPECT_EQ(30000u, fixture.CredentialValue().TimeoutMilliseconds());
}

TEST(GhRepositoryRead, ATruncatedCaptureIsOutputLimitExceededNotFailure)
{
	// A capture that hit the process output bound can still leave a parseable
	// header block over a cut body; what ran out is the capture, not the read.
	Fixture fixture;
	fixture.CredentialValue().Set(EBoundedProcessStatus::OutputLimitExceeded,
		"HTTP/2.0 200 OK\r\nContent-Type: application/json\r\n\r\n[1,2");
	auto account = fixture.Account();
	const auto result = fixture.Reader().Read(account, fixture.Probe(), Request(), nullptr);
	EXPECT_EQ(GhRepositoryResponseStatus::OutputLimitExceeded, result.Status());
	EXPECT_TRUE(result.Body().empty());
}
