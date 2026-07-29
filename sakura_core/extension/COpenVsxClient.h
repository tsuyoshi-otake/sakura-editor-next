/*!	@file
	@brief Open VSX Registry クライアント

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_
#define SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_
#pragma once

#include <string>
#include <vector>

#include "extension/CHttpClient.h"

/*!
	@brief Open VSX 上の拡張 1 件

	Open VSX の検索 API 応答 1 要素に対応する。
	省略され得る項目があるため、既定値で「無い」ことを表現している。
 */
struct SOpenVsxExtension {
	std::wstring	sNamespace;					//!< 名前空間。例 "dbaeumer"
	std::wstring	sName;						//!< 拡張名。例 "vscode-eslint"
	std::wstring	sDisplayName;				//!< 表示名。例 "ESLint"
	std::wstring	sDescription;				//!< 説明
	std::wstring	sVersion;					//!< バージョン。例 "3.0.34"
	std::wstring	sDownloadUrl;				//!< VSIX の URL（files.download）
	std::wstring	sSha256Url;					//!< sha256 の URL（files.sha256。無い場合は空）
	std::wstring	sIconUrl;					//!< アイコンの URL（files.icon。無い場合は空）
	long long		nDownloadCount = 0;			//!< ダウンロード数
	double			dAverageRating = -1.0;		//!< 平均評価。評価が無い場合は負
	bool			bVerified = false;			//!< 名前空間が検証済みか
	bool			bDeprecated = false;		//!< 非推奨か

	//! "namespace.name" 形式の一意識別子。インストール先フォルダー名に使う
	std::wstring GetUniqueId() const { return sNamespace + L"." + sName; }

	//! 評価が存在するか
	bool HasRating() const noexcept { return dAverageRating >= 0.0; }
};

//! 検索結果
struct SOpenVsxSearchResult {
	int								nOffset = 0;	//!< 応答の開始位置
	int								nTotalSize = 0;	//!< 該当総件数
	std::vector<SOpenVsxExtension>	extensions;		//!< この応答に含まれる拡張
};

/*!
	@brief Open VSX Registry の検索 API クライアント

	Microsoft の Marketplace は VS Code 以外の製品からの利用を規約で禁じているため、
	取得先は Open VSX に限定している。

	@note UI に依存しないのでワーカースレッドから使用できる。
		1 インスタンスを複数スレッドで共有してはならない。
 */
class COpenVsxClient {
public:
	//! 既定のレジストリ
	static constexpr const wchar_t* kDefaultRegistryUrl = L"https://open-vsx.org";

	//! 1 回の検索で取得する既定件数
	static constexpr int kDefaultPageSize = 25;

	explicit COpenVsxClient(std::wstring sRegistryUrl = kDefaultRegistryUrl);

	//! HTTP セッションが使用可能か
	bool IsOk() const noexcept { return m_cHttp.IsOk(); }

	/*!
		@brief 拡張を検索する（通信を行う）
		@param[in]  sQuery		検索文字列。空なら人気順の一覧になる
		@param[in]  nOffset	取得開始位置
		@param[in]  nSize		取得件数
		@param[out] result		検索結果
		@param[out] errorMsg	失敗理由（未ローカライズの技術的詳細）
	*/
	bool Search(
		const std::wstring&		sQuery,
		int						nOffset,
		int						nSize,
		SOpenVsxSearchResult&	result,
		std::wstring&			errorMsg);

	/*!
		@brief VSIX をダウンロードする（通信を行う）
		@param[in]  sDownloadUrl	SOpenVsxExtension::sDownloadUrl
		@param[in]  outPath		保存先
		@param[out] errorMsg		失敗理由
	*/
	bool DownloadVsix(const std::wstring& sDownloadUrl, const std::filesystem::path& outPath, std::wstring& errorMsg);

	//! 検索 URL を組み立てる。通信を行わないので単体で検証できる
	std::wstring BuildSearchUrl(const std::wstring& sQuery, int nOffset, int nSize) const;

	/*!
		@brief 検索応答 JSON を解析する

		通信を行わないので単体で検証できる。
		未知の項目は無視し、必須項目（namespace / name / version / files.download）が
		欠けている要素は取り込まない。
	*/
	static bool ParseSearchResponse(const std::string& sJson, SOpenVsxSearchResult& result, std::wstring& errorMsg);

private:
	std::wstring	m_sRegistryUrl;	//!< 末尾に '/' を含まない
	CHttpClient		m_cHttp;
};

#endif /* SAKURA_COPENVSXCLIENT_A47D9E32_1C60_4B8F_8D25_7F3E6A9C0B14_H_ */
