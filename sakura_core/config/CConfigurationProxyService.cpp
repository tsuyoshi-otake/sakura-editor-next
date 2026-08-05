/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/CConfigurationProxyService.h"

#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <string_view>

namespace config {
namespace {

using platform::request::EProxyMode;
using platform::request::EProxySelectionOutcome;
using platform::request::EProxySupport;
using platform::request::ProxySelection;

constexpr std::size_t kMaximumHostLength = 253;

ProxySelection Unsupported() noexcept
{
	return { EProxyMode::System, std::nullopt, false, EProxySelectionOutcome::UnsupportedPolicy };
}

ProxySelection Direct(bool bypassed = false) noexcept
{
	return { EProxyMode::Direct, std::nullopt, bypassed, EProxySelectionOutcome::Selected };
}

ProxySelection Manual(const std::wstring& proxyUrl)
{
	return { EProxyMode::Manual, proxyUrl, false, EProxySelectionOutcome::Selected };
}

bool IsCancelled(const platform::request::IRequestCancellation* cancellation) noexcept
{
	return cancellation != nullptr && cancellation->IsCancellationRequested();
}

bool IsDeadlineExceeded(std::optional<std::chrono::steady_clock::time_point> deadline) noexcept
{
	return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

wchar_t Lower(wchar_t value) noexcept
{
	return value <= 0x7f ? static_cast<wchar_t>(std::towlower(value)) : static_cast<wchar_t>(std::towlower(value));
}

std::wstring Lowered(std::wstring_view value)
{
	std::wstring result;
	result.reserve(value.size());
	for (const auto character : value) {
		result.push_back(Lower(character));
	}
	return result;
}

bool IsAsciiDigit(wchar_t value) noexcept
{
	return value >= L'0' && value <= L'9';
}

bool ParsePort(std::wstring_view value, std::optional<std::uint16_t>& port) noexcept
{
	if (value.empty() || value.size() > 5) {
		return false;
	}
	unsigned long numeric = 0;
	for (const auto character : value) {
		if (!IsAsciiDigit(character)) {
			return false;
		}
		numeric = numeric * 10 + static_cast<unsigned long>(character - L'0');
		if (numeric > 65535) {
			return false;
		}
	}
	if (numeric == 0) {
		return false;
	}
	port = static_cast<std::uint16_t>(numeric);
	return true;
}

bool IsIpv4(std::wstring_view value) noexcept
{
	unsigned int octets = 0;
	std::size_t start = 0;
	for (std::size_t index = 0; index <= value.size(); ++index) {
		if (index != value.size() && value[index] != L'.') {
			continue;
		}
		if (index == start || ++octets > 4) {
			return false;
		}
		unsigned int numeric = 0;
		for (std::size_t character = start; character < index; ++character) {
			if (!IsAsciiDigit(value[character])) {
				return false;
			}
			numeric = numeric * 10 + static_cast<unsigned int>(value[character] - L'0');
			if (numeric > 255) {
				return false;
			}
		}
		start = index + 1;
	}
	return octets == 4;
}

bool IsIpv6(std::wstring_view value) noexcept
{
	// A deliberately conservative syntactic check. IPv6 literals must be bracketed
	// in URIs/no-proxy rules, and this rejects paths, zones, and malformed runs.
	if (value.empty() || value.size() > 45 || value.find(L":::") != std::wstring_view::npos) {
		return false;
	}
	std::size_t colonCount = 0;
	for (const auto character : value) {
		const bool hexadecimal = (character >= L'0' && character <= L'9')
			|| (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F');
		if (character == L':') {
			++colonCount;
		} else if (!hexadecimal && character != L'.') {
			return false;
		}
	}
	return colonCount >= 2;
}

bool IsDomain(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumHostLength || value.front() == L'.' || value.back() == L'.') {
		return false;
	}
	bool labelStart = true;
	for (const auto character : value) {
		const bool alphaNumeric = (character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z') || IsAsciiDigit(character);
		if (character == L'.') {
			if (labelStart) {
				return false;
			}
			labelStart = true;
			continue;
		}
		if (!alphaNumeric && character != L'-') {
			return false;
		}
		if (labelStart && character == L'-') {
			return false;
		}
		labelStart = false;
	}
	return !labelStart;
}

struct Endpoint final {
	std::wstring host;
	std::uint16_t port = 0;
	bool ipv6 = false;
};

bool ParseAuthority(std::wstring_view authority, std::wstring_view scheme, Endpoint& endpoint) noexcept
{
	if (authority.empty() || authority.size() > kMaximumHostLength + 8 || authority.find(L'@') != std::wstring_view::npos) {
		return false;
	}
	std::wstring_view host;
	std::optional<std::uint16_t> port;
	if (authority.front() == L'[') {
		const auto close = authority.find(L']');
		if (close == std::wstring_view::npos || close == 1) {
			return false;
		}
		host = authority.substr(1, close - 1);
		if (!IsIpv6(host)) {
			return false;
		}
		const auto tail = authority.substr(close + 1);
		if (!tail.empty() && (tail.front() != L':' || !ParsePort(tail.substr(1), port))) {
			return false;
		}
		endpoint.ipv6 = true;
	} else {
		const auto colon = authority.find(L':');
		if (colon != std::wstring_view::npos) {
			if (authority.find(L':', colon + 1) != std::wstring_view::npos || !ParsePort(authority.substr(colon + 1), port)) {
				return false;
			}
			host = authority.substr(0, colon);
		} else {
			host = authority;
		}
		if (!(IsIpv4(host) || IsDomain(host))) {
			return false;
		}
	}
	if (host.empty()) {
		return false;
	}
	endpoint.host = Lowered(host);
	endpoint.port = port.value_or(scheme == L"https" ? 443 : scheme == L"http" ? 80 : 0);
	return endpoint.port != 0;
}

bool ParseTarget(std::wstring_view value, Endpoint& endpoint)
{
	const auto parsed = platform::uri::Uri::Parse(value);
	if (!parsed || !parsed.value->HasAuthority() || (parsed.value->Scheme() != L"http" && parsed.value->Scheme() != L"https")) {
		return false;
	}
	return ParseAuthority(parsed.value->Authority(), parsed.value->Scheme(), endpoint);
}

enum class ENoProxyKind : std::uint8_t { Exact, DomainSuffix, WildcardSuffix, Global };

struct NoProxyPattern final {
	ENoProxyKind kind = ENoProxyKind::Exact;
	std::wstring host;
	std::optional<std::uint16_t> port;
	bool ipv6 = false;
};

bool ParseNoProxyPattern(std::wstring_view value, NoProxyPattern& pattern) noexcept
{
	if (value.empty() || value.size() > 256 || value.find_first_of(L"/\\?#@ \t\r\n") != std::wstring_view::npos) {
		return false;
	}
	if (value == L"*") {
		pattern.kind = ENoProxyKind::Global;
		return true;
	}
	std::wstring_view host = value;
	if (value.front() == L'[') {
		const auto close = value.find(L']');
		if (close == std::wstring_view::npos || close == 1 || !IsIpv6(value.substr(1, close - 1))) {
			return false;
		}
		const auto tail = value.substr(close + 1);
		if (!tail.empty() && (tail.front() != L':' || !ParsePort(tail.substr(1), pattern.port))) {
			return false;
		}
		pattern.host = Lowered(value.substr(1, close - 1));
		pattern.ipv6 = true;
		return true;
	}
	const auto colon = value.find(L':');
	if (colon != std::wstring_view::npos) {
		if (value.find(L':', colon + 1) != std::wstring_view::npos || !ParsePort(value.substr(colon + 1), pattern.port)) {
			return false;
		}
		host = value.substr(0, colon);
	}
	if (host.starts_with(L"*.")) {
		pattern.kind = ENoProxyKind::WildcardSuffix;
		host.remove_prefix(2);
	} else if (host.starts_with(L".")) {
		pattern.kind = ENoProxyKind::DomainSuffix;
		host.remove_prefix(1);
	}
	if (!(IsIpv4(host) || IsDomain(host)) || host == L"*") {
		return false;
	}
	pattern.host = Lowered(host);
	return true;
}

bool EndsWithDomain(std::wstring_view host, std::wstring_view suffix) noexcept
{
	return host.size() > suffix.size() && host.ends_with(suffix) && host[host.size() - suffix.size() - 1] == L'.';
}

bool Matches(const Endpoint& endpoint, const NoProxyPattern& pattern) noexcept
{
	if (pattern.kind == ENoProxyKind::Global) {
		return true;
	}
	if (pattern.port.has_value() && *pattern.port != endpoint.port) {
		return false;
	}
	if (endpoint.ipv6 != pattern.ipv6) {
		return false;
	}
	switch (pattern.kind) {
	case ENoProxyKind::Exact:
		return endpoint.host == pattern.host;
	case ENoProxyKind::DomainSuffix:
		return endpoint.host == pattern.host || EndsWithDomain(endpoint.host, pattern.host);
	case ENoProxyKind::WildcardSuffix:
		return EndsWithDomain(endpoint.host, pattern.host);
	case ENoProxyKind::Global:
		return true;
	}
	return false;
}

//! Validates any system-supplied selection, including a legitimate Direct one:
//! WinHTTP reports Direct(bypassed=true) when the target matched the system's
//! own bypass list, and that is a valid "connect directly" answer, not a
//! contradiction. A Manual selection that also claims to be bypassed remains
//! contradictory and is rejected, so the bypassed check runs only once the
//! Direct case has already returned.
bool IsValidSystemProxySelection(const ProxySelection& selection) noexcept
{
	if (selection.outcome != EProxySelectionOutcome::Selected) {
		return false;
	}
	if (selection.mode == EProxyMode::Direct) {
		return !selection.proxyUrl.has_value();
	}
	if (selection.bypassed) {
		return false;
	}
	if (selection.mode != EProxyMode::Manual || !selection.proxyUrl.has_value()) {
		return false;
	}
	const auto parsed = platform::uri::Uri::Parse(*selection.proxyUrl);
	if (!parsed || !parsed.value->HasAuthority() || parsed.value->Query().has_value() || parsed.value->Fragment().has_value()
		|| (!parsed.value->Path().empty() && parsed.value->Path() != L"/")) {
		return false;
	}
	Endpoint endpoint;
	return ParseTarget(*selection.proxyUrl, endpoint);
}

} // namespace

CConfigurationProxyService::CConfigurationProxyService(
	const CConfigurationNetworkPolicy& networkPolicy,
	ISystemProxyResolver& systemResolver
)
	: m_networkPolicy(&networkPolicy)
	, m_systemResolver(systemResolver)
{
}

CConfigurationProxyService::CConfigurationProxyService(
	ConfigurationNetworkPolicySnapshot networkPolicySnapshot,
	ISystemProxyResolver& systemResolver
)
	: m_networkPolicySnapshot(std::move(networkPolicySnapshot))
	, m_systemResolver(systemResolver)
{
}

ProxySelection CConfigurationProxyService::SelectProxy(
	const platform::request::ProxyRequest& request,
	std::optional<std::chrono::steady_clock::time_point> deadline,
	const platform::request::IRequestCancellation* cancellation
)
{
	// An explicit Off is a hard no-lookup guarantee. It must remain usable even
	// when network-policy descriptors or a system resolver are unavailable.
	if (request.support == EProxySupport::Off) return Direct();

	// The dynamic form reads exactly one coherent configuration snapshot; the
	// detached form owns the already validated snapshot and performs no settings
	// read. Both forms share the exact selection path below.
	std::optional<ConfigurationNetworkPolicyResult> dynamicPolicy;
	const ConfigurationNetworkPolicySnapshot* policySnapshot = nullptr;
	if (m_networkPolicy != nullptr) {
		dynamicPolicy = m_networkPolicy->Snapshot();
		if (!*dynamicPolicy || !dynamicPolicy->snapshot.has_value()) {
			return Unsupported();
		}
		policySnapshot = &*dynamicPolicy->snapshot;
	} else if (m_networkPolicySnapshot.has_value()) {
		policySnapshot = &*m_networkPolicySnapshot;
	}
	if (IsCancelled(cancellation) || IsDeadlineExceeded(deadline) || policySnapshot == nullptr) {
		return Unsupported();
	}
	// Fallback is the request contract's ambient/default mode, so it inherits the
	// profile preference. Explicit caller modes remain explicit (notably Off is a
	// hard no-lookup guarantee) and therefore cannot be weakened by Settings.
	const auto support = request.support == EProxySupport::Fallback
		? policySnapshot->proxySupport
		: request.support;
	if (support == EProxySupport::Off) {
		return Direct();
	}

	Endpoint target;
	if (!ParseTarget(request.targetUrl, target)) {
		return Unsupported();
	}
	bool bypassed = false;
	for (const auto& rawPattern : policySnapshot->noProxy) {
		NoProxyPattern pattern;
		if (!ParseNoProxyPattern(rawPattern, pattern)) {
			return Unsupported();
		}
		if (Matches(target, pattern)) {
			bypassed = true;
		}
	}
	if (bypassed) {
		return Direct(true);
	}
	if (IsCancelled(cancellation) || IsDeadlineExceeded(deadline)) {
		return Unsupported();
	}

	switch (support) {
	case EProxySupport::Off:
		return Direct();
	case EProxySupport::On:
	case EProxySupport::Override:
		if (policySnapshot->proxyUrl.has_value()) {
			return Manual(*policySnapshot->proxyUrl);
		}
		break;
	case EProxySupport::Fallback:
		break;
	}

	if (support == EProxySupport::Fallback && policySnapshot->proxyUrl.has_value()) {
		// Fallback has system precedence; the manual URL is only used below when
		// the system resolver cannot select a proxy (Unavailable) or
		// authoritatively selects none (NoProxyRequired).
	} else if (support == EProxySupport::On || support == EProxySupport::Override) {
		// No configured proxy: a system resolver is required.
	}

	const auto system = m_systemResolver.Resolve(request, deadline, cancellation);
	if (IsCancelled(cancellation) || IsDeadlineExceeded(deadline)) {
		return Unsupported();
	}
	if (system.outcome == ESystemProxyResolutionOutcome::Selected) {
		if (!IsValidSystemProxySelection(system.selection)) {
			return Unsupported();
		}
		return system.selection;
	}
	if (system.outcome == ESystemProxyResolutionOutcome::NoProxyRequired) {
		// The system authoritatively reports that no proxy applies to this
		// target. VS Code's Electron `session.resolveProxy` reports this same
		// fact as the literal DIRECT and simply connects; it has no failure
		// state for "no proxy is configured". A direct connection is
		// therefore the normal outcome here too, not an unsupported policy.
		// A user-configured http.proxy still takes precedence under
		// Fallback; On/Override already returned Manual above when a proxy
		// was configured, so reaching this line under those modes means none
		// was, and Direct() is correct for them as well.
		if (support == EProxySupport::Fallback && policySnapshot->proxyUrl.has_value()) {
			return Manual(*policySnapshot->proxyUrl);
		}
		return Direct();
	}
	if (system.outcome == ESystemProxyResolutionOutcome::Unavailable
		&& support == EProxySupport::Fallback && policySnapshot->proxyUrl.has_value()) {
		// A genuine inability to resolve stays fail-closed except for this one
		// configured-fallback escape hatch; every other case below falls
		// through to Unsupported().
		return Manual(*policySnapshot->proxyUrl);
	}
	return Unsupported();
}

} // namespace config
