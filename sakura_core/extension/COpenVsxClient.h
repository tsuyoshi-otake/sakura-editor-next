/*! @file
 * @brief Open VSX Registry クライアント（legacy transport adapter）
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#ifndef SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_
#define SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "extension/CHttpClient.h"
#include "extension/openvsx/OpenVsxProtocol.h"

/*!
	@brief Open VSX Registry の旧来の HTTP クライアント

	公開 API と既存の call site は維持する。URL 構築・入力の正規化・JSON の解釈は
	OpenVsxProtocol に所属し、この型は CHttpClient を使う通信だけを担当する。
 */
class COpenVsxClient {
public:
	//! 既定のレジストリ
	static constexpr const wchar_t* kDefaultRegistryUrl = L"https://open-vsx.org";

	//! 1 回の検索で取得する既定件数
	static constexpr int kDefaultPageSize = extension::openvsx::OpenVsxProtocol::kDefaultPageSize;

	explicit COpenVsxClient(std::wstring sRegistryUrl = kDefaultRegistryUrl);

	//! HTTP セッションが使用可能か
	bool IsOk() const noexcept { return m_cHttp.IsOk(); }

	bool Search(
		const std::wstring& sQuery,
		int nOffset,
		int nSize,
		SOpenVsxSearchResult& result,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

	bool DownloadVsix(
		const std::wstring& sDownloadUrl,
		const std::filesystem::path& outPath,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

	//! legacy API。protocol utility へ委譲するため通信を行わない。
	std::wstring BuildSearchUrl(const std::wstring& sQuery, int nOffset, int nSize) const;

	//! legacy API。protocol utility へ委譲するため通信を行わない。
	static bool ParseSearchResponse(const std::string& sJson, SOpenVsxSearchResult& result, std::wstring& errorMsg);

private:
	std::wstring m_sRegistryUrl;	//!< 末尾に '/' を含まない
	CHttpClient m_cHttp;
};

#endif /* SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_ */
