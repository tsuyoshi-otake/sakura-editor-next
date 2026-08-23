/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "markdown/MarkdownRemoteImageFetcher.h"

#include "config/CConfigurationProxyService.h"
#include <sakura/request/win32/WinHttpRequestRuntime.h>
#include <sakura/request/win32/WinHttpSystemProxyResolver.h>
#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <list>
#include <mutex>
#include <optional>
#include <utility>

namespace markdown {
namespace {

constexpr std::size_t kMaximumRemoteImageBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumCachedImages = 16;
constexpr std::size_t kMaximumCachedBytes = 32U * 1024U * 1024U;
constexpr std::size_t kMaximumRedirects = 3;
constexpr std::size_t kMaximumRetries = 1;
constexpr auto kRequestTimeout = std::chrono::seconds(5);

[[nodiscard]] bool EqualsAsciiInsensitive(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		const auto leftValue = static_cast<wchar_t>(std::towlower(left[index]));
		const auto rightValue = static_cast<wchar_t>(std::towlower(right[index]));
		if (leftValue != rightValue) return false;
	}
	return true;
}

[[nodiscard]] std::wstring NormalizeMediaType(std::wstring_view value)
{
	const auto delimiter = value.find(L';');
	if (delimiter != std::wstring_view::npos) value = value.substr(0, delimiter);
	while (!value.empty() && std::iswspace(value.front())) value.remove_prefix(1);
	while (!value.empty() && std::iswspace(value.back())) value.remove_suffix(1);
	std::wstring result(value);
	std::ranges::transform(result, result.begin(), [](wchar_t character) {
		return static_cast<wchar_t>(std::towlower(character));
	});
	return result;
}

[[nodiscard]] std::optional<std::wstring> FindHeader(
	const std::vector<platform::request::HttpHeader>& headers, std::wstring_view name)
{
	for (const auto& header : headers) {
		if (EqualsAsciiInsensitive(header.name, name)) return header.value;
	}
	return std::nullopt;
}

[[nodiscard]] bool IsImageMediaType(std::wstring_view mediaType) noexcept
{
	return mediaType.size() > 6 && mediaType.starts_with(L"image/");
}

class NoCredentialService final : public platform::request::ICredentialService {
public:
	std::optional<platform::request::RequestCredential> GetCredential(
		const platform::request::CredentialRequest&,
		std::optional<std::chrono::steady_clock::time_point>,
		const platform::request::IRequestCancellation*) override
	{
		return std::nullopt;
	}
};

class MarkdownRemoteImageFetcher final : public IMarkdownRemoteImageFetcher {
public:
	explicit MarkdownRemoteImageFetcher(config::ConfigurationNetworkPolicySnapshot networkPolicy)
		: m_proxySupport(networkPolicy.proxySupport)
		, m_proxyService(std::move(networkPolicy), m_systemProxyResolver)
		, m_requestService(
			m_transport,
			m_proxyService,
			m_credentialService,
			m_clock,
			m_scheduler,
			m_jitterSource,
			nullptr,
			{ kMaximumRedirects, kMaximumRetries,
				std::chrono::milliseconds(250), std::chrono::seconds(2) })
	{
	}

	RemoteImageFetchResult Fetch(std::wstring_view url,
		const platform::request::IRequestCancellation* cancellation) override
	{
		std::wstring normalized;
		if (!NormalizeStrictHttpsImageUrl(url, &normalized)) {
			return { ERemoteImageFetchOutcome::InvalidUrl };
		}
		{
			std::lock_guard lock(m_cacheMutex);
			const auto cached = std::ranges::find_if(m_cache,
				[&normalized](const CacheEntry& entry) { return entry.url == normalized; });
			if (cached != m_cache.end()) {
				const auto value = cached->result;
				m_cache.splice(m_cache.begin(), m_cache, cached);
				return value;
			}
		}

		auto result = FetchStrictHttpsImage(
			m_requestService, m_proxySupport, normalized, cancellation);
		if (!result) return result;

		std::lock_guard lock(m_cacheMutex);
		m_cachedBytes += result.bytes->size();
		m_cache.push_front({ normalized, result });
		while (m_cache.size() > kMaximumCachedImages || m_cachedBytes > kMaximumCachedBytes) {
			m_cachedBytes -= m_cache.back().result.bytes->size();
			m_cache.pop_back();
		}
		return result;
	}

private:
	struct CacheEntry final {
		std::wstring url;
		RemoteImageFetchResult result;
	};

	platform::request::EProxySupport m_proxySupport;
	platform::request::win32::WinHttpRequestTransport m_transport;
	platform::request::win32::WinHttpSystemProxyResolver m_systemProxyResolver;
	config::CConfigurationProxyService m_proxyService;
	NoCredentialService m_credentialService;
	platform::request::win32::Win32RequestClock m_clock;
	platform::request::win32::Win32RequestScheduler m_scheduler;
	platform::request::win32::ThreadSafeRetryJitterSource m_jitterSource;
	platform::request::RequestService m_requestService;
	std::mutex m_cacheMutex;
	std::list<CacheEntry> m_cache;
	std::size_t m_cachedBytes = 0;
};

} // namespace

bool NormalizeStrictHttpsImageUrl(std::wstring_view url, std::wstring* normalizedUrl)
{
	if (normalizedUrl == nullptr) return false;
	normalizedUrl->clear();
	const auto parsed = platform::uri::Uri::Parse(url);
	if (!parsed || !parsed.value->HasAuthority()
		|| !EqualsAsciiInsensitive(parsed.value->Scheme(), L"https")
		|| parsed.value->Authority().empty()
		|| parsed.value->Authority().find(L'@') != std::wstring::npos) return false;
	const auto withoutFragment = platform::uri::Uri::FromComponents(
		parsed.value->Scheme(), parsed.value->Authority(), parsed.value->Path(),
		parsed.value->Query(), std::nullopt, true);
	if (!withoutFragment) return false;
	*normalizedUrl = withoutFragment.value->ToString();
	return !normalizedUrl->empty();
}

RemoteImageFetchResult FetchStrictHttpsImage(
	platform::request::IRequestService& requestService,
	platform::request::EProxySupport proxySupport,
	std::wstring_view url,
	const platform::request::IRequestCancellation* cancellation)
{
	std::wstring normalized;
	if (!NormalizeStrictHttpsImageUrl(url, &normalized)) {
		return { ERemoteImageFetchOutcome::InvalidUrl };
	}
	platform::request::Request request;
	request.url = normalized;
	request.headers = {
		{ L"Accept", L"image/avif,image/webp,image/apng,image/svg+xml,image/*;q=0.8" },
		{ L"User-Agent", L"Sakura-Editor-NEXT-Markdown-Preview" },
	};
	request.allowRedirects = true;
	request.deduplicationKey = L"markdown-preview-image " + normalized;
	request.proxySupport = proxySupport;
	request.limits.timeout = kRequestTimeout;
	request.limits.maxRedirects = kMaximumRedirects;
	request.limits.maxResponseHeaderBytes = 32U * 1024U;
	request.limits.maxResponseBodyBytes = kMaximumRemoteImageBytes;
	const auto fetched = requestService.Execute(request, cancellation);
	if (!fetched) {
		return { fetched.outcome == platform::request::ERequestOutcome::Cancelled
			? ERemoteImageFetchOutcome::Cancelled : ERemoteImageFetchOutcome::RequestFailed };
	}
	if (!fetched.response) return { ERemoteImageFetchOutcome::RequestFailed };
	const auto& response = *fetched.response;
	std::wstring finalUrl;
	if (response.statusCode != 200) {
		return { ERemoteImageFetchOutcome::HttpStatusRejected };
	}
	if (!NormalizeStrictHttpsImageUrl(response.finalUrl, &finalUrl)) {
		return { ERemoteImageFetchOutcome::InvalidUrl };
	}
	const auto rawMediaType = FindHeader(response.headers, L"Content-Type");
	const auto mediaType = rawMediaType ? NormalizeMediaType(*rawMediaType) : std::wstring{};
	if (!IsImageMediaType(mediaType)) {
		return { ERemoteImageFetchOutcome::UnsupportedMediaType };
	}
	if (response.body.empty()) return { ERemoteImageFetchOutcome::EmptyBody };
	auto bytes = std::make_shared<const std::vector<std::uint8_t>>(response.body);
	return { ERemoteImageFetchOutcome::Loaded, std::move(bytes), mediaType, std::move(finalUrl) };
}

std::shared_ptr<IMarkdownRemoteImageFetcher> CreateMarkdownRemoteImageFetcher(
	config::ConfigurationNetworkPolicySnapshot networkPolicy)
{
	// The WinHTTP transport always validates the server certificate. Refusing a
	// disabled proxy TLS check keeps the complete redirect/proxy path strict too.
	if (!networkPolicy.proxyStrictSSL) return {};
	try {
		return std::make_shared<MarkdownRemoteImageFetcher>(std::move(networkPolicy));
	}
	catch (...) {
		return {};
	}
}

} // namespace markdown
