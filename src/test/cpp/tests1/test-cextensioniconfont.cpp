/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/icons/CExtensionIconFont.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "util/file.h"

// mz_compress/mz_compressBound/MZ_OK 等の宣言だけを取り込む。miniz の C 実装は
// MINIZ_HEADER_FILE_ONLY で抑止され、実体は CZipFile.cpp のみが持つ。
//
// ただし MINIZ_HEADER_FILE_ONLY が抑止するのは C 側だけで、miniz_cpp::detail の
// C++ ヘルパー（join_path / split_path / crc32buf / directory_separator 等）は
// 外部リンケージのまま定義される。tests1 には zip_file.hpp を素の名前空間名で
// 取り込む test-czipfile.cpp が既にいるため、そのまま include すると LNK2005 に
// なる。CZipFile.cpp と同じく名前空間を私有名へ退避して衝突を避ける。
#define MINIZ_HEADER_FILE_ONLY
#define miniz_cpp sakura_icontest_miniz_cpp
#include <miniz-cpp/zip_file.hpp>
#undef miniz_cpp
#undef MINIZ_HEADER_FILE_ONLY

namespace icons = workbench::icons;
namespace icons_detail = workbench::icons::detail;

// このテストファイルは CExtensionIconFontRegistry / detail 名前空間の
// WOFF1・sfnt・fontCharacter・パス検証ロジックを検証する。
//
// AddFontMemResourceEx への依存について: CExtensionIconFontRegistry::Find が
// 実際にアイコンを解決できるかどうかは、最終的に Win32 の
// AddFontMemResourceEx が渡されたフォントを受理するかに懸かっている。
// この関数はディスプレイもウィンドウも要求しない（プロセス private な
// GDI フォントリソースの登録であり、UI を必要としない）が、name テーブル
// だけを持つ合成フォントは head/maxp/cmap 等の必須テーブルを一切持たない
// ため、GDI は構造的に必ず拒否する。これは環境依存ではなく、どのマシンでも
// 常に失敗する。したがってレジストリレベルのテスト（下記
// `==== CExtensionIconFontRegistry ====`）は、AvailableSystemTrueTypeFonts()
// が %WINDIR%\Fonts から読み込む本物の TrueType フォント（arial.ttf 等）を
// TempDirectory 配下へコピーして使う。detail::LoadFontAsSfnt は TTF/OTF/TTC
// をマジックのみ検証してそのまま通すため、これは「本物のフォントファイルが
// パス解決からレジストリ登録・ファミリー名抽出まで正しく通る」ことも同時に
// 検証する。システムフォントが 1 つも読めなかった場合に限り GTEST_SKIP() する
// ——これは本当に環境起因の条件である。
//
// 一方、detail::LoadFontAsSfnt / detail::ExtractFamilyName に対するテスト群
// （下記 `==== detail::LoadFontAsSfnt ====` / `==== detail::ExtractFamilyName ====`）
// は AddFontMemResourceEx を一切経由せず、合成した WOFF1/sfnt バイト列だけで
// デコード・ファミリー名抽出の正しさを直接検証する。こちらは環境に依存しない
// ため、引き続き合成フィクスチャ（BuildWoff1 / BuildBareSfnt 等）を使う。
namespace {

//
// ---- バイト列組み立ての基礎ヘルパー ----
//

void PushU8(std::vector<std::byte>& v, std::uint8_t value)
{
	v.push_back(static_cast<std::byte>(value));
}

void PushU16BE(std::vector<std::byte>& v, std::uint16_t value)
{
	v.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
	v.push_back(static_cast<std::byte>(value & 0xFF));
}

void PushU32BE(std::vector<std::byte>& v, std::uint32_t value)
{
	v.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
	v.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
	v.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
	v.push_back(static_cast<std::byte>(value & 0xFF));
}

void WriteU32BEAt(std::vector<std::byte>& v, std::size_t offset, std::uint32_t value)
{
	v[offset + 0] = static_cast<std::byte>((value >> 24) & 0xFF);
	v[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFF);
	v[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFF);
	v[offset + 3] = static_cast<std::byte>(value & 0xFF);
}

void WriteU16BEAt(std::vector<std::byte>& v, std::size_t offset, std::uint16_t value)
{
	v[offset + 0] = static_cast<std::byte>((value >> 8) & 0xFF);
	v[offset + 1] = static_cast<std::byte>(value & 0xFF);
}

//! ビッグエンディアンの 4 文字タグを組み立てる（16進の書き間違いを避けるため）
constexpr std::uint32_t MakeTag(char a, char b, char c, char d)
{
	return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 16) |
		(static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 8) |
		static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));
}

constexpr std::uint32_t kTestTrueTypeMagic = 0x00010000u;
constexpr std::uint32_t kTestOttoMagic = MakeTag('O', 'T', 'T', 'O');
constexpr std::uint32_t kTestTtcMagic = MakeTag('t', 't', 'c', 'f');
constexpr std::uint32_t kTestMacTrueMagic = MakeTag('t', 'r', 'u', 'e');
constexpr std::uint32_t kTestWoffMagic = MakeTag('w', 'O', 'F', 'F');
constexpr std::uint32_t kTestWoff2Magic = MakeTag('w', 'O', 'F', '2');
constexpr std::uint32_t kTestNameTag = MakeTag('n', 'a', 'm', 'e');

std::vector<std::byte> CompressBytesWithMzForTest(std::span<const std::byte> source)
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

//
// ---- name テーブル(format 0) 組み立てヘルパー ----
//

std::vector<std::byte> EncodeUtf16BE(std::wstring_view text)
{
	std::vector<std::byte> out;
	out.reserve(text.size() * 2);
	for (const wchar_t ch : text) {
		PushU16BE(out, static_cast<std::uint16_t>(ch));
	}
	return out;
}

std::vector<std::byte> EncodeLatin1(std::string_view text)
{
	std::vector<std::byte> out;
	out.reserve(text.size());
	for (const char ch : text) {
		PushU8(out, static_cast<std::uint8_t>(ch));
	}
	return out;
}

struct NameRecordSpec {
	std::uint16_t platformID;
	std::uint16_t encodingID;
	std::uint16_t languageID;
	std::uint16_t nameID;
	std::vector<std::byte> value; //!< エンコード済みの生バイト列（UTF-16BE または Latin-1）
};

//! sfnt name テーブル(format 0)を組み立てる。レコードは records の順番で並べる。
std::vector<std::byte> BuildNameTable(const std::vector<NameRecordSpec>& records)
{
	const std::size_t headerBytes = 6 + records.size() * 12;
	std::vector<std::byte> out(headerBytes, std::byte{ 0 });
	WriteU16BEAt(out, 0, 0); // format = 0
	WriteU16BEAt(out, 2, static_cast<std::uint16_t>(records.size()));
	WriteU16BEAt(out, 4, static_cast<std::uint16_t>(headerBytes)); // stringOffset

	std::vector<std::byte> storage;
	for (std::size_t i = 0; i < records.size(); ++i) {
		const auto& r = records[i];
		const std::size_t recOffset = 6 + i * 12;
		WriteU16BEAt(out, recOffset + 0, r.platformID);
		WriteU16BEAt(out, recOffset + 2, r.encodingID);
		WriteU16BEAt(out, recOffset + 4, r.languageID);
		WriteU16BEAt(out, recOffset + 6, r.nameID);
		WriteU16BEAt(out, recOffset + 8, static_cast<std::uint16_t>(r.value.size()));
		WriteU16BEAt(out, recOffset + 10, static_cast<std::uint16_t>(storage.size()));
		storage.insert(storage.end(), r.value.begin(), r.value.end());
	}
	out.insert(out.end(), storage.begin(), storage.end());
	return out;
}

//! 最小限の sfnt（オフセットテーブル + テーブルディレクトリ + 生テーブルデータ）を組み立てる。
//! LoadFontAsSfnt の TTF/OTF/TTC 経路はマジックの 4 バイトしか検証せず、
//! ExtractFamilyName は name テーブルの存在しか要求しない（checksum・head・
//! hhea 等はどこからも検証されない）ため、テストではこれで十分。
std::vector<std::byte> BuildBareSfnt(
	std::uint32_t sfntVersion, const std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>& tables)
{
	const std::uint16_t numTables = static_cast<std::uint16_t>(tables.size());
	std::size_t cursor = 12 + static_cast<std::size_t>(numTables) * 16;
	std::vector<std::byte> out(cursor, std::byte{ 0 });

	WriteU32BEAt(out, 0, sfntVersion);
	WriteU16BEAt(out, 4, numTables);
	WriteU16BEAt(out, 6, 0); // searchRange
	WriteU16BEAt(out, 8, 0); // entrySelector
	WriteU16BEAt(out, 10, 0); // rangeShift

	for (std::uint16_t i = 0; i < numTables; ++i) {
		const auto& [tag, data] = tables[i];
		const std::size_t dirOffset = 12 + static_cast<std::size_t>(i) * 16;
		WriteU32BEAt(out, dirOffset + 0, tag);
		WriteU32BEAt(out, dirOffset + 4, 0); // checksum（誰も検証しない）
		WriteU32BEAt(out, dirOffset + 8, static_cast<std::uint32_t>(cursor));
		WriteU32BEAt(out, dirOffset + 12, static_cast<std::uint32_t>(data.size()));
		out.insert(out.end(), data.begin(), data.end());
		cursor += data.size();
	}
	return out;
}

//
// ---- WOFF1 組み立てヘルパー ----
//

struct WoffTableSpec {
	std::uint32_t tag;
	std::vector<std::byte> origData;
	bool compress = false;
};

//! WOFF1 (44 バイトヘッダー + テーブルディレクトリ + テーブルデータ)を組み立てる。
//! DecodeWoff1 は flavor と numTables しかヘッダーから読まない（length /
//! totalSfntSize / majorVersion 等は無視される）ため、それ以外は 0 のままにする。
std::vector<std::byte> BuildWoff1(std::uint32_t flavor, const std::vector<WoffTableSpec>& tables)
{
	struct Laid {
		std::uint32_t tag;
		std::vector<std::byte> stored;
		std::uint32_t origLength;
	};
	std::vector<Laid> laid;
	laid.reserve(tables.size());
	for (const auto& t : tables) {
		Laid l;
		l.tag = t.tag;
		l.origLength = static_cast<std::uint32_t>(t.origData.size());
		l.stored = t.compress ? CompressBytesWithMzForTest(t.origData) : t.origData;
		laid.push_back(std::move(l));
	}

	const std::uint16_t numTables = static_cast<std::uint16_t>(laid.size());
	constexpr std::size_t headerBytes = 44;
	std::size_t cursor = headerBytes + static_cast<std::size_t>(numTables) * 20;
	std::vector<std::byte> out(cursor, std::byte{ 0 });

	WriteU32BEAt(out, 0, kTestWoffMagic);
	WriteU32BEAt(out, 4, flavor);
	WriteU32BEAt(out, 8, 0); // length（デコーダは参照しない）
	WriteU16BEAt(out, 12, numTables);
	WriteU16BEAt(out, 14, 0); // reserved
	WriteU32BEAt(out, 16, 0); // totalSfntSize（デコーダは参照しない）
	// オフセット 20..43 のバージョン/メタデータ/プライベートデータ各フィールドは
	// デコーダが参照しないため 0 のままにする。

	for (std::uint16_t i = 0; i < numTables; ++i) {
		const auto& l = laid[i];
		const std::size_t dirOffset = headerBytes + static_cast<std::size_t>(i) * 20;
		WriteU32BEAt(out, dirOffset + 0, l.tag);
		WriteU32BEAt(out, dirOffset + 4, static_cast<std::uint32_t>(cursor)); // tableOffset
		WriteU32BEAt(out, dirOffset + 8, static_cast<std::uint32_t>(l.stored.size())); // compLength
		WriteU32BEAt(out, dirOffset + 12, l.origLength); // origLength
		WriteU32BEAt(out, dirOffset + 16, 0); // origChecksum（そのまま引き継がれるだけで検証されない）
		out.insert(out.end(), l.stored.begin(), l.stored.end());
		cursor += l.stored.size();
	}
	return out;
}

//
// ---- ファイル/ディレクトリ用 RAII ヘルパー(test-cextensionmanager.cpp と同じパターン) ----
//

class TempDirectory {
public:
	TempDirectory() : m_path(GetTempFilePath(L"iconfontdir"))
	{
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
		std::filesystem::create_directory(m_path, ec);
	}
	~TempDirectory()
	{
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}
	TempDirectory(const TempDirectory&) = delete;
	TempDirectory& operator = (const TempDirectory&) = delete;

	const std::filesystem::path& GetPath() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

void WriteBytesToFile(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void WritePackageJson(const std::filesystem::path& extensionRoot, std::string_view jsonText)
{
	std::ofstream os(extensionRoot / L"package.json", std::ios::binary | std::ios::trunc);
	os.write(jsonText.data(), static_cast<std::streamsize>(jsonText.size()));
}

//
// ---- レジストリレベルのテスト用: システムの実 TrueType フォント ----
//

//! %WINDIR%\Fonts を返す。SHGetFolderPathW(CSIDL_FONTS) と実体は同じパスだが、
//! tests1 は shell32.lib への追加リンク依存を持たないため、常にリンクされて
//! いる kernel32 の GetWindowsDirectoryW から求める。取得できなければ空パス。
std::filesystem::path GetSystemFontsDirectory()
{
	wchar_t windowsDir[MAX_PATH] = {};
	const UINT len = ::GetWindowsDirectoryW(windowsDir, sizeof(windowsDir) / sizeof(windowsDir[0]));
	if (len == 0 || len >= sizeof(windowsDir) / sizeof(windowsDir[0])) {
		return {};
	}
	return std::filesystem::path(windowsDir) / L"Fonts";
}

//! ファイル全体を読み込む。開けない/読めない/空の場合は nullopt。
std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path& path)
{
	std::ifstream is(path, std::ios::binary);
	if (!is) {
		return std::nullopt;
	}
	is.seekg(0, std::ios::end);
	const auto size = is.tellg();
	if (size <= 0) {
		return std::nullopt;
	}
	is.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	is.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
	if (!is) {
		return std::nullopt;
	}
	return bytes;
}

//! 読み込み・LoadFontAsSfnt・ExtractFamilyName まで一貫して成功した、
//! レジストリテストにそのまま使えるシステムフォント 1 個分のフィクスチャ。
struct SSystemFontFixture {
	std::wstring sourceFileName;      //!< 診断用（例: L"arial.ttf"）
	std::vector<std::byte> rawBytes;  //!< ファイルの生バイト列（TTF はマジックのみ検証されそのまま sfnt として通る）
	std::wstring expectedFamilyName;  //!< detail::ExtractFamilyName で実際に抽出された期待値
};

//! %WINDIR%\Fonts 配下の既知の候補を順に試し、読み込み・デコード・ファミリー名
//! 抽出まで成功したものだけを候補順のまま列挙する。プロセス内で 1 度だけ実行し、
//! 結果はテスト間で使い回す（読み取り専用の OS 状態であり、決定的）。
const std::vector<SSystemFontFixture>& AvailableSystemTrueTypeFonts()
{
	static const std::vector<SSystemFontFixture> fonts = [] {
		std::vector<SSystemFontFixture> result;
		static constexpr std::wstring_view kCandidates[] = {
			L"arial.ttf", L"tahoma.ttf", L"consola.ttf", L"segoeui.ttf", L"micross.ttf", L"verdana.ttf",
		};
		const std::filesystem::path fontsDir = GetSystemFontsDirectory();
		if (fontsDir.empty()) {
			return result;
		}
		for (const std::wstring_view candidate : kCandidates) {
			auto raw = ReadWholeFile(fontsDir / candidate);
			if (!raw.has_value()) {
				continue;
			}
			std::vector<std::byte> sfnt;
			if (icons_detail::LoadFontAsSfnt(*raw, sfnt) != icons_detail::EFontDecodeError::None) {
				continue;
			}
			std::wstring familyName;
			if (icons_detail::ExtractFamilyName(sfnt, familyName) != icons_detail::EFamilyNameError::None) {
				continue;
			}
			result.push_back(SSystemFontFixture{ std::wstring(candidate), std::move(*raw), std::move(familyName) });
		}
		return result;
	}();
	return fonts;
}

//! index 番目に利用可能なシステムフォントを返す。利用可能なものがそれだけ
//! 無ければ nullptr（呼び出し側はこれを GTEST_SKIP() の条件にする）。
const SSystemFontFixture* NthAvailableSystemFont(std::size_t index)
{
	const auto& fonts = AvailableSystemTrueTypeFonts();
	return index < fonts.size() ? &fonts[index] : nullptr;
}

constexpr char kNoSystemFontSkipMessage[] =
	"No system TrueType font (arial/tahoma/consola/segoeui/micross/verdana.ttf under "
	"%WINDIR%\\Fonts) could be read in this environment. This is a genuine missing-font "
	"condition; it is unrelated to WOFF1/sfnt decode correctness, which the "
	"LoadFontAsSfnt/ExtractFamilyName tests above already cover directly.";

constexpr char kNeedTwoSystemFontsSkipMessage[] =
	"Fewer than two distinct system TrueType fonts could be read under %WINDIR%\\Fonts in this "
	"environment. Proving duplicate-id dedup/transfer requires two fonts with distinguishable "
	"family names.";

} // namespace

//
// ==== detail::LoadFontAsSfnt ====
//

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsEmptyInput)
{
	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::FileEmpty, icons_detail::LoadFontAsSfnt({}, sfnt));
	EXPECT_TRUE(sfnt.empty());
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsHeaderTooShortWhenMagicUnreadable)
{
	const std::vector<std::byte> tiny(3, std::byte{ 0xAB });
	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::HeaderTooShort, icons_detail::LoadFontAsSfnt(tiny, sfnt));
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsWoffHeaderTooShortWhenTruncated)
{
	std::vector<std::byte> buf;
	PushU32BE(buf, kTestWoffMagic);
	for (int i = 0; i < 10; ++i) {
		PushU8(buf, 0);
	}
	ASSERT_LT(buf.size(), std::size_t{ 44 });

	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::HeaderTooShort, icons_detail::LoadFontAsSfnt(buf, sfnt));
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsUnrecognizedMagic)
{
	std::vector<std::byte> buf;
	PushU32BE(buf, MakeTag('X', 'X', 'X', 'X'));
	PushU32BE(buf, 0);

	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::UnrecognizedFormat, icons_detail::LoadFontAsSfnt(buf, sfnt));
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsWoff2WithDistinctReason)
{
	std::vector<std::byte> buf;
	PushU32BE(buf, kTestWoff2Magic);
	for (int i = 0; i < 40; ++i) {
		PushU8(buf, 0);
	}

	std::vector<std::byte> sfnt;
	// WOFF2 は WOFF1 とは別の理由で拒否されなければならない（デコードは試みない）
	EXPECT_EQ(icons_detail::EFontDecodeError::UnsupportedWoff2, icons_detail::LoadFontAsSfnt(buf, sfnt));
	EXPECT_TRUE(sfnt.empty());
}

TEST(CExtensionIconFontLoadFontAsSfnt, PassesThroughSfntMagicsVerbatim)
{
	const std::vector<std::uint32_t> magics = { kTestTrueTypeMagic, kTestOttoMagic, kTestTtcMagic, kTestMacTrueMagic };
	for (const std::uint32_t magic : magics) {
		std::vector<std::byte> buf;
		PushU32BE(buf, magic);
		for (int i = 0; i < 12; ++i) {
			PushU8(buf, static_cast<std::uint8_t>(0xA5 + i));
		}

		std::vector<std::byte> sfnt;
		ASSERT_EQ(icons_detail::EFontDecodeError::None, icons_detail::LoadFontAsSfnt(buf, sfnt)) << "magic=" << magic;
		// TTF/OTF/TTC はマジック検証のみで、バイト列はそのまま素通りする
		EXPECT_EQ(buf, sfnt) << "magic=" << magic;
	}
}

TEST(CExtensionIconFontLoadFontAsSfnt, DecodesUncompressedWoff1TableAndExtractsFamilyName)
{
	const std::wstring expectedFamily = L"Otak Claude Uncompressed Test";
	const auto nameTable = BuildNameTable({ NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(expectedFamily) } });
	const auto woffBytes = BuildWoff1(kTestTrueTypeMagic, { WoffTableSpec{ kTestNameTag, nameTable, /*compress=*/false } });

	std::vector<std::byte> sfnt;
	ASSERT_EQ(icons_detail::EFontDecodeError::None, icons_detail::LoadFontAsSfnt(woffBytes, sfnt));

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(expectedFamily, familyName);
}

TEST(CExtensionIconFontLoadFontAsSfnt, DecodesZlibCompressedWoff1TableAndExtractsFamilyName)
{
	// 高い圧縮率を確実にするための繰り返しパターン（LF_FACESIZE-1=31 以内）
	const std::wstring expectedFamily(30, L'A');
	const auto nameTable = BuildNameTable({ NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(expectedFamily) } });
	const auto woffBytes = BuildWoff1(kTestTrueTypeMagic, { WoffTableSpec{ kTestNameTag, nameTable, /*compress=*/true } });

	std::vector<std::byte> sfnt;
	ASSERT_EQ(icons_detail::EFontDecodeError::None, icons_detail::LoadFontAsSfnt(woffBytes, sfnt));

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(expectedFamily, familyName);
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsWoffWithZeroTables)
{
	const auto woffBytes = BuildWoff1(kTestTrueTypeMagic, {});
	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::TableCountOutOfRange, icons_detail::LoadFontAsSfnt(woffBytes, sfnt));
	EXPECT_TRUE(sfnt.empty());
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsWoffWhenDirectoryTruncated)
{
	const auto nameTable = BuildNameTable({ NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"Truncated") } });
	auto woffBytes = BuildWoff1(kTestTrueTypeMagic, { WoffTableSpec{ kTestNameTag, nameTable, false } });
	ASSERT_GT(woffBytes.size(), std::size_t{ 44 + 10 });
	// ヘッダー直後、1件分のテーブルディレクトリエントリ(20バイト)の途中で切り詰める
	woffBytes.resize(44 + 10);

	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::TableOutOfRange, icons_detail::LoadFontAsSfnt(woffBytes, sfnt));
	EXPECT_TRUE(sfnt.empty());
}

TEST(CExtensionIconFontLoadFontAsSfnt, RejectsWoffWithOutOfRangeTableDataOffset)
{
	const auto nameTable = BuildNameTable({ NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"OutOfRange") } });
	auto woffBytes = BuildWoff1(kTestTrueTypeMagic, { WoffTableSpec{ kTestNameTag, nameTable, false } });
	// 唯一のテーブルディレクトリエントリの tableOffset（ディレクトリ先頭 + 4）を、
	// ファイル全体よりも後ろを指す明らかな範囲外の値へ書き換える。
	WriteU32BEAt(woffBytes, 44 + 4, static_cast<std::uint32_t>(woffBytes.size() + 1000));

	std::vector<std::byte> sfnt;
	EXPECT_EQ(icons_detail::EFontDecodeError::TableOutOfRange, icons_detail::LoadFontAsSfnt(woffBytes, sfnt));
	EXPECT_TRUE(sfnt.empty());
}

//
// ==== detail::ExtractFamilyName ====
//

TEST(CExtensionIconFontExtractFamilyName, TableNotFound)
{
	const std::vector<std::byte> dummyData(4, std::byte{ 0 });
	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { MakeTag('z', 'z', 'z', 'z'), dummyData } });

	std::wstring familyName;
	EXPECT_EQ(icons_detail::EFamilyNameError::TableNotFound, icons_detail::ExtractFamilyName(sfnt, familyName));
}

TEST(CExtensionIconFontExtractFamilyName, PrefersTypographicFamilyOverFontFamily)
{
	const auto table = BuildNameTable({
		NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"Font Family Name") },
		NameRecordSpec{ 3, 1, 0x0409, 16, EncodeUtf16BE(L"Typographic Family Name") },
	});
	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, table } });

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(L"Typographic Family Name", familyName);
}

TEST(CExtensionIconFontExtractFamilyName, FallsBackToFontFamilyWhenNoTypographicRecord)
{
	const auto table = BuildNameTable({
		NameRecordSpec{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"Font Family Only") },
	});
	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, table } });

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(L"Font Family Only", familyName);
}

TEST(CExtensionIconFontExtractFamilyName, FallsBackToMacintoshRecordWhenNoWindowsRecord)
{
	const auto table = BuildNameTable({
		NameRecordSpec{ 1, 0, 0, 1, EncodeLatin1("MacFallbackName") },
	});
	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, table } });

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(L"MacFallbackName", familyName);
}

TEST(CExtensionIconFontExtractFamilyName, RejectsNameTooLong)
{
	const std::wstring tooLong(32, L'B'); // LF_FACESIZE(32) - 1 = 31 が上限
	const auto table = BuildNameTable({
		NameRecordSpec{ 3, 1, 0x0409, 16, EncodeUtf16BE(tooLong) },
	});
	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, table } });

	std::wstring familyName;
	EXPECT_EQ(icons_detail::EFamilyNameError::NameTooLong, icons_detail::ExtractFamilyName(sfnt, familyName));
}

TEST(CExtensionIconFontExtractFamilyName, SkipsMalformedRecordButKeepsOthers)
{
	// nameテーブル: header(6) + 2レコード*12 = 30バイトの位置からストレージ開始
	std::vector<std::byte> table;
	PushU16BE(table, 0); // format
	PushU16BE(table, 2); // count
	PushU16BE(table, 30); // stringOffset

	// レコード0: Windows/nameID=1 だが offset/length がストレージ範囲外(壊れている)
	PushU16BE(table, 3); // platformID
	PushU16BE(table, 1); // encodingID
	PushU16BE(table, 0x0409); // languageID
	PushU16BE(table, 1); // nameID = Font Family
	PushU16BE(table, 4); // length
	PushU16BE(table, 9999); // offset（意図的に範囲外）

	// レコード1: Windows/nameID=1 で正しい
	const std::wstring good = L"GoodFallbackName";
	const auto goodBytes = EncodeUtf16BE(good);
	PushU16BE(table, 3);
	PushU16BE(table, 1);
	PushU16BE(table, 0x0409);
	PushU16BE(table, 1); // nameID = Font Family
	PushU16BE(table, static_cast<std::uint16_t>(goodBytes.size()));
	PushU16BE(table, 0); // offset 0（ストレージ先頭）

	// ストレージ領域（レコード1の分だけ）
	table.insert(table.end(), goodBytes.begin(), goodBytes.end());

	const auto sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, table } });

	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(good, familyName);
}

//
// ==== detail::EnsureUniqueFontIdentifier ====
//
// GDI の AddFontMemResourceEx は name テーブルに nameID 3（Unique font
// identifier）を 1 件も持たない sfnt を無条件で拒否する。Chromium（=実 VS Code）
// は同じフォントを描けるため、補完しないとこの製品でだけアイコンが出ない、
// という非互換になる。ここでは合成の有無・合成元の選択順・他テーブルの不変性・
// 失敗時に何もしないことを、GDI を一切呼ばずにバイト列だけで検証する。
//

namespace {

constexpr std::uint32_t kTestHeadTag = MakeTag('h', 'e', 'a', 'd');
constexpr std::uint32_t kTestCmapTag = MakeTag('c', 'm', 'a', 'p');

constexpr std::uint32_t kTestSfntCheckSumMagic = 0xB1B0AFBAu;
constexpr std::size_t kTestHeadCheckSumAdjustmentOffset = 8;

std::uint16_t ReadU16BEAt(const std::vector<std::byte>& v, std::size_t offset)
{
	return static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(v[offset])) << 8) |
		static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(v[offset + 1])));
}

std::uint32_t ReadU32BEAt(const std::vector<std::byte>& v, std::size_t offset)
{
	return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(v[offset])) << 24) |
		(static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(v[offset + 1])) << 16) |
		(static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(v[offset + 2])) << 8) |
		static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(v[offset + 3]));
}

//! sfnt のテーブルディレクトリから tag 一致エントリの [offset, length] を返す。
std::optional<std::pair<std::size_t, std::size_t>> FindSfntTableRangeForTest(
	const std::vector<std::byte>& sfnt, std::uint32_t tag)
{
	if (sfnt.size() < 12) {
		return std::nullopt;
	}
	const std::uint16_t numTables = ReadU16BEAt(sfnt, 4);
	for (std::uint16_t i = 0; i < numTables; ++i) {
		const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
		if (entry + 16 > sfnt.size()) {
			return std::nullopt;
		}
		if (ReadU32BEAt(sfnt, entry) != tag) {
			continue;
		}
		const std::size_t offset = ReadU32BEAt(sfnt, entry + 8);
		const std::size_t length = ReadU32BEAt(sfnt, entry + 12);
		if (offset > sfnt.size() || length > sfnt.size() - offset) {
			return std::nullopt;
		}
		return std::pair<std::size_t, std::size_t>{ offset, length };
	}
	return std::nullopt;
}

//! sfnt から tag 一致テーブルの本体バイト列を複製して返す。
std::optional<std::vector<std::byte>> FindSfntTableForTest(
	const std::vector<std::byte>& sfnt, std::uint32_t tag)
{
	const auto range = FindSfntTableRangeForTest(sfnt, tag);
	if (!range.has_value()) {
		return std::nullopt;
	}
	const auto first = sfnt.begin() + static_cast<std::ptrdiff_t>(range->first);
	return std::vector<std::byte>(first, first + static_cast<std::ptrdiff_t>(range->second));
}

//! name テーブル(format 0)を NameRecordSpec 列へ読み戻す（BuildNameTable の逆）。
std::vector<NameRecordSpec> ParseNameTableForTest(const std::vector<std::byte>& nameTable)
{
	std::vector<NameRecordSpec> out;
	if (nameTable.size() < 6) {
		return out;
	}
	const std::uint16_t count = ReadU16BEAt(nameTable, 2);
	const std::uint16_t storageOffset = ReadU16BEAt(nameTable, 4);
	for (std::uint16_t i = 0; i < count; ++i) {
		const std::size_t rec = 6 + static_cast<std::size_t>(i) * 12;
		if (rec + 12 > nameTable.size()) {
			break;
		}
		NameRecordSpec spec{};
		spec.platformID = ReadU16BEAt(nameTable, rec);
		spec.encodingID = ReadU16BEAt(nameTable, rec + 2);
		spec.languageID = ReadU16BEAt(nameTable, rec + 4);
		spec.nameID = ReadU16BEAt(nameTable, rec + 6);
		const std::size_t length = ReadU16BEAt(nameTable, rec + 8);
		const std::size_t valueStart =
			static_cast<std::size_t>(storageOffset) + ReadU16BEAt(nameTable, rec + 10);
		if (valueStart > nameTable.size() || length > nameTable.size() - valueStart) {
			break;
		}
		const auto first = nameTable.begin() + static_cast<std::ptrdiff_t>(valueStart);
		spec.value.assign(first, first + static_cast<std::ptrdiff_t>(length));
		out.push_back(std::move(spec));
	}
	return out;
}

//! sfnt のチェックサム（32bit ワードの単純加算、末尾は 0 埋め）。実装と同じ定義。
std::uint32_t SfntChecksumForTest(const std::vector<std::byte>& data)
{
	std::uint32_t sum = 0;
	std::size_t cursor = 0;
	for (; cursor + 4 <= data.size(); cursor += 4) {
		sum += ReadU32BEAt(data, cursor);
	}
	if (cursor < data.size()) {
		std::uint32_t tail = 0;
		for (std::size_t i = 0; i < 4; ++i) {
			const std::uint8_t byte = (cursor + i) < data.size()
				? std::to_integer<std::uint8_t>(data[cursor + i])
				: std::uint8_t{ 0 };
			tail = (tail << 8) | byte;
		}
		sum += tail;
	}
	return sum;
}

//! records から nameID 一致の最初のレコードを返す（無ければ nullptr）。
const NameRecordSpec* FindNameRecord(const std::vector<NameRecordSpec>& records, std::uint16_t nameID)
{
	for (const auto& record : records) {
		if (record.nameID == nameID) {
			return &record;
		}
	}
	return nullptr;
}

//! 12 バイト以上の擬似 head テーブル（checkSumAdjustment を含む長さがあれば十分）。
std::vector<std::byte> BuildStubHeadTable()
{
	std::vector<std::byte> head;
	PushU32BE(head, 0x00010000u); // version
	PushU32BE(head, 0x00010000u); // fontRevision
	PushU32BE(head, 0xDEADBEEFu); // checkSumAdjustment（作り直されるべき値）
	PushU32BE(head, 0x5F0F3CF5u); // magicNumber
	return head;
}

} // namespace

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, ReportsNotNeededAndLeavesBytesUntouchedWhenPresent)
{
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"already-fine") },
		{ 3, 1, 0x0409, 3, EncodeUtf16BE(L"already-fine:unique") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });
	const std::vector<std::byte> before = sfnt;

	EXPECT_EQ(icons_detail::ENameRepairResult::NotNeeded, icons_detail::EnsureUniqueFontIdentifier(sfnt));
	EXPECT_EQ(before, sfnt);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, SynthesizesUniqueIdFromWindowsFontFamily)
{
	// odangoo.otak-usage が同梱する otak-usage-icons.woff と同じ構成
	// （nameID 1/2/4/6 だけを持ち、nameID 3 が無い IcoMoon 系のアイコンフォント）。
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"otak-usage-icons") },
		{ 3, 1, 0x0409, 2, EncodeUtf16BE(L"Regular") },
		{ 3, 1, 0x0409, 4, EncodeUtf16BE(L"otak-usage-icons") },
		{ 3, 1, 0x0409, 6, EncodeUtf16BE(L"otak-usage-icons") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });

	ASSERT_EQ(icons_detail::ENameRepairResult::Repaired, icons_detail::EnsureUniqueFontIdentifier(sfnt));

	const auto rebuiltNameTable = FindSfntTableForTest(sfnt, kTestNameTag);
	ASSERT_TRUE(rebuiltNameTable.has_value());
	const auto records = ParseNameTableForTest(*rebuiltNameTable);
	ASSERT_EQ(std::size_t{ 5 }, records.size());

	const NameRecordSpec* uniqueId = FindNameRecord(records, 3);
	ASSERT_NE(nullptr, uniqueId);
	EXPECT_EQ(std::uint16_t{ 3 }, uniqueId->platformID);
	EXPECT_EQ(std::uint16_t{ 1 }, uniqueId->encodingID);
	EXPECT_EQ(std::uint16_t{ 0x0409 }, uniqueId->languageID);
	EXPECT_EQ(EncodeUtf16BE(L"otak-usage-icons"), uniqueId->value);

	// ファミリー名抽出は合成後も同じ答えでなければならない。
	std::wstring familyName;
	ASSERT_EQ(icons_detail::EFamilyNameError::None, icons_detail::ExtractFamilyName(sfnt, familyName));
	EXPECT_EQ(L"otak-usage-icons", familyName);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, KeepsRecordsSortedByPlatformEncodingLanguageName)
{
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 6, EncodeUtf16BE(L"PostScriptName") },
		{ 1, 0, 0x0000, 1, EncodeLatin1("MacFamily") },
		{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"WinFamily") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });

	ASSERT_EQ(icons_detail::ENameRepairResult::Repaired, icons_detail::EnsureUniqueFontIdentifier(sfnt));

	const auto rebuiltNameTable = FindSfntTableForTest(sfnt, kTestNameTag);
	ASSERT_TRUE(rebuiltNameTable.has_value());
	const auto records = ParseNameTableForTest(*rebuiltNameTable);
	ASSERT_EQ(std::size_t{ 4 }, records.size());

	for (std::size_t i = 1; i < records.size(); ++i) {
		const auto& a = records[i - 1];
		const auto& b = records[i];
		const auto keyA = std::tuple{ a.platformID, a.encodingID, a.languageID, a.nameID };
		const auto keyB = std::tuple{ b.platformID, b.encodingID, b.languageID, b.nameID };
		EXPECT_TRUE(keyA < keyB) << "record " << (i - 1) << " must sort before record " << i;
	}

	// Windows レコードが優先される（GDI が実際に読むのはそちらであるため）。
	const NameRecordSpec* uniqueId = FindNameRecord(records, 3);
	ASSERT_NE(nullptr, uniqueId);
	EXPECT_EQ(std::uint16_t{ 3 }, uniqueId->platformID);
	EXPECT_EQ(EncodeUtf16BE(L"WinFamily"), uniqueId->value);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, FallsBackToPostScriptNameWhenNoFontFamilyRecord)
{
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 4, EncodeUtf16BE(L"Full Name") },
		{ 3, 1, 0x0409, 6, EncodeUtf16BE(L"PostScriptName") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });

	ASSERT_EQ(icons_detail::ENameRepairResult::Repaired, icons_detail::EnsureUniqueFontIdentifier(sfnt));

	const auto rebuiltNameTable = FindSfntTableForTest(sfnt, kTestNameTag);
	ASSERT_TRUE(rebuiltNameTable.has_value());
	const auto records = ParseNameTableForTest(*rebuiltNameTable);
	const NameRecordSpec* uniqueId = FindNameRecord(records, 3);
	ASSERT_NE(nullptr, uniqueId);
	EXPECT_EQ(EncodeUtf16BE(L"PostScriptName"), uniqueId->value);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, FallsBackToMacintoshRecordWhenNoWindowsRecord)
{
	const auto nameTable = BuildNameTable({
		{ 1, 0, 0x0000, 1, EncodeLatin1("MacOnlyFamily") },
		{ 1, 0, 0x0000, 2, EncodeLatin1("Regular") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });

	ASSERT_EQ(icons_detail::ENameRepairResult::Repaired, icons_detail::EnsureUniqueFontIdentifier(sfnt));

	const auto rebuiltNameTable = FindSfntTableForTest(sfnt, kTestNameTag);
	ASSERT_TRUE(rebuiltNameTable.has_value());
	const auto records = ParseNameTableForTest(*rebuiltNameTable);
	const NameRecordSpec* uniqueId = FindNameRecord(records, 3);
	ASSERT_NE(nullptr, uniqueId);
	EXPECT_EQ(std::uint16_t{ 1 }, uniqueId->platformID);
	EXPECT_EQ(EncodeLatin1("MacOnlyFamily"), uniqueId->value);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, PreservesOtherTablesAndRecomputesHeadCheckSumAdjustment)
{
	const auto cmapBytes = EncodeLatin1("cmap-payload-bytes-verbatim");
	const auto headBytes = BuildStubHeadTable();
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 1, EncodeUtf16BE(L"WithHead") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic,
		{ { kTestNameTag, nameTable }, { kTestHeadTag, headBytes }, { kTestCmapTag, cmapBytes } });

	ASSERT_EQ(icons_detail::ENameRepairResult::Repaired, icons_detail::EnsureUniqueFontIdentifier(sfnt));

	// name 以外のテーブルは 1 バイトも変えてはならない。
	EXPECT_EQ(cmapBytes, FindSfntTableForTest(sfnt, kTestCmapTag).value_or(std::vector<std::byte>{}));

	const auto headRange = FindSfntTableRangeForTest(sfnt, kTestHeadTag);
	ASSERT_TRUE(headRange.has_value());
	ASSERT_EQ(headBytes.size(), headRange->second);
	const std::size_t adjustmentOffset = headRange->first + kTestHeadCheckSumAdjustmentOffset;

	// head.checkSumAdjustment はテーブル配置が変わった後の値で作り直されている。
	const std::uint32_t storedAdjustment = ReadU32BEAt(sfnt, adjustmentOffset);
	EXPECT_NE(0xDEADBEEFu, storedAdjustment);
	std::vector<std::byte> zeroed = sfnt;
	WriteU32BEAt(zeroed, adjustmentOffset, 0);
	EXPECT_EQ(kTestSfntCheckSumMagic - SfntChecksumForTest(zeroed), storedAdjustment);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, SkipsSfntWithoutNameTable)
{
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestHeadTag, BuildStubHeadTable() } });
	const std::vector<std::byte> before = sfnt;

	EXPECT_EQ(icons_detail::ENameRepairResult::Skipped, icons_detail::EnsureUniqueFontIdentifier(sfnt));
	EXPECT_EQ(before, sfnt);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, SkipsTrueTypeCollection)
{
	// TTC は複数フォントを束ねた別レイアウトであり、単一 sfnt として組み直せない。
	std::vector<std::byte> ttc;
	PushU32BE(ttc, kTestTtcMagic);
	PushU32BE(ttc, 0x00010000u); // majorVersion/minorVersion
	PushU32BE(ttc, 2);           // numFonts
	PushU32BE(ttc, 16);          // offsetTable[0]
	PushU32BE(ttc, 32);          // offsetTable[1]
	const std::vector<std::byte> before = ttc;

	EXPECT_EQ(icons_detail::ENameRepairResult::Skipped, icons_detail::EnsureUniqueFontIdentifier(ttc));
	EXPECT_EQ(before, ttc);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, SkipsWhenNoRecordCanSourceTheIdentifier)
{
	// 合成元は nameID 1/6/4 のいずれかに限る。サブファミリーや Typographic Family
	// しか無いフォントは「それらしい名前」を捏造せず、何もせず素通りする。
	const auto nameTable = BuildNameTable({
		{ 3, 1, 0x0409, 2, EncodeUtf16BE(L"Regular") },
		{ 3, 1, 0x0409, 16, EncodeUtf16BE(L"Typographic Only") },
	});
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, nameTable } });
	const std::vector<std::byte> before = sfnt;

	EXPECT_EQ(icons_detail::ENameRepairResult::Skipped, icons_detail::EnsureUniqueFontIdentifier(sfnt));
	EXPECT_EQ(before, sfnt);
}

TEST(CExtensionIconFontEnsureUniqueFontIdentifier, SkipsUnparsableNameTable)
{
	// count = 0 の name テーブルは解析できない。壊れた入力を推測で直さない。
	std::vector<std::byte> brokenNameTable;
	PushU16BE(brokenNameTable, 0); // format 0
	PushU16BE(brokenNameTable, 0); // count = 0
	PushU16BE(brokenNameTable, 6); // stringOffset
	std::vector<std::byte> sfnt = BuildBareSfnt(kTestTrueTypeMagic, { { kTestNameTag, brokenNameTable } });
	const std::vector<std::byte> before = sfnt;

	EXPECT_EQ(icons_detail::ENameRepairResult::Skipped, icons_detail::EnsureUniqueFontIdentifier(sfnt));
	EXPECT_EQ(before, sfnt);
}

//
// ==== detail::ParseFontCharacter ====
//

TEST(CExtensionIconFontParseFontCharacter, DecodesHexEscapeToBmpCodepoint)
{
	std::wstring glyph;
	ASSERT_EQ(icons_detail::EFontCharacterError::None, icons_detail::ParseFontCharacter("\\E901", glyph));
	ASSERT_EQ(std::size_t{ 1 }, glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE901), glyph[0]);
}

TEST(CExtensionIconFontParseFontCharacter, DecodesHexEscapeToAstralSurrogatePair)
{
	std::wstring glyph;
	// U+1F600 GRINNING FACE -> サロゲートペア D83D DE00
	ASSERT_EQ(icons_detail::EFontCharacterError::None, icons_detail::ParseFontCharacter("\\1F600", glyph));
	ASSERT_EQ(std::size_t{ 2 }, glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xD83D), glyph[0]);
	EXPECT_EQ(static_cast<wchar_t>(0xDE00), glyph[1]);
}

TEST(CExtensionIconFontParseFontCharacter, RejectsEmptyString)
{
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::Empty, icons_detail::ParseFontCharacter("", glyph));
}

TEST(CExtensionIconFontParseFontCharacter, RejectsEscapeWithNonHexDigits)
{
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::InvalidEscape, icons_detail::ParseFontCharacter("\\ZZZZ", glyph));
}

TEST(CExtensionIconFontParseFontCharacter, RejectsEscapeExceedingSixHexDigits)
{
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::InvalidEscape, icons_detail::ParseFontCharacter("\\1234567", glyph));
}

TEST(CExtensionIconFontParseFontCharacter, RejectsEscapeResolvingToSurrogateCodepoint)
{
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::InvalidEscape, icons_detail::ParseFontCharacter("\\D800", glyph));
}

TEST(CExtensionIconFontParseFontCharacter, DecodesBareSingleUtf8Character)
{
	// U+2605 BLACK STAR, UTF-8: E2 98 85
	const std::string star = { static_cast<char>(0xE2), static_cast<char>(0x98), static_cast<char>(0x85) };
	std::wstring glyph;
	ASSERT_EQ(icons_detail::EFontCharacterError::None, icons_detail::ParseFontCharacter(star, glyph));
	ASSERT_EQ(std::size_t{ 1 }, glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0x2605), glyph[0]);
}

TEST(CExtensionIconFontParseFontCharacter, RejectsBareMultiCharacterString)
{
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::InvalidUtf8, icons_detail::ParseFontCharacter("ab", glyph));
}

TEST(CExtensionIconFontParseFontCharacter, RejectsInvalidUtf8Bytes)
{
	const std::string lonelyContinuation = { static_cast<char>(0x80) };
	std::wstring glyph;
	EXPECT_EQ(icons_detail::EFontCharacterError::InvalidUtf8, icons_detail::ParseFontCharacter(lonelyContinuation, glyph));
}

//
// ==== detail::ResolveExtensionIconFontPath ====
//

TEST(CExtensionIconFontResolveExtensionIconFontPath, ResolvesNestedRelativePath)
{
	TempDirectory root;
	std::filesystem::create_directories(root.GetPath() / L"images");

	std::filesystem::path resolved;
	ASSERT_TRUE(icons_detail::ResolveExtensionIconFontPath(root.GetPath(), "images/font.woff", resolved));

	std::error_code ec;
	const auto expected = std::filesystem::weakly_canonical(root.GetPath() / L"images" / L"font.woff", ec);
	ASSERT_FALSE(ec);
	EXPECT_EQ(expected, resolved);
}

TEST(CExtensionIconFontResolveExtensionIconFontPath, RejectsPathEscapingRoot)
{
	TempDirectory root;
	std::filesystem::path resolved;
	EXPECT_FALSE(icons_detail::ResolveExtensionIconFontPath(root.GetPath(), "../../evil.ttf", resolved));
	EXPECT_TRUE(resolved.empty());
}

TEST(CExtensionIconFontResolveExtensionIconFontPath, RejectsAbsolutePath)
{
	TempDirectory root;
	std::filesystem::path resolved;
	EXPECT_FALSE(icons_detail::ResolveExtensionIconFontPath(root.GetPath(), "C:/Windows/evil.ttf", resolved));
	EXPECT_TRUE(resolved.empty());
}

TEST(CExtensionIconFontResolveExtensionIconFontPath, RejectsEmptyFontPath)
{
	TempDirectory root;
	std::filesystem::path resolved;
	EXPECT_FALSE(icons_detail::ResolveExtensionIconFontPath(root.GetPath(), "", resolved));
}

TEST(CExtensionIconFontResolveExtensionIconFontPath, RejectsEscapeIntoSiblingDirectoryWithSamePrefix)
{
	// "root" は "rootEvil" の文字列プレフィックスであるため、境界を意識しない
	// 素朴な前方一致比較では ".../rootEvil/..." を ".../root" の内側だと
	// 誤判定してしまう。IsPathWithinRoot はプレフィックス直後が
	// パス区切り文字であることまで確認しなければならない。
	TempDirectory parent;
	const std::filesystem::path rootDir = parent.GetPath() / L"root";
	const std::filesystem::path siblingDir = parent.GetPath() / L"rootEvil";
	std::filesystem::create_directories(rootDir);
	std::filesystem::create_directories(siblingDir);

	std::filesystem::path resolved;
	EXPECT_FALSE(icons_detail::ResolveExtensionIconFontPath(rootDir, "../rootEvil/font.ttf", resolved));
	EXPECT_TRUE(resolved.empty());
}

//
// ==== CExtensionIconFontRegistry ====
//

TEST(CExtensionIconFontRegistry, RegisterExtensionSucceedsWithZeroIconsWhenNoContributes)
{
	TempDirectory root;
	WritePackageJson(root.GetPath(), R"JSON({ "name": "no-icons-ext" })JSON");

	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(L"vendor.no-icons", root.GetPath()));
	EXPECT_FALSE(registry.Find(L"vendor.no-icons", L"anything").has_value());
	EXPECT_FALSE(registry.Find(L"anything").has_value());
}

TEST(CExtensionIconFontRegistry, RegisterExtensionFailsWhenManifestMissing)
{
	TempDirectory root; // package.json を書かない
	icons::CExtensionIconFontRegistry registry;
	EXPECT_FALSE(registry.RegisterExtension(L"vendor.missing-manifest", root.GetPath()));
}

TEST(CExtensionIconFontRegistry, RegisterExtensionFailsWhenManifestIsInvalidJson)
{
	TempDirectory root;
	WritePackageJson(root.GetPath(), "{ this is not valid JSON");
	icons::CExtensionIconFontRegistry registry;
	EXPECT_FALSE(registry.RegisterExtension(L"vendor.bad-json", root.GetPath()));
}

TEST(CExtensionIconFontRegistry, ResolvesRealWorldOtakUsageManifestAndSharesOneFontAcrossTwoIcons)
{
	const auto* font = NthAvailableSystemFont(0);
	if (font == nullptr) {
		GTEST_SKIP() << kNoSystemFontSkipMessage;
	}

	TempDirectory root;
	std::filesystem::create_directories(root.GetPath() / L"images");
	WriteBytesToFile(root.GetPath() / L"images" / L"otak-usage-icons.ttf", font->rawBytes);
	WritePackageJson(root.GetPath(), R"JSON({
  "name": "otak-usage",
  "publisher": "odangoo",
  "contributes": {
    "icons": {
      "otak-openai": { "default": { "fontPath": "./images/otak-usage-icons.ttf", "fontCharacter": "\\E900" } },
      "otak-claude": { "default": { "fontPath": "./images/otak-usage-icons.ttf", "fontCharacter": "\\E901" } }
    }
  }
})JSON");

	const std::wstring extId = L"odangoo.otak-usage";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extId, root.GetPath()));

	const auto claudeIcon = registry.Find(extId, L"otak-claude");
	ASSERT_TRUE(claudeIcon.has_value());
	EXPECT_EQ(font->expectedFamilyName, claudeIcon->faceName);
	ASSERT_EQ(std::size_t{ 1 }, claudeIcon->glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE901), claudeIcon->glyph[0]);

	const auto openaiIcon = registry.Find(extId, L"otak-openai");
	ASSERT_TRUE(openaiIcon.has_value());
	EXPECT_EQ(claudeIcon->faceName, openaiIcon->faceName); // 同一フォントファイルを共有する（重複排除）
	ASSERT_EQ(std::size_t{ 1 }, openaiIcon->glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE900), openaiIcon->glyph[0]);

	// グローバル解決（コーディネーター要求の Find(iconId) 単独版）も同じ結果になる
	const auto globalClaude = registry.Find(L"otak-claude");
	ASSERT_TRUE(globalClaude.has_value());
	EXPECT_EQ(claudeIcon->faceName, globalClaude->faceName);
	EXPECT_EQ(claudeIcon->glyph, globalClaude->glyph);
}

TEST(CExtensionIconFontRegistry, FindReturnsNulloptForUnknownExtensionOrIcon)
{
	const auto* font = NthAvailableSystemFont(0);
	if (font == nullptr) {
		GTEST_SKIP() << kNoSystemFontSkipMessage;
	}

	TempDirectory root;
	std::filesystem::create_directories(root.GetPath() / L"images");
	WriteBytesToFile(root.GetPath() / L"images" / L"font.ttf", font->rawBytes);
	WritePackageJson(root.GetPath(), R"JSON({
  "contributes": { "icons": { "known-icon": { "default": { "fontPath": "./images/font.ttf", "fontCharacter": "\\E902" } } } }
})JSON");

	const std::wstring extId = L"vendor.unknown-lookup";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extId, root.GetPath()));

	const auto known = registry.Find(extId, L"known-icon");
	ASSERT_TRUE(known.has_value());
	EXPECT_EQ(font->expectedFamilyName, known->faceName);

	EXPECT_FALSE(registry.Find(L"vendor.does-not-exist", L"known-icon").has_value());
	EXPECT_FALSE(registry.Find(extId, L"does-not-exist").has_value());
	EXPECT_FALSE(registry.Find(L"does-not-exist-global").has_value());
}

TEST(CExtensionIconFontRegistry, ReRegisteringSameExtensionIsIdempotentAndDoesNotLeak)
{
	const auto* font = NthAvailableSystemFont(0);
	if (font == nullptr) {
		GTEST_SKIP() << kNoSystemFontSkipMessage;
	}

	TempDirectory root;
	std::filesystem::create_directories(root.GetPath() / L"images");
	WriteBytesToFile(root.GetPath() / L"images" / L"font.ttf", font->rawBytes);
	WritePackageJson(root.GetPath(), R"JSON({
  "contributes": { "icons": { "icon-a": { "default": { "fontPath": "./images/font.ttf", "fontCharacter": "\\E903" } } } }
})JSON");

	const std::wstring extId = L"vendor.reregister";
	icons::CExtensionIconFontRegistry registry;

	// 何度再登録してもクラッシュせず、都度正しく解決できることを確認する。
	// （個々のハンドルの多重リークは黒箱テストからは直接観測できないが、
	// 少なくとも機能的な冪等性と UnregisterExtension 後の完全なクリーンアップは検証する）
	for (int i = 0; i < 5; ++i) {
		ASSERT_TRUE(registry.RegisterExtension(extId, root.GetPath()));
		const auto icon = registry.Find(extId, L"icon-a");
		ASSERT_TRUE(icon.has_value());
		EXPECT_EQ(font->expectedFamilyName, icon->faceName);
	}

	registry.UnregisterExtension(extId);
	EXPECT_FALSE(registry.Find(extId, L"icon-a").has_value());
	EXPECT_FALSE(registry.Find(L"icon-a").has_value());
}

TEST(CExtensionIconFontRegistry, UnregisterExtensionRemovesIconsAndGlobalEntry)
{
	const auto* font = NthAvailableSystemFont(0);
	if (font == nullptr) {
		GTEST_SKIP() << kNoSystemFontSkipMessage;
	}

	TempDirectory root;
	std::filesystem::create_directories(root.GetPath() / L"images");
	WriteBytesToFile(root.GetPath() / L"images" / L"font.ttf", font->rawBytes);
	WritePackageJson(root.GetPath(), R"JSON({
  "contributes": { "icons": { "icon-b": { "default": { "fontPath": "./images/font.ttf", "fontCharacter": "\\E904" } } } }
})JSON");

	const std::wstring extId = L"vendor.unregister";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extId, root.GetPath()));
	ASSERT_TRUE(registry.Find(extId, L"icon-b").has_value());
	ASSERT_TRUE(registry.Find(L"icon-b").has_value());

	registry.UnregisterExtension(extId);
	EXPECT_FALSE(registry.Find(extId, L"icon-b").has_value());
	EXPECT_FALSE(registry.Find(L"icon-b").has_value());
}

TEST(CExtensionIconFontRegistry, ClearRemovesEverything)
{
	const auto* font = NthAvailableSystemFont(0);
	if (font == nullptr) {
		GTEST_SKIP() << kNoSystemFontSkipMessage;
	}

	TempDirectory rootA;
	TempDirectory rootB;
	std::filesystem::create_directories(rootA.GetPath() / L"images");
	std::filesystem::create_directories(rootB.GetPath() / L"images");
	// icon id が両拡張で異なる（ここでは重複排除は検証対象外）ため、同じ
	// システムフォントを 2 か所へ複製しても Clear の検証したい性質は損なわれない。
	WriteBytesToFile(rootA.GetPath() / L"images" / L"a.ttf", font->rawBytes);
	WriteBytesToFile(rootB.GetPath() / L"images" / L"b.ttf", font->rawBytes);
	WritePackageJson(rootA.GetPath(), R"JSON({ "contributes": { "icons": { "icon-clear-a": { "default": { "fontPath": "./images/a.ttf", "fontCharacter": "\\E905" } } } } })JSON");
	WritePackageJson(rootB.GetPath(), R"JSON({ "contributes": { "icons": { "icon-clear-b": { "default": { "fontPath": "./images/b.ttf", "fontCharacter": "\\E906" } } } } })JSON");

	const std::wstring extIdA = L"vendor.clear-a";
	const std::wstring extIdB = L"vendor.clear-b";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extIdA, rootA.GetPath()));
	ASSERT_TRUE(registry.RegisterExtension(extIdB, rootB.GetPath()));
	ASSERT_TRUE(registry.Find(extIdA, L"icon-clear-a").has_value());
	ASSERT_TRUE(registry.Find(extIdB, L"icon-clear-b").has_value());

	registry.Clear();
	EXPECT_FALSE(registry.Find(extIdA, L"icon-clear-a").has_value());
	EXPECT_FALSE(registry.Find(extIdB, L"icon-clear-b").has_value());
	EXPECT_FALSE(registry.Find(L"icon-clear-a").has_value());
	EXPECT_FALSE(registry.Find(L"icon-clear-b").has_value());
}

// ---- 実 VS Code の IconRegistry 互換のグローバル解決（コーディネーター要求分） ----

TEST(CExtensionIconFontRegistry, GlobalFindResolvesFirstRegisteredOnDuplicateId)
{
	// 「勝者」と「敗者」の書体名を区別できることを証明するため、意図的に
	// 2 つの異なるシステムフォントを使う。
	const auto* fontA = NthAvailableSystemFont(0);
	const auto* fontB = NthAvailableSystemFont(1);
	if (fontA == nullptr || fontB == nullptr) {
		GTEST_SKIP() << kNeedTwoSystemFontsSkipMessage;
	}
	// 勝者/敗者の判定はファミリー名の比較で行うため、2 本が同名だと以下の
	// EXPECT_EQ は「先勝ち」を証明せず素通りする。候補リストが将来変わっても
	// 空振りしないよう、区別可能であること自体を前提として明示する。
	ASSERT_NE(fontA->expectedFamilyName, fontB->expectedFamilyName);

	TempDirectory rootA;
	TempDirectory rootB;
	std::filesystem::create_directories(rootA.GetPath() / L"images");
	std::filesystem::create_directories(rootB.GetPath() / L"images");
	WriteBytesToFile(rootA.GetPath() / L"images" / L"a.ttf", fontA->rawBytes);
	WriteBytesToFile(rootB.GetPath() / L"images" / L"b.ttf", fontB->rawBytes);
	WritePackageJson(rootA.GetPath(), R"JSON({ "contributes": { "icons": { "shared-icon": { "default": { "fontPath": "./images/a.ttf", "fontCharacter": "\\E910" } } } } })JSON");
	WritePackageJson(rootB.GetPath(), R"JSON({ "contributes": { "icons": { "shared-icon": { "default": { "fontPath": "./images/b.ttf", "fontCharacter": "\\E920" } } } } })JSON");

	const std::wstring extIdA = L"vendor.dedup-a";
	const std::wstring extIdB = L"vendor.dedup-b";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extIdA, rootA.GetPath())); // A が先に登録される
	ASSERT_TRUE(registry.RegisterExtension(extIdB, rootB.GetPath())); // B が後から同じ id を宣言する

	// 実 VS Code の registerIcon() と同じく、グローバル解決は最初の登録(A)を保持する
	const auto global = registry.Find(L"shared-icon");
	ASSERT_TRUE(global.has_value());
	EXPECT_EQ(fontA->expectedFamilyName, global->faceName);
	ASSERT_EQ(std::size_t{ 1 }, global->glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE910), global->glyph[0]);

	// 名前空間つき Find はグローバルな重複排除と独立に、それぞれ自分の宣言を引ける
	const auto namespacedB = registry.Find(extIdB, L"shared-icon");
	ASSERT_TRUE(namespacedB.has_value());
	EXPECT_EQ(fontB->expectedFamilyName, namespacedB->faceName);
	ASSERT_EQ(std::size_t{ 1 }, namespacedB->glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE920), namespacedB->glyph[0]);
}

TEST(CExtensionIconFontRegistry, GlobalFindTransfersToNextRegistrantWhenWinnerUnregisters)
{
	// GlobalFindResolvesFirstRegisteredOnDuplicateId と同じ理由で 2 つの異なる
	// システムフォントを使い、引き継ぎ後の書体名が確かに B のものであることを
	// A の書体名との取り違えなく検証できるようにする。
	const auto* fontA = NthAvailableSystemFont(0);
	const auto* fontB = NthAvailableSystemFont(1);
	if (fontA == nullptr || fontB == nullptr) {
		GTEST_SKIP() << kNeedTwoSystemFontsSkipMessage;
	}
	// 引き継ぎ先が B であることをファミリー名で判定するため、2 本が同名では
	// 「A のまま残っている」場合と区別できない。区別可能性を前提として明示する。
	ASSERT_NE(fontA->expectedFamilyName, fontB->expectedFamilyName);

	TempDirectory rootA;
	TempDirectory rootB;
	std::filesystem::create_directories(rootA.GetPath() / L"images");
	std::filesystem::create_directories(rootB.GetPath() / L"images");
	WriteBytesToFile(rootA.GetPath() / L"images" / L"a.ttf", fontA->rawBytes);
	WriteBytesToFile(rootB.GetPath() / L"images" / L"b.ttf", fontB->rawBytes);
	WritePackageJson(rootA.GetPath(), R"JSON({ "contributes": { "icons": { "shared-icon": { "default": { "fontPath": "./images/a.ttf", "fontCharacter": "\\E930" } } } } })JSON");
	WritePackageJson(rootB.GetPath(), R"JSON({ "contributes": { "icons": { "shared-icon": { "default": { "fontPath": "./images/b.ttf", "fontCharacter": "\\E940" } } } } })JSON");

	const std::wstring extIdA = L"vendor.transfer-a";
	const std::wstring extIdB = L"vendor.transfer-b";
	icons::CExtensionIconFontRegistry registry;
	ASSERT_TRUE(registry.RegisterExtension(extIdA, rootA.GetPath()));
	ASSERT_TRUE(registry.RegisterExtension(extIdB, rootB.GetPath()));

	const auto beforeUnregister = registry.Find(L"shared-icon");
	ASSERT_TRUE(beforeUnregister.has_value());
	ASSERT_EQ(fontA->expectedFamilyName, beforeUnregister->faceName); // 前提: A が最初勝ちしている

	registry.UnregisterExtension(extIdA);

	// A が登録解除されたので、グローバル解決は B へ「引き継がれる」。
	// nullopt になったり、A の古い書体名がダングリングで残ったりしてはならない。
	const auto transferred = registry.Find(L"shared-icon");
	ASSERT_TRUE(transferred.has_value());
	EXPECT_EQ(fontB->expectedFamilyName, transferred->faceName);
	ASSERT_EQ(std::size_t{ 1 }, transferred->glyph.size());
	EXPECT_EQ(static_cast<wchar_t>(0xE940), transferred->glyph[0]);

	// A の名前空間つき解決はもう存在しないが、B は引き続き自分の宣言を保持する
	EXPECT_FALSE(registry.Find(extIdA, L"shared-icon").has_value());
	EXPECT_TRUE(registry.Find(extIdB, L"shared-icon").has_value());
}
