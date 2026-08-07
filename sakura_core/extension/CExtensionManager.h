/*!	@file
	@brief VS Code 互換拡張の導入と管理

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CEXTENSIONMANAGER_D82F4A16_9E70_4C3B_B1A8_2D5C6E4F8073_H_
#define SAKURA_CEXTENSIONMANAGER_D82F4A16_9E70_4C3B_B1A8_2D5C6E4F8073_H_
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "extension/openvsx/IOpenVsxRegistryClient.h"

//! 拡張が信頼されていないワークスペースをどこまで支える宣言をしているか
enum class EExtensionUntrustedWorkspaceSupport {
	Supported,      //!< 制限モードでもそのまま動かしてよい
	Limited,        //!< 動かしてよいが、一部の設定は差し止められる
	NotSupported,   //!< 制限モードでは読み込んではならない
};

//! 導入済み拡張
struct SInstalledExtension {
	std::wstring			sUniqueId;		//!< "namespace.name"
	std::wstring			sVersion;		//!< バージョン
	std::wstring			sDisplayName;	//!< 表示名。package.json から読めなければ sUniqueId
	std::filesystem::path	dir;			//!< 導入先フォルダー
	//! 信頼されていないワークスペースへの対応。package.json から読めなければ NotSupported（fail closed）
	EExtensionUntrustedWorkspaceSupport untrustedWorkspaceSupport = EExtensionUntrustedWorkspaceSupport::NotSupported;
	/*!
		@brief capabilities.untrustedWorkspaces.restrictedConfigurations の宣言

		制限モードでも untrustedWorkspaceSupport が Limited のまま読み込んでよいが、
		ここに挙げたキーはワークスペース・スコープの値を効かせてはならない、という
		拡張自身の申告。package.json から読めなければ（宣言なし・型違い・不正 JSON の
		いずれでも）空になる。空は「制限なし」を意味してしまうため、この一覧だけでは
		Limited 拡張の安全性を語れない -- untrustedWorkspaceSupport 側の fail closed
		（読めなければ NotSupported）と組み合わせて初めて意味を持つ。
	*/
	std::vector<std::string> restrictedConfigurations;
};

/*!
	@brief 拡張の導入と管理

	既存のプラグイン導入（CPluginManager::InstZipPluginSub）と同じ流れを踏襲する。
	すなわち「取得 → 内容の検査 → 展開 → 登録」の順で、検査に通らないものは展開しない。

	導入先は ini と同じ階層の extensions フォルダー。plugins フォルダーの慣習に倣う。

	@note 拡張を「実行」する仕組み（拡張ホスト）は本クラスの責務ではない。
		ここは取得と配置だけを行う。
 */
class CExtensionManager {
public:
	//! VSIX 内で拡張本体が入っているフォルダー名
	static constexpr const wchar_t* kVsixContentDir = L"extension";

	//! 拡張のマニフェスト
	static constexpr const wchar_t* kManifestFileName = L"package.json";

	CExtensionManager();

	//! 明示的な導入先を使う。隔離された integration/test profile と埋め込み用途向け。
	explicit CExtensionManager(std::filesystem::path baseDir);

	//! 導入先フォルダー（末尾は区切り文字なし）
	const std::filesystem::path& GetBaseDir() const noexcept { return m_baseDir; }

	/*!
		@brief 拡張を導入する（通信を行う）

		VSIX を取得し、sha256 が公開されていれば検証し、展開して
		マニフェストの存在を確認する。途中で失敗した場合は導入先を残さない。

		@param[in]  ext		導入する拡張
		@param[in]  registryClient	Open VSX の型付き取得境界。HTTP/proxy 設定はここへ閉じ込める
		@param[out] errorMsg	失敗理由（未ローカライズの技術的詳細）
		@param[in]  requestCancellation	共有 request の中止 token。無ければ request 側の中止は行わない
		@param[in]  pCancelled	既存の install/uninstall worker 用中止フラグ
	*/
	bool Install(
		const SOpenVsxExtension& ext,
		extension::openvsx::IOpenVsxRegistryClient& registryClient,
		std::wstring& errorMsg,
		const platform::request::IRequestCancellation* requestCancellation = nullptr,
		const std::atomic<bool>* pCancelled = nullptr);

	//! 導入済み拡張を列挙する
	std::vector<SInstalledExtension> EnumInstalled() const;

	//! 導入済み拡張を削除する
	bool Uninstall(
		const std::wstring& sUniqueId,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

	//! 指定 ID が導入済みか。導入済みならそのバージョンを返す
	bool FindInstalled(const std::wstring& sUniqueId, SInstalledExtension& found) const;

	// -- -- 以下は通信・ファイル操作を伴わないため単体で検証できる -- -- //

	/*!
		@brief レジストリ由来の文字列をフォルダー名の一部として使って安全か

		導入先フォルダー名はレジストリの応答から組み立てるため、ここが
		パス・トラバーサルの入口になる。区切り文字・相対指定・制御文字・
		Windows の予約デバイス名などをすべて拒否する。
	*/
	static bool IsSafeNameComponent(std::wstring_view sComponent);

	/*!
		@brief 導入先フォルダー名を決める
		@retval 空 いずれかの構成要素が安全でないため名前を決められない
	*/
	static std::wstring MakeInstallFolderName(const SOpenVsxExtension& ext);

	/*!
		@brief sha256 ファイルの内容から 64 桁の 16 進ハッシュを取り出す

		レジストリが返す内容にファイル名などが併記されていても取り出せるよう、
		空白区切りの最初の 64 桁 16 進トークンを採用する。
		@retval 空 取り出せなかった
	*/
	static std::wstring ExtractSha256Hex(const std::string& sSha256FileBody);

	//! ファイルの sha256 を 16 進小文字で求める。失敗時は空
	static std::wstring ComputeSha256Hex(const std::filesystem::path& path);

	/*!
		@brief 検索応答とメタデータ応答から、このホストで動く配布物を決める

		Open VSX の `/api/-/search` は targetPlatform を無視して任意の 1 ビルドを
		返すため、検索結果の download URL をそのまま使うと別プラットフォームの
		VSIX を掴む。メタデータ endpoint の downloads で解決し直すのがこの関数。

		通信を行わないので単体で検証できる。

		@param[in]  ext				検索応答由来の拡張。download URL は信用しない
		@param[in]  assets			メタデータ endpoint の結果。Unsupported なら ext へ退避
		@param[in]  targetPlatform	このホストが必要とする targetPlatform
		@param[out] resolved		実際に取得すべき配布物で上書きした拡張
		@param[out] errorMsg		解決できなかった理由
		@retval false このホスト向けの配布物が無い（別プラットフォームを掴ませない）
	*/
	static bool ResolveInstallTarget(
		const SOpenVsxExtension& ext,
		const extension::openvsx::OpenVsxExtensionAssetsOperation& assets,
		std::wstring_view targetPlatform,
		SOpenVsxExtension& resolved,
		std::wstring& errorMsg);

	/*! @brief package.json 本文から untrustedWorkspaces の宣言を読む */
	static EExtensionUntrustedWorkspaceSupport ParseUntrustedWorkspaceSupport(const std::string& sManifestJson);

	//! 1 拡張が capabilities.untrustedWorkspaces.restrictedConfigurations に宣言できる項目数の上限。
	//! ParseRestrictedConfigurations がこれを超えた分を切り捨てる境界であり、テストからも参照できるよう公開する。
	static constexpr size_t kMaxRestrictedConfigurationEntries = 128;

	/*!
		@brief package.json 本文から capabilities.untrustedWorkspaces.restrictedConfigurations を読む

		ParseUntrustedWorkspaceSupport と同じ入力（本文の文字列そのもの）を受け取る純粋関数。
		宣言が無い・型が違う・JSON が壊れているなど、あらゆる読み取れない形は「制限なし」ではなく
		空の一覧として fail closed する（＝より少ない制限を勝手に発明しない）。文字列でないメンバーは
		そのメンバーだけを読み捨て、一覧全体は打ち切らない。
	*/
	static std::vector<std::string> ParseRestrictedConfigurations(const std::string& sManifestJson);

private:
	//! package.json をバイト境界（kMaxManifestBytes）内で読む。読めなければ空
	static std::string ReadManifestBody(const std::filesystem::path& manifestPath);

	//! package.json 本文から表示名を読む。読めなければ空
	static std::wstring ReadDisplayNameFromBody(const std::string& sManifestJson);

	std::filesystem::path	m_baseDir;	//!< extensions フォルダー
};

#endif /* SAKURA_CEXTENSIONMANAGER_D82F4A16_9E70_4C3B_B1A8_2D5C6E4F8073_H_ */
