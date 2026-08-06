/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "extension/openvsx/OpenVsxProtocol.h"
#include "extension/openvsx/OpenVsxRequestServiceAdapter.h"

#include <deque>
#include <type_traits>

namespace {

using namespace extension::openvsx;
using namespace platform::request;

static_assert(std::is_base_of_v<IOpenVsxRegistryClient, OpenVsxRequestServiceAdapter>);

class FakeRequestService final : public IRequestService {
public:
	std::deque<RequestResult> results;
	std::vector<Request> requests;

	RequestResult Execute(const Request& request, const IRequestCancellation*) override
	{
		requests.push_back(request);
		if (results.empty()) return { ERequestOutcome::TransportFailure, ETransportFailure::Network };
		auto result = std::move(results.front());
		results.pop_front();
		return result;
	}
};

class FakeCancellation final : public IRequestCancellation {
public:
	bool cancelled = false;
	bool IsCancellationRequested() const noexcept override { return cancelled; }
};

RequestResult HttpResult(int statusCode, std::wstring finalUrl, std::vector<std::uint8_t> body = {})
{
	RequestResult result;
	result.outcome = ERequestOutcome::Success;
	result.response = HttpResponse{ statusCode, {}, std::move(body), std::move(finalUrl) };
	return result;
}

OpenVsxRequestPolicy MakePolicy()
{
	OpenVsxRequestPolicy policy;
	policy.cachePolicy = ERequestCachePolicy::PreferCache;
	policy.allowRedirects = true;
	policy.proxySupport = EProxySupport::Override;
	policy.searchLimits = { std::chrono::milliseconds(111), 1, 101, 102 };
	policy.vsixLimits = { std::chrono::milliseconds(222), 2, 201, 202 };
	policy.sha256Limits = { std::chrono::milliseconds(333), 3, 301, 302 };
	policy.textLimits = { std::chrono::milliseconds(444), 4, 401, 402 };
	return policy;
}

void ExpectExactLimits(const RequestLimits& expected, const RequestLimits& actual)
{
	EXPECT_EQ(expected.timeout, actual.timeout);
	EXPECT_EQ(expected.maxRedirects, actual.maxRedirects);
	EXPECT_EQ(expected.maxResponseHeaderBytes, actual.maxResponseHeaderBytes);
	EXPECT_EQ(expected.maxResponseBodyBytes, actual.maxResponseBodyBytes);
}

TEST(OpenVsxRequestServiceAdapter, RoutesSearchVsixAndOptionalSha256ThroughTheSharedService)
{
	FakeRequestService requestService;
	requestService.results.push_back(HttpResult(200, L"https://registry.example/api/-/search?offset=0&size=25&query=a%26b",
		std::vector<std::uint8_t>{ '{', '"', 'o', 'f', 'f', 's', 'e', 't', '"', ':', '0', '}' }));
	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.vsix", { 1, 2, 3 }));
	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.sha256", { 'a', 'b', 'c' }));
	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.readme", { '#', ' ', 'R', 'E', 'A', 'D', 'M', 'E' }));

	const auto policy = MakePolicy();
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example///", policy);
	const IOpenVsxRegistryClient& registryClient = adapter;

	EXPECT_TRUE(registryClient.Search(L"a&b", -9, 25).status);
	const auto vsix = registryClient.FetchVsix(L"https://cdn.example/tool.vsix");
	ASSERT_TRUE(vsix.status);
	EXPECT_EQ(std::vector<std::uint8_t>({ 1, 2, 3 }), vsix.value);
	const auto sha256 = registryClient.FetchOptionalSha256(std::wstring(L"https://cdn.example/tool.sha256"));
	ASSERT_TRUE(sha256.status);
	EXPECT_EQ(std::vector<std::uint8_t>({ 'a', 'b', 'c' }), sha256.value);
	EXPECT_EQ(EOpenVsxRequestOutcome::NotRequested, registryClient.FetchOptionalSha256(std::nullopt).status.outcome);
	const auto readme = registryClient.FetchText(L"https://cdn.example/tool.readme");
	ASSERT_TRUE(readme.status);
	EXPECT_EQ(L"# README", readme.value);

	ASSERT_EQ(4u, requestService.requests.size());
	// 検索 URL には VS Code と同じく必ず targetPlatform が乗る。ホストの語彙を使うので
	// x64 / ARM64 のどちらのビルドでも同じ期待値で通る。
	EXPECT_EQ(std::wstring(L"https://registry.example/api/-/search?offset=0&size=25&query=a%26b&targetPlatform=") +
			std::wstring(OpenVsxProtocol::HostTargetPlatform()),
		requestService.requests[0].url);
	EXPECT_EQ(L"https://cdn.example/tool.vsix", requestService.requests[1].url);
	EXPECT_EQ(L"https://cdn.example/tool.sha256", requestService.requests[2].url);
	EXPECT_EQ(L"https://cdn.example/tool.readme", requestService.requests[3].url);
	EXPECT_EQ(L"GET", requestService.requests[0].method);
	ExpectExactLimits(policy.searchLimits, requestService.requests[0].limits);
	ExpectExactLimits(policy.vsixLimits, requestService.requests[1].limits);
	ExpectExactLimits(policy.sha256Limits, requestService.requests[2].limits);
	ExpectExactLimits(policy.textLimits, requestService.requests[3].limits);
	EXPECT_EQ(policy.proxySupport, requestService.requests[2].proxySupport);
	EXPECT_TRUE(requestService.requests[0].allowRedirects);
	EXPECT_EQ(ERequestCachePolicy::PreferCache, requestService.requests[1].cachePolicy);
	EXPECT_EQ(1u, requestService.requests[0].headers.size());
	EXPECT_EQ(L"Accept", requestService.requests[0].headers[0].name);
	EXPECT_EQ(L"application/json", requestService.requests[0].headers[0].value);
}

TEST(OpenVsxRequestServiceAdapter, PreservesTerminalStatusAndRejectsCancellationDowngradeAndOversizedResponses)
{
	FakeRequestService requestService;
	const auto policy = MakePolicy();
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example", policy);

	FakeCancellation cancellation;
	cancellation.cancelled = true;
	const auto cancelled = adapter.Search(L"query", 0, 1, &cancellation);
	EXPECT_EQ(EOpenVsxRequestOutcome::Cancelled, cancelled.status.outcome);
	EXPECT_EQ(ERequestOutcome::Cancelled, cancelled.status.requestOutcome);
	EXPECT_TRUE(requestService.requests.empty());

	requestService.results.push_back({ ERequestOutcome::Timeout, ETransportFailure::Timeout });
	const auto timeout = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::Timeout, timeout.status.outcome);
	EXPECT_EQ(ERequestOutcome::Timeout, timeout.status.requestOutcome);

	requestService.results.push_back(HttpResult(404, L"https://cdn.example/tool.vsix"));
	const auto notFound = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::HttpStatusFailure, notFound.status.outcome);
	EXPECT_EQ(404, *notFound.status.httpStatusCode);

	requestService.results.push_back(HttpResult(200, L"http://cdn.example/tool.vsix", { 1 }));
	const auto downgrade = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::HttpsDowngradeRejected, downgrade.status.outcome);

	auto oversizedHeaders = HttpResult(200, L"https://cdn.example/tool.vsix");
	oversizedHeaders.response->headers.push_back({ std::wstring(100, L'h'), std::wstring(100, L'v') });
	requestService.results.push_back(std::move(oversizedHeaders));
	const auto oversizedHeaderResult = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded, oversizedHeaderResult.status.outcome);
	EXPECT_EQ(ERequestOutcome::ResponseHeaderLimitExceeded, oversizedHeaderResult.status.requestOutcome);

	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.vsix", std::vector<std::uint8_t>(203, 0)));
	const auto oversized = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::ResponseBodyLimitExceeded, oversized.status.outcome);
	EXPECT_EQ(ERequestOutcome::ResponseBodyLimitExceeded, oversized.status.requestOutcome);
	EXPECT_EQ(policy.vsixLimits.maxResponseBodyBytes, requestService.requests.back().limits.maxResponseBodyBytes);

	const auto plainHttp = adapter.FetchVsix(L"http://cdn.example/tool.vsix");
	EXPECT_EQ(EOpenVsxRequestOutcome::InvalidEndpointUri, plainHttp.status.outcome);
	EXPECT_EQ(5u, requestService.requests.size());
}

TEST(OpenVsxRequestServiceAdapter, MapsEveryRequestServiceTerminalOutcome)
{
	struct Mapping {
		ERequestOutcome requestOutcome;
		EOpenVsxRequestOutcome adapterOutcome;
	};
	constexpr Mapping mappings[] = {
		{ ERequestOutcome::Cancelled, EOpenVsxRequestOutcome::Cancelled },
		{ ERequestOutcome::InvalidRequest, EOpenVsxRequestOutcome::InvalidRequest },
		{ ERequestOutcome::OfflineCacheMiss, EOpenVsxRequestOutcome::OfflineCacheMiss },
		{ ERequestOutcome::RedirectLimitExceeded, EOpenVsxRequestOutcome::RedirectLimitExceeded },
		{ ERequestOutcome::HttpsDowngradeRejected, EOpenVsxRequestOutcome::HttpsDowngradeRejected },
		{ ERequestOutcome::InvalidRedirect, EOpenVsxRequestOutcome::InvalidRedirect },
		{ ERequestOutcome::Timeout, EOpenVsxRequestOutcome::Timeout },
		{ ERequestOutcome::ResponseHeaderLimitExceeded, EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded },
		{ ERequestOutcome::ResponseBodyLimitExceeded, EOpenVsxRequestOutcome::ResponseBodyLimitExceeded },
		{ ERequestOutcome::ServerAuthenticationRequired, EOpenVsxRequestOutcome::ServerAuthenticationRequired },
		{ ERequestOutcome::ProxyAuthenticationRequired, EOpenVsxRequestOutcome::ProxyAuthenticationRequired },
		{ ERequestOutcome::UnsupportedProxyPolicy, EOpenVsxRequestOutcome::UnsupportedProxyPolicy },
		{ ERequestOutcome::TransportFailure, EOpenVsxRequestOutcome::TransportFailure },
		{ ERequestOutcome::TlsCertificateFailure, EOpenVsxRequestOutcome::TlsCertificateFailure },
	};

	FakeRequestService requestService;
	for (const auto mapping : mappings) {
		requestService.results.push_back({ mapping.requestOutcome });
	}
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example", MakePolicy());

	for (const auto mapping : mappings) {
		const auto operation = adapter.FetchVsix(L"https://cdn.example/tool.vsix");
		EXPECT_EQ(mapping.adapterOutcome, operation.status.outcome);
		EXPECT_EQ(mapping.requestOutcome, operation.status.requestOutcome);
	}
}

TEST(OpenVsxRequestServiceAdapter, RoutesFetchVsixStreamedThroughTheSharedServiceWithTheSameVsixPolicy)
{
	FakeRequestService requestService;
	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.vsix"));

	const auto policy = MakePolicy();
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example", policy);
	const IOpenVsxRegistryClient& registryClient = adapter;

	std::vector<std::uint8_t> received;
	const OpenVsxBodyChunkSink sink = [&received](const std::uint8_t* data, std::size_t size) {
		received.insert(received.end(), data, data + size);
		return true;
	};

	const auto status = registryClient.FetchVsixStreamed(L"https://cdn.example/tool.vsix", sink);
	EXPECT_TRUE(status);
	EXPECT_EQ(EOpenVsxRequestOutcome::Success, status.outcome);

	ASSERT_EQ(1u, requestService.requests.size());
	const Request& dispatched = requestService.requests[0];
	EXPECT_EQ(L"GET", dispatched.method);
	EXPECT_EQ(L"https://cdn.example/tool.vsix", dispatched.url);
	ASSERT_EQ(1u, dispatched.headers.size());
	EXPECT_EQ(L"Accept", dispatched.headers[0].name);
	EXPECT_EQ(L"application/octet-stream", dispatched.headers[0].value);
	ExpectExactLimits(policy.vsixLimits, dispatched.limits);
	EXPECT_EQ(policy.cachePolicy, dispatched.cachePolicy);
	EXPECT_EQ(policy.allowRedirects, dispatched.allowRedirects);
	EXPECT_EQ(policy.proxySupport, dispatched.proxySupport);

	// adapter が Request へ載せる bodySink は呼び出し元の sink そのもの。RequestService/
	// transport が実際にこれを chunk ごとに呼ぶ経路は WinHttpRequestRuntime 側の責務なので、
	// ここでは adapter が正しい sink を配線したことだけを、直接 1 回呼び出して確認する。
	ASSERT_TRUE(static_cast<bool>(dispatched.bodySink));
	const std::vector<std::uint8_t> chunk{ 10, 20, 30 };
	EXPECT_TRUE(dispatched.bodySink(chunk.data(), chunk.size()));
	EXPECT_EQ(chunk, received);
}

TEST(OpenVsxRequestServiceAdapter, RejectsFetchVsixStreamedWithoutDispatchingWhenSinkOrUriOrCancellationIsInvalid)
{
	FakeRequestService requestService;
	const auto policy = MakePolicy();
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example", policy);
	const OpenVsxBodyChunkSink noopSink = [](const std::uint8_t*, std::size_t) { return true; };

	const auto missingSink = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", OpenVsxBodyChunkSink{});
	EXPECT_EQ(EOpenVsxRequestOutcome::InvalidRequest, missingSink.outcome);
	EXPECT_TRUE(requestService.requests.empty());

	FakeCancellation cancellation;
	cancellation.cancelled = true;
	const auto cancelled = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink, &cancellation);
	EXPECT_EQ(EOpenVsxRequestOutcome::Cancelled, cancelled.outcome);
	EXPECT_EQ(ERequestOutcome::Cancelled, cancelled.requestOutcome);
	EXPECT_TRUE(requestService.requests.empty());

	const auto plainHttp = adapter.FetchVsixStreamed(L"http://cdn.example/tool.vsix", noopSink);
	EXPECT_EQ(EOpenVsxRequestOutcome::InvalidEndpointUri, plainHttp.outcome);
	EXPECT_TRUE(requestService.requests.empty());
}

TEST(OpenVsxRequestServiceAdapter, MapsFetchVsixStreamedTerminalOutcomesFromTheSharedServiceResponse)
{
	FakeRequestService requestService;
	const auto policy = MakePolicy();
	const OpenVsxRequestServiceAdapter adapter(requestService, L"https://registry.example", policy);
	const OpenVsxBodyChunkSink noopSink = [](const std::uint8_t*, std::size_t) { return true; };

	requestService.results.push_back({ ERequestOutcome::Timeout, ETransportFailure::Timeout });
	const auto timeout = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink);
	EXPECT_EQ(EOpenVsxRequestOutcome::Timeout, timeout.outcome);
	EXPECT_EQ(ERequestOutcome::Timeout, timeout.requestOutcome);

	requestService.results.push_back(HttpResult(404, L"https://cdn.example/tool.vsix"));
	const auto notFound = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink);
	EXPECT_EQ(EOpenVsxRequestOutcome::HttpStatusFailure, notFound.outcome);
	EXPECT_EQ(404, *notFound.httpStatusCode);

	requestService.results.push_back(HttpResult(200, L"http://cdn.example/tool.vsix"));
	const auto downgrade = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink);
	EXPECT_EQ(EOpenVsxRequestOutcome::HttpsDowngradeRejected, downgrade.outcome);

	auto oversizedHeaders = HttpResult(200, L"https://cdn.example/tool.vsix");
	oversizedHeaders.response->headers.push_back({ std::wstring(100, L'h'), std::wstring(100, L'v') });
	requestService.results.push_back(std::move(oversizedHeaders));
	const auto oversizedHeaderResult = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink);
	EXPECT_EQ(EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded, oversizedHeaderResult.outcome);
	EXPECT_EQ(ERequestOutcome::ResponseHeaderLimitExceeded, oversizedHeaderResult.requestOutcome);

	// ストリーミング経路は応答本体を保持しない（transport が sink へ渡し終えた後は空のまま
	// 返る）ので、response->body の長さで上限を二重判定しない。実際の上限強制は
	// transport 側の chunk 単位チェック（WinHttpRequestRuntime.cpp）が既に担っている。
	// ここでは fake があえて本体付きの応答を返しても、この adapter 層では拒否されない
	// ことを確認して、この設計判断（二重判定しない）を固定する。
	requestService.results.push_back(HttpResult(200, L"https://cdn.example/tool.vsix", std::vector<std::uint8_t>(500, 0)));
	const auto notDoubleCheckedByBodySize = adapter.FetchVsixStreamed(L"https://cdn.example/tool.vsix", noopSink);
	EXPECT_TRUE(notDoubleCheckedByBodySize);

	ASSERT_EQ(5u, requestService.requests.size());
	for (const auto& dispatched : requestService.requests) {
		ASSERT_TRUE(static_cast<bool>(dispatched.bodySink));
	}
}

} // namespace
