/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/request/RequestService.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <limits>
#include <utility>

namespace platform::request {
namespace {

std::wstring ToLowerAscii(std::wstring_view text)
{
	std::wstring result;
	result.reserve(text.size());
	for (const wchar_t character : text) {
		result.push_back(character >= L'A' && character <= L'Z' ? character - L'A' + L'a' : character);
	}
	return result;
}

bool HeaderNameEquals(std::wstring_view name, std::wstring_view expected)
{
	return ToLowerAscii(name) == ToLowerAscii(expected);
}

std::optional<std::wstring_view> FindHeader(const std::vector<HttpHeader>& headers, std::wstring_view name)
{
	for (const auto& header : headers) {
		if (HeaderNameEquals(header.name, name)) {
			return header.value;
		}
	}
	return std::nullopt;
}

bool IsIdempotent(std::wstring_view method)
{
	const auto normalized = ToLowerAscii(method);
	return normalized == L"get" || normalized == L"head" || normalized == L"put" || normalized == L"delete" ||
		normalized == L"options" || normalized == L"trace";
}

bool IsRedirect(int statusCode) noexcept
{
	return statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308;
}

bool StartsWithIgnoreCase(std::wstring_view value, std::wstring_view prefix)
{
	return value.size() >= prefix.size() && ToLowerAscii(value.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

struct UrlParts {
	std::wstring scheme;
	std::wstring authority;
	std::wstring pathAndQuery;
};

bool IsAsciiControlOrSpace(wchar_t character) noexcept
{
	return character <= 0x20 || character == 0x7f;
}

bool IsValidPort(std::wstring_view port) noexcept
{
	if (port.empty()) return false;
	unsigned value = 0;
	for (const auto character : port) {
		if (character < L'0' || character > L'9') return false;
		value = value * 10 + static_cast<unsigned>(character - L'0');
		if (value > 65535) return false;
	}
	return value != 0;
}

bool IsValidAuthority(std::wstring_view authority) noexcept
{
	if (authority.empty() || authority.find(L'@') != std::wstring_view::npos) return false;
	for (const auto character : authority) {
		if (IsAsciiControlOrSpace(character) || character == L'\\') return false;
	}
	if (authority.front() == L'[') {
		const auto closingBracket = authority.find(L']');
		if (closingBracket == std::wstring_view::npos || closingBracket == 1) return false;
		const auto suffix = authority.substr(closingBracket + 1);
		return suffix.empty() || (suffix.front() == L':' && IsValidPort(suffix.substr(1)));
	}
	const auto firstColon = authority.find(L':');
	if (firstColon == std::wstring_view::npos) return true;
	if (firstColon == 0 || authority.find(L':', firstColon + 1) != std::wstring_view::npos) return false;
	return IsValidPort(authority.substr(firstColon + 1));
}

std::optional<UrlParts> ParseAbsoluteHttpUrl(std::wstring_view url)
{
	for (const auto character : url) {
		if (IsAsciiControlOrSpace(character) || character == L'\\') return std::nullopt;
	}
	const auto schemeEnd = url.find(L"://");
	if (schemeEnd == std::wstring_view::npos || schemeEnd == 0) {
		return std::nullopt;
	}
	UrlParts parts;
	parts.scheme.assign(url.substr(0, schemeEnd));
	if (!StartsWithIgnoreCase(parts.scheme, L"http") ||
		(ToLowerAscii(parts.scheme) != L"http" && ToLowerAscii(parts.scheme) != L"https")) {
		return std::nullopt;
	}
	const auto authorityBegin = schemeEnd + 3;
	const auto pathBegin = url.find_first_of(L"/?#", authorityBegin);
	parts.authority.assign(url.substr(authorityBegin, pathBegin == std::wstring_view::npos ? url.size() - authorityBegin : pathBegin - authorityBegin));
	if (!IsValidAuthority(parts.authority)) {
		return std::nullopt;
	}
	parts.pathAndQuery = pathBegin == std::wstring_view::npos ? L"/" : std::wstring(url.substr(pathBegin));
	const auto fragment = parts.pathAndQuery.find(L'#');
	if (fragment != std::wstring::npos) {
		parts.pathAndQuery.erase(fragment);
	}
	if (parts.pathAndQuery.empty()) {
		parts.pathAndQuery = L"/";
	} else if (parts.pathAndQuery.front() == L'?') {
		parts.pathAndQuery.insert(parts.pathAndQuery.begin(), L'/');
	}
	return parts;
}

std::wstring NormalizePathAndQuery(std::wstring_view pathAndQuery)
{
	const auto queryOffset = pathAndQuery.find(L'?');
	const auto path = pathAndQuery.substr(0, queryOffset);
	const auto query = queryOffset == std::wstring_view::npos ? std::wstring_view{} : pathAndQuery.substr(queryOffset);
	std::vector<std::wstring_view> segments;
	std::size_t begin = path.starts_with(L'/') ? 1 : 0;
	while (begin <= path.size()) {
		const auto end = path.find(L'/', begin);
		const auto segment = path.substr(begin, end == std::wstring_view::npos ? path.size() - begin : end - begin);
		if (segment == L"..") {
			if (!segments.empty()) segments.pop_back();
		} else if (!segment.empty() && segment != L".") {
			segments.push_back(segment);
		}
		if (end == std::wstring_view::npos) break;
		begin = end + 1;
	}
	std::wstring normalized = L"/";
	for (std::size_t index = 0; index < segments.size(); ++index) {
		if (index != 0) normalized.push_back(L'/');
		normalized.append(segments[index]);
	}
	if (path.size() > 1 && path.ends_with(L'/') && !normalized.ends_with(L'/')) normalized.push_back(L'/');
	normalized.append(query);
	return normalized;
}

std::optional<std::wstring> CanonicalizeAbsoluteHttpUrl(std::wstring_view url)
{
	const auto parts = ParseAbsoluteHttpUrl(url);
	if (!parts) return std::nullopt;
	return parts->scheme + L"://" + parts->authority + NormalizePathAndQuery(parts->pathAndQuery);
}

std::optional<std::wstring> ResolveRedirect(std::wstring_view baseUrl, std::wstring_view location)
{
	if (location.empty() || location.find_first_of(L"\r\n") != std::wstring_view::npos) {
		return std::nullopt;
	}
	const auto base = ParseAbsoluteHttpUrl(baseUrl);
	if (!base) {
		return std::nullopt;
	}
	if (location.find(L"://") != std::wstring_view::npos) {
		return CanonicalizeAbsoluteHttpUrl(location);
	}
	const std::wstring origin = base->scheme + L"://" + base->authority;
	if (location.starts_with(L"//")) {
		return CanonicalizeAbsoluteHttpUrl(base->scheme + L":" + std::wstring(location));
	}
	const auto fragment = location.find(L'#');
	location = location.substr(0, fragment);
	if (location.empty()) return CanonicalizeAbsoluteHttpUrl(baseUrl);
	if (location.starts_with(L"/")) {
		return origin + NormalizePathAndQuery(location);
	}
	if (location.starts_with(L"?")) {
		const auto query = base->pathAndQuery.find(L'?');
		return origin + NormalizePathAndQuery(base->pathAndQuery.substr(0, query) + std::wstring(location));
	}
	const auto query = base->pathAndQuery.find(L'?');
	const auto path = base->pathAndQuery.substr(0, query);
	const auto slash = path.find_last_of(L'/');
	return origin + NormalizePathAndQuery((slash == std::wstring::npos ? L"/" : path.substr(0, slash + 1)) + std::wstring(location));
}

std::wstring NormalizedOrigin(std::wstring_view url)
{
	const auto parts = ParseAbsoluteHttpUrl(url);
	return parts ? ToLowerAscii(parts->scheme) + L"://" + ToLowerAscii(parts->authority) : std::wstring{};
}

void RemoveHeaders(std::vector<HttpHeader>& headers, std::initializer_list<std::wstring_view> names)
{
	std::erase_if(headers, [names](const HttpHeader& header) {
		return std::any_of(names.begin(), names.end(), [&header](std::wstring_view name) {
			return HeaderNameEquals(header.name, name);
		});
	});
}

bool IsHttpTokenCharacter(wchar_t character) noexcept
{
	if ((character >= L'0' && character <= L'9') || (character >= L'A' && character <= L'Z') ||
		(character >= L'a' && character <= L'z')) return true;
	switch (character) {
	case L'!': case L'#': case L'$': case L'%': case L'&': case L'\'': case L'*': case L'+':
	case L'-': case L'.': case L'^': case L'_': case L'`': case L'|': case L'~': return true;
	default: return false;
	}
}

bool IsValidRequest(const Request& request)
{
	if (request.method.empty() || !std::all_of(request.method.begin(), request.method.end(), IsHttpTokenCharacter)) return false;
	if (!CanonicalizeAbsoluteHttpUrl(request.url)) return false;
	if (request.limits.timeout && *request.limits.timeout <= std::chrono::milliseconds::zero()) return false;
	if (request.limits.maxResponseHeaderBytes == 0 || request.limits.maxResponseBodyBytes == 0) return false;
	switch (request.proxySupport) {
	case EProxySupport::Off:
	case EProxySupport::On:
	case EProxySupport::Fallback:
	case EProxySupport::Override:
		break;
	default:
		return false;
	}
	return std::all_of(request.headers.begin(), request.headers.end(), [](const HttpHeader& header) {
		return !header.name.empty() &&
			std::all_of(header.name.begin(), header.name.end(), IsHttpTokenCharacter) &&
			header.value.find_first_of(L"\r\n") == std::wstring::npos;
	});
}

bool AreSameDeduplicatedRequest(const Request& left, const Request& right)
{
	if (left.method != right.method || left.url != right.url || left.body != right.body ||
		left.cachePolicy != right.cachePolicy || left.allowRedirects != right.allowRedirects ||
		left.limits.timeout != right.limits.timeout || left.limits.maxRedirects != right.limits.maxRedirects ||
		left.limits.maxResponseHeaderBytes != right.limits.maxResponseHeaderBytes ||
		left.limits.maxResponseBodyBytes != right.limits.maxResponseBodyBytes || left.proxySupport != right.proxySupport ||
		left.headers.size() != right.headers.size()) return false;
	for (std::size_t index = 0; index < left.headers.size(); ++index) {
		if (!HeaderNameEquals(left.headers[index].name, right.headers[index].name) ||
			left.headers[index].value != right.headers[index].value) return false;
	}
	return true;
}

std::optional<std::chrono::steady_clock::time_point> DeadlineFor(
	const RequestLimits& limits,
	std::chrono::steady_clock::time_point now
)
{
	if (!limits.timeout) return std::nullopt;
	const auto remaining = *limits.timeout;
	const auto maximum = std::chrono::steady_clock::time_point::max() - now;
	return remaining >= maximum ? std::chrono::steady_clock::time_point::max() : now + remaining;
}

bool HasTimedOut(
	const std::optional<std::chrono::steady_clock::time_point>& deadline,
	const IRequestClock& clock
)
{
	return deadline && clock.SteadyNow() >= *deadline;
}

std::size_t ResponseHeaderBytes(const std::vector<HttpHeader>& headers)
{
	std::size_t total = 0;
	for (const auto& header : headers) {
		const auto units = header.name.size() + header.value.size() + 4;
		if (units > (std::numeric_limits<std::size_t>::max() - total) / sizeof(wchar_t)) {
			return std::numeric_limits<std::size_t>::max();
		}
		total += units * sizeof(wchar_t);
	}
	return total;
}

ERequestOutcome OutcomeForTransportFailure(ETransportFailure failure) noexcept
{
	switch (failure) {
	case ETransportFailure::Timeout: return ERequestOutcome::Timeout;
	case ETransportFailure::TlsCertificateFailure: return ERequestOutcome::TlsCertificateFailure;
	case ETransportFailure::ResponseHeaderLimitExceeded: return ERequestOutcome::ResponseHeaderLimitExceeded;
	case ETransportFailure::ResponseBodyLimitExceeded: return ERequestOutcome::ResponseBodyLimitExceeded;
	case ETransportFailure::ProxyAuthenticationRequired: return ERequestOutcome::ProxyAuthenticationRequired;
	case ETransportFailure::UnsupportedProxyPolicy: return ERequestOutcome::UnsupportedProxyPolicy;
	default: return ERequestOutcome::TransportFailure;
	}
}

std::optional<AuthenticationChallenge> AuthenticationChallengeFor(
	const HttpResponse& response,
	ECredentialPurpose purpose
)
{
	const auto header = FindHeader(response.headers, purpose == ECredentialPurpose::Proxy ? L"Proxy-Authenticate" : L"WWW-Authenticate");
	if (!header || header->empty()) return std::nullopt;
	const auto schemeEnd = header->find_first_of(L" \t");
	const auto scheme = header->substr(0, schemeEnd);
	if (scheme.empty() || !std::all_of(scheme.begin(), scheme.end(), IsHttpTokenCharacter)) return std::nullopt;
	AuthenticationChallenge challenge;
	challenge.statusCode = response.statusCode;
	challenge.scheme.assign(scheme);
	const auto realmName = ToLowerAscii(*header).find(L"realm=\"");
	if (realmName != std::wstring::npos) {
		const auto realmStart = realmName + 7;
		const auto realmEnd = header->find(L'\"', realmStart);
		if (realmEnd != std::wstring::npos) {
			challenge.realm = std::wstring(header->substr(realmStart, realmEnd - realmStart));
		}
	}
	return challenge;
}

bool IsHttpsToHttpDowngrade(std::wstring_view source, std::wstring_view target)
{
	const auto from = ParseAbsoluteHttpUrl(source);
	const auto to = ParseAbsoluteHttpUrl(target);
	return from && to && ToLowerAscii(from->scheme) == L"https" && ToLowerAscii(to->scheme) == L"http";
}

std::optional<std::chrono::milliseconds> ParseDecimalSeconds(std::wstring_view value)
{
	if (value.empty()) {
		return std::nullopt;
	}
	std::uint64_t seconds = 0;
	for (const auto character : value) {
		if (character < L'0' || character > L'9') {
			return std::nullopt;
		}
		if (seconds > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) {
			return std::nullopt;
		}
		seconds = seconds * 10 + static_cast<std::uint64_t>(character - L'0');
	}
	constexpr auto maxMilliseconds = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	if (seconds > maxMilliseconds / 1000) {
		return std::chrono::milliseconds::max();
	}
	return std::chrono::milliseconds(seconds * 1000);
}

int MonthNumber(std::wstring_view month) noexcept
{
	constexpr std::array<std::wstring_view, 12> months = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
	for (std::size_t index = 0; index < months.size(); ++index) {
		if (ToLowerAscii(month) == ToLowerAscii(months[index])) {
			return static_cast<int>(index + 1);
		}
	}
	return 0;
}

// Howard Hinnant's civil-date conversion, kept local so HTTP date parsing needs no CRT time zone state.
std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept
{
	year -= month <= 2;
	const auto era = (year >= 0 ? year : year - 399) / 400;
	const auto yearOfEra = static_cast<unsigned>(year - era * 400);
	const auto dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

std::optional<std::chrono::system_clock::time_point> ParseHttpDate(std::wstring_view value)
{
	// IMF-fixdate only: Wed, 21 Oct 2015 07:28:00 GMT. Other HTTP-date forms are obsolete.
	if (value.size() != 29 || value[3] != L',' || value[4] != L' ' || value[7] != L' ' || value[11] != L' ' ||
		value[16] != L' ' || value[19] != L':' || value[22] != L':' || value.substr(25) != L" GMT") {
		return std::nullopt;
	}
	auto twoDigits = [&value](std::size_t offset) -> std::optional<int> {
		if (value[offset] < L'0' || value[offset] > L'9' || value[offset + 1] < L'0' || value[offset + 1] > L'9') return std::nullopt;
		return (value[offset] - L'0') * 10 + value[offset + 1] - L'0';
	};
	const auto day = twoDigits(5);
	const auto hour = twoDigits(17);
	const auto minute = twoDigits(20);
	const auto second = twoDigits(23);
	if (!day || !hour || !minute || !second || *day == 0 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;
	int year = 0;
	for (std::size_t index = 12; index < 16; ++index) {
		if (value[index] < L'0' || value[index] > L'9') return std::nullopt;
		year = year * 10 + value[index] - L'0';
	}
	const int month = MonthNumber(value.substr(8, 3));
	if (month == 0) return std::nullopt;
	const auto seconds = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(*day)) * 86400 + *hour * 3600 + *minute * 60 + *second;
	return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

std::optional<std::chrono::milliseconds> RetryAfter(const HttpResponse& response, const IRequestClock& clock)
{
	const auto value = FindHeader(response.headers, L"Retry-After");
	if (!value) return std::nullopt;
	if (const auto seconds = ParseDecimalSeconds(*value)) return seconds;
	const auto date = ParseHttpDate(*value);
	if (!date) return std::nullopt;
	const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(*date - clock.Now());
	return delta > std::chrono::milliseconds::zero() ? delta : std::chrono::milliseconds::zero();
}

std::chrono::milliseconds BoundedBackoff(const RequestServiceOptions& options, std::size_t retryIndex, IRetryJitterSource& jitter)
{
	const auto maximum = std::max(options.maxRetryDelay, std::chrono::milliseconds(1));
	const auto initial = std::clamp(options.initialRetryDelay, std::chrono::milliseconds(1), maximum);
	std::int64_t value = initial.count();
	for (std::size_t index = 0; index < retryIndex && value < maximum.count(); ++index) {
		value = std::min<std::int64_t>(value > maximum.count() / 2 ? maximum.count() : value * 2, maximum.count());
	}
	const double unit = std::clamp(jitter.NextUnitInterval(), 0.0, 1.0);
	const auto jittered = static_cast<std::int64_t>(std::ceil(static_cast<double>(value) * (0.5 + unit * 0.5)));
	return std::chrono::milliseconds(std::clamp<std::int64_t>(jittered, 1, maximum.count()));
}

} // namespace

struct RequestService::InFlightRequest {
	explicit InFlightRequest(const Request& requestIdentity) : identity(requestIdentity) {}
	Request identity;
	std::condition_variable completed;
	bool isComplete = false;
	RequestResult result;
};

RequestService::RequestService(
	IRequestTransport& transport,
	IProxyService& proxyService,
	ICredentialService& credentialService,
	IRequestClock& clock,
	IRequestScheduler& scheduler,
	IRetryJitterSource& jitterSource,
	IResponseCache* responseCache,
	RequestServiceOptions options
) :
	m_transport(transport),
	m_proxyService(proxyService),
	m_credentialService(credentialService),
	m_clock(clock),
	m_scheduler(scheduler),
	m_jitterSource(jitterSource),
	m_responseCache(responseCache),
	m_options(options)
{
	m_options.initialRetryDelay = std::max(m_options.initialRetryDelay, std::chrono::milliseconds(1));
	m_options.maxRetryDelay = std::max(m_options.maxRetryDelay, std::chrono::milliseconds(1));
}

RequestResult RequestService::Execute(const Request& request, const IRequestCancellation* cancellation)
{
	if (!IsValidRequest(request)) return { ERequestOutcome::InvalidRequest };
	if (!request.deduplicationKey || request.deduplicationKey->empty() || cancellation || !IsIdempotent(request.method)) {
		try {
			return ExecuteUnshared(request, cancellation);
		} catch (...) {
			return { ERequestOutcome::TransportFailure, ETransportFailure::Network };
		}
	}

	std::shared_ptr<InFlightRequest> inFlight;
	bool ownsExecution = false;
	{
		std::unique_lock lock(m_inFlightMutex);
		const auto existing = m_inFlight.find(*request.deduplicationKey);
		if (existing != m_inFlight.end()) {
			inFlight = existing->second;
			if (!AreSameDeduplicatedRequest(request, inFlight->identity)) {
				return { ERequestOutcome::InvalidRequest };
			}
			inFlight->completed.wait(lock, [&inFlight] { return inFlight->isComplete; });
			return inFlight->result;
		}
		inFlight = std::make_shared<InFlightRequest>(request);
		m_inFlight.emplace(*request.deduplicationKey, inFlight);
		ownsExecution = true;
	}

	RequestResult result;
	try {
		result = ExecuteUnshared(request, nullptr);
	} catch (...) {
		// Adapter exceptions are intentionally contained at this boundary so every waiter
		// receives one terminal result and cannot remain in-flight indefinitely.
		result = { ERequestOutcome::TransportFailure, ETransportFailure::Network };
	}
	if (ownsExecution) {
		std::lock_guard lock(m_inFlightMutex);
		inFlight->result = result;
		inFlight->isComplete = true;
		m_inFlight.erase(*request.deduplicationKey);
		inFlight->completed.notify_all();
	}
	return result;
}

RequestResult RequestService::ExecuteUnshared(const Request& request, const IRequestCancellation* cancellation)
{
	if (cancellation && cancellation->IsCancellationRequested()) {
		return { ERequestOutcome::Cancelled };
	}
	if (request.url.empty() || !ParseAbsoluteHttpUrl(request.url)) {
		return { ERequestOutcome::InvalidRequest };
	}

	const auto canonicalUrl = CanonicalizeAbsoluteHttpUrl(request.url);
	if (!canonicalUrl) return { ERequestOutcome::InvalidRequest };
	const auto cacheKey = request.method + L" " + *canonicalUrl;
	if (request.cachePolicy != ERequestCachePolicy::OnlineOnly && m_responseCache) {
		if (auto cached = m_responseCache->Get(cacheKey)) {
			cached->finalUrl = cached->finalUrl.empty() ? *canonicalUrl : cached->finalUrl;
			return { ERequestOutcome::Success, ETransportFailure::None, std::move(cached), true };
		}
	}
	if (request.cachePolicy == ERequestCachePolicy::OfflineOnly) {
		return { ERequestOutcome::OfflineCacheMiss };
	}

	Request current = request;
	current.url = *canonicalUrl;
	std::size_t redirects = 0;
	std::size_t retries = 0;
	const auto deadline = DeadlineFor(request.limits, m_clock.SteadyNow());
	std::optional<RequestCredential> serverCredential;
	std::optional<RequestCredential> proxyCredential;
	std::optional<std::wstring> credentialProxyUrl;
	bool attemptedServerAuthentication = false;
	bool attemptedProxyAuthentication = false;
	for (;;) {
		if (cancellation && cancellation->IsCancellationRequested()) {
			return { ERequestOutcome::Cancelled, ETransportFailure::None, std::nullopt, false, redirects, retries };
		}
		if (HasTimedOut(deadline, m_clock)) {
			return { ERequestOutcome::Timeout, ETransportFailure::Timeout, std::nullopt, false, redirects, retries };
		}

		ProxySelection proxy;
		if (current.proxySupport == EProxySupport::Off) {
			proxy = { EProxyMode::Direct, std::nullopt, true };
		} else {
			proxy = m_proxyService.SelectProxy({ current.url, current.proxySupport }, deadline, cancellation);
			if (cancellation && cancellation->IsCancellationRequested()) {
				return { ERequestOutcome::Cancelled, ETransportFailure::None, std::nullopt, false, redirects, retries };
			}
			if (HasTimedOut(deadline, m_clock)) {
				return { ERequestOutcome::Timeout, ETransportFailure::Timeout, std::nullopt, false, redirects, retries };
			}
			if (proxy.outcome != EProxySelectionOutcome::Selected) {
				return { ERequestOutcome::UnsupportedProxyPolicy, ETransportFailure::UnsupportedProxyPolicy, std::nullopt, false, redirects, retries };
			}
		}
		// A proxy resolver may return a different endpoint after a retry or a
		// network-policy change. Never forward credentials challenged by one proxy
		// to another proxy (or to a direct connection).
		if (proxyCredential && credentialProxyUrl != proxy.proxyUrl) {
			proxyCredential.reset();
			credentialProxyUrl.reset();
			attemptedProxyAuthentication = false;
		}
		TransportRequest transportRequest;
		transportRequest.method = current.method;
		transportRequest.url = current.url;
		transportRequest.headers = current.headers;
		transportRequest.body = current.body;
		transportRequest.proxy = proxy;
		transportRequest.serverCredential = serverCredential;
		transportRequest.proxyCredential = proxyCredential;
		transportRequest.limits = current.limits;
		transportRequest.deadline = deadline;
		auto transportResult = m_transport.Send(transportRequest, cancellation);
		if (cancellation && cancellation->IsCancellationRequested()) {
			return { ERequestOutcome::Cancelled, ETransportFailure::None, std::nullopt, false, redirects, retries };
		}
		if (HasTimedOut(deadline, m_clock)) {
			return { ERequestOutcome::Timeout, ETransportFailure::Timeout, std::nullopt, false, redirects, retries };
		}
		if (!transportResult.response) {
			return { OutcomeForTransportFailure(transportResult.failure), transportResult.failure, std::nullopt, false, redirects, retries };
		}

		auto response = std::move(*transportResult.response);
		response.finalUrl = current.url;
		if (ResponseHeaderBytes(response.headers) > current.limits.maxResponseHeaderBytes) {
			return { ERequestOutcome::ResponseHeaderLimitExceeded, ETransportFailure::ResponseHeaderLimitExceeded, std::move(response), false, redirects, retries };
		}
		if (response.body.size() > current.limits.maxResponseBodyBytes) {
			return { ERequestOutcome::ResponseBodyLimitExceeded, ETransportFailure::ResponseBodyLimitExceeded, std::move(response), false, redirects, retries };
		}

		if (response.statusCode == 401 || response.statusCode == 407) {
			const auto purpose = response.statusCode == 407 ? ECredentialPurpose::Proxy : ECredentialPurpose::Server;
			auto& attempted = purpose == ECredentialPurpose::Proxy ? attemptedProxyAuthentication : attemptedServerAuthentication;
			auto& credential = purpose == ECredentialPurpose::Proxy ? proxyCredential : serverCredential;
			const auto challenge = AuthenticationChallengeFor(response, purpose);
			if (!attempted && challenge) {
				attempted = true;
				credential = m_credentialService.GetCredential(
					{ current.url, purpose, proxy.proxyUrl, std::move(*challenge) }, deadline, cancellation);
				if (credential) {
					if (purpose == ECredentialPurpose::Proxy) credentialProxyUrl = proxy.proxyUrl;
					continue;
				}
			}
			const auto outcome = purpose == ECredentialPurpose::Proxy
				? ERequestOutcome::ProxyAuthenticationRequired : ERequestOutcome::ServerAuthenticationRequired;
			const auto failure = purpose == ECredentialPurpose::Proxy
				? ETransportFailure::ProxyAuthenticationRequired : ETransportFailure::None;
			return { outcome, failure, std::move(response), false, redirects, retries };
		}
		if (current.allowRedirects && IsRedirect(response.statusCode)) {
			const auto location = FindHeader(response.headers, L"Location");
			const auto nextUrl = location ? ResolveRedirect(current.url, *location) : std::nullopt;
			if (!nextUrl) {
				return { ERequestOutcome::InvalidRedirect, ETransportFailure::None, std::move(response), false, redirects, retries };
			}
			if (IsHttpsToHttpDowngrade(current.url, *nextUrl)) {
				return { ERequestOutcome::HttpsDowngradeRejected, ETransportFailure::None, std::move(response), false, redirects, retries };
			}
			const auto redirectLimit = current.limits.maxRedirects.value_or(m_options.maxRedirects);
			if (redirects >= redirectLimit) {
				return { ERequestOutcome::RedirectLimitExceeded, ETransportFailure::None, std::move(response), false, redirects, retries };
			}
			++redirects;
			const bool changesOrigin = NormalizedOrigin(current.url) != NormalizedOrigin(*nextUrl);
			if (changesOrigin) {
				RemoveHeaders(current.headers, { L"Authorization", L"Proxy-Authorization", L"Cookie", L"Cookie2", L"Referer" });
			}
			RemoveHeaders(current.headers, { L"Host" });
			if (response.statusCode == 303 || ((response.statusCode == 301 || response.statusCode == 302) && !IsIdempotent(current.method))) {
				current.method = L"GET";
				current.body.clear();
				RemoveHeaders(current.headers, { L"Content-Length", L"Content-Type", L"Content-Encoding", L"Content-Language", L"Content-Location", L"Transfer-Encoding", L"Digest" });
			}
			current.url = *nextUrl;
			serverCredential.reset();
			proxyCredential.reset();
			credentialProxyUrl.reset();
			attemptedServerAuthentication = false;
			attemptedProxyAuthentication = false;
			continue;
		}

		if ((response.statusCode == 429 || response.statusCode == 503) && IsIdempotent(current.method) && retries < m_options.maxRetries) {
			auto delay = BoundedBackoff(m_options, retries, m_jitterSource);
			if (const auto retryAfter = RetryAfter(response, m_clock)) {
				delay = std::min(m_options.maxRetryDelay, std::max(delay, *retryAfter));
			}
			if (deadline) {
				const auto now = m_clock.SteadyNow();
				if (now >= *deadline) {
					return { ERequestOutcome::Timeout, ETransportFailure::Timeout, std::nullopt, false, redirects, retries };
				}
				delay = std::min(delay, std::chrono::ceil<std::chrono::milliseconds>(*deadline - now));
			}
			++retries;
			if (!m_scheduler.WaitFor(delay, cancellation) || (cancellation && cancellation->IsCancellationRequested())) {
				return { ERequestOutcome::Cancelled, ETransportFailure::None, std::nullopt, false, redirects, retries };
			}
			if (HasTimedOut(deadline, m_clock)) {
				return { ERequestOutcome::Timeout, ETransportFailure::Timeout, std::nullopt, false, redirects, retries };
			}
			continue;
		}

		if (m_responseCache && request.cachePolicy != ERequestCachePolicy::OfflineOnly && response.statusCode >= 200 && response.statusCode < 300) {
			m_responseCache->Put(cacheKey, response);
		}
		return { ERequestOutcome::Success, ETransportFailure::None, std::move(response), false, redirects, retries };
	}
}

} // namespace platform::request
