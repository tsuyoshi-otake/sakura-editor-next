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
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

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

bool IsCancelled(const platform::request::IRequestCancellation* requestCancellation) noexcept
{
	return requestCancellation && requestCancellation->IsCancellationRequested();
}

bool IsInstallationCancelled(
	const platform::request::IRequestCancellation* requestCancellation,
	const std::atomic<bool>* pCancelled) noexcept
{
	return IsCancelled(requestCancellation) || IsCancelled(pCancelled);
}

bool WriteDownloadedVsix(
	const std::filesystem::path& path,
	const std::vector<std::uint8_t>& bytes,
	std::wstring& errorMsg)
{
	if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
		errorMsg = L"downloaded extension package is too large to stage";
		return false;
	}

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		errorMsg = L"cannot create temporary VSIX";
		return false;
	}
	if (!bytes.empty()) {
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	out.flush();
	if (!out) {
		errorMsg = L"cannot write temporary VSIX";
		return false;
	}
	return true;
}

/*!
	@brief VSIX をディスクへ逐次書き込みながら sha256 を計算するための状態

	FetchVsixStreamed() の chunk sink はこの構造体だけを介して呼ばれる。
	CExtensionManager::ComputeSha256Hex と同じ BCrypt 手順（SHA256 provider →
	hash object → 16 進エンコード）を、ファイル全体の再読み込みではなく
	chunk ごとの BCryptHashData 呼び出しへ置き換えたもの。
*/
struct StreamedVsixWrite {
	std::ofstream out;
	AlgProviderHolder alg;
	HashHolder hash;
	bool hashReady = false;
};

//! ストリーミング書き込みを開始する。出力ファイルと sha256 の両方を準備できなければ false。
bool BeginStreamedVsixWrite(
	const std::filesystem::path& path,
	StreamedVsixWrite& state,
	std::wstring& errorMsg)
{
	state.out.open(path, std::ios::binary | std::ios::trunc);
	if (!state.out) {
		errorMsg = L"cannot create temporary VSIX";
		return false;
	}
	if (::BCryptOpenAlgorithmProvider(&state.alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
		errorMsg = L"cannot initialize sha256 for streamed download";
		return false;
	}
	if (::BCryptCreateHash(state.alg.h, &state.hash.h, nullptr, 0, nullptr, 0, 0) < 0) {
		errorMsg = L"cannot initialize sha256 for streamed download";
		return false;
	}
	state.hashReady = true;
	return true;
}

/*!
	@brief 1 chunk をファイルへ書き、同時に sha256 へ取り込む

	platform::request::ResponseBodyChunkSink / OpenVsxBodyChunkSink の契約どおり、
	書き込みまたはハッシュ更新に失敗したら false を返して転送を打ち切らせる。
*/
bool WriteStreamedVsixChunk(StreamedVsixWrite& state, const std::uint8_t* data, std::size_t size)
{
	if (!state.hashReady) {
		return false;
	}
	if (size == 0) {
		return true;
	}
	if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
		return false;
	}
	state.out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	if (!state.out) {
		return false;
	}
	if (::BCryptHashData(state.hash.h, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0) {
		return false;
	}
	return true;
}

//! 書き込みを終え、16 進小文字の sha256 を返す。失敗時は空文字列
std::wstring FinishStreamedVsixWrite(StreamedVsixWrite& state)
{
	if (!state.hashReady) {
		return std::wstring();
	}
	state.out.flush();
	if (!state.out) {
		return std::wstring();
	}
	state.out.close();
	if (!state.out) {
		return std::wstring();
	}

	DWORD dwHashLength = 0;
	DWORD dwResultSize = 0;
	if (::BCryptGetProperty(state.alg.h, BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&dwHashLength), sizeof(dwHashLength), &dwResultSize, 0) < 0) {
		return std::wstring();
	}

	std::vector<UCHAR> digest(dwHashLength);
	if (::BCryptFinishHash(state.hash.h, digest.data(), dwHashLength, 0) < 0) {
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

CExtensionManager::CExtensionManager(std::filesystem::path baseDir)
	: m_baseDir(std::move(baseDir))
{
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

// 検索応答とメタデータ応答から、このホストで動く配布物を決める
bool CExtensionManager::ResolveInstallTarget(
	const SOpenVsxExtension& ext,
	const extension::openvsx::OpenVsxExtensionAssetsOperation& assets,
	std::wstring_view targetPlatform,
	SOpenVsxExtension& resolved,
	std::wstring& errorMsg)
{
	using extension::openvsx::OpenVsxProtocol;

	resolved = ext;
	errorMsg.clear();

	const auto IsUsableForThisHost = [&targetPlatform](std::wstring_view platform) {
		// 空は「プラットフォーム別ビルドを持たない拡張」を意味する
		return platform.empty()
			|| platform == targetPlatform
			|| platform == OpenVsxProtocol::kUniversalTargetPlatform;
	};

	if (!assets.status) {
		// Unsupported はメタデータ解決を持たない client。それ以外は通信・解析の失敗。
		// どちらでも手掛かりは検索応答の 1 ビルドだけなので、それがこのホストで
		// 動くと言える場合に限って使う。動かないものを黙って掴ませない。
		//
		// 検索応答は targetPlatform フィールドを持たないことがあり、そのときは
		// download URL の `@<platform>` だけがどのビルドかを語る。実測では
		// win32 機からの検索でも alpine-arm64 の URL が返るので、ここを見ないと
		// 「フィールドが空だから安全」と誤読して別プラットフォームを展開してしまう。
		std::wstring searchPlatform = ext.sTargetPlatform;
		if (searchPlatform.empty()) {
			searchPlatform = OpenVsxProtocol::TargetPlatformFromVsixUrl(ext.sDownloadUrl);
		}
		if (!IsUsableForThisHost(searchPlatform)) {
			errorMsg = L"the registry offered a " + searchPlatform + L" build but this host needs "
				+ std::wstring(targetPlatform) + L", and the extension metadata could not be resolved";
			return false;
		}
		if (resolved.sDownloadUrl.empty()) {
			errorMsg = L"the registry did not provide a download URL";
			return false;
		}
		return true;
	}

	const std::wstring sDownloadUrl = OpenVsxProtocol::SelectPlatformDownloadUrl(assets.value, targetPlatform);
	if (sDownloadUrl.empty()) {
		errorMsg = L"the extension does not provide a build for " + std::wstring(targetPlatform);
		return false;
	}

	resolved.sDownloadUrl = sDownloadUrl;
	if (!assets.value.sVersion.empty()) {
		// downloads はメタデータが指すバージョンのものなので、導入先フォルダー名も
		// そちらへ合わせる。検索応答との食い違いを名前に持ち込まない。
		resolved.sVersion = assets.value.sVersion;
	}

	resolved.sTargetPlatform.clear();
	for (const auto& [platform, url] : assets.value.downloads) {
		if (url == sDownloadUrl) {
			resolved.sTargetPlatform = platform;
			break;
		}
	}

	if (sDownloadUrl == assets.value.sDownloadUrl) {
		// files は選んだ配布物そのものを指しているので、副次資産まで信用できる
		resolved.sSha256Url = assets.value.sSha256Url;
		if (resolved.sTargetPlatform.empty()) resolved.sTargetPlatform = assets.value.sTargetPlatform;
		if (resolved.sIconUrl.empty()) resolved.sIconUrl = assets.value.sIconUrl;
		if (resolved.sReadmeUrl.empty()) resolved.sReadmeUrl = assets.value.sReadmeUrl;
		if (resolved.sChangelogUrl.empty()) resolved.sChangelogUrl = assets.value.sChangelogUrl;
	}
	else {
		// downloads は VSIX の URL しか持たない。別ビルドを選んだ以上、files の
		// sha256 は別物のハッシュなので使えない。同じ path の .sha256 を導く。
		// 導けなければ空にして、レジストリが sha256 を公開していない場合と同じ扱いにする。
		resolved.sSha256Url = OpenVsxProtocol::DeriveSha256Url(sDownloadUrl);
	}
	return true;
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
	extension::openvsx::IOpenVsxRegistryClient& registryClient,
	std::wstring& errorMsg,
	const platform::request::IRequestCancellation* requestCancellation,
	const std::atomic<bool>* pCancelled)
{
	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	if (!IsSafeNameComponent(ext.sNamespace) || !IsSafeNameComponent(ext.sName)) {
		errorMsg = L"registry returned an unsafe extension identifier";
		return false;
	}

	// 検索応答の download URL はプラットフォームを選べていない。取得を始める前に
	// メタデータ endpoint で解決し直す。
	constexpr std::wstring_view targetPlatform = extension::openvsx::OpenVsxProtocol::HostTargetPlatform();
	const auto assets = registryClient.FetchExtensionAssets(ext.sNamespace, ext.sName, requestCancellation);
	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	if (assets.status.outcome == extension::openvsx::EOpenVsxRequestOutcome::Cancelled) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	SOpenVsxExtension target;
	if (!ResolveInstallTarget(ext, assets, targetPlatform, target, errorMsg)) {
		return false;
	}

	const std::wstring sFolderName = MakeInstallFolderName(target);
	if (sFolderName.empty()) {
		errorMsg = L"registry returned an unsafe extension identifier";
		return false;
	}

	const std::filesystem::path destDir = m_baseDir / sFolderName;

	std::error_code ec;
	if (std::filesystem::exists(destDir, ec)) {
		errorMsg = L"extension is already installed";
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

	// VSIX を全量メモリーへためず、ディスクへ逐次書き込みながら sha256 も同時に
	// 計算する経路を優先する。registryClient が FetchVsixStreamed を実装しない
	// （既定の Unsupported を返す。テスト用 fake を含む）場合は、従来どおり
	// FetchVsix でメモリー上に取得してから書き出す経路へフォールバックする。
	bool usedStreamedFetch = false;
	std::wstring streamedSha256Hex;
	{
		StreamedVsixWrite streamState;
		std::wstring beginError;
		if (BeginStreamedVsixWrite(tempPath, streamState, beginError)) {
			const auto sink = [&streamState](const std::uint8_t* data, std::size_t size) {
				return WriteStreamedVsixChunk(streamState, data, size);
			};
			const auto streamedStatus = registryClient.FetchVsixStreamed(target.sDownloadUrl, sink, requestCancellation);
			if (streamedStatus.outcome != extension::openvsx::EOpenVsxRequestOutcome::Unsupported) {
				usedStreamedFetch = true;
				if (!streamedStatus) {
					errorMsg = streamedStatus.outcome == extension::openvsx::EOpenVsxRequestOutcome::Cancelled ||
						IsInstallationCancelled(requestCancellation, pCancelled)
						? L"extension installation cancelled"
						: L"cannot fetch extension package";
					return false;
				}
				if (IsInstallationCancelled(requestCancellation, pCancelled)) {
					errorMsg = L"extension installation cancelled";
					return false;
				}
				streamedSha256Hex = FinishStreamedVsixWrite(streamState);
				if (streamedSha256Hex.empty()) {
					errorMsg = L"cannot write temporary VSIX";
					return false;
				}
			}
		}
		// streamState（と保持している ofstream）はここでスコープを抜けて破棄される。
		// 実際にストリーミングを使わなかった場合でも、同じ tempPath を次の
		// フォールバック書き込みが開く前にファイルハンドルを確実に解放する。
	}

	if (!usedStreamedFetch) {
		const auto vsixResponse = registryClient.FetchVsix(target.sDownloadUrl, requestCancellation);
		if (!vsixResponse.status) {
			errorMsg = vsixResponse.status.outcome == extension::openvsx::EOpenVsxRequestOutcome::Cancelled ||
				IsInstallationCancelled(requestCancellation, pCancelled)
				? L"extension installation cancelled"
				: L"cannot fetch extension package";
			return false;
		}
		if (IsInstallationCancelled(requestCancellation, pCancelled)) {
			errorMsg = L"extension installation cancelled";
			return false;
		}
		if (!WriteDownloadedVsix(tempPath, vsixResponse.value, errorMsg)) {
			return false;
		}
	}

	// レジストリが sha256 を公開しているなら必ず検証する。
	// 取得したバイト列が公開されたものと一致することの確認になる。
	const std::optional<std::wstring> sha256Uri = target.sSha256Url.empty()
		? std::nullopt
		: std::optional<std::wstring>(target.sSha256Url);
	const auto sha256Response = registryClient.FetchOptionalSha256(sha256Uri, requestCancellation);
	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	if (sha256Response.status.outcome != extension::openvsx::EOpenVsxRequestOutcome::NotRequested) {
		if (!sha256Response.status) {
			errorMsg = sha256Response.status.outcome == extension::openvsx::EOpenVsxRequestOutcome::Cancelled ||
				IsInstallationCancelled(requestCancellation, pCancelled)
				? L"extension installation cancelled"
				: L"cannot fetch extension package integrity metadata";
			return false;
		}

		const std::string sha256Body(sha256Response.value.begin(), sha256Response.value.end());
		const std::wstring sExpected = ExtractSha256Hex(sha256Body);
		if (sExpected.empty()) {
			errorMsg = L"the registry returned a malformed sha256";
			return false;
		}

		// ストリーミング経路は書き込みと同時に増分計算した digest を再利用する。
		// tempPath を再度読み直す必要はない。フォールバック経路のみ ComputeSha256Hex
		// でファイルを読み直す。
		const std::wstring sActual = usedStreamedFetch ? streamedSha256Hex : ComputeSha256Hex(tempPath);
		if (sActual.empty()) {
			errorMsg = L"cannot compute the sha256 of the downloaded package";
			return false;
		}
		if (sActual != sExpected) {
			errorMsg = L"extension package sha256 did not match registry metadata";
			return false;
		}
	}

	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
		errorMsg = L"extension installation cancelled";
		return false;
	}
	if (!CZipFile::ExtractVsixSafely(tempPath, extractedDir, errorMsg, pCancelled)) {
		return false;
	}
	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
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
		errorMsg = L"extension is already installed";
		return false;
	}
	if (IsInstallationCancelled(requestCancellation, pCancelled)) {
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
