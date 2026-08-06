/*! @file
 * @brief Transport-neutral request contracts and bounded request orchestration.
 */
/*
    Copyright (C) 2026, Sakura Editor Organization

    SPDX-License-Identifier: Zlib
*/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace platform::request {

struct HttpHeader final {
	std::wstring name;
	std::wstring value;
};

struct HttpResponse final {
	int statusCode = 0;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	std::wstring finalUrl;
};

class IRequestCancellation {
public:
	virtual ~IRequestCancellation() = default;
	virtual bool IsCancellationRequested() const noexcept = 0;
};

enum class EProxyMode : std::uint8_t {
	System,
	Direct,
	Manual,
};

enum class EProxySupport : std::uint8_t {
	Off,
	On,
	Fallback,
	Override,
};

struct ProxyRequest final {
	std::wstring targetUrl;
	EProxySupport support = EProxySupport::Fallback;
};

enum class EProxySelectionOutcome : std::uint8_t {
	Selected,
	UnsupportedPolicy,
};

struct ProxySelection final {
	EProxyMode mode = EProxyMode::System;
	std::optional<std::wstring> proxyUrl;
	bool bypassed = false;
	EProxySelectionOutcome outcome = EProxySelectionOutcome::Selected;
};

enum class ESystemProxyResolutionOutcome : std::uint8_t {
	Selected,
	Unavailable,
	Cancelled,
	DeadlineExceeded,
	InvalidResult,
	NoProxyRequired,
};

struct SystemProxyResolution final {
	ESystemProxyResolutionOutcome outcome = ESystemProxyResolutionOutcome::Unavailable;
	ProxySelection selection;
};

class ISystemProxyResolver {
public:
	virtual ~ISystemProxyResolver() = default;
	virtual SystemProxyResolution Resolve(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) = 0;
};

class IProxyService {
public:
	virtual ~IProxyService() = default;
	virtual ProxySelection SelectProxy(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) = 0;
};

enum class ECredentialPurpose : std::uint8_t {
	Server,
	Proxy,
};

struct AuthenticationChallenge final {
	int statusCode = 0;
	std::wstring scheme;
	std::optional<std::wstring> realm;
};

struct CredentialRequest final {
	std::wstring targetUrl;
	ECredentialPurpose purpose = ECredentialPurpose::Server;
	std::optional<std::wstring> proxyUrl;
	AuthenticationChallenge challenge;
};

struct RequestCredential final {
	std::wstring userName;
	std::vector<std::uint8_t> secret;
};

class ICredentialService {
public:
	virtual ~ICredentialService() = default;
	virtual std::optional<RequestCredential> GetCredential(
		const CredentialRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) = 0;
};

enum class ETransportFailure : std::uint8_t {
	None,
	Network,
	Timeout,
	TlsCertificateFailure,
	Protocol,
	ResponseHeaderLimitExceeded,
	ResponseBodyLimitExceeded,
	ProxyAuthenticationRequired,
	UnsupportedProxyPolicy,
	SinkFailure,
};

struct RequestLimits final {
	std::optional<std::chrono::milliseconds> timeout{ std::chrono::seconds(30) };
	std::optional<std::size_t> maxRedirects;
	std::size_t maxResponseHeaderBytes = 64 * 1024;
	std::size_t maxResponseBodyBytes = 32 * 1024 * 1024;
};

//! 応答本体を chunk 単位で受け取る sink。false を返すと転送を打ち切る。
//! 設定されていれば transport は応答本体をメモリー上に溜め込まず、
//! 受信した端から sink へ渡す（HttpResponse::body は空のまま残る）。
using ResponseBodyChunkSink = std::function<bool(const std::uint8_t* data, std::size_t size)>;

struct TransportRequest final {
	std::wstring method = L"GET";
	std::wstring url;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	ProxySelection proxy;
	std::optional<RequestCredential> serverCredential;
	std::optional<RequestCredential> proxyCredential;
	RequestLimits limits;
	std::optional<std::chrono::steady_clock::time_point> deadline;
	ResponseBodyChunkSink bodySink;
};

struct TransportResult final {
	std::optional<HttpResponse> response;
	ETransportFailure failure = ETransportFailure::None;
};

class IRequestTransport {
public:
	virtual ~IRequestTransport() = default;
	virtual TransportResult Send(const TransportRequest& request, const IRequestCancellation* cancellation) = 0;
};

class IRequestClock {
public:
	virtual ~IRequestClock() = default;
	virtual std::chrono::system_clock::time_point Now() const = 0;
	virtual std::chrono::steady_clock::time_point SteadyNow() const = 0;
};

class IRequestScheduler {
public:
	virtual ~IRequestScheduler() = default;
	virtual bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation* cancellation) = 0;
};

class IRetryJitterSource {
public:
	virtual ~IRetryJitterSource() = default;
	virtual double NextUnitInterval() = 0;
};

class IResponseCache {
public:
	virtual ~IResponseCache() = default;
	virtual std::optional<HttpResponse> Get(std::wstring_view cacheKey) = 0;
	virtual void Put(std::wstring_view cacheKey, const HttpResponse& response) = 0;
};

enum class ERequestCachePolicy : std::uint8_t {
	OnlineOnly,
	OfflineOnly,
	PreferCache,
};

struct Request final {
	std::wstring method = L"GET";
	std::wstring url;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	ERequestCachePolicy cachePolicy = ERequestCachePolicy::OnlineOnly;
	bool allowRedirects = true;
	std::optional<std::wstring> deduplicationKey;
	RequestLimits limits;
	EProxySupport proxySupport = EProxySupport::Fallback;
	//! 設定されていれば応答本体を溜め込まずこの sink へ流す。
	//! deduplication・キャッシュヒットとは併用しない（IsValidRequest が拒否する）。
	ResponseBodyChunkSink bodySink;
};

enum class ERequestOutcome : std::uint8_t {
	Success,
	Cancelled,
	InvalidRequest,
	OfflineCacheMiss,
	RedirectLimitExceeded,
	HttpsDowngradeRejected,
	InvalidRedirect,
	Timeout,
	ResponseHeaderLimitExceeded,
	ResponseBodyLimitExceeded,
	ServerAuthenticationRequired,
	ProxyAuthenticationRequired,
	UnsupportedProxyPolicy,
	TransportFailure,
	TlsCertificateFailure,
};

struct RequestResult final {
	ERequestOutcome outcome = ERequestOutcome::TransportFailure;
	ETransportFailure transportFailure = ETransportFailure::None;
	std::optional<HttpResponse> response;
	bool fromCache = false;
	std::size_t redirectCount = 0;
	std::size_t retryCount = 0;

	explicit operator bool() const noexcept { return outcome == ERequestOutcome::Success; }
};

struct RequestServiceOptions final {
	std::size_t maxRedirects = 10;
	std::size_t maxRetries = 3;
	std::chrono::milliseconds initialRetryDelay{ 250 };
	std::chrono::milliseconds maxRetryDelay{ 30000 };
};

class IRequestService {
public:
	virtual ~IRequestService() = default;
	virtual RequestResult Execute(const Request& request, const IRequestCancellation* cancellation = nullptr) = 0;
};

class RequestService final : public IRequestService {
public:
	RequestService(
		IRequestTransport& transport,
		IProxyService& proxyService,
		ICredentialService& credentialService,
		IRequestClock& clock,
		IRequestScheduler& scheduler,
		IRetryJitterSource& jitterSource,
		IResponseCache* responseCache = nullptr,
		RequestServiceOptions options = {}
	);

	RequestResult Execute(const Request& request, const IRequestCancellation* cancellation = nullptr) override;

private:
	struct InFlightRequest;

	RequestResult ExecuteUnshared(const Request& request, const IRequestCancellation* cancellation);

	IRequestTransport& m_transport;
	IProxyService& m_proxyService;
	ICredentialService& m_credentialService;
	IRequestClock& m_clock;
	IRequestScheduler& m_scheduler;
	IRetryJitterSource& m_jitterSource;
	IResponseCache* m_responseCache;
	RequestServiceOptions m_options;
	std::mutex m_inFlightMutex;
	std::unordered_map<std::wstring, std::shared_ptr<InFlightRequest>> m_inFlight;
};

} // namespace platform::request
