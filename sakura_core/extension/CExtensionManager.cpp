/*!	@file
	@brief VS Code 互換拡張の導入と管理

*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionManager.h"

#include <bcrypt.h>

#include <picojson/picojson.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "extension/CHttpClient.h"
#include "io/CZipFile.h"
#include "util/file.h"
#include "util/string_ex.h"

namespace {

//! package.json の読み込み上限
constexpr size_t kMaxManifestBytes = 4u * 1024 * 1024;

//! sha256 の 16 進表記の桁数
constexpr size_t kSha256HexLength = 64;

//! Windows の予約デバイス名
constexpr std::array<const wchar_t*, 22> kReservedDeviceNames = {
	L"CON", L"PRN", L"AUX", L"NUL",
	L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
	L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
};

//! BCrypt のアルゴリズムプロバイダー
struct AlgProviderHolder {
	BCRYPT_ALG_HANDLE h = nullptr;
	~AlgProviderHolder() { if (h) { ::BCryptCloseAlgorithmProvider(h, 0); } }
};

//! BCrypt のハッシュオブジェクト
struct HashHolder {
	BCRYPT_HASH_HANDLE h = nullptr;
	~HashHolder() { if (h) { ::BCryptDestroyHash(h); } }
};

//! 16 進 1 桁か
bool IsHexDigit(wchar_t ch) noexcept
{
	return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F');
}

/*!
	@brief error_code を表示可能な文字列にする

	std::error_code::message() は環境の既定コードページ依存の文字列を返すため、
	文字化けの余地がない値そのものを示す。UI に出す文言はこのクラスの責務ではない。
 */
std::wstring FormatErrorCode(const std::error_code& ec)
{
	return L"error " + std::to_wstring(ec.value());
}

bool IsCancelled(const std::atomic<bool>* pCancelled) noexcept
{
	return pCancelled && pCancelled->load(std::memory_order_acquire);
}

bool IsDirectoryWithoutReparsePoint(const std::filesystem::path& path)
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
		(attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool CreateStagingDirectory(
	const std::filesystem::path& baseDir,
	const std::wstring& folderName,
	std::filesystem::path& stagingDir,
	std::wstring& errorMsg)
{
	std::error_code ec;
	if (!std::filesystem::create_directories(baseDir, ec) && ec) {
		errorMsg = L"cannot create extension directory '" + baseDir.wstring() + L"': " + FormatErrorCode(ec);
		return false;
	}
	if (!IsDirectoryWithoutReparsePoint(baseDir)) {
		errorMsg = L"unsafe extension directory '" + baseDir.wstring() + L"'";
		return false;
	}

	for (int attempt = 0; attempt < 3; ++attempt) {
		GUID guid = {};
		wchar_t guidText[40] = {};
		if (FAILED(::CoCreateGuid(&guid)) || ::StringFromGUID2(guid, guidText, _countof(guidText)) == 0) {
			errorMsg = L"cannot create a unique staging directory name";
			return false;
		}
		stagingDir = baseDir / (L"." + folderName + L".staging-" + guidText);
		ec.clear();
		if (std::filesystem::create_directory(stagingDir, ec)) {
			if (IsDirectoryWithoutReparsePoint(stagingDir)) {
				return true;
			}
			std::error_code ignored;
			std::filesystem::remove_all(stagingDir, ignored);
			errorMsg = L"unsafe staging directory";
			return false;
		}
		if (ec && ec != std::errc::file_exists) {
			errorMsg = L"cannot create staging directory: " + FormatErrorCode(ec);
			return false;
		}
	}
	errorMsg = L"cannot reserve a unique staging directory";
	return false;
}

bool RemoveTree(
	const std::filesystem::path& path,
	const std::atomic<bool>* pCancelled,
	std::wstring& errorMsg)
{
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension uninstall cancelled";
		return false;
	}
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES) {
		errorMsg = L"cannot inspect '" + path.wstring() + L"'";
		return false;
	}
	std::error_code ec;
	if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		if (!std::filesystem::remove(path, ec) || ec) {
			errorMsg = L"cannot remove '" + path.wstring() + L"': " + FormatErrorCode(ec);
			return false;
		}
		return true;
	}

	for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
		if (!RemoveTree(it->path(), pCancelled, errorMsg)) {
			return false;
		}
	}
	if (ec) {
		errorMsg = L"cannot enumerate '" + path.wstring() + L"': " + FormatErrorCode(ec);
		return false;
	}
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension uninstall cancelled";
		return false;
	}
	if (!std::filesystem::remove(path, ec) || ec) {
		errorMsg = L"cannot remove '" + path.wstring() + L"': " + FormatErrorCode(ec);
		return false;
	}
	return true;
}

} // namespace

CExtensionManager::CExtensionManager()
{
	// ini と同じ階層の extensions フォルダー。plugins の扱いに倣う。
	// ただし区切り文字は付けない。付けると filename() が空になり、
	// parent_path() での照合（Uninstall）が成り立たなくなる。
	WCHAR szPath[_MAX_PATH];
	GetInidir(szPath, L"extensions");
	m_baseDir = std::filesystem::path(szPath);
}

// フォルダー名の構成要素として安全か
bool CExtensionManager::IsSafeNameComponent(std::wstring_view sComponent)
{
	if (sComponent.empty()) {
		return false;
	}

	// 相対指定はフォルダーを遡れてしまう
	if (sComponent == L"." || sComponent == L"..") {
		return false;
	}

	// 末尾の '.' と空白は Windows が黙って落とすため、別名に化ける
	if (sComponent.back() == L'.' || sComponent.back() == L' ') {
		return false;
	}

	for (const wchar_t ch : sComponent) {
		// 制御文字
		if (ch < 0x20 || ch == 0x7F) {
			return false;
		}
		// 区切り文字とファイル名に使えない文字
		if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
			ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
			return false;
		}
	}

	// 予約デバイス名（拡張子が付いていても予約扱いなので、'.' より前で判定する）
	const size_t nDotPos = sComponent.find(L'.');
	const std::wstring sStem(sComponent.substr(0, nDotPos));
	for (const wchar_t* pszReserved : kReservedDeviceNames) {
		if (0 == wmemicmp(sStem.c_str(), pszReserved)) {
			return false;
		}
	}

	return true;
}

// 導入先フォルダー名を決める
std::wstring CExtensionManager::MakeInstallFolderName(const SOpenVsxExtension& ext)
{
	// 3 要素すべてがフォルダー名の一部になるので、個別に検査する
	if (!IsSafeNameComponent(ext.sNamespace) ||
		!IsSafeNameComponent(ext.sName) ||
		!IsSafeNameComponent(ext.sVersion)) {
		return std::wstring();
	}

	return ext.sNamespace + L"." + ext.sName + L"-" + ext.sVersion;
}

// sha256 ファイルの内容から 16 進ハッシュを取り出す
std::wstring CExtensionManager::ExtractSha256Hex(const std::string& sSha256FileBody)
{
	std::wistringstream stream(u8stowcs(sSha256FileBody));
	std::wstring sToken;
	while (stream >> sToken) {
		if (sToken.size() != kSha256HexLength) {
			continue;
		}
		if (std::all_of(sToken.begin(), sToken.end(), IsHexDigit)) {
			// 比較しやすいよう小文字に揃える
			std::transform(sToken.begin(), sToken.end(), sToken.begin(),
				[](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
			return sToken;
		}
	}
	return std::wstring();
}

// ファイルの sha256 を求める
std::wstring CExtensionManager::ComputeSha256Hex(const std::filesystem::path& path)
{
	AlgProviderHolder alg;
	if (::BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
		return std::wstring();
	}

	DWORD dwHashLength = 0;
	DWORD dwResultSize = 0;
	if (::BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&dwHashLength), sizeof(dwHashLength), &dwResultSize, 0) < 0) {
		return std::wstring();
	}

	HashHolder hash;
	if (::BCryptCreateHash(alg.h, &hash.h, nullptr, 0, nullptr, 0, 0) < 0) {
		return std::wstring();
	}

	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return std::wstring();
	}

	std::string buffer(64 * 1024, '\0');
	while (in) {
		in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const auto nRead = in.gcount();
		if (nRead <= 0) {
			break;
		}
		if (::BCryptHashData(hash.h, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(nRead), 0) < 0) {
			return std::wstring();
		}
	}
	if (in.bad()) {
		return std::wstring();
	}

	std::vector<UCHAR> digest(dwHashLength);
	if (::BCryptFinishHash(hash.h, digest.data(), dwHashLength, 0) < 0) {
		return std::wstring();
	}

	constexpr wchar_t szHex[] = L"0123456789abcdef";
	std::wstring sResult;
	sResult.reserve(digest.size() * 2);
	for (const UCHAR byte : digest) {
		sResult += szHex[byte >> 4];
		sResult += szHex[byte & 0x0F];
	}
	return sResult;
}

// package.json から表示名を読む
std::wstring CExtensionManager::ReadDisplayName(const std::filesystem::path& manifestPath)
{
	std::error_code ec;
	const auto nSize = std::filesystem::file_size(manifestPath, ec);
	if (ec || nSize == 0 || nSize > kMaxManifestBytes) {
		return std::wstring();
	}

	std::ifstream in(manifestPath, std::ios::binary);
	if (!in) {
		return std::wstring();
	}
	const std::string sJson((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

	picojson::value root;
	if (const std::string sError = picojson::parse(root, sJson); !sError.empty()) {
		return std::wstring();
	}
	if (!root.is<picojson::object>()) {
		return std::wstring();
	}

	const picojson::object& obj = root.get<picojson::object>();
	for (const char* pszKey : { "displayName", "name" }) {
		const auto it = obj.find(pszKey);
		if (it != obj.end() && it->second.is<std::string>()) {
			if (std::wstring sValue = u8stowcs(it->second.get<std::string>()); !sValue.empty()) {
				return sValue;
			}
		}
	}
	return std::wstring();
}

// 拡張を導入する
bool CExtensionManager::Install(
	const SOpenVsxExtension& ext,
	std::wstring& errorMsg,
	const std::atomic<bool>* pCancelled)
{
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	const std::wstring sFolderName = MakeInstallFolderName(ext);
	if (sFolderName.empty()) {
		errorMsg = L"registry returned an unsafe extension identifier";
		return false;
	}

	const std::filesystem::path destDir = m_baseDir / sFolderName;

	std::error_code ec;
	if (std::filesystem::exists(destDir, ec)) {
		errorMsg = L"'" + sFolderName + L"' is already installed";
		return false;
	}

	std::filesystem::path stagingDir;
	if (!CreateStagingDirectory(m_baseDir, sFolderName, stagingDir, errorMsg)) {
		return false;
	}
	struct StagingDirGuard {
		const std::filesystem::path& path;
		bool committed = false;
		~StagingDirGuard() {
			if (!committed) {
				std::error_code ignored;
				std::filesystem::remove_all(path, ignored);
			}
		}
	} stagingGuard{ stagingDir };
	const std::filesystem::path tempPath = stagingDir / L"package.vsix";
	const std::filesystem::path extractedDir = stagingDir / L"contents";

	CHttpClient cHttp;
	if (!cHttp.IsOk()) {
		errorMsg = L"cannot open an HTTP session";
		return false;
	}

	if (!cHttp.Download(ext.sDownloadUrl, tempPath, errorMsg, pCancelled)) {
		return false;
	}
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}

	// レジストリが sha256 を公開しているなら必ず検証する。
	// 取得したバイト列が公開されたものと一致することの確認になる。
	if (!ext.sSha256Url.empty()) {
		CHttpClient::Response sha256Response;
		std::wstring sha256Error;
		if (!cHttp.Get(ext.sSha256Url, sha256Response, sha256Error, pCancelled) || !sha256Response.IsOk()) {
			errorMsg = L"cannot fetch the sha256 of the package: " + sha256Error;
			return false;
		}

		const std::wstring sExpected = ExtractSha256Hex(sha256Response.body);
		if (sExpected.empty()) {
			errorMsg = L"the registry returned a malformed sha256";
			return false;
		}

		const std::wstring sActual = ComputeSha256Hex(tempPath);
		if (sActual.empty()) {
			errorMsg = L"cannot compute the sha256 of the downloaded package";
			return false;
		}
		if (sActual != sExpected) {
			errorMsg = L"sha256 mismatch: expected " + sExpected + L", got " + sActual;
			return false;
		}
	}

	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	if (!CZipFile::ExtractVsixSafely(tempPath, extractedDir, errorMsg, pCancelled)) {
		return false;
	}
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}

	// 同期展開済みのマニフェストを確認してから、同一ボリューム上で確定する。
	const std::filesystem::path manifestPath = extractedDir / kVsixContentDir / kManifestFileName;
	if (!std::filesystem::is_regular_file(manifestPath, ec) || ec) {
		errorMsg = L"VSIX archive did not extract extension/package.json";
		return false;
	}
	std::filesystem::remove(tempPath, ec);
	if (ec) {
		errorMsg = L"cannot remove temporary VSIX: " + FormatErrorCode(ec);
		return false;
	}
	if (std::filesystem::exists(destDir, ec)) {
		errorMsg = L"'" + sFolderName + L"' is already installed";
		return false;
	}
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	std::filesystem::rename(extractedDir, destDir, ec);
	if (ec) {
		errorMsg = L"cannot finalize extension installation: " + FormatErrorCode(ec);
		return false;
	}
	stagingGuard.committed = true;
	std::filesystem::remove_all(stagingDir, ec); // 空になった親だけを消す
	return true;
}

// 導入済み拡張を列挙する
std::vector<SInstalledExtension> CExtensionManager::EnumInstalled() const
{
	std::vector<SInstalledExtension> results;

	std::error_code ec;
	if (!std::filesystem::exists(m_baseDir, ec)) {
		return results;
	}

	for (const auto& entry : std::filesystem::directory_iterator(m_baseDir, ec)) {
		if (ec) {
			break;
		}
		if (!entry.is_directory()) {
			continue;
		}

		const std::filesystem::path manifestPath = entry.path() / kVsixContentDir / kManifestFileName;
		std::error_code ecManifest;
		if (!std::filesystem::exists(manifestPath, ecManifest)) {
			// 拡張として成立していないフォルダーは一覧に出さない
			continue;
		}

		// フォルダー名は "namespace.name-version"
		const std::wstring sFolderName = entry.path().filename().wstring();
		const size_t nHyphenPos = sFolderName.rfind(L'-');

		SInstalledExtension installed;
		installed.dir = entry.path();
		if (nHyphenPos == std::wstring::npos) {
			installed.sUniqueId = sFolderName;
		}
		else {
			installed.sUniqueId = sFolderName.substr(0, nHyphenPos);
			installed.sVersion = sFolderName.substr(nHyphenPos + 1);
		}

		installed.sDisplayName = ReadDisplayName(manifestPath);
		if (installed.sDisplayName.empty()) {
			installed.sDisplayName = installed.sUniqueId;
		}

		results.push_back(std::move(installed));
	}

	std::sort(results.begin(), results.end(),
		[](const SInstalledExtension& lhs, const SInstalledExtension& rhs) {
			return wmemicmp(lhs.sDisplayName.c_str(), rhs.sDisplayName.c_str()) < 0;
		});

	return results;
}

// 指定 ID が導入済みか
bool CExtensionManager::FindInstalled(const std::wstring& sUniqueId, SInstalledExtension& found) const
{
	for (auto& installed : EnumInstalled()) {
		if (0 == wmemicmp(installed.sUniqueId.c_str(), sUniqueId.c_str())) {
			found = installed;
			return true;
		}
	}
	return false;
}

// 導入済み拡張を削除する
bool CExtensionManager::Uninstall(
	const std::wstring& sUniqueId,
	std::wstring& errorMsg,
	const std::atomic<bool>* pCancelled)
{
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension uninstall cancelled";
		return false;
	}
	SInstalledExtension installed;
	if (!FindInstalled(sUniqueId, installed)) {
		errorMsg = L"'" + sUniqueId + L"' is not installed";
		return false;
	}

	// 削除対象が導入先の直下であることを確かめる。
	// 万一おかしなフォルダー名を拾っていた場合に、無関係な場所を消さないため。
	std::error_code ec;
	if (installed.dir.parent_path() != m_baseDir) {
		errorMsg = L"refusing to remove '" + installed.dir.wstring() + L"'";
		return false;
	}
	if (IsCancelled(pCancelled)) {
		errorMsg = L"extension uninstall cancelled";
		return false;
	}

	return RemoveTree(installed.dir, pCancelled, errorMsg);
}
