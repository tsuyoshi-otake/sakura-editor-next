/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "extension/openvsx/OpenVsxProductionClient.h"

#include "config/CConfigurationNetworkPolicy.h"
#include "config/CConfigurationProxyService.h"
#include "platform/profiles/UserDataProfileIdentity.h"
#include "platform/request/win32/WinHttpRequestRuntime.h"
#include "platform/request/win32/WinHttpSystemProxyResolver.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace extension::openvsx {
namespace {

constexpr std::size_t kMaximumResponseHeaderBytes = 64u * 1024u;
constexpr std::size_t kMaximumSearchResponseBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaximumSha256ResponseBytes = 64u * 1024u;
constexpr std::size_t kMaximumVsixResponseBytes = 512u * 1024u * 1024u;
constexpr std::size_t kMaximumRedirects = 3;
constexpr std::size_t kMaximumRetries = 2;

class NoCredentialService final : public platform::request::ICredentialService {
public:
	std::optional<platform::request::RequestCredential> GetCredential(
		const platform::request::CredentialRequest&,
		std::optional<std::chrono::steady_clock::time_point>,
		const platform::request::IRequestCancellation*
	) override
	{
		return std::nullopt;
	}
};

platform::request::RequestLimits MakeRequestLimits(
	const config::ConfigurationNetworkPolicySnapshot& snapshot,
	std::size_t maximumResponseBodyBytes
)
{
	return { snapshot.requestLimits.timeout, kMaximumRedirects, kMaximumResponseHeaderBytes, maximumResponseBodyBytes };
}

class OpenVsxProductionClient final : public IOpenVsxRegistryClient {
public:
	explicit OpenVsxProductionClient(config::ConfigurationNetworkPolicySnapshot snapshot)
		: m_proxyService(snapshot, m_systemProxyResolver)
		, m_requestService(
			m_transport,
			m_proxyService,
			m_credentialService,
			m_clock,
			m_scheduler,
			m_jitterSource,
			nullptr,
			{ kMaximumRedirects, kMaximumRetries, std::chrono::milliseconds(250), std::chrono::seconds(30) })
		, m_adapter(m_requestService, std::move(snapshot.openVsxRegistry), BuildOpenVsxProductionRequestPolicy(snapshot))
	{
	}

	OpenVsxSearchOperation Search(
		std::wstring_view query,
		int offset,
		int pageSize,
		const platform::request::IRequestCancellation* cancellation = nullptr
	) const override
	{
		return m_adapter.Search(query, offset, pageSize, cancellation);
	}

	OpenVsxBinaryOperation FetchVsix(
		std::wstring_view validatedHttpsVsixUri,
		const platform::request::IRequestCancellation* cancellation = nullptr
	) const override
	{
		return m_adapter.FetchVsix(validatedHttpsVsixUri, cancellation);
	}

	OpenVsxBinaryOperation FetchOptionalSha256(
		const std::optional<std::wstring>& validatedHttpsSha256Uri,
		const platform::request::IRequestCancellation* cancellation = nullptr
	) const override
	{
		return m_adapter.FetchOptionalSha256(validatedHttpsSha256Uri, cancellation);
	}

private:
	// Reverse destruction preserves every dependency while its consumer is torn down:
	// adapter → request service → time/credential/proxy services → WinHTTP boundaries.
	platform::request::win32::WinHttpRequestTransport m_transport;
	platform::request::win32::WinHttpSystemProxyResolver m_systemProxyResolver;
	config::CConfigurationProxyService m_proxyService;
	NoCredentialService m_credentialService;
	platform::request::win32::Win32RequestClock m_clock;
	platform::request::win32::Win32RequestScheduler m_scheduler;
	platform::request::win32::ThreadSafeRetryJitterSource m_jitterSource;
	platform::request::RequestService m_requestService;
	OpenVsxRequestServiceAdapter m_adapter;
};

OpenVsxProductionClientResult Failure(EOpenVsxProductionClientOutcome outcome, const char* diagnostic)
{
	return { outcome, {}, diagnostic };
}

} // namespace

OpenVsxRequestPolicy BuildOpenVsxProductionRequestPolicy(const config::ConfigurationNetworkPolicySnapshot& snapshot)
{
	OpenVsxRequestPolicy policy;
	policy.cachePolicy = platform::request::ERequestCachePolicy::OnlineOnly;
	policy.allowRedirects = true;
	policy.proxySupport = snapshot.proxySupport;
	policy.searchLimits = MakeRequestLimits(snapshot, kMaximumSearchResponseBytes);
	policy.vsixLimits = MakeRequestLimits(snapshot, kMaximumVsixResponseBytes);
	policy.sha256Limits = MakeRequestLimits(snapshot, kMaximumSha256ResponseBytes);
	return policy;
}

OpenVsxProductionClientResult CreateOpenVsxProductionClient(
	config::IConfigurationService& configurationService,
	std::wstring userDataProfileId
) noexcept
{
	// This is the selected user-data profile (e.g. Default's fixed literal id),
	// never the control authority, so its identity space is deliberately opaque
	// rather than canonical-hex.  Validate it as such before any network policy read.
	if (!platform::profiles::IsOpaqueUserDataProfileId(userDataProfileId)) {
		return Failure(EOpenVsxProductionClientOutcome::InvalidProfileId, "Open VSX requires a valid user-data profile identity");
	}

	try {
		config::CConfigurationNetworkPolicy networkPolicy(configurationService, std::move(userDataProfileId));
		const auto policy = networkPolicy.Snapshot();
		if (policy.outcome == config::EConfigurationNetworkPolicyOutcome::Unsupported) {
			return Failure(EOpenVsxProductionClientOutcome::ConfigurationUnavailable, "Open VSX network configuration is unavailable");
		}
		if (!policy.snapshot.has_value() || policy.outcome != config::EConfigurationNetworkPolicyOutcome::Ready) {
			return Failure(EOpenVsxProductionClientOutcome::ConfigurationInvalid, "Open VSX network configuration is invalid");
		}
		// The production WinHTTP transport deliberately provides no TLS-validation
		// escape hatch, so this compatibility setting cannot silently weaken TLS.
		if (!policy.snapshot->proxyStrictSSL) {
			return Failure(EOpenVsxProductionClientOutcome::ConfigurationInvalid, "Open VSX network configuration is unsupported");
		}

		OpenVsxProductionClientResult result;
		result.outcome = EOpenVsxProductionClientOutcome::Ready;
		result.client = std::make_shared<OpenVsxProductionClient>(std::move(*policy.snapshot));
		result.diagnostic = "Open VSX client is ready";
		return result;
	}
	catch (...) {
		return Failure(EOpenVsxProductionClientOutcome::InternalFailure, "Open VSX client initialization failed");
	}
}

} // namespace extension::openvsx
