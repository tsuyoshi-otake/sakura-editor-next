/*!	@file
	@brief Open VSX Registry クライアント

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/COpenVsxClient.h"

#include <picojson/picojson.h>

#include <algorithm>

#include "util/string_ex.h"

namespace {

//! 検索件数の上限。レジストリ側の制限に合わせる
constexpr int kMaxPageSize = 100;

/*!
	@brief URL のクエリ値として安全な形に符号化する

	RFC 3986 の unreserved 文字以外を UTF-8 バイト単位で百分率符号化する。
 */
std::wstring UrlEncode(const std::wstring& sInput)
{
	const std::string sUtf8 = wcstou8s(sInput);

	std::wstring sResult;
	sResult.reserve(sUtf8.size() * 3);

	for (const char ch : sUtf8) {
		const auto byte = static_cast<unsigned char>(ch);
		const bool bUnreserved =
			(byte >= 'A' && byte <= 'Z') ||
			(byte >= 'a' && byte <= 'z') ||
			(byte >= '0' && byte <= '9') ||
			byte == '-' || byte == '_' || byte == '.' || byte == '~';

		if (bUnreserved) {
			sResult += static_cast<wchar_t>(byte);
		}
		else {
			constexpr wchar_t szHex[] = L"0123456789ABCDEF";
			sResult += L'%';
			sResult += szHex[byte >> 4];
			sResult += szHex[byte & 0x0F];
		}
	}

	return sResult;
}

//! オブジェクトから文字列項目を取り出す。無ければ空
std::wstring GetWString(const picojson::object& obj, const char* pszKey)
{
	const auto it = obj.find(pszKey);
	if (it == obj.end() || !it->second.is<std::string>()) {
		return std::wstring();
	}
	return u8stowcs(it->second.get<std::string>());
}

//! オブジェクトから数値項目を取り出す。無ければ既定値
double GetNumber(const picojson::object& obj, const char* pszKey, double dDefault)
{
	const auto it = obj.find(pszKey);
	if (it == obj.end() || !it->second.is<double>()) {
		return dDefault;
	}
	return it->second.get<double>();
}

//! オブジェクトから真偽項目を取り出す。無ければ既定値
bool GetBool(const picojson::object& obj, const char* pszKey, bool bDefault)
{
	const auto it = obj.find(pszKey);
	if (it == obj.end() || !it->second.is<bool>()) {
		return bDefault;
	}
	return it->second.get<bool>();
}

//! オブジェクトから子オブジェクトを取り出す。無ければ nullptr
const picojson::object* GetObject(const picojson::object& obj, const char* pszKey)
{
	const auto it = obj.find(pszKey);
	if (it == obj.end() || !it->second.is<picojson::object>()) {
		return nullptr;
	}
	return &it->second.get<picojson::object>();
}

} // namespace

COpenVsxClient::COpenVsxClient(std::wstring sRegistryUrl)
	: m_sRegistryUrl(std::move(sRegistryUrl))
{
	// 末尾の '/' は URL 組み立て時に付けるので落としておく
	while (!m_sRegistryUrl.empty() && m_sRegistryUrl.back() == L'/') {
		m_sRegistryUrl.pop_back();
	}
}

// 検索 URL を組み立てる
std::wstring COpenVsxClient::BuildSearchUrl(const std::wstring& sQuery, int nOffset, int nSize) const
{
	const int nClampedOffset = (std::max)(0, nOffset);
	const int nClampedSize = std::clamp(nSize, 1, kMaxPageSize);

	std::wstring sUrl = m_sRegistryUrl + L"/api/-/search?offset=" + std::to_wstring(nClampedOffset)
		+ L"&size=" + std::to_wstring(nClampedSize);

	if (sQuery.empty()) {
		// 検索語が無いときは人気順の一覧を出す
		sUrl += L"&sortBy=downloadCount&sortOrder=desc";
	}
	else {
		sUrl += L"&query=" + UrlEncode(sQuery);
	}

	return sUrl;
}

// 拡張を検索する
bool COpenVsxClient::Search(
	const std::wstring&		sQuery,
	int						nOffset,
	int						nSize,
	SOpenVsxSearchResult&	result,
	std::wstring&			errorMsg)
{
	result = SOpenVsxSearchResult();

	CHttpClient::Response response;
	if (!m_cHttp.Get(BuildSearchUrl(sQuery, nOffset, nSize), response, errorMsg)) {
		return false;
	}
	if (!response.IsOk()) {
		errorMsg = L"unexpected HTTP status " + std::to_wstring(response.statusCode);
		return false;
	}

	return ParseSearchResponse(response.body, result, errorMsg);
}

// VSIX をダウンロードする
bool COpenVsxClient::DownloadVsix(const std::wstring& sDownloadUrl, const std::filesystem::path& outPath, std::wstring& errorMsg)
{
	if (sDownloadUrl.empty()) {
		errorMsg = L"download url is empty";
		return false;
	}
	return m_cHttp.Download(sDownloadUrl, outPath, errorMsg);
}

// 検索応答 JSON を解析する
bool COpenVsxClient::ParseSearchResponse(const std::string& sJson, SOpenVsxSearchResult& result, std::wstring& errorMsg)
{
	result = SOpenVsxSearchResult();

	picojson::value root;
	if (const std::string sParseError = picojson::parse(root, sJson); !sParseError.empty()) {
		errorMsg = L"invalid JSON: " + u8stowcs(sParseError);
		return false;
	}
	if (!root.is<picojson::object>()) {
		errorMsg = L"invalid JSON: root is not an object";
		return false;
	}

	const picojson::object& objRoot = root.get<picojson::object>();
	result.nOffset = static_cast<int>(GetNumber(objRoot, "offset", 0.0));
	result.nTotalSize = static_cast<int>(GetNumber(objRoot, "totalSize", 0.0));

	const auto itExtensions = objRoot.find("extensions");
	if (itExtensions == objRoot.end()) {
		// 該当なしの応答では extensions が省略され得る。空の結果として扱う
		return true;
	}
	if (!itExtensions->second.is<picojson::array>()) {
		errorMsg = L"invalid JSON: 'extensions' is not an array";
		return false;
	}

	const picojson::array& arrExtensions = itExtensions->second.get<picojson::array>();
	result.extensions.reserve(arrExtensions.size());

	for (const picojson::value& item : arrExtensions) {
		if (!item.is<picojson::object>()) {
			continue;
		}
		const picojson::object& objItem = item.get<picojson::object>();

		SOpenVsxExtension ext;
		ext.sNamespace		= GetWString(objItem, "namespace");
		ext.sName			= GetWString(objItem, "name");
		ext.sVersion		= GetWString(objItem, "version");
		ext.sDisplayName	= GetWString(objItem, "displayName");
		ext.sDescription	= GetWString(objItem, "description");
		ext.nDownloadCount	= static_cast<long long>(GetNumber(objItem, "downloadCount", 0.0));
		ext.dAverageRating	= GetNumber(objItem, "averageRating", -1.0);
		ext.bVerified		= GetBool(objItem, "verified", false);
		ext.bDeprecated		= GetBool(objItem, "deprecated", false);

		if (const picojson::object* pFiles = GetObject(objItem, "files"); pFiles) {
			ext.sDownloadUrl	= GetWString(*pFiles, "download");
			ext.sSha256Url		= GetWString(*pFiles, "sha256");
			ext.sIconUrl		= GetWString(*pFiles, "icon");
		}

		// 必須項目が欠けている要素は扱えないので取り込まない
		if (ext.sNamespace.empty() || ext.sName.empty() || ext.sVersion.empty() || ext.sDownloadUrl.empty()) {
			continue;
		}

		// 表示名が無い拡張があるので、その場合は拡張名で代替する
		if (ext.sDisplayName.empty()) {
			ext.sDisplayName = ext.sName;
		}

		result.extensions.push_back(std::move(ext));
	}

	return true;
}
