/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tests1 {

/*!
	@brief テストが組み立て／読み出しする ZIP エントリ一件

	`content` はディレクトリエントリ（`name` が `/` で終わるもの）では空。
 */
struct ZipArchiveEntry
{
	std::string name;
	std::string content;
};

/*!
	@brief エントリ列から ZIP 書庫を作って `zipPath` に保存する
	@param zipPath 保存先。既存ファイルは置き換えられる
	@param entries 書き込む順のエントリ列
 */
void WriteZipArchive(const std::filesystem::path& zipPath, const std::vector<ZipArchiveEntry>& entries);

/*!
	@brief ZIP 書庫を一度だけ開いて全エントリを読み出す
	@param zipPath 読み出す書庫
	@return 書庫内の順のエントリ列。ディレクトリエントリの `content` は空
 */
std::vector<ZipArchiveEntry> ReadZipArchive(const std::filesystem::path& zipPath);

/*!
	@brief 本物の RFC 1950 zlib ストリームへ圧縮する

	`CZipFile::InflateZlibStream` の入力を、展開側と別の実装で作るためのもの。
	圧縮に失敗した場合は空を返す（呼び出し側が ASSERT で捕まえられるように、
	ここでは GoogleTest のマクロを使わない）。
 */
std::vector<std::byte> CompressZlibStream(std::string_view source);

} // namespace tests1
