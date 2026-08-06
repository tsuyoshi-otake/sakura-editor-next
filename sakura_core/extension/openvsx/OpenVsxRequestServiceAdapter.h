/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "extension/openvsx/IOpenVsxRegistryClient.h"
#include <sakura/request/RequestService.h>
#include "util/string_ex.h"

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

//! Open VSX の HTTP 呼び出しを platform request 契約だけで実行する小さな adapter。
namespace extension::openvsx {

//! 呼び出し元が決めた request 方針。adapter はこの値を変更も補完もしない。
struct OpenVsxRequestPolicy {
	platform::request::ERequestCachePolicy cachePolicy = platform::request::ERequestCachePolicy::OnlineOnly;
	bool allowRedirects = true;
	platform::request::EProxySupport proxySupport = platform::request::EProxySupport::Fallback;
	platform::request::RequestLimits searchLimits;
	platform::request::RequestLimits vsixLimits;
	platform::request::RequestLimits sha256Limits;
	platform::request::RequestLimits textLimits;
};

/*!
 * @brief Open VSX registry を IRequestService 経由で取得する adapter。
 *
 * registry URI は呼び出し元が HTTPS として検証済みであることを前提に受け取るが、
 * 防御的にも HTTPS 以外・userinfo を持つ URI は request を開始せず拒否する。
 * credentials、設定値、任意 request header は受け取らない。認証が必要な場合は
 * IRequestService が challenge-scoped credential service に委譲する。
 */
class OpenVsxRequestServiceAdapter final : public IOpenVsxRegistryClient {
public:
	static constexpr int kDefaultPageSize = OpenVsxProtocol::kDefaultPageSize;
	static constexpr int kMaxPageSize = OpenVsxProtocol::kMaxPageSize;

	OpenVsxRequestServiceAdapter(
		platform::request::IRequestService& requestService,
		std::wstring validatedHttpsRegistryUri,
		OpenVsxRequestPolicy policy)
		: m_requestService(requestService)
		, m_registryUri(OpenVsxProtocol::NormalizeRegistryUrl(std::move(validatedHttpsRegistryUri)))
		, m_policy(std::move(policy))
		, m_hasValidRegistryUri(IsHttpsAbsoluteUri(m_registryUri))
	{
	}

	//! 通信を行わない検索 URI 構築。offset と page size は既存 Open VSX 契約と同じ範囲に収める。
	std::wstring BuildSearchUrl(std::wstring_view query, int offset, int pageSize) const
	{
		return OpenVsxProtocol::BuildSearchUrl(m_registryUri, query, offset, pageSize);
	}

	OpenVsxSearchOperation Search(
		std::wstring_view query,
		int offset,
		int pageSize,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		OpenVsxSearchOperation operation;
		const auto execution = ExecuteGet(BuildSearchUrl(query, offset, pageSize), m_policy.searchLimits, L"application/json", cancellation);
		operation.status = execution.status;
		if (!operation.status) return operation;

		const std::string json(execution.response->body.begin(), execution.response->body.end());
		if (!OpenVsxProtocol::ParseSearchResponse(json, operation.value, operation.status.message)) {
			operation.status.outcome = EOpenVsxRequestOutcome::SearchParseFailure;
		}
		return operation;
	}

	//! HTTPS VSIX URI をメモリー上の有界バイト列として取得する。ファイル書き込みは所有しない。
	OpenVsxBinaryOperation FetchVsix(
		std::wstring_view validatedHttpsVsixUri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		return FetchBinary(validatedHttpsVsixUri, m_policy.vsixLimits, L"application/octet-stream", cancellation);
	}

	//! sha256 URI が無い場合は通信せず NotRequested を返す。ある場合は HTTPS のみを要求する。
	OpenVsxBinaryOperation FetchOptionalSha256(
		const std::optional<std::wstring>& validatedHttpsSha256Uri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		if (!validatedHttpsSha256Uri || validatedHttpsSha256Uri->empty()) {
			return { { EOpenVsxRequestOutcome::NotRequested, platform::request::ERequestOutcome::Success, std::nullopt, L"sha256 URI was not provided" }, {} };
		}
		return FetchBinary(*validatedHttpsSha256Uri, m_policy.sha256Limits, L"text/plain", cancellation);
	}

	OpenVsxExtensionAssetsOperation FetchExtensionAssets(
		std::wstring_view namespaceName,
		std::wstring_view extensionName,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		OpenVsxExtensionAssetsOperation operation;
		const std::wstring uri = OpenVsxProtocol::BuildExtensionMetadataUrl(m_registryUri, namespaceName, extensionName);
		if (uri.empty()) {
			operation.status = { EOpenVsxRequestOutcome::InvalidEndpointUri,
				platform::request::ERequestOutcome::InvalidRequest, std::nullopt,
				L"extension identifier is not usable as a URI path segment" };
			return operation;
		}

		// メタデータは JSON なので、VSIX ではなく検索と同じ小さな予算で足りる。
		const auto execution = ExecuteGet(uri, m_policy.searchLimits, L"application/json", cancellation);
		operation.status = execution.status;
		if (!operation.status) return operation;

		const std::string json(execution.response->body.begin(), execution.response->body.end());
		if (!OpenVsxProtocol::ParseExtensionMetadataResponse(json, operation.value, operation.status.message)) {
			operation.status.outcome = EOpenVsxRequestOutcome::InvalidResponse;
		}
		return operation;
	}

	OpenVsxTextOperation FetchText(
		std::wstring_view validatedHttpsTextUri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		OpenVsxTextOperation operation;
		const auto execution = ExecuteGet(validatedHttpsTextUri, m_policy.textLimits, L"text/plain", cancellation);
		operation.status = execution.status;
		if (!operation.status) return operation;
		try {
			const std::string utf8(execution.response->body.begin(), execution.response->body.end());
			operation.value = u8stowcs(utf8);
		}
		catch (...) {
			operation.status.outcome = EOpenVsxRequestOutcome::InvalidResponse;
			operation.status.message = L"text response was not valid UTF-8";
		}
		return operation;
	}

	/*!
	 * @brief HTTPS VSIX URI を chunk sink へ直接ストリーミングする
	 *
	 * ExecuteGet()（メモリー取得の全 endpoint が使う共通経路）はあえて変更しない。
	 * この経路は自身では応答本体を保持しないので、response->body ではなく sink へ
	 * 渡した後の終端状態だけを組み立てる。既存の ExecuteGet() が持つ検証手順
	 * （registry/endpoint URI の HTTPS 検証、limits の健全性、最終 URI の HTTPS 維持、
	 * 応答ヘッダー上限）は private static helper 経由でそのまま再利用する。
	 */
	OpenVsxOperationStatus FetchVsixStreamed(
		std::wstring_view validatedHttpsVsixUri,
		const OpenVsxBodyChunkSink& sink,
		const platform::request::IRequestCancellation* cancellation = nullptr) const override
	{
		using namespace platform::request;
		if (cancellation && cancellation->IsCancellationRequested()) {
			return { EOpenVsxRequestOutcome::Cancelled, ERequestOutcome::Cancelled, std::nullopt, L"request was cancelled before dispatch" };
		}
		if (!m_hasValidRegistryUri) {
			return { EOpenVsxRequestOutcome::InvalidRegistryUri, ERequestOutcome::InvalidRequest, std::nullopt, L"registry URI must be an absolute HTTPS URI without userinfo" };
		}
		if (!IsHttpsAbsoluteUri(validatedHttpsVsixUri)) {
			return { EOpenVsxRequestOutcome::InvalidEndpointUri, ERequestOutcome::InvalidRequest, std::nullopt, L"Open VSX endpoint must be an absolute HTTPS URI without userinfo" };
		}
		if (!HasValidLimits(m_policy.vsixLimits)) {
			return { EOpenVsxRequestOutcome::InvalidRequest, ERequestOutcome::InvalidRequest, std::nullopt, L"request limits are invalid" };
		}
		if (!sink) {
			return { EOpenVsxRequestOutcome::InvalidRequest, ERequestOutcome::InvalidRequest, std::nullopt, L"streamed fetch requires a body sink" };
		}

		Request request;
		request.method = L"GET";
		request.url.assign(validatedHttpsVsixUri);
		request.headers.push_back({ L"Accept", L"application/octet-stream" });
		request.cachePolicy = m_policy.cachePolicy;
		request.allowRedirects = m_policy.allowRedirects;
		request.limits = m_policy.vsixLimits;
		request.proxySupport = m_policy.proxySupport;
		request.bodySink = sink;

		RequestResult result = m_requestService.Execute(request, cancellation);
		OpenVsxOperationStatus status{ MapRequestOutcome(result.outcome), result.outcome, std::nullopt, {} };
		if (result.response) status.httpStatusCode = result.response->statusCode;
		if (status.outcome != EOpenVsxRequestOutcome::Success) return status;
		if (!result.response) {
			status.outcome = EOpenVsxRequestOutcome::InvalidResponse;
			status.message = L"request service returned success without a response";
			return status;
		}
		if (!IsHttpsAbsoluteUri(result.response->finalUrl)) {
			status.outcome = HasAsciiScheme(result.response->finalUrl, L"http://")
				? EOpenVsxRequestOutcome::HttpsDowngradeRejected
				: EOpenVsxRequestOutcome::InvalidResponse;
			status.message = L"request service returned a non-HTTPS final URI";
			return status;
		}
		if (ResponseHeaderBytes(result.response->headers) > m_policy.vsixLimits.maxResponseHeaderBytes) {
			status.outcome = EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded;
			status.requestOutcome = ERequestOutcome::ResponseHeaderLimitExceeded;
			status.message = L"response headers exceeded the caller-provided limit";
			return status;
		}
		// response body はストリーミング済みで空のまま残る。上限強制は transport 側の
		// chunk ごとの逐次チェックが既に担っているので、ここで response->body の
		// サイズを二重判定しない（常に 0 で無意味なため）。
		if (!IsSuccessfulHttpStatus(result.response->statusCode)) {
			status.outcome = EOpenVsxRequestOutcome::HttpStatusFailure;
			status.message = L"unexpected HTTP status " + std::to_wstring(result.response->statusCode);
		}
		return status;
	}

private:
	struct RequestExecution {
		OpenVsxOperationStatus status;
		std::optional<platform::request::HttpResponse> response;
	};

	static bool IsHttpsAbsoluteUri(std::wstring_view uri) noexcept
	{
		constexpr std::wstring_view scheme = L"https://";
		if (!HasAsciiScheme(uri, scheme)) return false;
		const auto authorityEnd = uri.find_first_of(L"/?#", scheme.size());
		const auto authority = uri.substr(scheme.size(), authorityEnd == std::wstring_view::npos ? uri.size() - scheme.size() : authorityEnd - scheme.size());
		if (authority.empty() || authority.find(L'@') != std::wstring_view::npos) return false;
		for (const wchar_t character : uri) {
			if (character <= 0x20 || character == 0x7f || character == L'\\') return false;
		}
		return true;
	}

	static bool HasAsciiScheme(std::wstring_view uri, std::wstring_view scheme) noexcept
	{
		if (uri.size() <= scheme.size()) return false;
		for (std::size_t index = 0; index < scheme.size(); ++index) {
			const wchar_t character = uri[index] >= L'A' && uri[index] <= L'Z' ? uri[index] - L'A' + L'a' : uri[index];
			if (character != scheme[index]) return false;
		}
		return true;
	}

	static std::size_t ResponseHeaderBytes(const std::vector<platform::request::HttpHeader>& headers) noexcept
	{
		std::size_t total = 0;
		for (const auto& header : headers) {
			const auto units = header.name.size() + header.value.size() + 4;
			if (units > ((std::numeric_limits<std::size_t>::max)() - total) / sizeof(wchar_t)) {
				return (std::numeric_limits<std::size_t>::max)();
			}
			total += units * sizeof(wchar_t);
		}
		return total;
	}

	static EOpenVsxRequestOutcome MapRequestOutcome(platform::request::ERequestOutcome outcome) noexcept
	{
		using platform::request::ERequestOutcome;
		switch (outcome) {
		case ERequestOutcome::Success: return EOpenVsxRequestOutcome::Success;
		case ERequestOutcome::Cancelled: return EOpenVsxRequestOutcome::Cancelled;
		case ERequestOutcome::InvalidRequest: return EOpenVsxRequestOutcome::InvalidRequest;
		case ERequestOutcome::OfflineCacheMiss: return EOpenVsxRequestOutcome::OfflineCacheMiss;
		case ERequestOutcome::RedirectLimitExceeded: return EOpenVsxRequestOutcome::RedirectLimitExceeded;
		case ERequestOutcome::HttpsDowngradeRejected: return EOpenVsxRequestOutcome::HttpsDowngradeRejected;
		case ERequestOutcome::InvalidRedirect: return EOpenVsxRequestOutcome::InvalidRedirect;
		case ERequestOutcome::Timeout: return EOpenVsxRequestOutcome::Timeout;
		case ERequestOutcome::ResponseHeaderLimitExceeded: return EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded;
		case ERequestOutcome::ResponseBodyLimitExceeded: return EOpenVsxRequestOutcome::ResponseBodyLimitExceeded;
		case ERequestOutcome::ServerAuthenticationRequired: return EOpenVsxRequestOutcome::ServerAuthenticationRequired;
		case ERequestOutcome::ProxyAuthenticationRequired: return EOpenVsxRequestOutcome::ProxyAuthenticationRequired;
		case ERequestOutcome::UnsupportedProxyPolicy: return EOpenVsxRequestOutcome::UnsupportedProxyPolicy;
		case ERequestOutcome::TransportFailure: return EOpenVsxRequestOutcome::TransportFailure;
		case ERequestOutcome::TlsCertificateFailure: return EOpenVsxRequestOutcome::TlsCertificateFailure;
		}
		return EOpenVsxRequestOutcome::TransportFailure;
	}

	static bool IsSuccessfulHttpStatus(int statusCode) noexcept
	{
		return statusCode >= 200 && statusCode < 300;
	}

	static bool HasValidLimits(const platform::request::RequestLimits& limits) noexcept
	{
		return (!limits.timeout || *limits.timeout > std::chrono::milliseconds::zero()) &&
			limits.maxResponseHeaderBytes != 0 && limits.maxResponseBodyBytes != 0;
	}

	RequestExecution ExecuteGet(
		std::wstring_view uri,
		const platform::request::RequestLimits& limits,
		std::wstring_view accept,
		const platform::request::IRequestCancellation* cancellation) const
	{
		using namespace platform::request;
		if (cancellation && cancellation->IsCancellationRequested()) {
			return { { EOpenVsxRequestOutcome::Cancelled, ERequestOutcome::Cancelled, std::nullopt, L"request was cancelled before dispatch" }, std::nullopt };
		}
		if (!m_hasValidRegistryUri) {
			return { { EOpenVsxRequestOutcome::InvalidRegistryUri, ERequestOutcome::InvalidRequest, std::nullopt, L"registry URI must be an absolute HTTPS URI without userinfo" }, std::nullopt };
		}
		if (!IsHttpsAbsoluteUri(uri)) {
			return { { EOpenVsxRequestOutcome::InvalidEndpointUri, ERequestOutcome::InvalidRequest, std::nullopt, L"Open VSX endpoint must be an absolute HTTPS URI without userinfo" }, std::nullopt };
		}
		if (!HasValidLimits(limits)) {
			return { { EOpenVsxRequestOutcome::InvalidRequest, ERequestOutcome::InvalidRequest, std::nullopt, L"request limits are invalid" }, std::nullopt };
		}

		Request request;
		request.method = L"GET";
		request.url.assign(uri);
		request.headers.push_back({ L"Accept", std::wstring(accept) });
		request.cachePolicy = m_policy.cachePolicy;
		request.allowRedirects = m_policy.allowRedirects;
		request.limits = limits;
		request.proxySupport = m_policy.proxySupport;

		RequestResult result = m_requestService.Execute(request, cancellation);
		RequestExecution execution{ { MapRequestOutcome(result.outcome), result.outcome, std::nullopt, {} }, std::move(result.response) };
		if (execution.response) execution.status.httpStatusCode = execution.response->statusCode;
		if (execution.status.outcome != EOpenVsxRequestOutcome::Success) return execution;
		if (!execution.response) {
			execution.status.outcome = EOpenVsxRequestOutcome::InvalidResponse;
			execution.status.message = L"request service returned success without a response";
			return execution;
		}
		if (!IsHttpsAbsoluteUri(execution.response->finalUrl)) {
			execution.status.outcome = HasAsciiScheme(execution.response->finalUrl, L"http://")
				? EOpenVsxRequestOutcome::HttpsDowngradeRejected
				: EOpenVsxRequestOutcome::InvalidResponse;
			execution.status.message = L"request service returned a non-HTTPS final URI";
			return execution;
		}
		if (ResponseHeaderBytes(execution.response->headers) > limits.maxResponseHeaderBytes) {
			execution.status.outcome = EOpenVsxRequestOutcome::ResponseHeaderLimitExceeded;
			execution.status.requestOutcome = ERequestOutcome::ResponseHeaderLimitExceeded;
			execution.status.message = L"response headers exceeded the caller-provided limit";
			return execution;
		}
		if (execution.response->body.size() > limits.maxResponseBodyBytes) {
			execution.status.outcome = EOpenVsxRequestOutcome::ResponseBodyLimitExceeded;
			execution.status.requestOutcome = ERequestOutcome::ResponseBodyLimitExceeded;
			execution.status.message = L"response body exceeded the caller-provided limit";
			return execution;
		}
		if (!IsSuccessfulHttpStatus(execution.response->statusCode)) {
			execution.status.outcome = EOpenVsxRequestOutcome::HttpStatusFailure;
			execution.status.message = L"unexpected HTTP status " + std::to_wstring(execution.response->statusCode);
		}
		return execution;
	}

	OpenVsxBinaryOperation FetchBinary(
		std::wstring_view uri,
		const platform::request::RequestLimits& limits,
		std::wstring_view accept,
		const platform::request::IRequestCancellation* cancellation) const
	{
		OpenVsxBinaryOperation operation;
		const auto execution = ExecuteGet(uri, limits, accept, cancellation);
		operation.status = execution.status;
		if (operation.status) operation.value = execution.response->body;
		return operation;
	}

	platform::request::IRequestService& m_requestService;
	const std::wstring m_registryUri;
	const OpenVsxRequestPolicy m_policy;
	const bool m_hasValidRegistryUri;
};

} // namespace extension::openvsx
