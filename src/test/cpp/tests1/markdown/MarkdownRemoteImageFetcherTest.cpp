/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "markdown/CMarkdownPreviewWnd.h"
#include "markdown/MarkdownRemoteImageFetcher.h"

#include <array>
#include <stdexcept>

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

TEST(MarkdownSvgTextOverlay, RendersLegitimateTextPixels)
{
	constexpr int width = 64;
	constexpr int height = 32;
	std::array<BYTE, width * height * 4> pixels{};
	const auto result = detail::OverlaySvgText(pixels.data(), width, height, width * 4,
		R"(<svg><text x="3" y="22" font-size="16" fill="#ff0000">Hi</text></svg>)",
		width, height, 100'000);

	ASSERT_EQ(detail::SvgTextOverlayResult::Completed, result);
	bool renderedPixel = false;
	for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
		const auto index = pixel * 4;
		renderedPixel = renderedPixel || (pixels[index + 2] != 0 && pixels[index + 3] != 0);
	}
	EXPECT_TRUE(renderedPixel);
	const auto original = pixels;
	EXPECT_EQ(detail::SvgTextOverlayResult::Completed,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4,
			R"(<svg><text x="100000" y="22" font-size="16">A</text></svg>)",
			width, height, 100'000));
	EXPECT_EQ(original, pixels);
	EXPECT_EQ(detail::SvgTextOverlayResult::Failed,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4,
			R"(<svg><text x="1e300" y="22" font-size="16">A</text></svg>)",
			width, height, 100'000));
	EXPECT_EQ(original, pixels);
}

TEST(MarkdownSvgTextOverlay, MaximumRunCountCannotPartiallyApplyPastWorkBudget)
{
	constexpr int width = 64;
	constexpr int height = 32;
	std::array<BYTE, width * height * 4> pixels{};
	pixels.fill(0x5a);
	const auto original = pixels;
	std::string xml = "<svg>";
	for (int run = 0; run < 256; ++run) {
		xml += "<text x=\"3\" y=\"22\" font-size=\"16\" fill=\"#ff0000\">A</text>";
	}
	xml += "</svg>";

	const auto result = detail::OverlaySvgText(pixels.data(), width, height, width * 4,
		xml, width, height, 5'000);
	EXPECT_EQ(detail::SvgTextOverlayResult::WorkBudgetExceeded, result);
	EXPECT_EQ(original, pixels);

	constexpr int largeEdge = 1600;
	std::vector<BYTE> largePixels(static_cast<size_t>(largeEdge) * largeEdge * 4, 0x5a);
	const auto largeOriginal = largePixels;
	std::string largeXml = "<svg>";
	for (int run = 0; run < 256; ++run) {
		largeXml += "<text x=\"0\" y=\"1500\" font-size=\"1600\">A</text>";
	}
	largeXml += "</svg>";
	EXPECT_EQ(detail::SvgTextOverlayResult::WorkBudgetExceeded,
		detail::OverlaySvgText(largePixels.data(), largeEdge, largeEdge, largeEdge * 4,
			largeXml, largeEdge, largeEdge, 16'777'216));
	EXPECT_EQ(largeOriginal, largePixels);
}

TEST(MarkdownSvgTextOverlay, ExcessTextIsRejectedWithoutPublishingAPrefix)
{
	constexpr int width = 64;
	constexpr int height = 32;
	std::array<BYTE, width * height * 4> pixels{};
	std::string xml = "<svg>";
	const std::string run = "<text x=\"3\" y=\"22\" font-size=\"16\">A</text>";
	for (int i = 0; i < 256; ++i) xml += run;
	const std::string atLimit = xml + "</svg>";
	EXPECT_EQ(detail::SvgTextOverlayResult::Completed,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4,
			atLimit, width, height, 2'000'000));
	const auto original = pixels;
	xml += run + "</svg>";
	EXPECT_EQ(detail::SvgTextOverlayResult::WorkBudgetExceeded,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4,
			xml, width, height, 2'000'000));
	EXPECT_EQ(original, pixels);
	const std::string tooLong = "<svg><text>" + std::string(4097, 'A') + "</text></svg>";
	EXPECT_EQ(detail::SvgTextOverlayResult::WorkBudgetExceeded,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4,
			tooLong, width, height, 2'000'000));
	EXPECT_EQ(original, pixels);
}

TEST(MarkdownSvgTextOverlay, CancellationDiscardsStagedPixels)
{
	constexpr int width = 64;
	constexpr int height = 32;
	constexpr std::string_view xml = R"(<svg><text x="3" y="22" font-size="16" fill="#ff0000">Hi</text><text x="20" y="22" font-size="16">B</text></svg>)";
	std::array<BYTE, width * height * 4> original{};
	original.fill(0x31);
	auto completed = original;
	int totalChecks = 0;
	ASSERT_EQ(detail::SvgTextOverlayResult::Completed,
		detail::OverlaySvgText(completed.data(), width, height, width * 4,
			xml, width, height, 100'000, [&] { ++totalChecks; return false; }));
	ASSERT_GT(totalChecks, 0);
	ASSERT_NE(original, completed);
	// Inject cancellation at every reachable check, then retry from the same
	// destination. The interrupted attempt must never publish staged pixels.
	for (int cancelAt = 1; cancelAt <= totalChecks; ++cancelAt) {
		SCOPED_TRACE(cancelAt);
		auto pixels = original;
		int checks = 0;
		ASSERT_EQ(detail::SvgTextOverlayResult::Cancelled,
			detail::OverlaySvgText(pixels.data(), width, height, width * 4,
				xml, width, height, 100'000, [&] { return ++checks >= cancelAt; }));
		EXPECT_EQ(original, pixels);
		ASSERT_EQ(detail::SvgTextOverlayResult::Completed,
			detail::OverlaySvgText(pixels.data(), width, height, width * 4,
				xml, width, height, 100'000));
		EXPECT_EQ(completed, pixels);
	}
	const auto gdiObjectsBefore = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	const auto probeFont = ::CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	ASSERT_NE(probeFont, nullptr);
	const auto gdiObjectsWithProbe = ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS);
	const auto probeDeleted = ::DeleteObject(probeFont);
	ASSERT_TRUE(probeDeleted);
	ASSERT_EQ(gdiObjectsBefore + 1, gdiObjectsWithProbe);
	ASSERT_EQ(gdiObjectsBefore, ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	for (int attempt = 0; attempt < 32; ++attempt) {
		auto pixels = original;
		int checks = 0;
		ASSERT_EQ(detail::SvgTextOverlayResult::Failed,
			detail::OverlaySvgText(pixels.data(), width, height, width * 4,
				xml, width, height, 100'000, [&] {
					if (++checks == 15) throw std::runtime_error("cancellation callback failed");
					return false;
				}));
		EXPECT_EQ(original, pixels);
	}
	EXPECT_EQ(gdiObjectsBefore, ::GetGuiResources(::GetCurrentProcess(), GR_GDIOBJECTS));
	auto pixels = original;
	EXPECT_EQ(detail::SvgTextOverlayResult::Completed,
		detail::OverlaySvgText(pixels.data(), width, height, width * 4, xml, width, height, 100'000));
	EXPECT_EQ(completed, pixels);
}

TEST(MarkdownSvgTextOverlay, OneShotCancellationDuringExtractionIsNotLost)
{
	std::array<BYTE, 64 * 32 * 4> pixels{};
	const auto original = pixels;
	int checks = 0;
	EXPECT_EQ(detail::SvgTextOverlayResult::Cancelled,
		detail::OverlaySvgText(pixels.data(), 64, 32, 64 * 4,
			R"(<svg><text x="3" y="22">Hi</text></svg>)", 64, 32, 100'000,
			[&] { return ++checks == 2; }));
	EXPECT_EQ(original, pixels);
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
