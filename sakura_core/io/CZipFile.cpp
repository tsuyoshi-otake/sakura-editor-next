/*!	@file
	@brief ZIP file操作

*/
/*
	Copyright (C) 2011, Uchi
	Copyright (C) 2018-2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "io/CZipFile.h"

// miniz-cpp は C API の実装と C++ ラッパーを同じヘッダーで提供する。
// この翻訳単位だけが C API を実体化し、未使用のラッパー名はテスト側と衝突しない
// プライベート名前空間へ退避する。
#define miniz_cpp sakura_czip_miniz_cpp
#include "../../externals/miniz-cpp/zip_file.hpp"
#undef miniz_cpp

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <vector>

namespace {

constexpr uintmax_t kMaxVsixArchiveBytes = 128u * 1024 * 1024;
constexpr uint64_t kMaxVsixExtractedBytes = 256u * 1024 * 1024;
constexpr uint64_t kMaxVsixEntryBytes = 64u * 1024 * 1024;
constexpr mz_uint kMaxVsixEntries = 4096;

// InflateZlibStream() の展開後サイズ上限。単一 VSIX エントリの上限
// kMaxVsixEntryBytes を踏襲する。呼び出し元（WOFF フォントのテーブル等）は
// 実際にはこれよりずっと小さいが、小さい圧縮入力から巨大な確保を要求する
// zip 爆弾を防ぐため、上限は必ず設ける。
constexpr std::size_t kMaxInflateExpandedBytes = 64u * 1024 * 1024;

constexpr std::array<const wchar_t*, 22> kReservedDeviceNames = {
	L"CON", L"PRN", L"AUX", L"NUL",
	L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
	L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
};

bool IsSafeWindowsPathComponent(std::wstring_view component)
{
	if (component.empty() || component == L"." || component == L".." ||
		component.back() == L'.' || component.back() == L' ') {
		return false;
	}
	for (const wchar_t ch : component) {
		if (ch < 0x20 || ch == 0x7f || ch == L'\\' || ch == L'/' || ch == L':' ||
			ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
			return false;
		}
	}
	const std::wstring stem(component.substr(0, component.find(L'.')));
	for (const wchar_t* reserved : kReservedDeviceNames) {
		if (0 == wmemicmp(stem.c_str(), reserved)) {
			return false;
		}
	}
	return true;
}

bool IsCancelled(const std::atomic<bool>* pCancelled) noexcept
{
	return pCancelled && pCancelled->load(std::memory_order_acquire);
}

bool Utf8ToWideStrict(std::string_view source, std::wstring& result)
{
	result.clear();
	if (source.empty() || source.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) {
		return false;
	}
	const int chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
		static_cast<int>(source.size()), nullptr, 0);
	if (chars <= 0) {
		return false;
	}
	result.resize(chars);
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
		static_cast<int>(source.size()), result.data(), chars) == chars;
}

bool IsSafeDirectory(const std::filesystem::path& path)
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool EnsureSafeDirectories(
	const std::filesystem::path& root,
	const std::filesystem::path& relative,
	std::wstring& errorMsg)
{
	std::filesystem::path current = root;
	if (!IsSafeDirectory(current)) {
		errorMsg = L"unsafe extraction directory '" + current.wstring() + L"'";
		return false;
	}
	for (const auto& component : relative) {
		current /= component;
		std::error_code ec;
		if (!std::filesystem::create_directory(current, ec) && ec && ec != std::errc::file_exists) {
			errorMsg = L"cannot create extraction directory '" + current.wstring() + L"'";
			return false;
		}
		if (!IsSafeDirectory(current)) {
			errorMsg = L"unsafe extraction directory '" + current.wstring() + L"'";
			return false;
		}
	}
	return true;
}

struct FileHandle {
	HANDLE handle = INVALID_HANDLE_VALUE;
	~FileHandle() { if (handle != INVALID_HANDLE_VALUE) { ::CloseHandle(handle); } }
	FileHandle(const FileHandle&) = delete;
	FileHandle& operator=(const FileHandle&) = delete;
	FileHandle() = default;
};

struct ZipFileSink {
	HANDLE handle = INVALID_HANDLE_VALUE;
	const std::atomic<bool>* cancelled = nullptr;
	mz_uint64 bytesWritten = 0;
	mz_uint64 expectedBytes = 0;
};

size_t WriteZipFileSink(void* opaque, mz_uint64 offset, const void* data, size_t bytes)
{
	auto* sink = static_cast<ZipFileSink*>(opaque);
	if (IsCancelled(sink->cancelled) || offset != sink->bytesWritten ||
		sink->bytesWritten > sink->expectedBytes ||
		static_cast<mz_uint64>(bytes) > sink->expectedBytes - sink->bytesWritten) {
		return 0;
	}
	const auto* cursor = static_cast<const BYTE*>(data);
	size_t remaining = bytes;
	while (remaining != 0) {
		if (IsCancelled(sink->cancelled)) {
			return 0;
		}
		const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
		DWORD written = 0;
		if (!::WriteFile(sink->handle, cursor, chunk, &written, nullptr) || written != chunk) {
			return 0;
		}
		cursor += written;
		remaining -= written;
	}
	sink->bytesWritten += static_cast<mz_uint64>(bytes);
	return bytes;
}

struct ZipReaderHolder {
	mz_zip_archive archive = {};
	bool initialized = false;
	~ZipReaderHolder() { if (initialized) { mz_zip_reader_end(&archive); } }
};

struct VsixEntry {
	mz_uint index = 0;
	std::filesystem::path relativePath;
	bool directory = false;
};

} // namespace

// コンストラクタ
CZipFile::CZipFile() {
	if (const auto hr = m_pShellDispatch.CreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER); FAILED(hr)) {
		m_pShellDispatch = nullptr;
	}
}

// デストラクタ
CZipFile::~CZipFile() = default;

// Zip File名 設定
bool CZipFile::SetZip(const std::filesystem::path& zipPath)
{
	m_pZipFolder = nullptr;
	if (!m_pShellDispatch) {
		return false;
	}

	// ZIP Folder設定
	_variant_t var(zipPath.c_str());
	if (const auto hr = m_pShellDispatch->NameSpace(var, &m_pZipFolder); hr != S_OK) {
		m_pZipFolder = nullptr;
		return false;
	}

	m_ZipPath = zipPath;

	return true;
}

/* static */ bool CZipFile::IsSafeArchiveEntryPath(std::string_view entryName)
{
	if (entryName.empty() || entryName.size() > 1024 || entryName.front() == '/' || entryName.front() == '\\') {
		return false;
	}

	size_t begin = 0;
	while (begin < entryName.size()) {
		const size_t end = entryName.find('/', begin);
		const size_t length = (end == std::string_view::npos) ? entryName.size() - begin : end - begin;
		if (length == 0) {
			// 末尾の '/' だけはディレクトリエントリとして許可する
			return end == std::string_view::npos && begin + 1 == entryName.size();
		}
		std::wstring component;
		if (!Utf8ToWideStrict(entryName.substr(begin, length), component) || !IsSafeWindowsPathComponent(component)) {
			return false;
		}
		if (end == std::string_view::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

/* static */ bool CZipFile::InflateZlibStream(
	std::span<const std::byte> compressed,
	std::size_t expandedSize,
	std::vector<std::byte>& out) noexcept
{
	out.clear();

	// mz_ulong (unsigned long) は LLP64 では 32bit。std::size_t (64bit) の
	// 値をそのまま渡すとサイレントに切り詰められるため、事前に範囲を検査する。
	constexpr std::size_t kMaxMzUlong = static_cast<std::size_t>((std::numeric_limits<mz_ulong>::max)());
	if (compressed.empty() || expandedSize == 0 || expandedSize > kMaxInflateExpandedBytes ||
		expandedSize > kMaxMzUlong || compressed.size() > kMaxMzUlong) {
		return false;
	}

	try {
		std::vector<std::byte> buffer(expandedSize);
		mz_ulong destLen = static_cast<mz_ulong>(expandedSize);
		const int status = mz_uncompress(
			reinterpret_cast<unsigned char*>(buffer.data()), &destLen,
			reinterpret_cast<const unsigned char*>(compressed.data()),
			static_cast<mz_ulong>(compressed.size()));
		if (status != MZ_OK || destLen != static_cast<mz_ulong>(expandedSize)) {
			return false;
		}
		out = std::move(buffer);
		return true;
	}
	catch (const std::exception&) {
		out.clear();
		return false;
	}
}

/* static */ bool CZipFile::ExtractVsixSafely(
	const std::filesystem::path& zipPath,
	const std::filesystem::path& outDir,
	std::wstring& errorMsg,
	const std::atomic<bool>* pCancelled)
{
	errorMsg.clear();
	try {
		if (IsCancelled(pCancelled)) {
			errorMsg = L"VSIX extraction cancelled";
			return false;
		}
		std::error_code ec;
		const uintmax_t archiveSize = std::filesystem::file_size(zipPath, ec);
		if (ec || archiveSize == 0 || archiveSize > kMaxVsixArchiveBytes) {
			errorMsg = L"invalid VSIX archive size";
			return false;
		}
		if (!std::filesystem::create_directory(outDir, ec) && ec && ec != std::errc::file_exists) {
			errorMsg = L"cannot create extraction directory '" + outDir.wstring() + L"'";
			return false;
		}
		if (!IsSafeDirectory(outDir)) {
			errorMsg = L"unsafe extraction directory '" + outDir.wstring() + L"'";
			return false;
		}

		std::ifstream input(zipPath, std::ios::binary);
		if (!input) {
			errorMsg = L"cannot read VSIX archive";
			return false;
		}
		std::vector<char> archive(static_cast<size_t>(archiveSize));
		if (!input.read(archive.data(), static_cast<std::streamsize>(archive.size()))) {
			errorMsg = L"cannot read VSIX archive";
			return false;
		}

		ZipReaderHolder reader;
		if (!mz_zip_reader_init_mem(&reader.archive, archive.data(), archive.size(), 0)) {
			errorMsg = L"the downloaded package is not a valid zip archive";
			return false;
		}
		reader.initialized = true;
		const mz_uint count = mz_zip_reader_get_num_files(&reader.archive);
		if (count == 0 || count > kMaxVsixEntries) {
			errorMsg = L"VSIX archive has an unsafe number of entries";
			return false;
		}

		std::vector<VsixEntry> entries;
		entries.reserve(count);
		std::set<std::wstring> outputNames;
		uint64_t totalExtracted = 0;
		bool hasManifest = false;
		for (mz_uint index = 0; index < count; ++index) {
			if (IsCancelled(pCancelled)) {
				errorMsg = L"VSIX extraction cancelled";
				return false;
			}
			mz_zip_archive_file_stat stat = {};
			if (!mz_zip_reader_file_stat(&reader.archive, index, &stat) ||
				std::char_traits<char>::length(stat.m_filename) >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE - 1 ||
				mz_zip_reader_is_file_encrypted(&reader.archive, index) ||
				((stat.m_external_attr >> 16) & 0170000) == 0120000 ||
				!IsSafeArchiveEntryPath(stat.m_filename)) {
				errorMsg = L"VSIX archive contains an unsafe entry";
				return false;
			}
			const bool directory = mz_zip_reader_is_file_a_directory(&reader.archive, index) != MZ_FALSE;
			if (!directory && (stat.m_uncomp_size > kMaxVsixEntryBytes ||
				totalExtracted > kMaxVsixExtractedBytes - stat.m_uncomp_size)) {
				errorMsg = L"VSIX archive exceeds extraction limits";
				return false;
			}
			if (!directory) {
				totalExtracted += stat.m_uncomp_size;
			}

			std::wstring relativeName;
			if (!Utf8ToWideStrict(stat.m_filename, relativeName)) {
				errorMsg = L"VSIX archive contains a non-UTF-8 entry";
				return false;
			}
			const std::filesystem::path relativePath(relativeName);
			std::wstring comparison = relativePath.lexically_normal().wstring();
			std::transform(comparison.begin(), comparison.end(), comparison.begin(), [](wchar_t ch) {
				return static_cast<wchar_t>(std::towlower(ch));
			});
			if (!outputNames.insert(comparison).second) {
				errorMsg = L"VSIX archive contains duplicate entries";
				return false;
			}
			if (!directory && relativeName == L"extension/package.json") {
				hasManifest = true;
			}
			entries.push_back({ index, relativePath, directory });
		}
		if (!hasManifest) {
			errorMsg = L"VSIX archive does not contain extension/package.json";
			return false;
		}

		for (const VsixEntry& entry : entries) {
			if (IsCancelled(pCancelled)) {
				errorMsg = L"VSIX extraction cancelled";
				return false;
			}
			const std::filesystem::path outputPath = outDir / entry.relativePath;
			if (entry.directory) {
				if (!EnsureSafeDirectories(outDir, entry.relativePath, errorMsg)) {
					return false;
				}
				continue;
			}
			if (!EnsureSafeDirectories(outDir, entry.relativePath.parent_path(), errorMsg)) {
				return false;
			}
			FileHandle output;
			output.handle = ::CreateFileW(outputPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (output.handle == INVALID_HANDLE_VALUE) {
				errorMsg = L"cannot create extracted file '" + outputPath.wstring() + L"'";
				return false;
			}
			mz_zip_archive_file_stat stat = {};
			if (!mz_zip_reader_file_stat(&reader.archive, entry.index, &stat)) {
				errorMsg = L"cannot inspect VSIX archive entry";
				return false;
			}
			ZipFileSink sink{ output.handle, pCancelled, 0, stat.m_uncomp_size };
			if (!mz_zip_reader_extract_to_callback(&reader.archive, entry.index, WriteZipFileSink, &sink, 0) ||
				sink.bytesWritten != sink.expectedBytes) {
				errorMsg = L"cannot extract VSIX archive entry";
				return false;
			}
		}
		return true;
	}
	catch (const std::exception&) {
		errorMsg = L"cannot process VSIX archive";
		return false;
	}
}

// ZIP File 内 フォルダー名取得と定義ファイル検査(Plugin用)
bool CZipFile::ChkPluginDef(std::wstring_view sDefFile, std::wstring& sFolderName)
{
	sFolderName.clear();

	if (!m_pZipFolder) {
		return false;
	}

	// ZIP File List
	cxx::com_pointer<FolderItems> pZipFileItems = nullptr;
	if (const auto hr = m_pZipFolder->Items(&pZipFileItems); FAILED(hr)) {
		m_pZipFolder = nullptr;
		return false;
	}

	// 検査
	long lCount = 0;
	if (const auto hr = pZipFileItems->get_Count(&lCount); hr != S_OK) {
		return false;
	}

	for (_variant_t vari(long(0), VT_I4); vari.lVal <= lCount; ++vari.lVal) {
		cxx::com_pointer<FolderItem> pFileItem = nullptr;
		_bstr_t buffer;
		VARIANT_BOOL isFolder;
		cxx::com_pointer<FolderItems> pFileItems2 = nullptr;
		cxx::com_pointer<Folder> pFile = nullptr;
		long lCount2 = 0;

		if (FAILED(pZipFileItems->Item(vari, &pFileItem)) ||
			FAILED(pFileItem->get_Name(buffer.GetAddress())) ||
			FAILED(pFileItem->get_IsFolder(&isFolder)) ||
			!isFolder ||
			FAILED(pFileItem->get_GetFolder((IDispatch**)&pFile)) ||
			FAILED(pFile->Items(&pFileItems2)) ||
			FAILED(pFileItems2->get_Count(&lCount2)))
		{
			continue;
		}

		sFolderName = buffer;

		for (_variant_t varj(long(0), VT_I4); varj.lVal < lCount2; ++varj.lVal) {
			if (FAILED(pFileItems2->Item(varj, &pFileItem)) ||
				FAILED(pFileItem->get_IsFolder(&isFolder)) ||
				isFolder ||
				FAILED(pFileItem->get_Name(buffer.GetAddress())))
			{
				continue;
			}

			// 定義ファイルか
			if (0 == wmemicmp(buffer, std::data(sDefFile))) {
				return true;
			}
		}
	}

	return false;
}

// ZIP File 解凍
bool CZipFile::Unzip(const std::filesystem::path& outDir)
{
	if (!m_pZipFolder) {
		return false;
	}

	// ZIP File List
	cxx::com_pointer<FolderItems> pZipFileItems = nullptr;
	if (const auto hr = m_pZipFolder->Items(&pZipFileItems); FAILED(hr)) {
		m_pZipFolder = nullptr;
		return false;
	}

	// 出力Folder設定
	cxx::com_pointer<Folder> pOutFolder = nullptr;
	_variant_t var(outDir.c_str());
	if (const auto hr = m_pShellDispatch->NameSpace(var, &pOutFolder); hr != S_OK) {
		return false;
	}

	// 展開の設定
	var = _variant_t(LPDISPATCH(pZipFileItems), true);
	_variant_t varOpt(long(FOF_SILENT | FOF_NOCONFIRMATION), VT_I4);

	// 展開
	return SUCCEEDED(pOutFolder->CopyHere(var, varOpt));
}
