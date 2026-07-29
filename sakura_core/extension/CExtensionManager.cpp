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
#include <chrono>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#include "extension/CHttpClient.h"
#include "io/CZipFile.h"
#include "util/file.h"
#include "util/string_ex.h"

namespace {

//! Shell の展開完了を待つ上限
constexpr int kExtractWaitMs = 30 * 1000;

//! 展開完了確認の間隔
constexpr int kExtractPollMs = 50;

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

// Shell による展開の完了を待つ
bool CExtensionManager::WaitForExtracted(const std::filesystem::path& marker, std::wstring& errorMsg)
{
	// CZipFile::Unzip は Shell の CopyHere を使うため、呼び出しから戻った時点では
	// 展開が終わっていない。想定の成果物が現れるまで待つ。
	for (int nElapsedMs = 0; nElapsedMs < kExtractWaitMs; nElapsedMs += kExtractPollMs) {
		std::error_code ec;
		if (std::filesystem::exists(marker, ec)) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kExtractPollMs));
	}

	errorMsg = L"timed out waiting for '" + marker.wstring() + L"'";
	return false;
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
bool CExtensionManager::Install(const SOpenVsxExtension& ext, std::wstring& errorMsg)
{
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

	// VSIX は ZIP だが、Shell の Zip Folder は拡張子で判定するため
	// .vsix のままでは開けない。.zip 名の一時ファイルに落とす。
	const std::filesystem::path tempPath =
		std::filesystem::temp_directory_path(ec) / (sFolderName + L".vsix.zip");
	if (ec) {
		errorMsg = L"cannot resolve the temporary directory";
		return false;
	}

	// 失敗しても一時ファイルを残さない
	struct TempFileGuard {
		const std::filesystem::path& path;
		~TempFileGuard() { std::error_code ec2; std::filesystem::remove(path, ec2); }
	} tempGuard{ tempPath };

	CHttpClient cHttp;
	if (!cHttp.IsOk()) {
		errorMsg = L"cannot open an HTTP session";
		return false;
	}

	if (!cHttp.Download(ext.sDownloadUrl, tempPath, errorMsg)) {
		return false;
	}

	// レジストリが sha256 を公開しているなら必ず検証する。
	// 取得したバイト列が公開されたものと一致することの確認になる。
	if (!ext.sSha256Url.empty()) {
		CHttpClient::Response sha256Response;
		std::wstring sha256Error;
		if (!cHttp.Get(ext.sSha256Url, sha256Response, sha256Error) || !sha256Response.IsOk()) {
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

	// ここから先で失敗したら導入先を残さない
	struct DestDirGuard {
		const std::filesystem::path&	path;
		bool							bCommitted = false;
		~DestDirGuard()
		{
			if (!bCommitted) {
				std::error_code ec2;
				std::filesystem::remove_all(path, ec2);
			}
		}
	} destGuard{ destDir };

	if (!std::filesystem::create_directories(destDir, ec) && ec) {
		errorMsg = L"cannot create '" + destDir.wstring() + L"': " + FormatErrorCode(ec);
		return false;
	}

	CZipFile cZipFile;
	if (!cZipFile.IsOk()) {
		errorMsg = L"the shell zip support is unavailable";
		return false;
	}
	if (!cZipFile.SetZip(tempPath)) {
		errorMsg = L"the downloaded package is not a valid zip archive";
		return false;
	}
	if (!cZipFile.Unzip(destDir)) {
		errorMsg = L"cannot extract the package into '" + destDir.wstring() + L"'";
		return false;
	}

	// VSIX は extension/package.json を必ず含む。これを展開完了の目印とし、
	// 同時に「拡張として成立しているか」の検査も兼ねる。
	const std::filesystem::path manifestPath = destDir / kVsixContentDir / kManifestFileName;
	if (!WaitForExtracted(manifestPath, errorMsg)) {
		return false;
	}

	destGuard.bCommitted = true;
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
bool CExtensionManager::Uninstall(const std::wstring& sUniqueId, std::wstring& errorMsg)
{
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

	std::filesystem::remove_all(installed.dir, ec);
	if (ec) {
		errorMsg = L"cannot remove '" + installed.dir.wstring() + L"': " + FormatErrorCode(ec);
		return false;
	}

	return true;
}
