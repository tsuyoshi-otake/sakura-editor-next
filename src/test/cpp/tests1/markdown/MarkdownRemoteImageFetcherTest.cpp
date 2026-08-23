/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "markdown/MarkdownRemoteImageFetcher.h"

namespace markdown {
namespace {

class FakeRequestService final : public platform::request::IRequestService {
public:
	platform::request::RequestResult Execute(
		const platform::request::Request& request,
		const platform::request::IRequestCancellation*) override
	{
		lastRequest = request;
		++executeCount;
		return result;
	}

	platform::request::RequestResult result;
	platform::request::Request lastRequest;
	int executeCount = 0;
};

platform::request::RequestResult Response(
	int status, std::wstring contentType, std::wstring finalUrl,
	std::vector<std::uint8_t> body = { 1, 2, 3 })
{
	platform::request::RequestResult result;
	result.outcome = platform::request::ERequestOutcome::Success;
	result.response = platform::request::HttpResponse{
		status,
		{ { L"Content-Type", std::move(contentType) } },
		std::move(body),
		std::move(finalUrl),
	};
	return result;
}

TEST(MarkdownRemoteImageFetcher, AdmitsOnlyCanonicalStrictHttpsUrls)
{
	std::wstring normalized;
	EXPECT_TRUE(NormalizeStrictHttpsImageUrl(
		L"HTTPS://example.test/a%20b.png?size=2#section", &normalized));
	EXPECT_EQ(L"https://example.test/a%20b.png?size=2", normalized);
	EXPECT_FALSE(NormalizeStrictHttpsImageUrl(L"http://example.test/a.png", &normalized));
	EXPECT_FALSE(NormalizeStrictHttpsImageUrl(L"//example.test/a.png", &normalized));
	EXPECT_FALSE(NormalizeStrictHttpsImageUrl(L"https://user@example.test/a.png", &normalized));
	EXPECT_FALSE(NormalizeStrictHttpsImageUrl(L"https://example.test/bad path.png", &normalized));
}

TEST(MarkdownRemoteImageFetcher, FetchesOneBoundedAnonymousImageResponse)
{
	FakeRequestService service;
	service.result = Response(200, L"Image/PNG; charset=binary",
		L"https://cdn.example.test/image.png");
	const auto fetched = FetchStrictHttpsImage(service,
		platform::request::EProxySupport::Fallback,
		L"https://cdn.example.test/image.png#ignored");

	ASSERT_TRUE(fetched);
	EXPECT_EQ(L"image/png", fetched.mediaType);
	EXPECT_EQ(L"https://cdn.example.test/image.png", fetched.finalUrl);
	EXPECT_EQ(1, service.executeCount);
	EXPECT_EQ(L"https://cdn.example.test/image.png", service.lastRequest.url);
	EXPECT_EQ(std::chrono::seconds(5), service.lastRequest.limits.timeout);
	EXPECT_EQ(3u, service.lastRequest.limits.maxRedirects);
	EXPECT_EQ(8u * 1024u * 1024u, service.lastRequest.limits.maxResponseBodyBytes);
	EXPECT_TRUE(service.lastRequest.deduplicationKey.has_value());
	for (const auto& header : service.lastRequest.headers) {
		EXPECT_NE(L"Authorization", header.name);
		EXPECT_NE(L"Cookie", header.name);
		EXPECT_NE(L"Referer", header.name);
	}
}

TEST(MarkdownRemoteImageFetcher, RejectsEveryUnsafeTerminalResponse)
{
	FakeRequestService service;
	service.result = Response(200, L"text/html", L"https://example.test/image.png");
	EXPECT_EQ(ERemoteImageFetchOutcome::UnsupportedMediaType,
		FetchStrictHttpsImage(service, platform::request::EProxySupport::Fallback,
			L"https://example.test/image.png").outcome);

	service.result = Response(404, L"image/png", L"https://example.test/image.png");
	EXPECT_EQ(ERemoteImageFetchOutcome::HttpStatusRejected,
		FetchStrictHttpsImage(service, platform::request::EProxySupport::Fallback,
			L"https://example.test/image.png").outcome);

	service.result = Response(200, L"image/png", L"http://example.test/image.png");
	EXPECT_EQ(ERemoteImageFetchOutcome::InvalidUrl,
		FetchStrictHttpsImage(service, platform::request::EProxySupport::Fallback,
			L"https://example.test/image.png").outcome);

	service.result.outcome = platform::request::ERequestOutcome::Cancelled;
	service.result.response.reset();
	EXPECT_EQ(ERemoteImageFetchOutcome::Cancelled,
		FetchStrictHttpsImage(service, platform::request::EProxySupport::Fallback,
			L"https://example.test/image.png").outcome);
}

} // namespace
} // namespace markdown
