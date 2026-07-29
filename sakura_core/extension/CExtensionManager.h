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

#include <filesystem>
#include <string>
#include <vector>

#include "extension/COpenVsxClient.h"

//! 導入済み拡張
struct SInstalledExtension {
	std::wstring			sUniqueId;		//!< "namespace.name"
	std::wstring			sVersion;		//!< バージョン
	std::wstring			sDisplayName;	//!< 表示名。package.json から読めなければ sUniqueId
	std::filesystem::path	dir;			//!< 導入先フォルダー
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

	//! 導入先フォルダー（末尾は区切り文字なし）
	const std::filesystem::path& GetBaseDir() const noexcept { return m_baseDir; }

	/*!
		@brief 拡張を導入する（通信を行う）

		VSIX を取得し、sha256 が公開されていれば検証し、展開して
		マニフェストの存在を確認する。途中で失敗した場合は導入先を残さない。

		@param[in]  ext		導入する拡張
		@param[out] errorMsg	失敗理由（未ローカライズの技術的詳細）
	*/
	bool Install(const SOpenVsxExtension& ext, std::wstring& errorMsg);

	//! 導入済み拡張を列挙する
	std::vector<SInstalledExtension> EnumInstalled() const;

	//! 導入済み拡張を削除する
	bool Uninstall(const std::wstring& sUniqueId, std::wstring& errorMsg);

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

private:
	//! Shell による展開は非同期なので、想定の成果物が現れるまで待つ
	static bool WaitForExtracted(const std::filesystem::path& marker, std::wstring& errorMsg);

	//! package.json から表示名を読む。読めなければ空
	static std::wstring ReadDisplayName(const std::filesystem::path& manifestPath);

	std::filesystem::path	m_baseDir;	//!< extensions フォルダー
};

#endif /* SAKURA_CEXTENSIONMANAGER_D82F4A16_9E70_4C3B_B1A8_2D5C6E4F8073_H_ */
