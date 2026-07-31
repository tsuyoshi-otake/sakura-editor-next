/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "extension/openvsx/OpenVsxProtocol.h"
#include "platform/request/RequestService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace extension::openvsx {

//! Open VSX 操作を呼び出し元が分岐できる型付きの終端状態。
enum class EOpenVsxRequestOutcome : std::uint8_t {
	Success,
	NotRequested,
	Cancelled,
	InvalidRegistryUri,
	InvalidEndpointUri,
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
	HttpStatusFailure,
	InvalidResponse,
	SearchParseFailure,
};

struct OpenVsxOperationStatus {
	EOpenVsxRequestOutcome outcome = EOpenVsxRequestOutcome::InvalidRequest;
	platform::request::ERequestOutcome requestOutcome = platform::request::ERequestOutcome::InvalidRequest;
	std::optional<int> httpStatusCode;
	std::wstring message;

	explicit operator bool() const noexcept { return outcome == EOpenVsxRequestOutcome::Success; }
};

struct OpenVsxSearchOperation {
	OpenVsxOperationStatus status;
	SOpenVsxSearchResult value;
};

struct OpenVsxBinaryOperation {
	OpenVsxOperationStatus status;
	std::vector<std::uint8_t> value;
};

/*! 
 * @brief Open VSX registry の通信契約。
 *
 * 返却値は transport 固有の例外や未型付け応答ではなく、各 endpoint 固有の value と
 * 終端状態を常に組にする。URI は呼び出し前に HTTPS endpoint として検証済みである
 * ことを意図するが、実装は不正 URI を安全に拒否しなければならない。
 */
class IOpenVsxRegistryClient {
public:
	virtual ~IOpenVsxRegistryClient() = default;

	virtual OpenVsxSearchOperation Search(
		std::wstring_view query,
		int offset,
		int pageSize,
		const platform::request::IRequestCancellation* cancellation = nullptr) const = 0;

	virtual OpenVsxBinaryOperation FetchVsix(
		std::wstring_view validatedHttpsVsixUri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const = 0;

	virtual OpenVsxBinaryOperation FetchOptionalSha256(
		const std::optional<std::wstring>& validatedHttpsSha256Uri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const = 0;
};

} // namespace extension::openvsx
