/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "ZipArchiveFixture.h"

// このファイルは tests1 で <miniz-cpp/zip_file.hpp> を include してよい唯一の翻訳単位。
//
// miniz-cpp のヘッダは二種類の実体を同時に吐く。
//
//  1. miniz の C API 実装（mz_* 群）。これは CZipFile.cpp が製品側で唯一ガードなしに
//     取り込んでいるので、テスト側では MINIZ_HEADER_FILE_ONLY で宣言だけに絞り、実体は
//     CZipFile.obj のものへリンクさせる。ガードを外すと mz_adler32 などが CZipFile.obj
//     と重複して LNK2005 になる。
//  2. C++ ラッパー miniz_cpp::detail::* の非 inline な関数・変数。これは
//     MINIZ_HEADER_FILE_ONLY では抑止されない。CZipFile.cpp は名前空間ごと
//     sakura_czip_miniz_cpp へ退避しているので製品側とは衝突しないが、tests1 の中で
//     二つ以上のテストがこのヘッダを取り込むと互いに重複定義になる。
//
// 2 が、ZIP を使うテストが一つしかなかった間ずっと見えていなかった制約。externals/ は
// 上流コードなのでヘッダ側では直せない。したがって「取り込み元をこの一つに固定し、
// テストは下の関数だけを使う」のが唯一の一方向な解決になる。新しくテストで ZIP や
// zlib を扱いたくなったら、ここを include せずにこのヘッダへ API を足すこと。
#define MINIZ_HEADER_FILE_ONLY
#include <miniz-cpp/zip_file.hpp>
#undef MINIZ_HEADER_FILE_ONLY

namespace tests1 {

namespace {

//! ZIP のディレクトリエントリか（中身を持たないので read してはいけない）
bool IsDirectoryEntry(std::string_view name) noexcept
{
	return !name.empty() && name.back() == '/';
}

} // namespace

void WriteZipArchive(const std::filesystem::path& zipPath, const std::vector<ZipArchiveEntry>& entries)
{
	miniz_cpp::zip_file archive;
	for (const auto& entry : entries) {
		archive.writestr(entry.name, entry.content);
	}
	archive.save(zipPath.string());
}

std::vector<ZipArchiveEntry> ReadZipArchive(const std::filesystem::path& zipPath)
{
	// 書庫を開くのは一度だけ。エントリごとに開き直すと、テストの読み出しが
	// エントリ数に対して二乗で重くなる。
	miniz_cpp::zip_file archive(zipPath.string());

	std::vector<ZipArchiveEntry> entries;
	for (const auto& name : archive.namelist()) {
		ZipArchiveEntry entry;
		entry.name = name;
		if (!IsDirectoryEntry(name)) {
			entry.content = archive.read(name);
		}
		entries.push_back(std::move(entry));
	}
	return entries;
}

std::vector<std::byte> CompressZlibStream(std::string_view source)
{
	mz_ulong destLen = mz_compressBound(static_cast<mz_ulong>(source.size()));
	std::vector<std::byte> dest(destLen);
	const int status = mz_compress(
		reinterpret_cast<unsigned char*>(dest.data()), &destLen,
		reinterpret_cast<const unsigned char*>(source.data()), static_cast<mz_ulong>(source.size()));
	if (status != MZ_OK) {
		return {};
	}
	dest.resize(destLen);
	return dest;
}

} // namespace tests1
