/*!	@file
	@brief HTTP(S) GET クライアント（WinHTTP ラッパ）

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CHttpClient.h"

#include <winhttp.h>

#include <fstream>
#include <system_error>

#include "cxx/ResourceHolder.hpp"

namespace {

using InternetHolder = cxx::ResourceHolder<&::WinHttpCloseHandle>;

// タイムアウト（ミリ秒）
constexpr int kResolveTimeoutMs =  10 * 1000;
constexpr int kConnectTimeoutMs =  15 * 1000;
constexpr int kSendTimeoutMs    =  30 * 1000;
constexpr int kReceiveTimeoutMs =  60 * 1000;

//! 1 回の WinHttpReadData で読む量
constexpr size_t kReadChunkBytes = 64 * 1024;

/*!
	@brief 失敗した API 名と Win32 エラーコードから技術的詳細を組み立てる

	@note ここではローカライズしない。文言はメッセージリソースを持つ
		呼び出し側（UI 層）の責務とし、この層は原因の特定に足る情報だけを返す。
 */
std::wstring FormatApiError(const wchar_t* pszApi, DWORD dwError)
{
	return std::wstring(pszApi) + L" failed (error " + std::to_wstring(dwError) + L")";
}

} // namespace

CHttpClient::CHttpClient()
{
	// WINHTTP_ACCESS_TYPE_DEFAULT_PROXY はシステムのプロキシ設定に従う。
	// 企業内などプロキシ必須の環境で、利用者が OS に設定した内容をそのまま使うため
	// これを選ぶ。自動構成スクリプト（WPAD）まで面倒を見たい場合は
	// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY に変える余地がある。
	m_hSession = ::WinHttpOpen(
		L"SakuraEditor",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0);
	if (!m_hSession) {
		return;
	}

	::WinHttpSetTimeouts(m_hSession, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

	// 古い TLS を使わせない
	DWORD dwProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
	dwProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
	::WinHttpSetOption(m_hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &dwProtocols, sizeof(dwProtocols));
}

CHttpClient::~CHttpClient()
{
	if (m_hSession) {
		::WinHttpCloseHandle(m_hSession);
		m_hSession = nullptr;
	}
}

// GET してメモリに受け取る
bool CHttpClient::Get(const std::wstring& url, Response& response, std::wstring& errorMsg)
{
	response.statusCode = 0;
	response.body.clear();

	return Request(
		url,
		kMaxTextBytes,
		response.statusCode,
		[&response](const char* pData, size_t nBytes) {
			response.body.append(pData, nBytes);
			return true;
		},
		errorMsg);
}

// GET してファイルに保存する
bool CHttpClient::Download(const std::wstring& url, const std::filesystem::path& outPath, std::wstring& errorMsg)
{
	std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
	if (!out) {
		errorMsg = L"cannot open '" + outPath.wstring() + L"' for writing";
		return false;
	}

	unsigned long statusCode = 0;
	const bool bRequested = Request(
		url,
		kMaxBinaryBytes,
		statusCode,
		[&out](const char* pData, size_t nBytes) {
			out.write(pData, static_cast<std::streamsize>(nBytes));
			return out.good();
		},
		errorMsg);

	const bool bWritten = out.good();
	out.close();

	if (bRequested && statusCode == 200 && bWritten) {
		return true;
	}

	// 中途半端なファイルを残さない
	std::error_code ec;
	std::filesystem::remove(outPath, ec);

	if (bRequested && statusCode != 200) {
		errorMsg = L"unexpected HTTP status " + std::to_wstring(statusCode);
	}
	else if (bRequested && !bWritten) {
		errorMsg = L"failed to write '" + outPath.wstring() + L"'";
	}
	return false;
}

bool CHttpClient::Request(
	const std::wstring&	url,
	size_t				maxBytes,
	unsigned long&		statusCode,
	const Sink&			sink,
	std::wstring&		errorMsg)
{
	statusCode = 0;

	if (!IsOk()) {
		errorMsg = L"WinHttpOpen failed";
		return false;
	}

	// URL を分解する。
	// 各要素は元の URL より長くなり得ないので、この大きさで足りることが保証される。
	// （INTERNET_MAX_URL_LENGTH は wininet.h の定義であり winhttp.h では使えない）
	const size_t nPartCapacity = url.size() + 1;
	std::wstring sHost(nPartCapacity, L'\0');
	std::wstring sPath(nPartCapacity, L'\0');
	std::wstring sExtra(nPartCapacity, L'\0');

	URL_COMPONENTS uc = {};
	uc.dwStructSize		= sizeof(uc);
	uc.lpszHostName		= sHost.data();
	uc.dwHostNameLength	= static_cast<DWORD>(sHost.size());
	uc.lpszUrlPath		= sPath.data();
	uc.dwUrlPathLength	= static_cast<DWORD>(sPath.size());
	uc.lpszExtraInfo	= sExtra.data();
	uc.dwExtraInfoLength = static_cast<DWORD>(sExtra.size());

	if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
		errorMsg = FormatApiError(L"WinHttpCrackUrl", ::GetLastError());
		return false;
	}

	// 平文通信は許可しない
	if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
		errorMsg = L"only https is allowed";
		return false;
	}

	sHost.resize(uc.dwHostNameLength);
	const std::wstring sResource =
		std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
		std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);

	InternetHolder hConnect = ::WinHttpConnect(m_hSession, sHost.c_str(), uc.nPort, 0);
	if (!hConnect) {
		errorMsg = FormatApiError(L"WinHttpConnect", ::GetLastError());
		return false;
	}

	InternetHolder hRequest = ::WinHttpOpenRequest(
		hConnect,
		L"GET",
		sResource.c_str(),
		nullptr,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		WINHTTP_FLAG_SECURE);
	if (!hRequest) {
		errorMsg = FormatApiError(L"WinHttpOpenRequest", ::GetLastError());
		return false;
	}

	if (!::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		errorMsg = FormatApiError(L"WinHttpSendRequest", ::GetLastError());
		return false;
	}

	if (!::WinHttpReceiveResponse(hRequest, nullptr)) {
		errorMsg = FormatApiError(L"WinHttpReceiveResponse", ::GetLastError());
		return false;
	}

	// ステータスコードを取得する
	DWORD dwStatus = 0;
	DWORD dwStatusSize = sizeof(dwStatus);
	if (!::WinHttpQueryHeaders(
			hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&dwStatus,
			&dwStatusSize,
			WINHTTP_NO_HEADER_INDEX)) {
		errorMsg = FormatApiError(L"WinHttpQueryHeaders", ::GetLastError());
		return false;
	}
	statusCode = dwStatus;

	// 本体を読む
	std::string buffer(kReadChunkBytes, '\0');
	size_t nTotal = 0;
	for (;;) {
		DWORD dwRead = 0;
		if (!::WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &dwRead)) {
			errorMsg = FormatApiError(L"WinHttpReadData", ::GetLastError());
			return false;
		}
		if (dwRead == 0) {
			break;	// 受信完了
		}

		nTotal += dwRead;
		if (nTotal > maxBytes) {
			errorMsg = L"response exceeds " + std::to_wstring(maxBytes) + L" bytes";
			return false;
		}

		if (!sink(buffer.data(), dwRead)) {
			errorMsg = L"failed to store received data";
			return false;
		}
	}

	return true;
}
