/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "extension/openvsx/OpenVsxProtocol.h"
#include <sakura/request/RequestService.h>

#include <cstdint>
#include <functional>
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
	Unsupported,
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

struct OpenVsxTextOperation {
	OpenVsxOperationStatus status;
	std::wstring value;
};

struct OpenVsxExtensionAssetsOperation {
	OpenVsxOperationStatus status;
	SOpenVsxExtensionAssets value;
};

//! VSIX 本体を chunk 単位で受け取る sink。false を返すと取得を打ち切る。
//! platform::request::ResponseBodyChunkSink と同じ形だが、この層は request
//! 実装の詳細を知らずに済むよう独自の別名として持つ。
using OpenVsxBodyChunkSink = std::function<bool(const std::uint8_t* data, std::size_t size)>;

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

	virtual OpenVsxTextOperation FetchText(
		std::wstring_view validatedHttpsTextUri,
		const platform::request::IRequestCancellation* cancellation = nullptr) const
	{
		(void)validatedHttpsTextUri;
		(void)cancellation;
		return { { EOpenVsxRequestOutcome::Unsupported,
			platform::request::ERequestOutcome::InvalidRequest,
			std::nullopt, L"text fetch is unsupported" }, {} };
	}

	/*!
	 * @brief 拡張メタデータ endpoint から全 targetPlatform の配布物一覧を取る
	 *
	 * 検索応答は targetPlatform を尊重しないので、導入時にはこちらで解決し直す。
	 * 既定実装が Unsupported を返すのは、これを実装しない client（テスト用の
	 * fake など）が検索応答の URL にフォールバックできるようにするため。
	 */
	virtual OpenVsxExtensionAssetsOperation FetchExtensionAssets(
		std::wstring_view namespaceName,
		std::wstring_view extensionName,
		const platform::request::IRequestCancellation* cancellation = nullptr) const
	{
		(void)namespaceName;
		(void)extensionName;
		(void)cancellation;
		return { { EOpenVsxRequestOutcome::Unsupported,
			platform::request::ERequestOutcome::InvalidRequest,
			std::nullopt, L"extension metadata fetch is unsupported" }, {} };
	}

	/*!
	 * @brief VSIX を全量メモリーへためずに sink へストリーミング取得する
	 *
	 * 既定実装が Unsupported を返すのは、これを実装しない client（テスト用の
	 * fake を含む）を呼び出し元が FetchVsix によるメモリー取得へ安全に
	 * フォールバックできるようにするため。値は sink 側へ渡るため、他の
	 * 操作と異なり戻り値に本体バイト列を持たない。
	 */
	virtual OpenVsxOperationStatus FetchVsixStreamed(
		std::wstring_view validatedHttpsVsixUri,
		const OpenVsxBodyChunkSink& sink,
		const platform::request::IRequestCancellation* cancellation = nullptr) const
	{
		(void)validatedHttpsVsixUri;
		(void)sink;
		(void)cancellation;
		return { EOpenVsxRequestOutcome::Unsupported,
			platform::request::ERequestOutcome::InvalidRequest,
			std::nullopt, L"streamed VSIX fetch is unsupported" };
	}
};

} // namespace extension::openvsx
