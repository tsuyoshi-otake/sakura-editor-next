/*! @file
 * @brief Open VSX Registry クライアント（legacy transport adapter）
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "extension/COpenVsxClient.h"

#include <utility>

COpenVsxClient::COpenVsxClient(std::wstring sRegistryUrl)
	: m_sRegistryUrl(extension::openvsx::OpenVsxProtocol::NormalizeRegistryUrl(std::move(sRegistryUrl)))
{
}

std::wstring COpenVsxClient::BuildSearchUrl(const std::wstring& sQuery, int nOffset, int nSize) const
{
	return extension::openvsx::OpenVsxProtocol::BuildSearchUrl(m_sRegistryUrl, sQuery, nOffset, nSize);
}

bool COpenVsxClient::Search(
	const std::wstring& sQuery,
	int nOffset,
	int nSize,
	SOpenVsxSearchResult& result,
	std::wstring& errorMsg,
	const std::atomic<bool>* pCancelled)
{
	result = SOpenVsxSearchResult();

	CHttpClient::Response response;
	if (!m_cHttp.Get(BuildSearchUrl(sQuery, nOffset, nSize), response, errorMsg, pCancelled)) return false;
	if (!response.IsOk()) {
		errorMsg = L"unexpected HTTP status " + std::to_wstring(response.statusCode);
		return false;
	}
	return ParseSearchResponse(response.body, result, errorMsg);
}

bool COpenVsxClient::DownloadVsix(
	const std::wstring& sDownloadUrl,
	const std::filesystem::path& outPath,
	std::wstring& errorMsg,
	const std::atomic<bool>* pCancelled)
{
	if (sDownloadUrl.empty()) {
		errorMsg = L"download url is empty";
		return false;
	}
	return m_cHttp.Download(sDownloadUrl, outPath, errorMsg, pCancelled);
}

bool COpenVsxClient::ParseSearchResponse(const std::string& sJson, SOpenVsxSearchResult& result, std::wstring& errorMsg)
{
	return extension::openvsx::OpenVsxProtocol::ParseSearchResponse(sJson, result, errorMsg);
}
