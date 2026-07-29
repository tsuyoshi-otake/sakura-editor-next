/*!	@file
	@brief HTTP(S) GET クライアント（WinHTTP ラッパ）

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CHTTPCLIENT_6B2E4D71_0A93_4C58_9F17_5E8A3C2D4B60_H_
#define SAKURA_CHTTPCLIENT_6B2E4D71_0A93_4C58_9F17_5E8A3C2D4B60_H_
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

/*!
	@brief HTTP(S) GET を行う最小クライアント

	WinHTTP の同期 API を用いる。UI に一切依存しないので
	ワーカースレッドから呼び出して構わない。
	ただし 1 インスタンスを複数スレッドで共有してはならない。

	@note 接続先は https に限定している。拡張機能の取得という用途上、
		平文通信を許すべきではないため。
*/
class CHttpClient {
public:
	//! 応答
	struct Response {
		unsigned long	statusCode = 0;	//!< HTTP ステータスコード
		std::string		body;			//!< 応答本体（バイト列。JSON なら UTF-8）

		bool IsOk() const noexcept { return statusCode == 200; }
	};

	//! 受信量の上限。非信頼入力に対する防御
	static constexpr size_t kMaxTextBytes   =   8u * 1024 * 1024;	//!< JSON 等のテキスト応答
	static constexpr size_t kMaxBinaryBytes = 128u * 1024 * 1024;	//!< VSIX 等のバイナリ応答

	CHttpClient();
	CHttpClient(const CHttpClient&) = delete;
	CHttpClient& operator = (const CHttpClient&) = delete;
	CHttpClient(CHttpClient&&) noexcept = delete;
	CHttpClient& operator = (CHttpClient&&) noexcept = delete;
	~CHttpClient();

	//! セッションが使用可能か
	bool IsOk() const noexcept { return m_hSession != nullptr; }

	/*!
		@brief GET してメモリに受け取る
		@param[in]  url			取得先。https のみ
		@param[out] response	ステータスコードと本体
		@param[out] errorMsg	失敗理由（未ローカライズの技術的詳細）
		@retval true 通信自体は成功。HTTP ステータスは response で確認すること
	*/
	bool Get(
		const std::wstring& url,
		Response& response,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

	/*!
		@brief GET してファイルに保存する
		@param[in]  url			取得先。https のみ
		@param[in]  outPath		保存先。失敗時は削除する
		@param[out] errorMsg	失敗理由（未ローカライズの技術的詳細）
		@retval true 200 応答を受け取り、outPath に保存できた
	*/
	bool Download(
		const std::wstring& url,
		const std::filesystem::path& outPath,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

private:
	//! 受信データの受け取り先。false を返すと受信を中止する
	using Sink = std::function<bool(const char* pData, size_t nBytes)>;

	bool Request(
		const std::wstring&	url,
		size_t				maxBytes,
		unsigned long&		statusCode,
		const Sink&			sink,
		std::wstring&		errorMsg,
		const std::atomic<bool>* pCancelled,
		unsigned int		redirectsRemaining);

	void*	m_hSession = nullptr;	//!< HINTERNET（セッションハンドル）
};

#endif /* SAKURA_CHTTPCLIENT_6B2E4D71_0A93_4C58_9F17_5E8A3C2D4B60_H_ */
