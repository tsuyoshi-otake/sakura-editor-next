/*!	@file
	@brief ZIP file操作

*/
/*
	Copyright (C) 2011, Uchi
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#ifndef SAKURA_CZIPFILE_EA7F9762_A67F_449D_B346_EAB3075A9E2C_H_
#define SAKURA_CZIPFILE_EA7F9762_A67F_449D_B346_EAB3075A9E2C_H_
#pragma once

#include "cxx/com_pointer.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CZipFile {
private:
	using IShellDispatchPtr = cxx::com_pointer<IShellDispatch>;
	using FolderPtr = cxx::com_pointer<Folder>;

	using Me = CZipFile;

public:
	CZipFile();		// コンストラクタ
	CZipFile(const Me&) = delete;
	Me& operator = (const Me&) = delete;
	CZipFile(Me&&) noexcept = delete;
	Me& operator = (Me&&) noexcept = delete;
	~CZipFile();	// デストラクタ

	bool	IsOk() const noexcept { return (m_pShellDispatch != nullptr); }			// Zip Folderが使用できるか?
	bool	SetZip(const std::filesystem::path& zipPath);		// Zip File名 設定
	bool	Unzip(const std::filesystem::path& outDir);			// Zip File 解凍
	bool	ChkPluginDef(std::wstring_view defFileName, std::wstring& sFolderName);	// ZIP File 内 フォルダー名取得と定義ファイル検査(Plugin用)

	/*!
		@brief VSIX を検査して同期的に展開する

		Shell の CopyHere は非同期で、アーカイブ内のパスを呼び出し側で
		検査できないため、拡張機能の導入には使わない。中央ディレクトリを
		全件検査してから、サイズ上限を守って展開する。
	 */
	static bool ExtractVsixSafely(
		const std::filesystem::path& zipPath,
		const std::filesystem::path& outDir,
		std::wstring& errorMsg,
		const std::atomic<bool>* pCancelled = nullptr);

	//! VSIX の中央ディレクトリ名として安全か。通信・ファイル操作を伴わない
	static bool IsSafeArchiveEntryPath(std::string_view entryName);

	/*!
		@brief zlib ストリーム（RFC 1950）を展開する

		miniz の実体（`mz_uncompress` 等）はこのクラスの翻訳単位
		（CZipFile.cpp）だけがヘッダーガードなしで取り込んでいる。zlib 展開が
		必要な呼び出し元（例: WOFF1 フォントのテーブル展開）は、miniz を
		二重に実体化させないためこの関数を経由する。

		expandedSize は呼び出し側があらかじめ知っている展開後の正確なサイズ
		（WOFF テーブルディレクトリの origLength など）を渡すこと。展開結果の
		サイズがこれと一致しない場合は失敗として扱う。expandedSize が
		kMaxInflateExpandedBytes（64 MiB。単一 VSIX エントリの上限
		kMaxVsixEntryBytes と同じ値を踏襲）を超える場合は、小さい圧縮入力から
		巨大な確保を要求する zip 爆弾を防ぐため即座に拒否する。

		miniz のエラー・サイズ不一致を含め、失敗時は必ず false を返し out を
		空にする。例外は投げない（内部で捕捉する）。
	 */
	static bool InflateZlibStream(
		std::span<const std::byte> compressed,
		std::size_t expandedSize,
		std::vector<std::byte>& out) noexcept;

private:
	IShellDispatchPtr	m_pShellDispatch = nullptr;
	FolderPtr			m_pZipFolder = nullptr;
	std::wstring		m_ZipPath;
};

#endif /* SAKURA_CZIPFILE_EA7F9762_A67F_449D_B346_EAB3075A9E2C_H_ */
