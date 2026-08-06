/*! @file
 *
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <string>
#include <string_view>
#include <utility>
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

	/*!
	 * @brief この配布物が対象とするプラットフォーム。例 "win32-x64"、"universal"
	 *
	 * 検索応答は要求したプラットフォームを尊重しない（Open VSX の /api/-/search は
	 * targetPlatform を無視し、任意の 1 ビルドを返す）ので、この値は「検索が
	 * たまたま返した配布物」を表すに過ぎない。導入時にはメタデータ endpoint で
	 * 解決し直す必要がある。空なら不明。
	 */
	std::wstring	sTargetPlatform;

	std::wstring	sReadmeUrl;
	std::wstring	sChangelogUrl;

	//! "namespace.name" 形式の一意識別子。インストール先フォルダー名に使う
	std::wstring GetUniqueId() const { return sNamespace + L"." + sName; }

	//! 評価が存在するか
	bool HasRating() const noexcept { return dAverageRating >= 0.0; }
};

/*!
 * @brief 拡張のメタデータ endpoint が公開する配布物の所在
 *
 * `/api/{namespace}/{name}` は files（既定 1 ビルド分の download / sha256 / icon /
 * readme / changelog）と downloads（全 targetPlatform の VSIX URL）を返す。
 * 前者はプラットフォームを選べないが、後者は選べる。
 */
struct SOpenVsxExtensionAssets {
	std::wstring	sVersion;			//!< 既定ビルドのバージョン
	std::wstring	sTargetPlatform;	//!< files が指すビルドの targetPlatform
	std::wstring	sDownloadUrl;		//!< files.download
	std::wstring	sSha256Url;			//!< files.sha256
	std::wstring	sIconUrl;			//!< files.icon
	std::wstring	sReadmeUrl;			//!< files.readme
	std::wstring	sChangelogUrl;		//!< files.changelog

	//! targetPlatform と VSIX URL の対。JSON object を写すので targetPlatform 名の昇順
	std::vector<std::pair<std::wstring, std::wstring>>	downloads;
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

	//! プラットフォーム非依存な配布物を表す Open VSX の予約語
	static constexpr std::wstring_view kUniversalTargetPlatform = L"universal";

	//! このビルドが動作するホストの targetPlatform。VS Code が送るものと同じ語彙を使う。
	static constexpr std::wstring_view HostTargetPlatform() noexcept
	{
#if defined(_M_ARM64) || defined(__aarch64__)
		return L"win32-arm64";
#elif defined(_M_IX86) || defined(__i386__)
		return L"win32-ia32";
#else
		return L"win32-x64";
#endif
	}

	//! レジストリ URI の末尾の '/' を取り除く。URI の妥当性検査は transport 側の責務。
	static std::wstring NormalizeRegistryUrl(std::wstring registryUrl);

	/*!
	 * @brief 検索 endpoint URI を構築する。query は UTF-8 の RFC 3986 percent encoding を用いる。
	 *
	 * targetPlatform は VS Code と同じく常に送る。ただし open-vsx.org の現行実装は
	 * これを無視して任意のビルドを返すため、これだけでは正しい配布物は得られない。
	 * 導入時の解決は BuildExtensionMetadataUrl 経由で行うこと。
	 */
	static std::wstring BuildSearchUrl(
		std::wstring_view registryUrl,
		std::wstring_view query,
		int offset,
		int pageSize,
		std::wstring_view targetPlatform = HostTargetPlatform());

	/*!
	 * @brief 拡張メタデータ endpoint の URI を構築する
	 *
	 * 名前空間名と拡張名はそのまま path segment になる。percent encoding で
	 * 誤魔化すと元の識別子と別物を要求してしまうので、区切り文字（'/'・'\'・'%'・
	 * '?'・'#'・':'）や相対セグメント（"."・".."）を含む値は encode せずに拒否し、
	 * 空文字列を返して通信そのものを起こさせない。
	 */
	static std::wstring BuildExtensionMetadataUrl(
		std::wstring_view registryUrl,
		std::wstring_view namespaceName,
		std::wstring_view extensionName);

	//! Open VSX search JSON を public extension model に変換する。
	static bool ParseSearchResponse(const std::string& json, SOpenVsxSearchResult& result, std::wstring& errorMsg);

	//! 拡張メタデータ JSON を配布物の所在に変換する。
	static bool ParseExtensionMetadataResponse(
		const std::string& json,
		SOpenVsxExtensionAssets& assets,
		std::wstring& errorMsg);

	/*!
	 * @brief downloads から targetPlatform に合う VSIX URL を選ぶ
	 *
	 * 完全一致 → universal の順に探す。どちらも無ければ空文字列を返す。
	 * 「合うものが無い」ことと「たまたま別のプラットフォームの物が返ってきた」ことを
	 * 呼び出し元が区別できるよう、勝手な代替は選ばない。
	 */
	static std::wstring SelectPlatformDownloadUrl(
		const SOpenVsxExtensionAssets& assets,
		std::wstring_view targetPlatform);

	/*!
	 * @brief VSIX URL から、同じ配布物の sha256 URL を導く
	 *
	 * Open VSX は `<...>.vsix` と `<...>.sha256` を同じ path に並べて公開する。
	 * downloads は VSIX URL しか持たないので、整合性検証を維持するにはこの規則を使う。
	 * 拡張子が .vsix でなければ空文字列を返す（推測で URL を組み立てない）。
	 */
	static std::wstring DeriveSha256Url(std::wstring_view vsixUrl);

	/*!
	 * @brief VSIX URL に埋め込まれた targetPlatform を読み取る
	 *
	 * Open VSX はプラットフォーム別ビルドを
	 * `<name>-<version>@<targetPlatform>.vsix` というファイル名で公開する。
	 * 検索応答は targetPlatform フィールドを持たないことがあり、その場合
	 * URL のこの部分だけが「どのビルドなのか」を語る唯一の証拠になる。
	 *
	 * 実測（2026-08-06）: `/api/-/search?query=claude%20code` の
	 * `files.download` は win32 機からの要求でも
	 * `...@alpine-arm64.vsix` を返し、targetPlatform フィールドは含まない。
	 *
	 * @retval 空 `@` 修飾が無い（= プラットフォーム非依存の配布物）
	 */
	static std::wstring TargetPlatformFromVsixUrl(std::wstring_view vsixUrl);
};

} // namespace extension::openvsx
