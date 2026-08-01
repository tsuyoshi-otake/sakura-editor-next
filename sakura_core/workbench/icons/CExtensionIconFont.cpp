/*! @file
	@brief 拡張機能の contributes.icons を native なアイコンフォントとして登録する
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/icons/CExtensionIconFont.h"

#include <picojson/picojson.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>
#include <tuple>

#include "io/CZipFile.h"

// このファイルは package.json / フォントファイルを直接 std::ifstream /
// std::filesystem で読む。io/CLAUDE.md は新規 workbench コードに
// IFileService/IFileSystemProvider の利用を求めているが、拡張機能の
// マニフェストとアイコンフォントは常にローカルディスク上の
// インストール済み拡張ディレクトリ配下にしか存在せず、既存の
// CExtensionManager::ReadDisplayName（sakura_core/extension/CExtensionManager.cpp）
// も同じ理由で直接 I/O を使っている。本コンポーネントの公開 API
// （RegisterExtension の extensionRoot 引数）自体も std::filesystem::path を
// 受け取る設計であり、この前例に合わせる。

namespace workbench::icons::detail {

namespace {

// ---- 汎用のビッグエンディアン読み書きヘルパー ----------------------------

[[nodiscard]] bool ReadU16BE(std::span<const std::byte> data, std::size_t offset, uint16_t& out) noexcept
{
	if (offset > data.size() || data.size() - offset < 2) {
		return false;
	}
	out = static_cast<uint16_t>(
		(static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset])) << 8) |
		static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + 1])));
	return true;
}

[[nodiscard]] bool ReadU32BE(std::span<const std::byte> data, std::size_t offset, uint32_t& out) noexcept
{
	if (offset > data.size() || data.size() - offset < 4) {
		return false;
	}
	out = (static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset])) << 24) |
		(static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + 1])) << 16) |
		(static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + 2])) << 8) |
		static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + 3]));
	return true;
}

void WriteU16BE(std::vector<std::byte>& buffer, std::size_t offset, uint16_t value) noexcept
{
	buffer[offset] = static_cast<std::byte>((value >> 8) & 0xFFu);
	buffer[offset + 1] = static_cast<std::byte>(value & 0xFFu);
}

void WriteU32BE(std::vector<std::byte>& buffer, std::size_t offset, uint32_t value) noexcept
{
	buffer[offset] = static_cast<std::byte>((value >> 24) & 0xFFu);
	buffer[offset + 1] = static_cast<std::byte>((value >> 16) & 0xFFu);
	buffer[offset + 2] = static_cast<std::byte>((value >> 8) & 0xFFu);
	buffer[offset + 3] = static_cast<std::byte>(value & 0xFFu);
}

[[nodiscard]] std::size_t Align4(std::size_t value) noexcept
{
	return (value + 3) & ~static_cast<std::size_t>(3);
}

[[nodiscard]] uint16_t Log2Floor(uint16_t value) noexcept
{
	uint16_t result = 0;
	while (value > 1) {
		value = static_cast<uint16_t>(value >> 1);
		++result;
	}
	return result;
}

// ---- 定数 ------------------------------------------------------------

// フォントファイル（WOFF1/TTF/OTF/TTC いずれも）の入力サイズ上限。
// 実用アイコンフォントは数十〜数百 KB 程度だが、装飾的な複数ウェイト入り
// TTC 等にも余裕を持たせつつ、悪意あるファイルによる無制限確保は防ぐ。
constexpr std::size_t kMaxFontFileBytes = 16u * 1024 * 1024;

constexpr std::size_t kWoffHeaderBytes = 44;
constexpr std::size_t kWoffTableDirEntryBytes = 20;
constexpr std::size_t kSfntTableDirEntryBytes = 16;

// 実在するフォントはせいぜい十数〜数十テーブル。過大な numTables は
// ディレクトリ解析コストと確保量を膨らませる攻撃なので早期に拒否する。
constexpr std::size_t kMaxTableCount = 128;

// WOFF テーブル 1 件あたりの展開後サイズ上限。kMaxFontFileBytes と同じ桁。
// CZipFile::InflateZlibStream 自身にも 64 MiB の上限があるが、他クラスの
// 内部定数に暗黙に依存しないよう、このクラス独自の上限を先に適用する。
constexpr std::size_t kMaxTableOrigLengthBytes = 16u * 1024 * 1024;

// 再構築後の sfnt 全体サイズ上限。
constexpr std::size_t kMaxReconstructedSfntBytes = 32u * 1024 * 1024;

constexpr uint32_t kWoffSignature = 0x774F4646u;   // 'wOFF'
constexpr uint32_t kWoff2Signature = 0x774F4632u;  // 'wOF2'
constexpr uint32_t kTtcSignature = 0x74746366u;    // 'ttcf'
constexpr uint32_t kTrueTypeSignature = 0x00010000u;
constexpr uint32_t kOttoSignature = 0x4F54544Fu;   // 'OTTO'
constexpr uint32_t kMacTrueTypeSignature = 0x74727565u; // 'true'

constexpr uint32_t kNameTag = 0x6E616D65u; // 'name'

constexpr uint16_t kPlatformWindows = 3;
constexpr uint16_t kEncodingWindowsUnicodeBmp = 1;
constexpr uint16_t kPlatformMacintosh = 1;
constexpr uint16_t kNameIdFontFamily = 1;
constexpr uint16_t kNameIdUniqueId = 3;
constexpr uint16_t kNameIdFullName = 4;
constexpr uint16_t kNameIdPostScriptName = 6;
constexpr uint16_t kNameIdTypographicFamily = 16;

// name テーブルの storage area 内オフセットは uint16 なので、合成後の
// 文字列領域はこの大きさを超えられない。
constexpr std::size_t kMaxNameStorageBytes = 0xFFFFu;

// head テーブルの checkSumAdjustment に使う魔法定数（sfnt 仕様）。
constexpr uint32_t kSfntCheckSumMagic = 0xB1B0AFBAu;
constexpr uint32_t kHeadTag = 0x68656164u; // 'head'
constexpr std::size_t kHeadCheckSumAdjustmentOffset = 8;

constexpr std::size_t kNameTableRecordBytes = 12;
constexpr std::size_t kMaxNameRecords = 4096; // name テーブルの実レコード数は通常数十件程度

struct DecodedWoffTable {
	uint32_t tag = 0;
	uint32_t checksum = 0;       // WOFF エントリの origChecksum をそのまま引き継ぐ
	std::vector<std::byte> data; // origLength バイトぶんの展開済みデータ
};

EFontDecodeError DecodeWoff1(std::span<const std::byte> raw, std::vector<std::byte>& outSfntBytes)
{
	outSfntBytes.clear();
	if (raw.size() < kWoffHeaderBytes) {
		return EFontDecodeError::HeaderTooShort;
	}

	uint32_t flavor = 0;
	uint16_t numTables = 0;
	if (!ReadU32BE(raw, 4, flavor) || !ReadU16BE(raw, 12, numTables)) {
		return EFontDecodeError::MalformedHeader;
	}
	if (numTables == 0 || numTables > kMaxTableCount) {
		return EFontDecodeError::TableCountOutOfRange;
	}

	const std::size_t directoryStart = kWoffHeaderBytes;
	const std::size_t directoryBytes = static_cast<std::size_t>(numTables) * kWoffTableDirEntryBytes;
	if (raw.size() - directoryStart < directoryBytes) {
		return EFontDecodeError::TableOutOfRange;
	}

	std::vector<DecodedWoffTable> tables;
	tables.reserve(numTables);

	std::size_t reconstructedTotal = 12 + static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;

	for (uint16_t i = 0; i < numTables; ++i) {
		const std::size_t entryOffset = directoryStart + static_cast<std::size_t>(i) * kWoffTableDirEntryBytes;
		uint32_t tag = 0, tableOffset = 0, compLength = 0, origLength = 0, origChecksum = 0;
		if (!ReadU32BE(raw, entryOffset, tag) ||
			!ReadU32BE(raw, entryOffset + 4, tableOffset) ||
			!ReadU32BE(raw, entryOffset + 8, compLength) ||
			!ReadU32BE(raw, entryOffset + 12, origLength) ||
			!ReadU32BE(raw, entryOffset + 16, origChecksum)) {
			return EFontDecodeError::MalformedHeader;
		}
		if (origLength > kMaxTableOrigLengthBytes) {
			return EFontDecodeError::TableTooLarge;
		}
		if (tableOffset > raw.size() || compLength > raw.size() - tableOffset) {
			return EFontDecodeError::TableOutOfRange;
		}

		const auto compressedSpan = raw.subspan(tableOffset, compLength);
		std::vector<std::byte> tableData;
		if (compLength == origLength) {
			// 無圧縮格納
			tableData.assign(compressedSpan.begin(), compressedSpan.end());
		}
		else {
			// zlib (RFC 1950) 圧縮格納。CZipFile が唯一の miniz 実体化点。
			if (!CZipFile::InflateZlibStream(compressedSpan, origLength, tableData)) {
				return EFontDecodeError::ZlibInflateFailed;
			}
		}

		reconstructedTotal += Align4(tableData.size());
		if (reconstructedTotal > kMaxReconstructedSfntBytes) {
			return EFontDecodeError::ReconstructedFontTooLarge;
		}

		tables.push_back(DecodedWoffTable{ tag, origChecksum, std::move(tableData) });
	}

	// sfnt のテーブルディレクトリは tag 昇順でなければならない（WOFF の
	// ディレクトリ順は任意なので、ここで並べ替える）。
	std::sort(tables.begin(), tables.end(),
		[](const DecodedWoffTable& a, const DecodedWoffTable& b) { return a.tag < b.tag; });

	outSfntBytes.assign(reconstructedTotal, std::byte{ 0 });

	const uint16_t entrySelector = Log2Floor(numTables);
	const uint16_t searchRangeUnit = static_cast<uint16_t>(1u << entrySelector);
	const uint16_t searchRange = static_cast<uint16_t>(searchRangeUnit * 16);
	const uint16_t rangeShift = static_cast<uint16_t>(numTables * 16 - searchRange);

	WriteU32BE(outSfntBytes, 0, flavor);
	WriteU16BE(outSfntBytes, 4, numTables);
	WriteU16BE(outSfntBytes, 6, searchRange);
	WriteU16BE(outSfntBytes, 8, entrySelector);
	WriteU16BE(outSfntBytes, 10, rangeShift);

	std::size_t dataCursor = 12 + static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;
	for (uint16_t i = 0; i < numTables; ++i) {
		const DecodedWoffTable& table = tables[i];
		const std::size_t dirEntryOffset = 12 + static_cast<std::size_t>(i) * kSfntTableDirEntryBytes;
		WriteU32BE(outSfntBytes, dirEntryOffset, table.tag);
		WriteU32BE(outSfntBytes, dirEntryOffset + 4, table.checksum);
		WriteU32BE(outSfntBytes, dirEntryOffset + 8, static_cast<uint32_t>(dataCursor));
		WriteU32BE(outSfntBytes, dirEntryOffset + 12, static_cast<uint32_t>(table.data.size()));

		std::copy(table.data.begin(), table.data.end(), outSfntBytes.begin() + static_cast<std::ptrdiff_t>(dataCursor));
		dataCursor += Align4(table.data.size()); // パディング部分は resize 時に既に 0 埋め済み
	}

	return EFontDecodeError::None;
}

[[nodiscard]] bool FindSfntTable(
	std::span<const std::byte> sfnt, uint32_t tag, std::size_t& outOffset, std::size_t& outLength) noexcept
{
	std::size_t base = 0;
	uint32_t sfntVersion = 0;
	if (!ReadU32BE(sfnt, 0, sfntVersion)) {
		return false;
	}
	if (sfntVersion == kTtcSignature) {
		// TTC: 先頭フォント（インデックス 0）のオフセットテーブルを使う
		uint32_t numFonts = 0;
		uint32_t firstOffset = 0;
		if (!ReadU32BE(sfnt, 8, numFonts) || numFonts == 0 || !ReadU32BE(sfnt, 12, firstOffset)) {
			return false;
		}
		base = firstOffset;
	}

	uint16_t numTables = 0;
	if (base > sfnt.size() || sfnt.size() - base < 12 || !ReadU16BE(sfnt, base + 4, numTables)) {
		return false;
	}
	if (numTables == 0 || numTables > kMaxTableCount) {
		return false;
	}
	const std::size_t directoryStart = base + 12;
	const std::size_t directoryBytes = static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;
	if (directoryStart > sfnt.size() || sfnt.size() - directoryStart < directoryBytes) {
		return false;
	}

	for (uint16_t i = 0; i < numTables; ++i) {
		const std::size_t entryOffset = directoryStart + static_cast<std::size_t>(i) * kSfntTableDirEntryBytes;
		uint32_t entryTag = 0, entryOffsetValue = 0, entryLength = 0;
		if (!ReadU32BE(sfnt, entryOffset, entryTag) ||
			!ReadU32BE(sfnt, entryOffset + 8, entryOffsetValue) ||
			!ReadU32BE(sfnt, entryOffset + 12, entryLength)) {
			return false;
		}
		if (entryTag != tag) {
			continue;
		}
		if (entryOffsetValue > sfnt.size() || entryLength > sfnt.size() - entryOffsetValue) {
			return false;
		}
		outOffset = entryOffsetValue;
		outLength = entryLength;
		return true;
	}
	return false;
}

// ---- name テーブルの合成（GDI が要求する nameID 3 の補完） ----------------

//! sfnt のテーブルチェックサム（32bit ワードの単純加算、末尾は 0 埋め）。
[[nodiscard]] uint32_t SfntTableChecksum(std::span<const std::byte> data) noexcept
{
	uint32_t sum = 0;
	std::size_t cursor = 0;
	for (; cursor + 4 <= data.size(); cursor += 4) {
		uint32_t word = 0;
		(void)ReadU32BE(data, cursor, word);
		sum += word;
	}
	if (cursor < data.size()) {
		uint32_t tail = 0;
		for (std::size_t i = 0; i < 4; ++i) {
			const uint8_t byte = (cursor + i) < data.size()
				? std::to_integer<uint8_t>(data[cursor + i])
				: uint8_t{ 0 };
			tail = (tail << 8) | byte;
		}
		sum += tail;
	}
	return sum;
}

//! sfnt テーブルディレクトリの 1 エントリと、その実データ。
struct SfntTable {
	uint32_t tag = 0;
	uint32_t checksum = 0;
	std::vector<std::byte> data;
};

//! name テーブルの 1 レコード（文字列は実バイト列を複製して持つ）。
struct NameRecord {
	uint16_t platformID = 0;
	uint16_t encodingID = 0;
	uint16_t languageID = 0;
	uint16_t nameID = 0;
	std::vector<std::byte> value;
};

//! ディレクトリ順（tag 昇順）でテーブルを全部読み出す。TTC と壊れた入力は false。
[[nodiscard]] bool ReadAllSfntTables(std::span<const std::byte> sfnt, std::vector<SfntTable>& outTables)
{
	outTables.clear();

	uint32_t sfntVersion = 0;
	if (!ReadU32BE(sfnt, 0, sfntVersion) || sfntVersion == kTtcSignature) {
		return false;
	}
	uint16_t numTables = 0;
	if (!ReadU16BE(sfnt, 4, numTables) || numTables == 0 || numTables > kMaxTableCount) {
		return false;
	}
	const std::size_t directoryBytes = static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;
	if (sfnt.size() < 12 || sfnt.size() - 12 < directoryBytes) {
		return false;
	}

	outTables.reserve(numTables);
	for (uint16_t i = 0; i < numTables; ++i) {
		const std::size_t entryOffset = 12 + static_cast<std::size_t>(i) * kSfntTableDirEntryBytes;
		uint32_t tag = 0, checksum = 0, offset = 0, length = 0;
		if (!ReadU32BE(sfnt, entryOffset, tag) || !ReadU32BE(sfnt, entryOffset + 4, checksum) ||
			!ReadU32BE(sfnt, entryOffset + 8, offset) || !ReadU32BE(sfnt, entryOffset + 12, length)) {
			return false;
		}
		if (offset > sfnt.size() || length > sfnt.size() - offset) {
			return false;
		}
		const auto body = sfnt.subspan(offset, length);
		outTables.push_back(SfntTable{ tag, checksum, std::vector<std::byte>(body.begin(), body.end()) });
	}
	return true;
}

//! name テーブル（format 0）を全レコードへ分解する。
[[nodiscard]] bool ParseNameRecords(std::span<const std::byte> nameTable, std::vector<NameRecord>& outRecords)
{
	outRecords.clear();

	uint16_t count = 0, storageOffset = 0;
	if (nameTable.size() < 6 || !ReadU16BE(nameTable, 2, count) || !ReadU16BE(nameTable, 4, storageOffset)) {
		return false;
	}
	if (count == 0 || count > kMaxNameRecords) {
		return false;
	}
	const std::size_t recordsBytes = static_cast<std::size_t>(count) * kNameTableRecordBytes;
	if (nameTable.size() - 6 < recordsBytes || storageOffset > nameTable.size()) {
		return false;
	}

	outRecords.reserve(count);
	for (uint16_t i = 0; i < count; ++i) {
		const std::size_t recordOffset = 6 + static_cast<std::size_t>(i) * kNameTableRecordBytes;
		NameRecord record;
		uint16_t length = 0, offset = 0;
		if (!ReadU16BE(nameTable, recordOffset, record.platformID) ||
			!ReadU16BE(nameTable, recordOffset + 2, record.encodingID) ||
			!ReadU16BE(nameTable, recordOffset + 4, record.languageID) ||
			!ReadU16BE(nameTable, recordOffset + 6, record.nameID) ||
			!ReadU16BE(nameTable, recordOffset + 8, length) ||
			!ReadU16BE(nameTable, recordOffset + 10, offset)) {
			return false;
		}
		const std::size_t valueStart = static_cast<std::size_t>(storageOffset) + offset;
		if (valueStart > nameTable.size() || nameTable.size() - valueStart < length) {
			return false;
		}
		const auto body = nameTable.subspan(valueStart, length);
		record.value.assign(body.begin(), body.end());
		outRecords.push_back(std::move(record));
	}
	return true;
}

//! name レコード列を format 0 の name テーブルへ書き戻す。
[[nodiscard]] bool BuildNameTable(std::vector<NameRecord> records, std::vector<std::byte>& outTable)
{
	// name テーブルのレコードは (platformID, encodingID, languageID, nameID) の
	// 昇順でなければならない。合成したレコードを末尾へ足すだけでは並びが崩れる。
	std::sort(records.begin(), records.end(), [](const NameRecord& a, const NameRecord& b) {
		return std::tie(a.platformID, a.encodingID, a.languageID, a.nameID) <
			std::tie(b.platformID, b.encodingID, b.languageID, b.nameID);
	});

	std::size_t storageBytes = 0;
	for (const auto& record : records) {
		storageBytes += record.value.size();
	}
	if (records.size() > kMaxNameRecords || storageBytes > kMaxNameStorageBytes) {
		return false;
	}

	const std::size_t headerBytes = 6 + records.size() * kNameTableRecordBytes;
	if (headerBytes > kMaxNameStorageBytes) {
		return false;
	}
	outTable.assign(headerBytes + storageBytes, std::byte{ 0 });
	WriteU16BE(outTable, 0, 0); // format 0
	WriteU16BE(outTable, 2, static_cast<uint16_t>(records.size()));
	WriteU16BE(outTable, 4, static_cast<uint16_t>(headerBytes));

	std::size_t valueCursor = 0;
	for (std::size_t i = 0; i < records.size(); ++i) {
		const NameRecord& record = records[i];
		const std::size_t recordOffset = 6 + i * kNameTableRecordBytes;
		WriteU16BE(outTable, recordOffset, record.platformID);
		WriteU16BE(outTable, recordOffset + 2, record.encodingID);
		WriteU16BE(outTable, recordOffset + 4, record.languageID);
		WriteU16BE(outTable, recordOffset + 6, record.nameID);
		WriteU16BE(outTable, recordOffset + 8, static_cast<uint16_t>(record.value.size()));
		WriteU16BE(outTable, recordOffset + 10, static_cast<uint16_t>(valueCursor));
		std::copy(record.value.begin(), record.value.end(),
			outTable.begin() + static_cast<std::ptrdiff_t>(headerBytes + valueCursor));
		valueCursor += record.value.size();
	}
	return true;
}

//! テーブル一式から sfnt を組み直す（tag 昇順・4 バイト整列・チェックサム再計算）。
[[nodiscard]] bool AssembleSfnt(
	uint32_t sfntVersion, std::vector<SfntTable> tables, std::vector<std::byte>& outSfntBytes)
{
	if (tables.empty() || tables.size() > kMaxTableCount) {
		return false;
	}
	std::sort(tables.begin(), tables.end(),
		[](const SfntTable& a, const SfntTable& b) { return a.tag < b.tag; });

	const auto numTables = static_cast<uint16_t>(tables.size());
	std::size_t totalBytes = 12 + static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;
	for (const auto& table : tables) {
		totalBytes += Align4(table.data.size());
		if (totalBytes > kMaxReconstructedSfntBytes) {
			return false;
		}
	}

	outSfntBytes.assign(totalBytes, std::byte{ 0 });

	const uint16_t entrySelector = Log2Floor(numTables);
	const auto searchRange = static_cast<uint16_t>((1u << entrySelector) * 16);
	const auto rangeShift = static_cast<uint16_t>(numTables * 16 - searchRange);
	WriteU32BE(outSfntBytes, 0, sfntVersion);
	WriteU16BE(outSfntBytes, 4, numTables);
	WriteU16BE(outSfntBytes, 6, searchRange);
	WriteU16BE(outSfntBytes, 8, entrySelector);
	WriteU16BE(outSfntBytes, 10, rangeShift);

	std::size_t headOffset = 0;
	std::size_t dataCursor = 12 + static_cast<std::size_t>(numTables) * kSfntTableDirEntryBytes;
	for (uint16_t i = 0; i < numTables; ++i) {
		const SfntTable& table = tables[i];
		const std::size_t dirEntryOffset = 12 + static_cast<std::size_t>(i) * kSfntTableDirEntryBytes;
		WriteU32BE(outSfntBytes, dirEntryOffset, table.tag);
		WriteU32BE(outSfntBytes, dirEntryOffset + 4, table.checksum);
		WriteU32BE(outSfntBytes, dirEntryOffset + 8, static_cast<uint32_t>(dataCursor));
		WriteU32BE(outSfntBytes, dirEntryOffset + 12, static_cast<uint32_t>(table.data.size()));
		std::copy(table.data.begin(), table.data.end(),
			outSfntBytes.begin() + static_cast<std::ptrdiff_t>(dataCursor));
		if (table.tag == kHeadTag) {
			headOffset = dataCursor;
		}
		dataCursor += Align4(table.data.size());
	}

	// テーブル配置が変わったので head.checkSumAdjustment は必ず作り直す。
	// 古い値を残すと「フォント全体のチェックサム」が自己矛盾する。
	if (headOffset != 0 && outSfntBytes.size() - headOffset >= kHeadCheckSumAdjustmentOffset + 4) {
		WriteU32BE(outSfntBytes, headOffset + kHeadCheckSumAdjustmentOffset, 0);
		const uint32_t fileChecksum = SfntTableChecksum(outSfntBytes);
		WriteU32BE(outSfntBytes, headOffset + kHeadCheckSumAdjustmentOffset,
			kSfntCheckSumMagic - fileChecksum);
	}
	return true;
}

} // namespace

ENameRepairResult EnsureUniqueFontIdentifier(std::vector<std::byte>& sfntBytes)
{
	uint32_t sfntVersion = 0;
	if (!ReadU32BE(sfntBytes, 0, sfntVersion)) {
		return ENameRepairResult::Skipped;
	}

	std::vector<SfntTable> tables;
	if (!ReadAllSfntTables(sfntBytes, tables)) {
		return ENameRepairResult::Skipped;
	}
	const auto nameIt = std::find_if(tables.begin(), tables.end(),
		[](const SfntTable& table) { return table.tag == kNameTag; });
	if (nameIt == tables.end()) {
		return ENameRepairResult::Skipped;
	}

	std::vector<NameRecord> records;
	if (!ParseNameRecords(nameIt->data, records)) {
		return ENameRepairResult::Skipped;
	}
	if (std::any_of(records.begin(), records.end(),
			[](const NameRecord& record) { return record.nameID == kNameIdUniqueId; })) {
		return ENameRepairResult::NotNeeded;
	}

	// 合成元は「そのフォント自身が名乗っている名前」に限る。無ければ何もしない。
	// Windows レコードを優先するのは、GDI が実際に読むのがそちらだからである。
	const auto pick = [&records](uint16_t platformID, uint16_t nameID) -> const NameRecord* {
		const auto found = std::find_if(records.begin(), records.end(),
			[platformID, nameID](const NameRecord& record) {
				return record.platformID == platformID && record.nameID == nameID && !record.value.empty();
			});
		return found == records.end() ? nullptr : &*found;
	};
	const NameRecord* source = pick(kPlatformWindows, kNameIdFontFamily);
	if (source == nullptr) source = pick(kPlatformWindows, kNameIdPostScriptName);
	if (source == nullptr) source = pick(kPlatformWindows, kNameIdFullName);
	if (source == nullptr) source = pick(kPlatformMacintosh, kNameIdFontFamily);
	if (source == nullptr) source = pick(kPlatformMacintosh, kNameIdPostScriptName);
	if (source == nullptr) source = pick(kPlatformMacintosh, kNameIdFullName);
	if (source == nullptr) {
		return ENameRepairResult::Skipped;
	}

	NameRecord synthesized = *source;
	synthesized.nameID = kNameIdUniqueId;
	records.push_back(std::move(synthesized));

	std::vector<std::byte> rebuiltNameTable;
	if (!BuildNameTable(std::move(records), rebuiltNameTable)) {
		return ENameRepairResult::Skipped;
	}
	nameIt->checksum = SfntTableChecksum(rebuiltNameTable);
	nameIt->data = std::move(rebuiltNameTable);

	std::vector<std::byte> rebuilt;
	if (!AssembleSfnt(sfntVersion, std::move(tables), rebuilt)) {
		return ENameRepairResult::Skipped;
	}
	sfntBytes = std::move(rebuilt);
	return ENameRepairResult::Repaired;
}

EFontDecodeError LoadFontAsSfnt(std::span<const std::byte> rawFileBytes, std::vector<std::byte>& outSfntBytes)
{
	outSfntBytes.clear();
	if (rawFileBytes.empty()) {
		return EFontDecodeError::FileEmpty;
	}
	if (rawFileBytes.size() > kMaxFontFileBytes) {
		return EFontDecodeError::FileTooLarge;
	}
	if (rawFileBytes.size() < 4) {
		return EFontDecodeError::HeaderTooShort;
	}

	uint32_t magic = 0;
	if (!ReadU32BE(rawFileBytes, 0, magic)) {
		return EFontDecodeError::HeaderTooShort;
	}

	if (magic == kWoffSignature) {
		return DecodeWoff1(rawFileBytes, outSfntBytes);
	}
	if (magic == kWoff2Signature) {
		// WOFF2 (Brotli 圧縮) は非対応。WOFF1 と区別できる専用の理由で
		// 明示的に失敗させ、デコードは試みない。
		return EFontDecodeError::UnsupportedWoff2;
	}
	if (magic == kTrueTypeSignature || magic == kOttoSignature || magic == kMacTrueTypeSignature ||
		magic == kTtcSignature) {
		// TTF/OTF/TTC はそのまま sfnt として扱う。内容の妥当性検証は
		// AddFontMemResourceEx（Win32 側）に委ねる。
		outSfntBytes.assign(rawFileBytes.begin(), rawFileBytes.end());
		return EFontDecodeError::None;
	}
	return EFontDecodeError::UnrecognizedFormat;
}

EFamilyNameError ExtractFamilyName(std::span<const std::byte> sfntBytes, std::wstring& outFamilyName)
{
	outFamilyName.clear();

	std::size_t nameOffset = 0, nameLength = 0;
	if (!FindSfntTable(sfntBytes, kNameTag, nameOffset, nameLength)) {
		return EFamilyNameError::TableNotFound;
	}

	uint16_t count = 0, stringOffset = 0;
	if (nameLength < 6 || !ReadU16BE(sfntBytes, nameOffset + 2, count) ||
		!ReadU16BE(sfntBytes, nameOffset + 4, stringOffset)) {
		return EFamilyNameError::MalformedNameTable;
	}
	if (count > kMaxNameRecords) {
		return EFamilyNameError::MalformedNameTable;
	}
	const std::size_t recordsBytes = static_cast<std::size_t>(count) * kNameTableRecordBytes;
	if (nameLength < 6 + recordsBytes) {
		return EFamilyNameError::MalformedNameTable;
	}
	const std::size_t storageAreaOffset = nameOffset + stringOffset;
	if (storageAreaOffset > sfntBytes.size()) {
		return EFamilyNameError::MalformedNameTable;
	}

	struct Candidate {
		uint16_t platformID = 0;
		uint16_t nameID = 0;
		std::size_t stringOffsetAbs = 0;
		uint16_t length = 0;
	};
	std::optional<Candidate> best16;  // platform=3/encoding=1/nameID=16 (Typographic Family)
	std::optional<Candidate> best1;   // platform=3/encoding=1/nameID=1  (Font Family)
	std::optional<Candidate> bestMac; // platform=1 (Macintosh)、nameID 16 を 1 より優先

	for (uint16_t i = 0; i < count; ++i) {
		const std::size_t recordOffset = nameOffset + 6 + static_cast<std::size_t>(i) * kNameTableRecordBytes;
		uint16_t platformID = 0, encodingID = 0, languageID = 0, nameID = 0, length = 0, offset = 0;
		if (!ReadU16BE(sfntBytes, recordOffset, platformID) ||
			!ReadU16BE(sfntBytes, recordOffset + 2, encodingID) ||
			!ReadU16BE(sfntBytes, recordOffset + 4, languageID) ||
			!ReadU16BE(sfntBytes, recordOffset + 6, nameID) ||
			!ReadU16BE(sfntBytes, recordOffset + 8, length) ||
			!ReadU16BE(sfntBytes, recordOffset + 10, offset)) {
			return EFamilyNameError::MalformedNameTable;
		}
		static_cast<void>(languageID); // 優先順位の判定には言語を使わない

		if (static_cast<std::size_t>(offset) > sfntBytes.size() - storageAreaOffset ||
			static_cast<std::size_t>(length) > sfntBytes.size() - storageAreaOffset - offset) {
			continue; // このレコード 1 件だけを無視し、テーブル全体は捨てない
		}
		const Candidate candidate{ platformID, nameID, storageAreaOffset + offset, length };

		if (platformID == kPlatformWindows && encodingID == kEncodingWindowsUnicodeBmp &&
			nameID == kNameIdTypographicFamily && !best16) {
			best16 = candidate;
		}
		else if (platformID == kPlatformWindows && encodingID == kEncodingWindowsUnicodeBmp &&
			nameID == kNameIdFontFamily && !best1) {
			best1 = candidate;
		}
		else if (platformID == kPlatformMacintosh &&
			(nameID == kNameIdTypographicFamily || nameID == kNameIdFontFamily)) {
			if (!bestMac || (bestMac->nameID != kNameIdTypographicFamily && nameID == kNameIdTypographicFamily)) {
				bestMac = candidate;
			}
		}
	}

	const Candidate* chosen = best16 ? &*best16 : (best1 ? &*best1 : (bestMac ? &*bestMac : nullptr));
	if (chosen == nullptr) {
		return EFamilyNameError::NameMissing;
	}

	std::wstring decoded;
	if (chosen->platformID == kPlatformMacintosh) {
		// タスク指示に基づき Latin-1 として単純にデコードする（実際の Mac Roman
		// とは厳密には異なるが、意図的な簡略化として扱う。詳細は
		// workbench/icons/CLAUDE.md 参照）。
		decoded.reserve(chosen->length);
		for (uint16_t i = 0; i < chosen->length; ++i) {
			decoded.push_back(static_cast<wchar_t>(std::to_integer<uint8_t>(sfntBytes[chosen->stringOffsetAbs + i])));
		}
	}
	else {
		if (chosen->length % 2 != 0) {
			return EFamilyNameError::MalformedNameTable;
		}
		decoded.reserve(chosen->length / 2);
		for (std::size_t i = 0; i < chosen->length; i += 2) {
			const uint16_t unit = static_cast<uint16_t>(
				(static_cast<uint32_t>(std::to_integer<uint8_t>(sfntBytes[chosen->stringOffsetAbs + i])) << 8) |
				static_cast<uint32_t>(std::to_integer<uint8_t>(sfntBytes[chosen->stringOffsetAbs + i + 1])));
			decoded.push_back(static_cast<wchar_t>(unit));
		}
	}

	if (decoded.empty()) {
		return EFamilyNameError::NameMissing;
	}
	if (decoded.size() > static_cast<std::size_t>(LF_FACESIZE - 1)) {
		return EFamilyNameError::NameTooLong;
	}
	outFamilyName = std::move(decoded);
	return EFamilyNameError::None;
}

namespace {

[[nodiscard]] bool DecodeUtf8CodepointExact(std::string_view text, uint32_t& outCodepoint) noexcept
{
	if (text.empty() || text.size() > 4) {
		return false;
	}
	const auto b0 = static_cast<unsigned char>(text[0]);
	std::size_t expectedLen = 0;
	uint32_t codepoint = 0;
	if ((b0 & 0x80u) == 0x00u) {
		expectedLen = 1;
		codepoint = b0;
	}
	else if ((b0 & 0xE0u) == 0xC0u) {
		expectedLen = 2;
		codepoint = b0 & 0x1Fu;
	}
	else if ((b0 & 0xF0u) == 0xE0u) {
		expectedLen = 3;
		codepoint = b0 & 0x0Fu;
	}
	else if ((b0 & 0xF8u) == 0xF0u) {
		expectedLen = 4;
		codepoint = b0 & 0x07u;
	}
	else {
		return false;
	}
	if (text.size() != expectedLen) {
		return false;
	}
	for (std::size_t i = 1; i < expectedLen; ++i) {
		const auto b = static_cast<unsigned char>(text[i]);
		if ((b & 0xC0u) != 0x80u) {
			return false;
		}
		codepoint = (codepoint << 6) | (b & 0x3Fu);
	}
	// 過長エンコーディング・有効範囲・サロゲート域を拒否する
	constexpr uint32_t kMinForLen[5] = { 0, 0, 0x80u, 0x800u, 0x10000u };
	if (codepoint < kMinForLen[expectedLen] || codepoint > 0x10FFFFu ||
		(codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
		return false;
	}
	outCodepoint = codepoint;
	return true;
}

void AppendCodepointAsUtf16(uint32_t codepoint, std::wstring& out)
{
	if (codepoint <= 0xFFFFu) {
		out.push_back(static_cast<wchar_t>(codepoint));
	}
	else {
		const uint32_t adjusted = codepoint - 0x10000u;
		out.push_back(static_cast<wchar_t>(0xD800u + (adjusted >> 10)));
		out.push_back(static_cast<wchar_t>(0xDC00u + (adjusted & 0x3FFu)));
	}
}

} // namespace

EFontCharacterError ParseFontCharacter(std::string_view rawUtf8, std::wstring& outGlyph)
{
	outGlyph.clear();
	if (rawUtf8.empty()) {
		return EFontCharacterError::Empty;
	}

	if (rawUtf8.front() == '\\') {
		const std::string_view hex = rawUtf8.substr(1);
		if (hex.empty() || hex.size() > 6) {
			return EFontCharacterError::InvalidEscape;
		}
		uint32_t codepoint = 0;
		for (const char ch : hex) {
			uint32_t digit = 0;
			if (ch >= '0' && ch <= '9') {
				digit = static_cast<uint32_t>(ch - '0');
			}
			else if (ch >= 'A' && ch <= 'F') {
				digit = static_cast<uint32_t>(ch - 'A' + 10);
			}
			else if (ch >= 'a' && ch <= 'f') {
				digit = static_cast<uint32_t>(ch - 'a' + 10);
			}
			else {
				return EFontCharacterError::InvalidEscape;
			}
			codepoint = (codepoint << 4) | digit;
		}
		if (codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
			return EFontCharacterError::InvalidEscape;
		}
		AppendCodepointAsUtf16(codepoint, outGlyph);
		return EFontCharacterError::None;
	}

	uint32_t codepoint = 0;
	if (!DecodeUtf8CodepointExact(rawUtf8, codepoint)) {
		return EFontCharacterError::InvalidUtf8;
	}
	AppendCodepointAsUtf16(codepoint, outGlyph);
	return EFontCharacterError::None;
}

namespace {

[[nodiscard]] bool IsPathWithinRoot(
	const std::filesystem::path& canonicalRoot, const std::filesystem::path& canonicalCandidate) noexcept
{
	const auto& rootNative = canonicalRoot.native();
	const auto& candidateNative = canonicalCandidate.native();
	if (candidateNative.size() < rootNative.size() ||
		candidateNative.compare(0, rootNative.size(), rootNative) != 0) {
		return false;
	}
	// 完全一致、またはルートの直後がパス区切り文字であることを要求する。
	// 単純な文字列 prefix 判定だけだと "root" と "rootEvil" を誤って
	// 同一ルート配下と判定してしまう。
	return candidateNative.size() == rootNative.size() ||
		candidateNative[rootNative.size()] == std::filesystem::path::preferred_separator;
}

} // namespace

bool ResolveExtensionIconFontPath(
	const std::filesystem::path& extensionRoot, std::string_view fontPathUtf8, std::filesystem::path& outResolved)
{
	outResolved.clear();
	if (fontPathUtf8.empty()) {
		return false;
	}

	// ロケールに依存せず UTF-8 として解釈する
	const std::u8string u8FontPath(
		reinterpret_cast<const char8_t*>(fontPathUtf8.data()), fontPathUtf8.size());
	const std::filesystem::path relative(u8FontPath);
	if (relative.empty() || relative.is_absolute()) {
		// std::filesystem::path::operator/ は右辺が絶対パスだと左辺を丸ごと
		// 捨てて置き換えてしまう。ここで弾かないと、拡張ルートの外を指す
		// 絶対パスがそのまま通ってしまう。
		return false;
	}

	std::error_code rootEc;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(extensionRoot, rootEc);
	if (rootEc) {
		return false;
	}

	const std::filesystem::path candidate = extensionRoot / relative;
	std::error_code candidateEc;
	const std::filesystem::path canonicalCandidate = std::filesystem::weakly_canonical(candidate, candidateEc);
	if (candidateEc) {
		return false;
	}

	if (!IsPathWithinRoot(canonicalRoot, canonicalCandidate)) {
		return false;
	}

	outResolved = canonicalCandidate;
	return true;
}

// ---- CRegisteredMemoryFont ---------------------------------------------

CRegisteredMemoryFont::CRegisteredMemoryFont(std::vector<std::byte> sfntBytes) noexcept
	: m_fontBytes(std::move(sfntBytes))
{
	if (m_fontBytes.empty()) {
		return;
	}
	DWORD numFontsInstalled = 0;
	HANDLE handle = ::AddFontMemResourceEx(
		m_fontBytes.data(), static_cast<DWORD>(m_fontBytes.size()), nullptr, &numFontsInstalled);
	if (handle != nullptr && numFontsInstalled == 0) {
		// ハンドルが返っても実際には 0 個しか登録されなかった場合は失敗として扱う
		::RemoveFontMemResourceEx(handle);
		handle = nullptr;
	}
	m_fontResourceHandle = handle;
}

CRegisteredMemoryFont::~CRegisteredMemoryFont()
{
	if (m_fontResourceHandle != nullptr) {
		::RemoveFontMemResourceEx(static_cast<HANDLE>(m_fontResourceHandle));
	}
}

} // namespace workbench::icons::detail

namespace workbench::icons {

namespace {

// package.json の読み込み上限（sakura_core/extension/CExtensionManager.cpp の
// kMaxManifestBytes と同じ桁を踏襲する）
constexpr std::size_t kMaxManifestBytes = 4u * 1024 * 1024;
constexpr const wchar_t* kManifestFileName = L"package.json";

// detail::LoadFontAsSfnt 側の kMaxFontFileBytes（16 MiB）と同じ値。
// ファイル読み込み時点で早期に足切りするための、このファイル独自の定数として
// 意図的に重複させている（detail 側の匿名名前空間定数を名前空間をまたいで
// 参照する複雑さを避けるため）。値を変更する場合は両方を揃えること。
constexpr std::size_t kMaxIconFontFileBytes = 16u * 1024 * 1024;

bool ReadManifestJson(const std::filesystem::path& manifestPath, picojson::value& outRoot)
{
	std::error_code ec;
	const auto size = std::filesystem::file_size(manifestPath, ec);
	if (ec || size == 0 || size > kMaxManifestBytes) {
		return false;
	}
	std::ifstream in(manifestPath, std::ios::binary);
	if (!in) {
		return false;
	}
	const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (const std::string err = picojson::parse(outRoot, json); !err.empty()) {
		return false;
	}
	return outRoot.is<picojson::object>();
}

// picojson のオブジェクトキー（アイコン ID）は UTF-8 の std::string で来る。
// std::wstring(narrowString) は単純なバイト幅拡張になってしまい、非 ASCII
// アイコン ID を壊すため、明示的に UTF-8 -> UTF-16 変換する。VS Code の
// contributes.icons の id は慣習的に ASCII のケバブケースだが、このコードは
// それを前提にせず、不正な UTF-8 のアイコン ID はこのアイコンだけを黙って
// 読み飛ばす（拡張全体は失敗させない）。
[[nodiscard]] bool DecodeUtf8ToWide(std::string_view utf8, std::wstring& outWide)
{
	outWide.clear();
	std::size_t i = 0;
	while (i < utf8.size()) {
		const auto b0 = static_cast<unsigned char>(utf8[i]);
		std::size_t len = 0;
		uint32_t codepoint = 0;
		if ((b0 & 0x80u) == 0x00u) {
			len = 1;
			codepoint = b0;
		}
		else if ((b0 & 0xE0u) == 0xC0u) {
			len = 2;
			codepoint = b0 & 0x1Fu;
		}
		else if ((b0 & 0xF0u) == 0xE0u) {
			len = 3;
			codepoint = b0 & 0x0Fu;
		}
		else if ((b0 & 0xF8u) == 0xF0u) {
			len = 4;
			codepoint = b0 & 0x07u;
		}
		else {
			return false;
		}
		if (utf8.size() - i < len) {
			return false;
		}
		for (std::size_t k = 1; k < len; ++k) {
			const auto b = static_cast<unsigned char>(utf8[i + k]);
			if ((b & 0xC0u) != 0x80u) {
				return false;
			}
			codepoint = (codepoint << 6) | (b & 0x3Fu);
		}
		constexpr uint32_t kMinForLen[5] = { 0, 0, 0x80u, 0x800u, 0x10000u };
		if (codepoint < kMinForLen[len] || codepoint > 0x10FFFFu ||
			(codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
			return false;
		}
		if (codepoint <= 0xFFFFu) {
			outWide.push_back(static_cast<wchar_t>(codepoint));
		}
		else {
			const uint32_t adjusted = codepoint - 0x10000u;
			outWide.push_back(static_cast<wchar_t>(0xD800u + (adjusted >> 10)));
			outWide.push_back(static_cast<wchar_t>(0xDC00u + (adjusted & 0x3FFu)));
		}
		i += len;
	}
	return true;
}

bool ReadFontFileBytes(const std::filesystem::path& fontPath, std::vector<std::byte>& outBytes)
{
	outBytes.clear();
	std::error_code ec;
	const auto size = std::filesystem::file_size(fontPath, ec);
	if (ec || size == 0 || size > kMaxIconFontFileBytes) {
		return false;
	}
	std::ifstream in(fontPath, std::ios::binary);
	if (!in) {
		return false;
	}
	std::vector<std::byte> raw(static_cast<std::size_t>(size));
	if (!in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
		return false;
	}
	outBytes = std::move(raw);
	return true;
}

} // namespace

struct CExtensionIconFontRegistry::Impl {
	struct IconEntry {
		std::wstring fontKey;
		std::wstring glyph;
	};
	struct FontSlot {
		std::unique_ptr<detail::CRegisteredMemoryFont> font;
		std::wstring familyName;
		std::size_t refCount = 0;
	};
	//! グローバルアイコン id レジストリ（実 VS Code の IconRegistry 相当）上の
	//! 1 件の登録候補。実 VS Code の registerIcon() は重複 id を「最初の登録を
	//! 保持し、以降はエラーとして無視する」ため、ここでも sequence の一番小さい
	//! （＝最初に登録された）現存候補が常に「勝者」になる。
	struct GlobalIconCandidate {
		std::wstring extensionId;
		std::wstring fontKey;
		std::wstring glyph;
	};
	struct ExtensionEntry {
		std::map<std::wstring, IconEntry, std::less<>> icons;
		std::vector<std::wstring> fontKeysUsed; // 重複込み（参照カウント管理のため）
		// この拡張がグローバル候補として登録した (iconId, sequence) の一覧。
		// UnregisterExtensionLocked はこの拡張の候補だけを id ごとの候補集合から
		// 正確に除去するためにこれを使う（他拡張の候補には触れない）。
		std::vector<std::pair<std::wstring, std::uint64_t>> globalCandidacies;
	};

	std::map<std::wstring, ExtensionEntry, std::less<>> extensions;
	std::map<std::wstring, FontSlot, std::less<>> fonts;

	// アイコン id -> (登録 sequence -> 候補) 。std::map の begin() は常に
	// sequence が最小＝最初に登録され、かつまだ生きている候補を指すため、
	// 「最初勝ち」の勝者はここから O(log n) で引ける。ある拡張が登録解除されて
	// 最有力候補が消えても、次に古い候補が自動的に繰り上がる
	// （= 別拡張への「引き継ぎ」）。
	std::map<std::wstring, std::map<std::uint64_t, GlobalIconCandidate>, std::less<>> globalIconCandidates;
	std::uint64_t nextGlobalSequence = 0;

	void ReleaseFontRef(const std::wstring& fontKey)
	{
		const auto it = fonts.find(fontKey);
		if (it == fonts.end()) {
			return;
		}
		if (it->second.refCount > 0) {
			--it->second.refCount;
		}
		if (it->second.refCount == 0) {
			fonts.erase(it); // FontSlot のデストラクタが RemoveFontMemResourceEx を呼ぶ
		}
	}

	void UnregisterExtensionLocked(std::wstring_view extensionId)
	{
		const auto it = extensions.find(extensionId);
		if (it == extensions.end()) {
			return;
		}
		for (const auto& [iconIdWide, sequence] : it->second.globalCandidacies) {
			const auto idIt = globalIconCandidates.find(iconIdWide);
			if (idIt == globalIconCandidates.end()) {
				continue;
			}
			idIt->second.erase(sequence);
			if (idIt->second.empty()) {
				globalIconCandidates.erase(idIt);
			}
		}
		for (const std::wstring& fontKey : it->second.fontKeysUsed) {
			ReleaseFontRef(fontKey);
		}
		extensions.erase(it);
	}
};

CExtensionIconFontRegistry::CExtensionIconFontRegistry() : m_impl(std::make_unique<Impl>()) {}

CExtensionIconFontRegistry::~CExtensionIconFontRegistry() = default;

bool CExtensionIconFontRegistry::RegisterExtension(
	std::wstring_view extensionId, const std::filesystem::path& extensionRoot)
{
	if (extensionId.empty()) {
		return false;
	}
	const std::wstring id(extensionId);

	// 冪等性: 既存登録があれば、読み直す前に必ず先に破棄する
	// （フォントハンドルの多重リーク・重複登録を防ぐ）。
	m_impl->UnregisterExtensionLocked(id);

	picojson::value root;
	const std::filesystem::path manifestPath = extensionRoot / kManifestFileName;
	if (!ReadManifestJson(manifestPath, root)) {
		return false;
	}
	const picojson::object& rootObj = root.get<picojson::object>();

	Impl::ExtensionEntry entry;

	const auto contributesIt = rootObj.find("contributes");
	if (contributesIt != rootObj.end() && contributesIt->second.is<picojson::object>()) {
		const picojson::object& contributesObj = contributesIt->second.get<picojson::object>();
		const auto iconsIt = contributesObj.find("icons");
		if (iconsIt != contributesObj.end() && iconsIt->second.is<picojson::object>()) {
			const picojson::object& iconsObj = iconsIt->second.get<picojson::object>();

			for (const auto& [iconId, iconValue] : iconsObj) {
				if (iconId.empty() || !iconValue.is<picojson::object>()) {
					continue; // このアイコンだけ読み飛ばす
				}
				// picojson のオブジェクトキー（アイコン id）は UTF-8 の std::string で
				// 来る。std::wstring(iconId) は単純なバイト幅拡張になってしまい非 ASCII
				// アイコン id を壊すため、明示的に UTF-8 -> UTF-16 変換する。
				std::wstring iconIdWide;
				if (!DecodeUtf8ToWide(iconId, iconIdWide)) {
					continue; // このアイコンだけ読み飛ばす（不正な UTF-8 のアイコン id）
				}
				const picojson::object& iconObj = iconValue.get<picojson::object>();
				const auto defaultIt = iconObj.find("default");
				if (defaultIt == iconObj.end() || !defaultIt->second.is<picojson::object>()) {
					continue;
				}
				const picojson::object& defaultObj = defaultIt->second.get<picojson::object>();

				const auto fontPathIt = defaultObj.find("fontPath");
				const auto fontCharIt = defaultObj.find("fontCharacter");
				if (fontPathIt == defaultObj.end() || !fontPathIt->second.is<std::string>() ||
					fontCharIt == defaultObj.end() || !fontCharIt->second.is<std::string>()) {
					continue;
				}

				std::wstring glyph;
				if (detail::ParseFontCharacter(fontCharIt->second.get<std::string>(), glyph) !=
					detail::EFontCharacterError::None) {
					continue;
				}

				std::filesystem::path resolvedFontPath;
				if (!detail::ResolveExtensionIconFontPath(
						extensionRoot, fontPathIt->second.get<std::string>(), resolvedFontPath)) {
					continue;
				}
				const std::wstring fontKey = resolvedFontPath.wstring();

				auto fontIt = m_impl->fonts.find(fontKey);
				if (fontIt == m_impl->fonts.end()) {
					std::vector<std::byte> raw;
					if (!ReadFontFileBytes(resolvedFontPath, raw)) {
						continue; // このアイコンだけ諦める
					}
					std::vector<std::byte> sfntBytes;
					if (detail::LoadFontAsSfnt(raw, sfntBytes) != detail::EFontDecodeError::None) {
						continue;
					}
					// GDI は nameID 3 を持たない sfnt を無条件に拒否する。アイコン
					// フォントジェネレーターはこのレコードを省くことが多く、実 VS Code
					// （Chromium）では描けるフォントがここでだけ登録できない、という
					// 非互換になるため、登録の直前に補完する。詳細は
					// detail::EnsureUniqueFontIdentifier の宣言を参照。
					(void)detail::EnsureUniqueFontIdentifier(sfntBytes);
					std::wstring familyName;
					if (detail::ExtractFamilyName(sfntBytes, familyName) != detail::EFamilyNameError::None) {
						continue;
					}
					auto registeredFont = std::make_unique<detail::CRegisteredMemoryFont>(std::move(sfntBytes));
					if (!registeredFont->IsValid()) {
						continue; // AddFontMemResourceEx が失敗した
					}
					Impl::FontSlot slot;
					slot.font = std::move(registeredFont);
					slot.familyName = std::move(familyName);
					slot.refCount = 0;
					fontIt = m_impl->fonts.emplace(fontKey, std::move(slot)).first;
				}

				++fontIt->second.refCount;
				entry.fontKeysUsed.push_back(fontKey);

				// グローバルアイコン id レジストリ（実 VS Code の IconRegistry 相当）
				// への登録候補を追加する。this拡張の中では iconIdWide は一意
				// （picojson::object のキーであるため）なので、ここでの emplace は
				// 常に新規挿入になる。
				const std::uint64_t sequence = m_impl->nextGlobalSequence++;
				m_impl->globalIconCandidates[iconIdWide].emplace(
					sequence, Impl::GlobalIconCandidate{ id, fontKey, glyph });
				entry.globalCandidacies.emplace_back(iconIdWide, sequence);

				entry.icons.emplace(std::move(iconIdWide), Impl::IconEntry{ fontKey, std::move(glyph) });
			}
		}
	}

	m_impl->extensions.emplace(id, std::move(entry));
	return true;
}

std::optional<SExtensionContributedIcon> CExtensionIconFontRegistry::Find(
	std::wstring_view extensionId, std::wstring_view iconId) const
{
	const auto extIt = m_impl->extensions.find(extensionId);
	if (extIt == m_impl->extensions.end()) {
		return std::nullopt;
	}
	const auto iconIt = extIt->second.icons.find(iconId);
	if (iconIt == extIt->second.icons.end()) {
		return std::nullopt;
	}
	const auto fontIt = m_impl->fonts.find(iconIt->second.fontKey);
	if (fontIt == m_impl->fonts.end() || !fontIt->second.font || !fontIt->second.font->IsValid()) {
		// 何らかの理由でフォントが失われていたら、偽のグリフを返さず nullopt にする
		return std::nullopt;
	}

	SExtensionContributedIcon result;
	result.faceName = fontIt->second.familyName;
	result.glyph = iconIt->second.glyph;
	return result;
}

std::optional<SExtensionContributedIcon> CExtensionIconFontRegistry::Find(std::wstring_view iconId) const
{
	const auto idIt = m_impl->globalIconCandidates.find(iconId);
	if (idIt == m_impl->globalIconCandidates.end() || idIt->second.empty()) {
		return std::nullopt;
	}
	// begin() は sequence が最小＝最初に登録され、かつ拡張がまだ登録解除されて
	// いない現存候補。実 VS Code の registerIcon() の「最初の登録を保持する」
	// 挙動と同じ「最初勝ち」の勝者がここに来る。最有力候補の拡張が
	// UnregisterExtension されていれば、この集合からも既に取り除かれているため、
	// 次に古い候補へ自動的に「引き継がれる」。
	const Impl::GlobalIconCandidate& candidate = idIt->second.begin()->second;
	const auto fontIt = m_impl->fonts.find(candidate.fontKey);
	if (fontIt == m_impl->fonts.end() || !fontIt->second.font || !fontIt->second.font->IsValid()) {
		// 何らかの理由でフォントが失われていたら、偽のグリフを返さず nullopt にする
		return std::nullopt;
	}

	SExtensionContributedIcon result;
	result.faceName = fontIt->second.familyName;
	result.glyph = candidate.glyph;
	return result;
}

void CExtensionIconFontRegistry::UnregisterExtension(std::wstring_view extensionId)
{
	m_impl->UnregisterExtensionLocked(extensionId);
}

void CExtensionIconFontRegistry::Clear()
{
	m_impl->extensions.clear();
	m_impl->globalIconCandidates.clear();
	m_impl->fonts.clear(); // 各 FontSlot のデストラクタが RemoveFontMemResourceEx を呼ぶ
}

} // namespace workbench::icons
