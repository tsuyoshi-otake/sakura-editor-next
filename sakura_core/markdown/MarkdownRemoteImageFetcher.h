/*! @file
 * @brief Strict HTTPS image loading boundary for the native Markdown preview.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/CConfigurationNetworkPolicy.h"

#include <sakura/request/RequestService.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace markdown {

enum class ERemoteImageFetchOutcome : std::uint8_t {
	Loaded,
	InvalidUrl,
	Cancelled,
	RequestFailed,
	HttpStatusRejected,
	UnsupportedMediaType,
	EmptyBody,
};

struct RemoteImageFetchResult final {
	ERemoteImageFetchOutcome outcome = ERemoteImageFetchOutcome::RequestFailed;
	std::shared_ptr<const std::vector<std::uint8_t>> bytes;
	std::wstring mediaType;
	std::wstring finalUrl;

	explicit operator bool() const noexcept
	{
		return outcome == ERemoteImageFetchOutcome::Loaded && bytes && !bytes->empty();
	}
};

class IMarkdownRemoteImageFetcher {
public:
	virtual ~IMarkdownRemoteImageFetcher() = default;
	virtual RemoteImageFetchResult Fetch(
		std::wstring_view url,
		const platform::request::IRequestCancellation* cancellation) = 0;
};

//! Canonicalizes one URL for the Strict preview policy. Only absolute HTTPS
//! URLs without user-info are admitted, and fragments are removed before I/O.
[[nodiscard]] bool NormalizeStrictHttpsImageUrl(
	std::wstring_view url, std::wstring* normalizedUrl);

//! Executes one bounded, anonymous request. Exposed so tests can provide a
//! fake IRequestService without opening a network connection.
[[nodiscard]] RemoteImageFetchResult FetchStrictHttpsImage(
	platform::request::IRequestService& requestService,
	platform::request::EProxySupport proxySupport,
	std::wstring_view url,
	const platform::request::IRequestCancellation* cancellation = nullptr);

//! Owns the complete detached WinHTTP/request-service stack and a bounded
//! successful-response cache. A non-strict TLS policy fails closed.
[[nodiscard]] std::shared_ptr<IMarkdownRemoteImageFetcher>
	CreateMarkdownRemoteImageFetcher(
		config::ConfigurationNetworkPolicySnapshot networkPolicy);

} // namespace markdown
