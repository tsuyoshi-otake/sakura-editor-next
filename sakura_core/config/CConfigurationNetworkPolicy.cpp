/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/CConfigurationNetworkPolicy.h"

#include "platform/profiles/UserDataProfileIdentity.h"
#include <sakura/uri/UriIdentity.h>

#include <chrono>
#include <limits>
#include <utility>

namespace config {
namespace {

constexpr std::size_t kMaximumUrlLength = 2048;
const std::vector<std::string> kNetworkPolicyKeys {
	"http.proxy",
	"http.proxySupport",
	"http.noProxy",
	"http.proxyStrictSSL",
	"http.systemCertificates",
	"http.timeout",
};

ConfigurationNetworkPolicyResult Unsupported(std::string diagnostic)
{
	return { EConfigurationNetworkPolicyOutcome::Unsupported, std::nullopt, std::move(diagnostic) };
}

ConfigurationNetworkPolicyResult Invalid(std::string diagnostic)
{
	return { EConfigurationNetworkPolicyOutcome::InvalidConfiguration, std::nullopt, std::move(diagnostic) };
}

bool IsProxyUrl(const std::wstring& value, std::wstring& normalized)
{
	if (value.empty() || value.size() > kMaximumUrlLength) {
		return false;
	}
	const auto parsed = platform::uri::Uri::Parse(value);
	if (!parsed || !parsed.value->HasAuthority() || parsed.value->Authority().empty()
		|| parsed.value->Authority().find(L'@') != std::wstring::npos
		|| parsed.value->Query().has_value() || parsed.value->Fragment().has_value()) {
		return false;
	}
	const auto& scheme = parsed.value->Scheme();
	if (scheme != L"http" && scheme != L"https") {
		return false;
	}
	if (!parsed.value->Path().empty() && parsed.value->Path() != L"/") {
		return false;
	}
	normalized = parsed.value->ToString();
	return true;
}

template<typename TValue>
bool ReadExact(const ConfigurationValue& source, TValue& output)
{
	const auto value = std::get_if<TValue>(&source.Value());
	if (!value) {
		return false;
	}
	output = *value;
	return true;
}

} // namespace

CConfigurationNetworkPolicy::CConfigurationNetworkPolicy(
	IConfigurationService& configurationService,
	std::wstring profileId
)
	: m_configurationService(configurationService)
	, m_target { std::move(profileId), std::nullopt, std::nullopt, std::nullopt }
{
}

ConfigurationNetworkPolicyResult CConfigurationNetworkPolicy::Snapshot() const
{
	// This target is the selected user-data profile handed down from bootstrap
	// (e.g. the Default profile's fixed literal id, mirroring VS Code's
	// '__default__profile__'), never the control authority, so its identity
	// space is deliberately opaque rather than canonical-hex.  Validate it as
	// such before any read reaches configuration.
	if (!platform::profiles::IsOpaqueUserDataProfileId(m_target.profileId)) {
		return Unsupported("network policy requires a valid user-data profile target");
	}

	const auto read = m_configurationService.ReadSnapshot(kNetworkPolicyKeys, m_target);
	if (read.outcome == EConfigurationOutcome::InvalidKey) {
		return Unsupported("network policy descriptors are unavailable");
	}
	if (read.outcome != EConfigurationOutcome::Applied || !read.snapshot
		|| read.snapshot->values.size() != kNetworkPolicyKeys.size()) {
		return Invalid("network policy could not read one coherent settings snapshot");
	}
	const auto& values = read.snapshot->values;
	std::wstring proxy;
	std::wstring proxySupport;
	ConfigurationValue::Array noProxy;
	bool strictSsl = true;
	bool systemCertificates = true;
	std::int64_t timeoutMilliseconds = 0;
	if (!ReadExact(values[0], proxy)
		|| !ReadExact(values[1], proxySupport)
		|| !ReadExact(values[2], noProxy)
		|| !ReadExact(values[3], strictSsl)
		|| !ReadExact(values[4], systemCertificates)
		|| !ReadExact(values[5], timeoutMilliseconds)) {
		return Invalid("network policy has an invalid setting type or state");
	}

	ConfigurationNetworkPolicySnapshot snapshot;
	if (proxySupport == L"off") {
		snapshot.proxySupport = platform::request::EProxySupport::Off;
	} else if (proxySupport == L"on") {
		snapshot.proxySupport = platform::request::EProxySupport::On;
	} else if (proxySupport == L"fallback") {
		snapshot.proxySupport = platform::request::EProxySupport::Fallback;
	} else if (proxySupport == L"override") {
		snapshot.proxySupport = platform::request::EProxySupport::Override;
	} else {
		return Invalid("network policy has an unsupported proxy mode");
	}

	if (!proxy.empty()) {
		std::wstring normalizedProxy;
		if (!IsProxyUrl(proxy, normalizedProxy)) {
			return Invalid("network policy proxy URL is invalid");
		}
		snapshot.proxyUrl = std::move(normalizedProxy);
	}
	if (timeoutMilliseconds <= 0
		|| timeoutMilliseconds > static_cast<std::int64_t>(std::chrono::milliseconds::max().count())) {
		return Invalid("network policy timeout is outside the supported range");
	}
	for (const auto& item : noProxy) {
		const auto value = std::get_if<std::wstring>(&item.Value());
		if (!value || value->empty() || value->size() > 256) {
			return Invalid("network policy no-proxy list is invalid");
		}
		snapshot.noProxy.push_back(*value);
	}
	snapshot.proxyStrictSSL = strictSsl;
	snapshot.systemCertificates = systemCertificates;
	snapshot.requestLimits.timeout = std::chrono::milliseconds(timeoutMilliseconds);
	return { EConfigurationNetworkPolicyOutcome::Ready, std::move(snapshot), {} };
}

} // namespace config
