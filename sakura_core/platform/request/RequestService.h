/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//! UI や WinHTTP に依存しない HTTP request の契約。
namespace platform::request {

struct HttpHeader {
	std::wstring name;
	std::wstring value;
};

struct HttpResponse {
	int statusCode = 0;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	std::wstring finalUrl;
};

//! 実行時に中止を確認するための読み取り専用 token。
class IRequestCancellation {
public:
	virtual ~IRequestCancellation() = default;
	virtual bool IsCancellationRequested() const noexcept = 0;
};

//! proxy の no-proxy 判定を含む、proxy service が返す最終的な選択。
enum class EProxyMode : std::uint8_t {
	System,
	Direct,
	Manual,
};

//! VS Code の http.proxySupport と同じ意図を表す request ごとの proxy 方針。
//! 実際の system/PAC/environment/no-proxy 解決は IProxyService の責務である。
enum class EProxySupport : std::uint8_t {
	//! Proxy 解決を行わず、必ず direct 接続する。
	Off,
	//! Proxy 解決を必須とする。実装不能なら UnsupportedPolicy を返す。
	On,
	//! 設定 proxy が使えない場合は system/environment 解決へ委譲する。
	Fallback,
	//! 明示設定を system/environment より優先して解決する。
	Override,
};

struct ProxyRequest {
	std::wstring targetUrl;
	EProxySupport support = EProxySupport::Fallback;
};

enum class EProxySelectionOutcome : std::uint8_t {
	Selected,
	UnsupportedPolicy,
};

struct ProxySelection {
	EProxyMode mode = EProxyMode::System;
	//! Manual の時だけ proxy URI を持つ。System/Direct の no-proxy 結果は
	//! bypassed=true として proxy service が明示する。
	std::optional<std::wstring> proxyUrl;
	bool bypassed = false;
	EProxySelectionOutcome outcome = EProxySelectionOutcome::Selected;
};

//! no-proxy の書式・一致規則はここでなく実装側 proxy service が所有する。
class IProxyService {
public:
	virtual ~IProxyService() = default;
	//! System/PAC resolution may perform blocking work, so it shares the overall
	//! request deadline and cancellation ownership with credentials and transport.
	virtual ProxySelection SelectProxy(
		const ProxyRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) = 0;
};

enum class ECredentialPurpose : std::uint8_t {
	Server,
	Proxy,
};

//! 401/407 challenge に結び付く credential lookup の範囲。
//! header 値全体を保持しないため、認証情報が request header/log に混ざらない。
struct AuthenticationChallenge {
	int statusCode = 0;
	std::wstring scheme;
	std::optional<std::wstring> realm;
};

struct CredentialRequest {
	std::wstring targetUrl;
	ECredentialPurpose purpose = ECredentialPurpose::Server;
	std::optional<std::wstring> proxyUrl;
	AuthenticationChallenge challenge;
};

//! credential value は transport への一回限りの入力であり、設定や request headers に
//! 書き戻してはならない。
struct RequestCredential {
	std::wstring userName;
	std::vector<std::uint8_t> secret;
};

class ICredentialService {
public:
	virtual ~ICredentialService() = default;
	//! Credential lookup is synchronous at this boundary, but it must still obey
	//! the caller's monotonic deadline and cancellation token. Implementations
	//! that prompt or cross a process boundary may not outlive either one.
	virtual std::optional<RequestCredential> GetCredential(
		const CredentialRequest& request,
		std::optional<std::chrono::steady_clock::time_point> deadline,
		const IRequestCancellation* cancellation) = 0;
};

//! transport は certificate validation を無効化する escape hatch を提供してはならない。
//! TlsCertificateFailure は不正・未信頼・hostname 不一致を含む。
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
};

//! Transport にも渡す上限。streaming transport は受信中に強制し、service は
//! 完成済み response にも再確認する。0 は安全でないため InvalidRequest となる。
struct RequestLimits {
	std::optional<std::chrono::milliseconds> timeout{ std::chrono::seconds(30) };
	//! 未指定なら RequestServiceOptions::maxRedirects を用いる。
	std::optional<std::size_t> maxRedirects;
	std::size_t maxResponseHeaderBytes = 64 * 1024;
	std::size_t maxResponseBodyBytes = 32 * 1024 * 1024;
};

struct TransportRequest {
	std::wstring method = L"GET";
	std::wstring url;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	ProxySelection proxy;
	std::optional<RequestCredential> serverCredential;
	std::optional<RequestCredential> proxyCredential;
	RequestLimits limits;
	//! Overall request deadline. A monotonic clock prevents wall-clock changes
	//! from extending or prematurely expiring network work.
	std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct TransportResult {
	std::optional<HttpResponse> response;
	ETransportFailure failure = ETransportFailure::None;
};

class IRequestTransport {
public:
	virtual ~IRequestTransport() = default;
	virtual TransportResult Send(const TransportRequest& request, const IRequestCancellation* cancellation) = 0;
};

//! 実時間と待機を分離し、Retry-After date と retry backoff を決定的に試験できる。
class IRequestClock {
public:
	virtual ~IRequestClock() = default;
	//! Wall time is used only for HTTP-date Retry-After parsing.
	virtual std::chrono::system_clock::time_point Now() const = 0;
	//! Monotonic time owns request deadlines and timeout decisions.
	virtual std::chrono::steady_clock::time_point SteadyNow() const = 0;
};

class IRequestScheduler {
public:
	virtual ~IRequestScheduler() = default;
	//! 待機中に cancellation が観測されたら false を返す。
	virtual bool WaitFor(std::chrono::milliseconds delay, const IRequestCancellation* cancellation) = 0;
};

//! [0, 1] の乱数を返す。service は retry storm を避けるためにこれを jitter に使う。
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

struct Request {
	//! 任意の RFC token method を渡せる。service は文字列を GET 等の enum に限定しない。
	std::wstring method = L"GET";
	std::wstring url;
	std::vector<HttpHeader> headers;
	std::vector<std::uint8_t> body;
	ERequestCachePolicy cachePolicy = ERequestCachePolicy::OnlineOnly;
	bool allowRedirects = true;
	//! 同じ論理 request を呼び出し側が明示的に同一視した場合だけ in-flight dedupe 対象。
	//! cancellation 付き call は各 caller の terminal cancellation を守るため dedupe しない。
	std::optional<std::wstring> deduplicationKey;
	RequestLimits limits;
	EProxySupport proxySupport = EProxySupport::Fallback;
};

enum class ERequestOutcome : std::uint8_t {
	Success,
	Cancelled,
	//! method/URL/header または同一 deduplication key の request identity が不正。
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

struct RequestResult {
	ERequestOutcome outcome = ERequestOutcome::TransportFailure;
	ETransportFailure transportFailure = ETransportFailure::None;
	std::optional<HttpResponse> response;
	bool fromCache = false;
	std::size_t redirectCount = 0;
	std::size_t retryCount = 0;

	explicit operator bool() const noexcept { return outcome == ERequestOutcome::Success; }
};

struct RequestServiceOptions {
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

//! 同期 orchestration 実装。transport, proxy, credentials, cache, time はすべて注入される。
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
