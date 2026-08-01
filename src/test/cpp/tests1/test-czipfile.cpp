/*! @file */
/*
	Copyright (C) 2021-2025, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "io/CZipFile.h"

#include "cxx/lock_resource.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <locale>
#include <string>
#include <vector>

#define MINIZ_HEADER_FILE_ONLY
#include <miniz-cpp/zip_file.hpp>
#undef MINIZ_HEADER_FILE_ONLY

#include "util/file.h"

#include "tests1_rc.h"
#include "rt_zipres.h"

using BinarySequence = std::basic_string<std::byte>;
using BinarySequenceView = std::basic_string_view<std::byte>;

/*!
	バイナリデータをファイルに書き込む

	@param [in] bin バイナリデータ
	@param [in] path 書き込み先ファイルパス
 */
bool WriteBinaryToFile(BinarySequenceView bin, std::filesystem::path path)
{
	if (bin.empty()) {
		return false;
	}

	try {
		// 内部的なストリームインスタンスを用意する
		// std::byteでパラメータ化したstd::basic_ofstreamだとMinGWビルドが動作しないので、
		// あえて標準の1バイト実装を使う
		std::ofstream os{ path, std::ios::binary | std::ios::trunc };

		if (!os) {
			return false;
		}

		os.write(std::bit_cast<const char*>(bin.data()), bin.length());
	}
	catch (...) {
		return false;
	}

	return true;
}

/*!
 * 新しいテンポラリファイルパスを生成する
 * （拡張子を指定できる特殊バージョン）
 *
 * CZipFileが依存するIShellDispatchのZIP展開機能には
 * 拡張子がzipでないアーカイブを解凍できない
 * の制約があるためで作成した。
 * 
 * @param [in] prefix ファイル名の前に付ける3文字の接頭辞。
 * @param [in] extension ファイルの拡張子（.zipを指定する）。
 */
std::filesystem::path GetTempFilePathWithExt(std::wstring_view prefix, std::wstring_view extension)
{
	std::error_code ec;

	// 1回だけリトライする
	for (auto n = 0; n <= 1; ++n) {
		// 拡張子指定なし版を呼び出す
		auto tempPath = GetTempFilePath(prefix);

		// 作成された一時ファイルを削除する
		std::filesystem::remove(tempPath, ec);

		tempPath.replace_extension(extension.data());

		if (std::error_code ec; !std::filesystem::exists(tempPath, ec)) {
			return tempPath;
		}
	}

	return {};
}

void extract_zip(
	const std::filesystem::path& zipPath,
	const std::filesystem::path& outDir
)
{
	// 出力先ディレクトリを作成する
	std::filesystem::create_directories(outDir);

	miniz_cpp::zip_file z(zipPath.string());

	for (const auto& name : z.namelist()) {
		const auto outPath = outDir / std::filesystem::path(name);

		// base 配下に収まってるか（../ 脱出対策）
		auto b = std::filesystem::weakly_canonical(outDir);
		auto x = std::filesystem::weakly_canonical(outPath);

		// 文字列比較で prefix 判定（簡易だが実用十分）
		auto& bs = b.native();
		if (auto& xs = x.native(); xs.size() < bs.size() || !xs.starts_with(bs)) {
			throw std::domain_error(std::format("skip dangerous entry: {}", name));
		}

        // ZIP のディレクトリエントリを考慮
        if (!name.empty() && !outPath.has_filename()) {
			std::filesystem::create_directories(outPath);
            continue;
        }

		if (const auto parentDir = outPath.parent_path(); !fexist(parentDir)) {
			std::filesystem::create_directories(parentDir);
		}

        const auto data = z.read(name); // 展開済みバイト列が std::string で返る

        std::ofstream ofs(outPath, std::ios::binary);
        if (!ofs) {
            std::cerr << "failed to open: " << outPath << "\n";
            continue;
        }

		ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
}

void extract_zip_resource(
	WORD id,
	const std::optional<std::filesystem::path>& optOutDir
)
{
	std::error_code ec;

	// 一時ファイル名を生成する
	auto tempPath = GetTempFilePath(L"tes");

	// リソースからzipファイルデータを抽出する
	const auto bin = cxx::lock_resource<std::byte>(
		id,
		[] (std::span<const std::byte> resData) {
			return BinarySequence(resData.begin(), resData.end());
		},
		RT_ZIPRES
	);

	// 取得したzipファイルデータを一時ファイルに書き込む
	WriteBinaryToFile(bin, tempPath);
	assert(std::filesystem::exists(tempPath));

	extract_zip(tempPath, optOutDir.value_or(GetIniFileName().remove_filename()));

	// 作成した一時ファイルを削除する
	std::filesystem::remove(tempPath, ec);
}

/*!
 * @brief CZipFIleのテスト
 */
TEST(CZipFile, DISABLED_IsNG) // 安定しないので無効化する
{
	// IShellDispatchを使うためにOLEを初期化する必要がある
	// このテストでは初期化を忘れた場合の挙動を確認する
	CZipFile cZipFile;
	EXPECT_FALSE(cZipFile.IsOk());

	// この場合、他のメソッドを呼び出すと落ちる。
}

//! 中央ディレクトリ内の名前は展開前に Windows のパス規則で拒否する
TEST(CZipFile, IsSafeArchiveEntryPath)
{
	EXPECT_TRUE(CZipFile::IsSafeArchiveEntryPath("extension/package.json"));
	EXPECT_TRUE(CZipFile::IsSafeArchiveEntryPath("extension/media/icon.png"));
	EXPECT_TRUE(CZipFile::IsSafeArchiveEntryPath("extension/"));

	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("../package.json"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/../../outside"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("/absolute/path"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("C:/absolute/path"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/file:stream"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension\\backslash"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension//duplicate-separator"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/CON/file"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/NUL.txt"));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/."));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/name."));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/name "));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath("extension/name. "));
	EXPECT_FALSE(CZipFile::IsSafeArchiveEntryPath(std::string_view("extension/a\0b", 13)));
}

//! 安全な VSIX だけを同期展開し、zip-slip は展開前に拒否する
TEST(CZipFile, ExtractVsixSafely_ValidatesAllCentralDirectoryEntriesBeforeWriting)
{
	const auto zipPath = GetTempFilePath(L"vxs");
	const auto outputDir = GetTempFilePath(L"vxo");
	const auto outsidePath = outputDir.parent_path() / L"outside.txt";
	std::error_code ec;
	std::filesystem::remove(zipPath, ec);
	std::filesystem::remove(outputDir, ec);
	std::filesystem::remove(outsidePath, ec);

	{
		miniz_cpp::zip_file archive;
		archive.writestr("extension/package.json", "{\"name\":\"safe\"}");
		archive.writestr("extension/readme.md", "safe content");
		archive.save(zipPath.string());
	}
	std::wstring errorMsg;
	ASSERT_TRUE(CZipFile::ExtractVsixSafely(zipPath, outputDir, errorMsg)) << errorMsg;
	EXPECT_TRUE(std::filesystem::exists(outputDir / L"extension" / L"package.json"));
	std::filesystem::remove_all(outputDir, ec);

	{
		miniz_cpp::zip_file archive;
		archive.writestr("extension/package.json", "{\"name\":\"unsafe\"}");
		archive.writestr("../outside.txt", "must not be written");
		archive.save(zipPath.string());
	}
	errorMsg.clear();
	EXPECT_FALSE(CZipFile::ExtractVsixSafely(zipPath, outputDir, errorMsg));
	EXPECT_FALSE(errorMsg.empty());
	EXPECT_FALSE(std::filesystem::exists(outsidePath));
	EXPECT_FALSE(std::filesystem::exists(outputDir / L"extension" / L"package.json"));

	std::filesystem::remove(zipPath, ec);
	std::filesystem::remove_all(outputDir, ec);
}

//! Windows で同じ出力名になるエントリや必須マニフェスト不足を拒否する
TEST(CZipFile, ExtractVsixSafely_RejectsAmbiguousOrIncompleteArchives)
{
	const auto zipPath = GetTempFilePath(L"vxa");
	const auto outputDir = GetTempFilePath(L"vxb");
	std::error_code ec;
	std::filesystem::remove(zipPath, ec);
	std::filesystem::remove(outputDir, ec);

	{
		miniz_cpp::zip_file archive;
		archive.writestr("extension/package.json", "{\"name\":\"safe\"}");
		archive.writestr("extension/PACKAGE.JSON", "{\"name\":\"duplicate\"}");
		archive.save(zipPath.string());
	}
	std::wstring errorMsg;
	EXPECT_FALSE(CZipFile::ExtractVsixSafely(zipPath, outputDir, errorMsg));
	EXPECT_FALSE(std::filesystem::exists(outputDir / L"extension" / L"package.json"));
	std::filesystem::remove_all(outputDir, ec);

	{
		miniz_cpp::zip_file archive;
		archive.writestr("extension/readme.md", "missing manifest");
		archive.save(zipPath.string());
	}
	errorMsg.clear();
	EXPECT_FALSE(CZipFile::ExtractVsixSafely(zipPath, outputDir, errorMsg));
	EXPECT_FALSE(std::filesystem::exists(outputDir / L"extension" / L"readme.md"));

	std::filesystem::remove(zipPath, ec);
	std::filesystem::remove_all(outputDir, ec);
}

//! 事前に取り消された展開は出力ディレクトリさえ作らない
TEST(CZipFile, ExtractVsixSafely_HonorsPreCancellation)
{
	const auto zipPath = GetTempFilePath(L"vxc");
	const auto outputDir = GetTempFilePath(L"vxd");
	std::error_code ec;
	std::filesystem::remove(zipPath, ec);
	std::filesystem::remove(outputDir, ec);
	{
		miniz_cpp::zip_file archive;
		archive.writestr("extension/package.json", "{\"name\":\"safe\"}");
		archive.save(zipPath.string());
	}

	std::atomic<bool> cancelled{ true };
	std::wstring errorMsg;
	EXPECT_FALSE(CZipFile::ExtractVsixSafely(zipPath, outputDir, errorMsg, &cancelled));
	EXPECT_FALSE(errorMsg.empty());
	EXPECT_FALSE(std::filesystem::exists(outputDir));

	std::filesystem::remove(zipPath, ec);
}

/*!
 * @brief CZipFIleのテスト
 */
TEST(CZipFile, CZipFIle)
{
	std::error_code ec;

	// IShellDispatchを使うためにOLEを初期化する
	if (FAILED(::OleInitialize(nullptr))) {
		FAIL();
	}
	else {
		// インスタンス作成時にOLEが初期化されていればIsOkはtrueを返す
		CZipFile cZipFile;
		ASSERT_TRUE(cZipFile.IsOk());

		std::wstring folderName;
		EXPECT_FALSE(cZipFile.ChkPluginDef(L"plugin.def", folderName));

		EXPECT_FALSE(cZipFile.Unzip(L"out"));

		// 一時ファイル名を生成する
		// zipファイルパスの拡張子はzipにしないと動かない。
		auto tempPath = GetTempFilePathWithExt(L"tes", L"zip");

		// リソースからzipファイルデータを抽出して一時ファイルに書き込む
		const auto bin = cxx::lock_resource<std::byte>(
			IDR_ZIPRES1,
			[] (std::span<const std::byte> resData) {
				return BinarySequence(resData.begin(), resData.end());
			},
			RT_ZIPRES
		);
		ASSERT_FALSE(bin.empty());
		ASSERT_TRUE(WriteBinaryToFile(bin, tempPath));
		ASSERT_TRUE(std::filesystem::exists(tempPath));

		// zipファイルパスを設定する
		EXPECT_TRUE(cZipFile.SetZip(tempPath.c_str()));

		// プラグイン設定があるかチェックする
		EXPECT_TRUE(cZipFile.ChkPluginDef(L"plugin.def", folderName));

		// zipファイルを解凍する
		// 展開自体はWindowsの機能なので、展開後パスの存在チェックのみ行う
		const auto dest = std::filesystem::current_path().append(L"unzipped").append(L"");
		std::filesystem::create_directories(dest);
		EXPECT_TRUE(cZipFile.Unzip(dest.c_str()));
		EXPECT_TRUE(std::filesystem::exists(dest / folderName.c_str() / L"plugin.def"));
		std::filesystem::remove_all(dest, ec);

		// 意図的に失敗させる
		EXPECT_FALSE(cZipFile.Unzip(GetExeFileName()));

		// zipファイルパスをクリアする
		EXPECT_TRUE(cZipFile.SetZip(L""));

		// 存在しないzipファイルパスを設定する
		EXPECT_FALSE(cZipFile.SetZip(L"not found"));

		// 作成した一時ファイルを削除する
		std::filesystem::remove(tempPath, ec);
	}

	// OLEをシャットダウンする
	::OleUninitialize();
}

// InflateZlibStream() 用のテストデータ生成ヘルパー。
// mz_compress() は宣言（このファイル冒頭の MINIZ_HEADER_FILE_ONLY 付き
// include）のみを見ており、実体は CZipFile.cpp が唯一ガードなしで取り込む
// 側にある。両者は同一ヘッダーテキストの extern "C" 宣言を経由するため
// リンク時に解決できる。テスト専用の圧縮 API を新たに増やす代わりに、
// この既存の再利用パターン（miniz_cpp::zip_file 等ですでに使われている
// ものと同じ仕組み）を使い、本物の RFC 1950 zlib ストリームを合成する。
std::vector<std::byte> CompressWithMzForTest(std::string_view source)
{
	mz_ulong destLen = mz_compressBound(static_cast<mz_ulong>(source.size()));
	std::vector<std::byte> dest(destLen);
	const int status = mz_compress(
		reinterpret_cast<unsigned char*>(dest.data()), &destLen,
		reinterpret_cast<const unsigned char*>(source.data()), static_cast<mz_ulong>(source.size()));
	EXPECT_EQ(MZ_OK, status);
	dest.resize(destLen);
	return dest;
}

//! zlib（RFC 1950）ストリームを展開し、期待サイズと一致した場合だけ成功する
TEST(CZipFile, InflateZlibStream_RoundTripsAndRejectsSizeMismatch)
{
	std::string payload;
	payload.reserve(4096);
	for (size_t i = 0; i < 4096; ++i) {
		payload.push_back(static_cast<char>('A' + (i % 7)));
	}
	const std::vector<std::byte> compressed = CompressWithMzForTest(payload);
	ASSERT_FALSE(compressed.empty());

	std::vector<std::byte> inflated;
	ASSERT_TRUE(CZipFile::InflateZlibStream(compressed, payload.size(), inflated));
	ASSERT_EQ(payload.size(), inflated.size());
	for (size_t i = 0; i < payload.size(); ++i) {
		ASSERT_EQ(static_cast<std::byte>(payload[i]), inflated[i]) << "mismatch at offset " << i;
	}

	// 呼び出し側が伝える期待サイズが実際の展開結果とずれている場合は、
	// miniz が成功していても失敗として扱い、out を空にする
	std::vector<std::byte> tooSmall;
	EXPECT_FALSE(CZipFile::InflateZlibStream(compressed, payload.size() - 1, tooSmall));
	EXPECT_TRUE(tooSmall.empty());

	std::vector<std::byte> tooLarge;
	EXPECT_FALSE(CZipFile::InflateZlibStream(compressed, payload.size() + 1, tooLarge));
	EXPECT_TRUE(tooLarge.empty());
}

//! 壊れた、または切り詰められたストリームはクラッシュせず失敗として扱う
TEST(CZipFile, InflateZlibStream_RejectsCorruptOrTruncatedStream)
{
	const std::string_view payload = "InflateZlibStream corrupt-stream regression payload";
	const std::vector<std::byte> compressed = CompressWithMzForTest(payload);
	ASSERT_GT(compressed.size(), 4u);

	// 末尾を切り詰めた不完全なストリーム
	const std::vector<std::byte> truncated(compressed.begin(), compressed.end() - 4);
	std::vector<std::byte> outTruncated;
	EXPECT_FALSE(CZipFile::InflateZlibStream(truncated, payload.size(), outTruncated));
	EXPECT_TRUE(outTruncated.empty());

	// 中間バイトを反転させた破損ストリーム
	std::vector<std::byte> corrupted = compressed;
	corrupted[corrupted.size() / 2] ^= std::byte{ 0xFF };
	std::vector<std::byte> outCorrupted;
	EXPECT_FALSE(CZipFile::InflateZlibStream(corrupted, payload.size(), outCorrupted));
	EXPECT_TRUE(outCorrupted.empty());
}

//! 空入力・ゼロサイズ・文書化された上限を超えるサイズは miniz を呼び出す前に拒否する
TEST(CZipFile, InflateZlibStream_RejectsDegenerateInputsWithoutAllocating)
{
	std::vector<std::byte> out;

	// 圧縮データが空
	EXPECT_FALSE(CZipFile::InflateZlibStream(std::span<const std::byte>{}, 16, out));
	EXPECT_TRUE(out.empty());

	const std::vector<std::byte> dummy(4, std::byte{ 0 });

	// 期待展開サイズがゼロ
	EXPECT_FALSE(CZipFile::InflateZlibStream(dummy, 0, out));
	EXPECT_TRUE(out.empty());

	// クラスが文書化している 64 MiB の上限を大きく超えるサイズは、
	// 巨大なバッファ確保を試みる前に即座に拒否されなければならない
	EXPECT_FALSE(CZipFile::InflateZlibStream(dummy, 1ull * 1024 * 1024 * 1024, out));
	EXPECT_TRUE(out.empty());
}
