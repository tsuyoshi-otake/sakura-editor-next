/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

/*! 
 * @brief Open VSX 上の拡張 1 件
 *
 * Open VSX の検索 API 応答 1 要素に対応する。省略され得る項目は既定値で
 * 「無い」ことを表現する。この型は既存の extension UI と installation API の
 * 公開契約なので、transport adapter には所属させない。
 */
struct SOpenVsxExtension {
	std::wstring	sNamespace;					//!< 名前空間。例 "dbaeumer"
	std::wstring	sName;						//!< 拡張名。例 "vscode-eslint"
	std::wstring	sDisplayName;				//!< 表示名。例 "ESLint"
	std::wstring	sDescription;				//!< 説明
	std::wstring	sVersion;					//!< バージョン。例 "3.0.34"
	std::wstring	sDownloadUrl;				//!< VSIX の URL（files.download）
	std::wstring	sSha256Url;				//!< sha256 の URL（files.sha256。無い場合は空）
	std::wstring	sIconUrl;					//!< アイコンの URL（files.icon。無い場合は空）
	long long		nDownloadCount = 0;			//!< ダウンロード数
	double			dAverageRating = -1.0;		//!< 平均評価。評価が無い場合は負
	bool			bVerified = false;			//!< 名前空間が検証済みか
	bool			bDeprecated = false;			//!< 非推奨か

	//! "namespace.name" 形式の一意識別子。インストール先フォルダー名に使う
	std::wstring GetUniqueId() const { return sNamespace + L"." + sName; }

	//! 評価が存在するか
	bool HasRating() const noexcept { return dAverageRating >= 0.0; }
};

//! 検索結果
struct SOpenVsxSearchResult {
	int						nOffset = 0;	//!< 応答の開始位置
	int						nTotalSize = 0;	//!< 該当総件数
	std::vector<SOpenVsxExtension>	extensions;		//!< この応答に含まれる拡張
};

//! Open VSX wire protocol の、transport 非依存な変換処理。
namespace extension::openvsx {

class OpenVsxProtocol final {
public:
	static constexpr int kDefaultPageSize = 25;
	static constexpr int kMaxPageSize = 100;

	//! レジストリ URI の末尾の '/' を取り除く。URI の妥当性検査は transport 側の責務。
	static std::wstring NormalizeRegistryUrl(std::wstring registryUrl);

	//! 検索 endpoint URI を構築する。query は UTF-8 の RFC 3986 percent encoding を用いる。
	static std::wstring BuildSearchUrl(std::wstring_view registryUrl, std::wstring_view query, int offset, int pageSize);

	//! Open VSX search JSON を public extension model に変換する。
	static bool ParseSearchResponse(const std::string& json, SOpenVsxSearchResult& result, std::wstring& errorMsg);
};

} // namespace extension::openvsx
