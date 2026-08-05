/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <sakura/request/RequestService.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace platform::request;

namespace {

HttpResponse Response(int statusCode, std::initializer_list<HttpHeader> headers = {})
{
	return { statusCode, std::vector<HttpHeader>(headers), {}, {} };
}

class FakeTransport final : public IRequestTransport {
public:
	std::deque<TransportResult> results;
	std::vector<TransportRequest> requests;

	TransportResult Send(const TransportRequest& request, const IRequestCancellation*) override
	{
		requests.push_back(request);
		if (results.empty()) {
			return { std::nullopt, ETransportFailure::Network };
		}
		auto result = std::move(results.front());
		results.pop_front();
		return result;
	}
};

class FakeProxyService final : public IProxyService {
public:
	ProxySelection selection{};
	std::deque<ProxySelection> selections;
	std::vector<std::wstring> selectedUrls;
	std::vector<ProxyRequest> requests;
	std::vector<std::optional<std::chrono::steady_clock::time_point>> deadlines;
	std::vector<const IRequestCancellation*> cancellations;

	ProxySelection SelectProxy(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) override
	{
		selectedUrls.emplace_back(request.targetUrl);
		requests.push_back(request);
		deadlines.push_back(deadline);
		cancellations.push_back(cancellation);
		if (!selections.empty()) {
			auto next = std::move(selections.front());
			selections.pop_front();
			return next;
		}
		return selection;
	}
};

class FakeCredentialService final : public ICredentialService {
public:
	std::vector<CredentialRequest> requests;
	std::vector<std::optional<std::chrono::steady_clock::time_point>> deadlines;
	std::vector<const IRequestCancellation*> cancellations;

	std::optional<RequestCredential> GetCredential(const CredentialRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) override
	{
		requests.push_back(request);
		deadlines.push_back(deadline);
		cancellations.push_back(cancellation);
		return RequestCredential{ L"test-user", { 0x01, 0x02 } };
	}
};

class FakeClock final : public IRequestClock {
public:
	std::chrono::system_clock::time_point now{ std::chrono::seconds(0) };
	std::chrono::steady_clock::time_point steadyNow{ std::chrono::seconds(0) };

	std::chrono::system_clock::time_point Now() const override { return now; }
	std::chrono::steady_clock::time_point SteadyNow() const override { return steadyNow; }
};

class FakeScheduler final : public IRequestScheduler {
public:
	std::vector<std::chrono::milliseconds> waits;
	bool completeWait = true;
	FakeClock* clock = nullptr;

	bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation*) override
	{
		waits.push_back(delay);
		if (clock) clock->steadyNow += delay;
		return completeWait;
	}
};

class FakeJitterSource final : public IRetryJitterSource {
public:
	double value = 1.0;
	double NextUnitInterval() override { return value; }
};

class FakeResponseCache final : public IResponseCache {
public:
	std::unordered_map<std::wstring, HttpResponse> values;
	std::vector<std::wstring> reads;

	std::optional<HttpResponse> Get(std::wstring_view key) override
	{
		reads.emplace_back(key);
		const auto found = values.find(std::wstring(key));
		return found == values.end() ? std::nullopt : std::optional<HttpResponse>(found->second);
	}

	void Put(std::wstring_view key, const HttpResponse& response) override
	{
		values[std::wstring(key)] = response;
	}
};

class FakeCancellation final : public IRequestCancellation {
public:
	bool cancelled = false;
	bool IsCancellationRequested() const noexcept override { return cancelled; }
};

bool HasHeader(const TransportRequest& request, std::wstring_view name)
{
	return std::any_of(request.headers.begin(), request.headers.end(), [name](const HttpHeader& header) {
		if (header.name.size() != name.size()) return false;
		for (std::size_t index = 0; index < name.size(); ++index) {
			const auto left = header.name[index] >= L'A' && header.name[index] <= L'Z' ? header.name[index] - L'A' + L'a' : header.name[index];
			const auto right = name[index] >= L'A' && name[index] <= L'Z' ? name[index] - L'A' + L'a' : name[index];
			if (left != right) return false;
		}
		return true;
	});
}

class ThrowingProxyService final : public IProxyService {
public:
	ProxySelection SelectProxy(
		const ProxyRequest&,
		std::optional<std::chrono::steady_clock::time_point>,
		const IRequestCancellation*) override
	{
		throw std::runtime_error("proxy adapter failed");
	}
};

class AdvancingTransport final : public IRequestTransport {
public:
	explicit AdvancingTransport(FakeClock& clock) : m_clock(clock) {}

	TransportResult Send(const TransportRequest& request, const IRequestCancellation*) override
	{
		lastRequest = request;
		// Wall time deliberately moves backwards while monotonic time advances.
		// Request timeout ownership must not depend on a mutable system clock.
		m_clock.now -= std::chrono::hours(1);
		m_clock.steadyNow += std::chrono::milliseconds(100);
		return { Response(200) };
	}

	std::optional<TransportRequest> lastRequest;

private:
	FakeClock& m_clock;
};

class BlockingTransport final : public IRequestTransport {
public:
	TransportResult Send(const TransportRequest& request, const IRequestCancellation*) override
	{
		std::unique_lock lock(m_mutex);
		requests.push_back(request);
		m_entered = true;
		m_changed.notify_all();
		m_changed.wait_for(lock, std::chrono::seconds(2), [this] { return m_released; });
		return { Response(200) };
	}

	bool WaitUntilEntered()
	{
		std::unique_lock lock(m_mutex);
		return m_changed.wait_for(lock, std::chrono::seconds(1), [this] { return m_entered; });
	}

	void Release()
	{
		std::lock_guard lock(m_mutex);
		m_released = true;
		m_changed.notify_all();
	}

	std::vector<TransportRequest> requests;

private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_entered = false;
	bool m_released = false;
};

RequestService CreateService(
	FakeTransport& transport,
	FakeProxyService& proxy,
	FakeCredentialService& credentials,
	FakeClock& clock,
	FakeScheduler& scheduler,
	FakeJitterSource& jitter,
	IResponseCache* cache = nullptr,
	RequestServiceOptions options = {}
)
{
	return RequestService(transport, proxy, credentials, clock, scheduler, jitter, cache, options);
}

} // namespace

TEST(RequestService, ResolvesRelativeRedirectWithoutAnyUiOrPlatformTransport)
{
	FakeTransport transport;
	transport.results.push_back({ Response(302, { { L"Location", L"child" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/parent/start" });

	ASSERT_TRUE(result);
	EXPECT_EQ(1u, result.redirectCount);
	ASSERT_EQ(2u, transport.requests.size());
	EXPECT_EQ(L"https://example.test/parent/child", transport.requests[1].url);
	ASSERT_TRUE(result.response);
	EXPECT_EQ(L"https://example.test/parent/child", result.response->finalUrl);
}

TEST(RequestService, RejectsHttpsDowngradeBeforeSendingSecondRequest)
{
	FakeTransport transport;
	transport.results.push_back({ Response(302, { { L"Location", L"http://example.test/plain" } }) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/secure" });

	EXPECT_EQ(ERequestOutcome::HttpsDowngradeRejected, result.outcome);
	EXPECT_EQ(1u, transport.requests.size());
}

TEST(RequestService, EnforcesConfiguredRedirectLimit)
{
	FakeTransport transport;
	transport.results.push_back({ Response(302, { { L"Location", L"/next" } }) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	RequestServiceOptions options;
	options.maxRedirects = 0;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter, nullptr, options);

	const auto result = service.Execute({ L"GET", L"https://example.test/start" });

	EXPECT_EQ(ERequestOutcome::RedirectLimitExceeded, result.outcome);
	EXPECT_EQ(0u, result.redirectCount);
	EXPECT_EQ(1u, transport.requests.size());
}

TEST(RequestService, CancellationIsAVisibleTerminalOutcomeWithoutTransportWork)
{
	FakeTransport transport;
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);
	FakeCancellation cancellation;
	cancellation.cancelled = true;

	const auto result = service.Execute({ L"GET", L"https://example.test/cancel" }, &cancellation);

	EXPECT_EQ(ERequestOutcome::Cancelled, result.outcome);
	EXPECT_TRUE(transport.requests.empty());
}

TEST(RequestService, OfflinePolicyReturnsCachedResponseAndTypedCacheMissWithoutNetwork)
{
	FakeTransport transport;
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	FakeResponseCache cache;
	cache.values.emplace(L"GET https://example.test/cached", Response(200));
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter, &cache);

	const auto hit = service.Execute({ L"GET", L"https://example.test/cached", {}, {}, ERequestCachePolicy::OfflineOnly });
	const auto miss = service.Execute({ L"GET", L"https://example.test/missing", {}, {}, ERequestCachePolicy::OfflineOnly });

	EXPECT_TRUE(hit);
	EXPECT_TRUE(hit.fromCache);
	ASSERT_TRUE(hit.response);
	EXPECT_EQ(L"https://example.test/cached", hit.response->finalUrl);
	EXPECT_EQ(ERequestOutcome::OfflineCacheMiss, miss.outcome);
	EXPECT_TRUE(transport.requests.empty());
}

TEST(RequestService, Retries429UsingBoundedRetryAfterAndNeverLoopsImmediately)
{
	FakeTransport transport;
	transport.results.push_back({ Response(429, { { L"Retry-After", L"100000" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	RequestServiceOptions options;
	options.initialRetryDelay = std::chrono::milliseconds(100);
	options.maxRetryDelay = std::chrono::milliseconds(500);
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter, nullptr, options);

	const auto result = service.Execute({ L"GET", L"https://example.test/rate-limited" });

	ASSERT_TRUE(result);
	EXPECT_EQ(1u, result.retryCount);
	ASSERT_EQ(1u, scheduler.waits.size());
	EXPECT_EQ(std::chrono::milliseconds(500), scheduler.waits.front());
	EXPECT_GT(scheduler.waits.front(), std::chrono::milliseconds::zero());
	EXPECT_EQ(2u, transport.requests.size());
}

TEST(RequestService, DoesNotRetryNonIdempotentMethodAfter503)
{
	FakeTransport transport;
	transport.results.push_back({ Response(503, { { L"Retry-After", L"1" } }) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"POST", L"https://example.test/publish", {}, { 0x7f } });

	ASSERT_TRUE(result);
	EXPECT_EQ(0u, result.retryCount);
	EXPECT_TRUE(scheduler.waits.empty());
	EXPECT_EQ(1u, transport.requests.size());
}

TEST(RequestService, ObtainsProxyCredentialsOnlyForA407Challenge)
{
	FakeTransport transport;
	transport.results.push_back({ Response(407, { { L"Proxy-Authenticate", L"Basic realm=\"corporate\"" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	proxy.selection = { EProxyMode::Manual, L"http://proxy.example.test:8080", false };
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"PATCH", L"https://example.test/resource", { { L"X-Request", L"1" } }, { 1, 2, 3 } });

	ASSERT_TRUE(result);
	ASSERT_EQ(2u, transport.requests.size());
	EXPECT_EQ(EProxyMode::Manual, transport.requests.front().proxy.mode);
	EXPECT_EQ(L"http://proxy.example.test:8080", *transport.requests.front().proxy.proxyUrl);
	EXPECT_FALSE(transport.requests.front().proxyCredential);
	ASSERT_TRUE(transport.requests[1].proxyCredential);
	ASSERT_EQ(1u, credentials.requests.size());
	EXPECT_EQ(ECredentialPurpose::Proxy, credentials.requests[0].purpose);
	EXPECT_EQ(407, credentials.requests[0].challenge.statusCode);
	EXPECT_EQ(L"Basic", credentials.requests[0].challenge.scheme);
	ASSERT_TRUE(credentials.requests[0].challenge.realm);
	EXPECT_EQ(L"corporate", *credentials.requests[0].challenge.realm);
	ASSERT_EQ(1u, credentials.deadlines.size());
	EXPECT_TRUE(credentials.deadlines[0].has_value());
	ASSERT_EQ(1u, credentials.cancellations.size());
	EXPECT_EQ(nullptr, credentials.cancellations[0]);
	EXPECT_FALSE(transport.requests.front().headers.empty());
	EXPECT_EQ(3u, transport.requests.front().body.size());
}

TEST(RequestService, NeverForwardsAProxyCredentialWhenResolutionChangesProxy)
{
	FakeTransport transport;
	transport.results.push_back({ Response(407, { { L"Proxy-Authenticate", L"Basic realm=\"first\"" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	proxy.selections.push_back({ EProxyMode::Manual, L"http://proxy-one.example.test:8080", false });
	proxy.selections.push_back({ EProxyMode::Manual, L"http://proxy-two.example.test:8080", false });
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/resource" });

	ASSERT_TRUE(result);
	ASSERT_EQ(2u, transport.requests.size());
	ASSERT_EQ(2u, proxy.requests.size());
	ASSERT_TRUE(transport.requests[0].proxy.proxyUrl);
	ASSERT_TRUE(transport.requests[1].proxy.proxyUrl);
	EXPECT_NE(transport.requests[0].proxy.proxyUrl, transport.requests[1].proxy.proxyUrl);
	EXPECT_FALSE(transport.requests[0].proxyCredential);
	EXPECT_FALSE(transport.requests[1].proxyCredential);
	EXPECT_EQ(1u, credentials.requests.size());
}

TEST(RequestService, SurfacesCertificateValidationFailureAsTypedTlsOutcome)
{
	FakeTransport transport;
	transport.results.push_back({ std::nullopt, ETransportFailure::TlsCertificateFailure });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://untrusted.example.test/" });

	EXPECT_EQ(ERequestOutcome::TlsCertificateFailure, result.outcome);
	EXPECT_EQ(ETransportFailure::TlsCertificateFailure, result.transportFailure);
	EXPECT_TRUE(result.response == std::nullopt);
}

TEST(RequestService, NormalizesRedirectDotSegmentsAndRemovesFragment)
{
	FakeTransport transport;
	transport.results.push_back({ Response(302, { { L"Location", L"../child/./item#section" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/parent/start" });

	ASSERT_TRUE(result);
	ASSERT_EQ(2u, transport.requests.size());
	EXPECT_EQ(L"https://example.test/child/item", transport.requests[1].url);
}

TEST(RequestService, CrossOriginRedirectDoesNotForwardSensitiveCallerHeaders)
{
	FakeTransport transport;
	transport.results.push_back({ Response(302, { { L"Location", L"https://other.test/final" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/start", {
		{ L"Authorization", L"Bearer secret" },
		{ L"Cookie", L"session=secret" },
		{ L"Proxy-Authorization", L"Basic secret" },
		{ L"Host", L"example.test" },
		{ L"X-Stays", L"yes" },
	} });

	ASSERT_TRUE(result);
	ASSERT_EQ(2u, transport.requests.size());
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Authorization"));
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Cookie"));
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Proxy-Authorization"));
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Host"));
	EXPECT_TRUE(HasHeader(transport.requests[1], L"X-Stays"));
	EXPECT_TRUE(credentials.requests.empty());
}

TEST(RequestService, RedirectToGetRemovesEntityHeadersAndBody)
{
	FakeTransport transport;
	transport.results.push_back({ Response(303, { { L"Location", L"/done" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"POST", L"https://example.test/submit", {
		{ L"Authorization", L"Bearer same-origin" },
		{ L"Content-Type", L"application/json" },
		{ L"Content-Length", L"3" },
		{ L"Transfer-Encoding", L"chunked" },
	}, { 1, 2, 3 } });

	ASSERT_TRUE(result);
	ASSERT_EQ(2u, transport.requests.size());
	EXPECT_EQ(L"GET", transport.requests[1].method);
	EXPECT_TRUE(transport.requests[1].body.empty());
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Content-Type"));
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Content-Length"));
	EXPECT_FALSE(HasHeader(transport.requests[1], L"Transfer-Encoding"));
	EXPECT_TRUE(HasHeader(transport.requests[1], L"Authorization"));
}

TEST(RequestService, RejectsMalformedRequestsBeforeCallingAdapters)
{
	FakeTransport transport;
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);

	const auto invalidMethod = service.Execute({ L"GET\r\nInjected", L"https://example.test/" });
	const auto embeddedCredential = service.Execute({ L"GET", L"https://user@example.test/" });
	const auto invalidHeader = service.Execute({ L"GET", L"https://example.test/", { { L"X-Test", L"ok\r\nInjected: yes" } } });

	EXPECT_EQ(ERequestOutcome::InvalidRequest, invalidMethod.outcome);
	EXPECT_EQ(ERequestOutcome::InvalidRequest, embeddedCredential.outcome);
	EXPECT_EQ(ERequestOutcome::InvalidRequest, invalidHeader.outcome);
	EXPECT_TRUE(proxy.selectedUrls.empty());
	EXPECT_TRUE(transport.requests.empty());
}

TEST(RequestService, ContainsAdapterExceptionsForNonDeduplicatedCalls)
{
	FakeTransport transport;
	ThrowingProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	RequestService service(transport, proxy, credentials, clock, scheduler, jitter);

	const auto result = service.Execute({ L"GET", L"https://example.test/" });

	EXPECT_EQ(ERequestOutcome::TransportFailure, result.outcome);
	EXPECT_EQ(ETransportFailure::Network, result.transportFailure);
}

TEST(RequestService, RejectsConcurrentDeduplicationKeyReuseForDifferentRequestIdentity)
{
	BlockingTransport transport;
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	RequestService service(transport, proxy, credentials, clock, scheduler, jitter);
	RequestResult firstResult;
	Request first{ L"GET", L"https://example.test/first" };
	first.deduplicationKey = L"logical-operation";
	std::jthread firstCaller([&] { firstResult = service.Execute(first); });
	ASSERT_TRUE(transport.WaitUntilEntered());
	Request second = first;
	second.url = L"https://example.test/second";

	const auto secondResult = service.Execute(second);
	transport.Release();
	firstCaller.join();

	EXPECT_EQ(ERequestOutcome::InvalidRequest, secondResult.outcome);
	EXPECT_TRUE(firstResult);
	EXPECT_EQ(1u, transport.requests.size());
}

TEST(RequestService, ProxySupportOffUsesDirectTransportWithoutResolvingProxy)
{
	FakeTransport transport;
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	proxy.selection = { EProxyMode::Manual, L"http://must-not-be-used.example.test:8080", false };
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);
	Request request{ L"GET", L"https://example.test/direct" };
	request.proxySupport = EProxySupport::Off;

	const auto result = service.Execute(request);

	ASSERT_TRUE(result);
	EXPECT_TRUE(proxy.requests.empty());
	ASSERT_EQ(1u, transport.requests.size());
	EXPECT_EQ(EProxyMode::Direct, transport.requests.front().proxy.mode);
	EXPECT_TRUE(transport.requests.front().proxy.bypassed);
}

TEST(RequestService, PropagatesProxySupportAndReturnsTypedUnsupportedPolicy)
{
	FakeTransport transport;
	FakeProxyService proxy;
	proxy.selection.outcome = EProxySelectionOutcome::UnsupportedPolicy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);
	Request request{ L"GET", L"https://example.test/policy" };
	request.proxySupport = EProxySupport::Override;

	const auto result = service.Execute(request);

	EXPECT_EQ(ERequestOutcome::UnsupportedProxyPolicy, result.outcome);
	EXPECT_EQ(ETransportFailure::UnsupportedProxyPolicy, result.transportFailure);
	ASSERT_EQ(1u, proxy.requests.size());
	EXPECT_EQ(L"https://example.test/policy", proxy.requests.front().targetUrl);
	EXPECT_EQ(EProxySupport::Override, proxy.requests.front().support);
	ASSERT_EQ(1u, proxy.deadlines.size());
	EXPECT_TRUE(proxy.deadlines.front().has_value());
	ASSERT_EQ(1u, proxy.cancellations.size());
	EXPECT_EQ(nullptr, proxy.cancellations.front());
	EXPECT_TRUE(transport.requests.empty());
}

TEST(RequestService, CapsRetryWaitAtTheOverallMonotonicDeadline)
{
	FakeTransport transport;
	transport.results.push_back({ Response(429, { { L"Retry-After", L"10" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	scheduler.clock = &clock;
	FakeJitterSource jitter;
	RequestServiceOptions options;
	options.maxRetryDelay = std::chrono::seconds(30);
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter, nullptr, options);
	Request request{ L"GET", L"https://example.test/deadline" };
	request.limits.timeout = std::chrono::milliseconds(75);

	const auto result = service.Execute(request);

	EXPECT_EQ(ERequestOutcome::Timeout, result.outcome);
	EXPECT_EQ(ETransportFailure::Timeout, result.transportFailure);
	ASSERT_EQ(1u, scheduler.waits.size());
	EXPECT_EQ(std::chrono::milliseconds(75), scheduler.waits.front());
	EXPECT_EQ(1u, transport.requests.size());
}

TEST(RequestService, EnforcesResponseHeaderAndBodyLimitsWithTypedResults)
{
	FakeTransport headerTransport;
	headerTransport.results.push_back({ Response(200, { { L"X-Long", L"header" } }) });
	FakeTransport bodyTransport;
	auto largeBody = Response(200);
	largeBody.body = { 1, 2 };
	bodyTransport.results.push_back({ std::move(largeBody) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto headerService = CreateService(headerTransport, proxy, credentials, clock, scheduler, jitter);
	auto bodyService = CreateService(bodyTransport, proxy, credentials, clock, scheduler, jitter);
	Request headerRequest{ L"GET", L"https://example.test/headers" };
	headerRequest.limits.maxResponseHeaderBytes = 1;
	Request bodyRequest{ L"GET", L"https://example.test/body" };
	bodyRequest.limits.maxResponseBodyBytes = 1;

	const auto headerResult = headerService.Execute(headerRequest);
	const auto bodyResult = bodyService.Execute(bodyRequest);

	EXPECT_EQ(ERequestOutcome::ResponseHeaderLimitExceeded, headerResult.outcome);
	EXPECT_EQ(ETransportFailure::ResponseHeaderLimitExceeded, headerResult.transportFailure);
	EXPECT_EQ(ERequestOutcome::ResponseBodyLimitExceeded, bodyResult.outcome);
	EXPECT_EQ(ETransportFailure::ResponseBodyLimitExceeded, bodyResult.transportFailure);
}

TEST(RequestService, PassesDeadlineToTransportAndTimesOutAfterLateResponse)
{
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	AdvancingTransport transport(clock);
	RequestService service(transport, proxy, credentials, clock, scheduler, jitter);
	Request request{ L"GET", L"https://example.test/timeout" };
	request.limits.timeout = std::chrono::milliseconds(50);

	const auto result = service.Execute(request);

	EXPECT_EQ(ERequestOutcome::Timeout, result.outcome);
	EXPECT_EQ(ETransportFailure::Timeout, result.transportFailure);
	ASSERT_TRUE(transport.lastRequest);
	ASSERT_TRUE(transport.lastRequest->deadline);
	EXPECT_EQ(std::chrono::milliseconds(50), *transport.lastRequest->deadline - std::chrono::steady_clock::time_point{});
}

TEST(RequestService, RejectsUnsafePerRequestLimitsBeforeCallingAdapters)
{
	FakeTransport transport;
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	FakeJitterSource jitter;
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter);
	Request zeroTimeout{ L"GET", L"https://example.test/timeout" };
	zeroTimeout.limits.timeout = std::chrono::milliseconds::zero();
	Request zeroBodyLimit{ L"GET", L"https://example.test/body" };
	zeroBodyLimit.limits.maxResponseBodyBytes = 0;

	const auto timeoutResult = service.Execute(zeroTimeout);
	const auto bodyResult = service.Execute(zeroBodyLimit);

	EXPECT_EQ(ERequestOutcome::InvalidRequest, timeoutResult.outcome);
	EXPECT_EQ(ERequestOutcome::InvalidRequest, bodyResult.outcome);
	EXPECT_TRUE(proxy.requests.empty());
	EXPECT_TRUE(transport.requests.empty());
}
