/*! @file */
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/

#include <sakura/request/RequestService.h>

#if __has_include("platform/request/RequestService.h") || __has_include("RequestService.h")
#error "sakura_request_tests consumer can reach the provider private request header"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <iostream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

using namespace platform::request;

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
		if (results.empty()) return { std::nullopt, ETransportFailure::Network };
		auto result = std::move(results.front());
		results.pop_front();
		return result;
	}
};

class FakeProxyService final : public IProxyService {
public:
	ProxySelection selection{};
	std::deque<ProxySelection> selections;
	std::vector<ProxyRequest> requests;

	ProxySelection SelectProxy(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point>,
		const IRequestCancellation*) override
	{
		requests.push_back(request);
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

	std::optional<RequestCredential> GetCredential(
		const CredentialRequest& request,
		std::optional<std::chrono::steady_clock::time_point>,
		const IRequestCancellation*) override
	{
		requests.push_back(request);
		return RequestCredential{ L"pilot-user", { 0x01, 0x02 } };
	}
};

class FakeClock final : public IRequestClock {
public:
	std::chrono::system_clock::time_point Now() const override { return std::chrono::system_clock::time_point{}; }
	std::chrono::steady_clock::time_point SteadyNow() const override { return m_now; }

	std::chrono::steady_clock::time_point m_now{};
};

class FakeScheduler final : public IRequestScheduler {
public:
	std::vector<std::chrono::milliseconds> waits;
	FakeClock* clock = nullptr;

	bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation*) override
	{
		waits.push_back(delay);
		if (clock) clock->m_now += delay;
		return true;
	}
};

class FakeJitterSource final : public IRetryJitterSource {
public:
	double NextUnitInterval() override { return 1.0; }
};

class FakeResponseCache final : public IResponseCache {
public:
	std::unordered_map<std::wstring, HttpResponse> values;

	std::optional<HttpResponse> Get(std::wstring_view key) override
	{
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

RequestService CreateService(
	FakeTransport& transport,
	FakeProxyService& proxy,
	FakeCredentialService& credentials,
	FakeClock& clock,
	FakeScheduler& scheduler,
	FakeJitterSource& jitter,
	IResponseCache* cache = nullptr,
	RequestServiceOptions options = {})
{
	return RequestService(transport, proxy, credentials, clock, scheduler, jitter, cache, options);
}

bool HasHeader(const TransportRequest& request, std::wstring_view name)
{
	return std::any_of(request.headers.begin(), request.headers.end(), [name](const HttpHeader& header) {
		if (header.name.size() != name.size()) return false;
		for (std::size_t index = 0; index < name.size(); ++index) {
			const auto lower = [](wchar_t value) {
				return value >= L'A' && value <= L'Z' ? value - L'A' + L'a' : value;
			};
			if (lower(header.name[index]) != lower(name[index])) return false;
		}
		return true;
	});
}

bool CancellationIsTerminalBeforeTransport()
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
	return result.outcome == ERequestOutcome::Cancelled && transport.requests.empty();
}

bool RelativeRedirectIsResolvedByRequestCore()
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
	return result && result.redirectCount == 1 && transport.requests.size() == 2
		&& transport.requests[1].url == L"https://example.test/parent/child";
}

bool OfflineCacheHasNoTransportFallback()
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
	return hit && hit.fromCache && miss.outcome == ERequestOutcome::OfflineCacheMiss && transport.requests.empty();
}

bool RetryAfterIsBoundedAndScheduled()
{
	FakeTransport transport;
	transport.results.push_back({ Response(429, { { L"Retry-After", L"100000" } }) });
	transport.results.push_back({ Response(200) });
	FakeProxyService proxy;
	FakeCredentialService credentials;
	FakeClock clock;
	FakeScheduler scheduler;
	scheduler.clock = &clock;
	FakeJitterSource jitter;
	RequestServiceOptions options;
	options.initialRetryDelay = std::chrono::milliseconds(100);
	options.maxRetryDelay = std::chrono::milliseconds(500);
	auto service = CreateService(transport, proxy, credentials, clock, scheduler, jitter, nullptr, options);
	const auto result = service.Execute({ L"GET", L"https://example.test/rate-limited" });
	return result && result.retryCount == 1 && scheduler.waits.size() == 1
		&& scheduler.waits.front() == std::chrono::milliseconds(500) && transport.requests.size() == 2;
}

bool CrossOriginRedirectDropsSensitiveHeaders()
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
	return result && transport.requests.size() == 2
		&& !HasHeader(transport.requests[1], L"Authorization")
		&& !HasHeader(transport.requests[1], L"Cookie")
		&& !HasHeader(transport.requests[1], L"Proxy-Authorization")
		&& !HasHeader(transport.requests[1], L"Host")
		&& HasHeader(transport.requests[1], L"X-Stays");
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{"CancellationIsTerminalBeforeTransport", CancellationIsTerminalBeforeTransport},
	TestCase{"RelativeRedirectIsResolvedByRequestCore", RelativeRedirectIsResolvedByRequestCore},
	TestCase{"OfflineCacheHasNoTransportFallback", OfflineCacheHasNoTransportFallback},
	TestCase{"RetryAfterIsBoundedAndScheduled", RetryAfterIsBoundedAndScheduled},
	TestCase{"CrossOriginRedirectDropsSensitiveHeaders", CrossOriginRedirectDropsSensitiveHeaders},
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "RequestService.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "RequestService.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "RequestService." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
