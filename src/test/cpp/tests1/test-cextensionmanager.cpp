/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <Windows.h>
#include "extension/CExtensionManager.h"
#include "extension/CExtensionProfileState.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

#include "config/BuiltinConfigurationDescriptors.h"
#include "config/CConfigurationService.h"
#include "extension/openvsx/OpenVsxProductionClient.h"
#include "util/file.h"

/*!
	@brief 通信を伴わない部分だけを検証する

	通信を伴わない契約と、型付き registry fake を通る失敗経路を検証する。
	実ネットワークと Shell の ZIP 展開成功経路は disabled live test に隔離する。
 */

namespace {

//! 一時ファイルを作り、抜けるときに消す
class TempFile {
public:
	explicit TempFile(std::string_view contents)
		: m_path(GetTempFilePath(L"ext"))
	{
		std::ofstream os{ m_path, std::ios::binary | std::ios::trunc };
		os.write(contents.data(), static_cast<std::streamsize>(contents.length()));
	}
	~TempFile()
	{
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
	}
	TempFile(const TempFile&) = delete;
	TempFile& operator = (const TempFile&) = delete;

	const std::filesystem::path& GetPath() const noexcept { return m_path; }

private:
	std::filesystem::path	m_path;
};

//! 導入先を実プロファイルから隔離する。失敗時の staging cleanup を観測できる。
class TempDirectory {
public:
	TempDirectory()
		: m_path(GetTempFilePath(L"extdir"))
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

extension::openvsx::OpenVsxOperationStatus SuccessfulOperation()
{
	return { extension::openvsx::EOpenVsxRequestOutcome::Success,
		platform::request::ERequestOutcome::Success, std::nullopt, {} };
}

extension::openvsx::OpenVsxOperationStatus FailedOperation(
	extension::openvsx::EOpenVsxRequestOutcome outcome,
	std::wstring message = L"secret=https://example.invalid/token")
{
	return { outcome,
		outcome == extension::openvsx::EOpenVsxRequestOutcome::Cancelled
			? platform::request::ERequestOutcome::Cancelled
			: platform::request::ERequestOutcome::TransportFailure,
		std::nullopt, std::move(message) };
}

//! 通信せず、manager が型付き failure と staging cleanup を正しく扱うかを検証する fake。
class FakeOpenVsxRegistryClient final : public extension::openvsx::IOpenVsxRegistryClient {
public:
	mutable int fetchVsixCalls = 0;
	mutable int fetchSha256Calls = 0;
	extension::openvsx::OpenVsxBinaryOperation vsix{ FailedOperation(extension::openvsx::EOpenVsxRequestOutcome::TransportFailure), {} };
	extension::openvsx::OpenVsxBinaryOperation sha256{ { extension::openvsx::EOpenVsxRequestOutcome::NotRequested,
		platform::request::ERequestOutcome::Success, std::nullopt, {} }, {} };

	extension::openvsx::OpenVsxSearchOperation Search(
		std::wstring_view,
		int,
		int,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		return { FailedOperation(extension::openvsx::EOpenVsxRequestOutcome::InvalidRequest), {} };
	}

	extension::openvsx::OpenVsxBinaryOperation FetchVsix(
		std::wstring_view,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		++fetchVsixCalls;
		return vsix;
	}

	extension::openvsx::OpenVsxBinaryOperation FetchOptionalSha256(
		const std::optional<std::wstring>&,
		const platform::request::IRequestCancellation* = nullptr) const override
	{
		++fetchSha256Calls;
		return sha256;
	}
};

class StaticRequestCancellation final : public platform::request::IRequestCancellation {
public:
	explicit StaticRequestCancellation(bool cancelled) noexcept
		: m_cancelled(cancelled)
	{
	}

	bool IsCancellationRequested() const noexcept override { return m_cancelled; }

private:
	bool m_cancelled;
};

//! 名前・バージョンだけを持つ拡張を作る
SOpenVsxExtension MakeExtension(std::wstring sNamespace, std::wstring sName, std::wstring sVersion)
{
	SOpenVsxExtension ext;
	ext.sNamespace	= std::move(sNamespace);
	ext.sName		= std::move(sName);
	ext.sVersion	= std::move(sVersion);
	return ext;
}

SOpenVsxExtension MakeDownloadableExtension()
{
	auto ext = MakeExtension(L"test", L"extension", L"1.0.0");
	ext.sDownloadUrl = L"https://example.invalid/download.vsix";
	ext.sSha256Url = L"https://example.invalid/download.vsix.sha256";
	return ext;
}

constexpr std::wstring_view kCanonicalProfileId = L"0123456789abcdef0123456789abcdef";

} // namespace

TEST(CExtensionManager, IsSafeNameComponent_AcceptsOrdinaryNames)
{
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"dbaeumer"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"vscode-eslint"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"3.0.10"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"1.0.0-beta.1+build"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"日本語"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"a"));
}

//! 導入先フォルダー名はレジストリの応答から組むので、区切り文字を通してはならない
TEST(CExtensionManager, IsSafeNameComponent_RejectsPathTraversal)
{
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L".."));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"."));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"..\\..\\windows"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"../../windows"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a\\b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a/b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"C:"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"C:\\windows"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a:stream"));
}

TEST(CExtensionManager, IsSafeNameComponent_RejectsInvalidFileNameChars)
{
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L""));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a*b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a?b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a\"b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a<b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a>b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a|b"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a\tb"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(std::wstring_view(L"a\0b", 3)));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"a\x7F" L"b"));
}

//! 末尾の '.' と空白は Windows が黙って落とすため、別のフォルダーに化ける
TEST(CExtensionManager, IsSafeNameComponent_RejectsTrailingDotAndSpace)
{
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"name."));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"name "));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"na me"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L".name"));
}

TEST(CExtensionManager, IsSafeNameComponent_RejectsReservedDeviceNames)
{
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"CON"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"con"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"NUL"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"COM1"));
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"LPT9"));
	// 拡張子が付いていても予約扱い
	EXPECT_FALSE(CExtensionManager::IsSafeNameComponent(L"nul.txt"));
	// 予約名に見えるが予約ではないもの
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"COM0"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"CONSOLE"));
	EXPECT_TRUE(CExtensionManager::IsSafeNameComponent(L"NULL"));
}

TEST(CExtensionManager, MakeInstallFolderName)
{
	EXPECT_STREQ(
		L"dbaeumer.vscode-eslint-3.0.10",
		CExtensionManager::MakeInstallFolderName(MakeExtension(L"dbaeumer", L"vscode-eslint", L"3.0.10")).c_str());
}

//! どの構成要素が汚染されていても名前を決めないこと
TEST(CExtensionManager, MakeInstallFolderName_RejectsUnsafeComponents)
{
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"..", L"n", L"1.0.0")).empty());
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"ns", L"..\\..\\x", L"1.0.0")).empty());
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"ns", L"n", L"..\\evil")).empty());
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"", L"n", L"1.0.0")).empty());
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"ns", L"", L"1.0.0")).empty());
	EXPECT_TRUE(CExtensionManager::MakeInstallFolderName(MakeExtension(L"ns", L"n", L"")).empty());
}

TEST(CExtensionManager, ExtractSha256Hex)
{
	// ハッシュのみ
	EXPECT_STREQ(
		L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		CExtensionManager::ExtractSha256Hex(
			"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").c_str());

	// sha256sum 形式（ハッシュ + ファイル名）
	EXPECT_STREQ(
		L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		CExtensionManager::ExtractSha256Hex(
			"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  a.vsix\n").c_str());

	// 大文字は小文字に揃える
	EXPECT_STREQ(
		L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		CExtensionManager::ExtractSha256Hex(
			"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\r\n").c_str());
}

TEST(CExtensionManager, ExtractSha256Hex_RejectsMalformedInput)
{
	EXPECT_TRUE(CExtensionManager::ExtractSha256Hex("").empty());
	EXPECT_TRUE(CExtensionManager::ExtractSha256Hex("not a hash").empty());
	// 63 桁（短い）
	EXPECT_TRUE(CExtensionManager::ExtractSha256Hex(
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a").empty());
	// 65 桁（長い）
	EXPECT_TRUE(CExtensionManager::ExtractSha256Hex(
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015adX").empty());
	// 桁数は合うが 16 進でない
	EXPECT_TRUE(CExtensionManager::ExtractSha256Hex(
		"zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").empty());
}

TEST(CExtensionManager, ComputeSha256Hex)
{
	// FIPS 180-2 の既知の値
	const TempFile file("abc");
	EXPECT_STREQ(
		L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
		CExtensionManager::ComputeSha256Hex(file.GetPath()).c_str());
}

TEST(CExtensionManager, ComputeSha256Hex_EmptyFile)
{
	const TempFile file("");
	EXPECT_STREQ(
		L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
		CExtensionManager::ComputeSha256Hex(file.GetPath()).c_str());
}

//! 読み取りチャンク境界（64KiB）を越えても正しく求まること
TEST(CExtensionManager, ComputeSha256Hex_LargerThanReadChunk)
{
	// 'a' を 1,000,000 個並べたものの既知の値
	const TempFile file(std::string(1000000, 'a'));
	EXPECT_STREQ(
		L"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
		CExtensionManager::ComputeSha256Hex(file.GetPath()).c_str());
}

TEST(CExtensionManager, ComputeSha256Hex_MissingFileIsEmpty)
{
	EXPECT_TRUE(CExtensionManager::ComputeSha256Hex(L"Z:\\no\\such\\file.vsix").empty());
}

//! ハッシュの比較が成立する形になっていること
TEST(CExtensionManager, ComputeSha256Hex_MatchesExtractedHash)
{
	const TempFile file("abc");
	EXPECT_EQ(
		CExtensionManager::ExtractSha256Hex(
			"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  a.vsix"),
		CExtensionManager::ComputeSha256Hex(file.GetPath()));
}

//! 導入先は ini と同じ階層の extensions フォルダー
TEST(CExtensionManager, GetBaseDir)
{
	const CExtensionManager manager;
	EXPECT_STREQ(L"extensions", manager.GetBaseDir().filename().c_str());
	EXPECT_TRUE(manager.GetBaseDir().is_absolute());
}

//! 型付き request failure は value/URL を診断へ写さず、予約済み staging を残さない。
TEST(CExtensionManager, Install_TypedVsixFailureCleansStagingAndDoesNotExposeRemoteMessage)
{
	const TempDirectory directory;
	ASSERT_TRUE(std::filesystem::exists(directory.GetPath()));
	CExtensionManager manager(directory.GetPath());
	FakeOpenVsxRegistryClient client;
	client.vsix.status = FailedOperation(extension::openvsx::EOpenVsxRequestOutcome::TransportFailure,
		L"access token for https://private.example.invalid/vsix");
	std::wstring errorMsg;

	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(1, client.fetchVsixCalls);
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_EQ(L"cannot fetch extension package", errorMsg);
	EXPECT_EQ(std::wstring::npos, errorMsg.find(L"private.example.invalid"));
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! cancellation は request を開始せず terminal cancelled として終える。
TEST(CExtensionManager, Install_AtomicCancellationPreventsFetchAndLeavesNoStaging)
{
	const TempDirectory directory;
	ASSERT_TRUE(std::filesystem::exists(directory.GetPath()));
	CExtensionManager manager(directory.GetPath());
	FakeOpenVsxRegistryClient client;
	std::atomic<bool> cancelled{ true };
	std::wstring errorMsg;

	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg, nullptr, &cancelled));
	EXPECT_EQ(0, client.fetchVsixCalls);
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_EQ(L"extension installation cancelled", errorMsg);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! 共有 request token も、worker の atomic flag と同じ terminal cancellation になる。
TEST(CExtensionManager, Install_RequestCancellationPreventsFetchAndLeavesNoStaging)
{
	const TempDirectory directory;
	ASSERT_TRUE(std::filesystem::exists(directory.GetPath()));
	CExtensionManager manager(directory.GetPath());
	FakeOpenVsxRegistryClient client;
	const StaticRequestCancellation cancelled(true);
	std::wstring errorMsg;

	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg, &cancelled));
	EXPECT_EQ(0, client.fetchVsixCalls);
	EXPECT_EQ(0, client.fetchSha256Calls);
	EXPECT_EQ(L"extension installation cancelled", errorMsg);
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

//! integrity metadata の取得失敗後にも、一時 VSIX と staging を確定させない。
TEST(CExtensionManager, Install_TypedSha256FailureCleansWrittenStaging)
{
	const TempDirectory directory;
	ASSERT_TRUE(std::filesystem::exists(directory.GetPath()));
	CExtensionManager manager(directory.GetPath());
	FakeOpenVsxRegistryClient client;
	client.vsix = { SuccessfulOperation(), { 'n', 'o', 't', '-', 'a', '-', 'z', 'i', 'p' } };
	client.sha256 = { FailedOperation(extension::openvsx::EOpenVsxRequestOutcome::HttpStatusFailure,
		L"https://private.example.invalid/sha256"), {} };
	std::wstring errorMsg;

	EXPECT_FALSE(manager.Install(MakeDownloadableExtension(), client, errorMsg));
	EXPECT_EQ(1, client.fetchVsixCalls);
	EXPECT_EQ(1, client.fetchSha256Calls);
	EXPECT_EQ(L"cannot fetch extension package integrity metadata", errorMsg);
	EXPECT_EQ(std::wstring::npos, errorMsg.find(L"private.example.invalid"));
	EXPECT_TRUE(std::filesystem::is_empty(directory.GetPath()));
}

/*!
	@brief 実際に Open VSX から拡張を導入する

	外部サービスと Shell の ZIP 機能に依存するので既定では実行しない。
	取得 → sha256 検証 → 展開 → 列挙 → 削除 の一連を通しで確認する。
	@code
	tests1.exe --gtest_also_run_disabled_tests --gtest_filter=*Live*
	@endcode
 */
TEST(CExtensionManager, DISABLED_Install_Live)
{
	// CZipFile が使う IShellDispatch のために OLE を初期化する
	ASSERT_HRESULT_SUCCEEDED(::OleInitialize(nullptr));

	config::CConfigurationService configuration(config::BuiltinConfigurationDescriptors());
	const auto clientResult = extension::openvsx::CreateOpenVsxProductionClient(
		configuration, std::wstring(kCanonicalProfileId));
	ASSERT_TRUE(clientResult) << clientResult.diagnostic.c_str();

	// 小さく、名前空間が検証済みの拡張を選ぶ
	std::wstring errorMsg;
	const auto search = clientResult.client->Search(L"vscode-icons", 0, 1);
	ASSERT_TRUE(search.status) << search.status.message;
	const SOpenVsxSearchResult& searchResult = search.value;
	ASSERT_FALSE(searchResult.extensions.empty());

	const SOpenVsxExtension& ext = searchResult.extensions[0];
	ASSERT_FALSE(ext.sSha256Url.empty()) << L"sha256 の検証経路を通せない";

	CExtensionManager manager;
	const std::wstring sUniqueId = ext.GetUniqueId();

	// 前回の残骸があれば片付けてから始める
	if (SInstalledExtension stale; manager.FindInstalled(sUniqueId, stale)) {
		std::wstring ignored;
		manager.Uninstall(sUniqueId, ignored);
	}

	ASSERT_TRUE(manager.Install(ext, *clientResult.client, errorMsg)) << errorMsg;

	// マニフェストまで展開されていること
	SInstalledExtension installed;
	ASSERT_TRUE(manager.FindInstalled(sUniqueId, installed));
	EXPECT_STREQ(ext.sVersion.c_str(), installed.sVersion.c_str());
	EXPECT_FALSE(installed.sDisplayName.empty());
	EXPECT_TRUE(std::filesystem::exists(
		installed.dir / CExtensionManager::kVsixContentDir / CExtensionManager::kManifestFileName));

	// 二重導入は拒否されること
	EXPECT_FALSE(manager.Install(ext, *clientResult.client, errorMsg));

	// 後片付け
	EXPECT_TRUE(manager.Uninstall(sUniqueId, errorMsg)) << errorMsg;
	EXPECT_FALSE(std::filesystem::exists(installed.dir));

	::OleUninitialize();
}

//! sha256 が合わない配布物を展開しないこと
TEST(CExtensionManager, DISABLED_Install_Live_RejectsTamperedSha256)
{
	ASSERT_HRESULT_SUCCEEDED(::OleInitialize(nullptr));

	config::CConfigurationService configuration(config::BuiltinConfigurationDescriptors());
	const auto clientResult = extension::openvsx::CreateOpenVsxProductionClient(
		configuration, std::wstring(kCanonicalProfileId));
	ASSERT_TRUE(clientResult) << clientResult.diagnostic.c_str();

	std::wstring errorMsg;
	const auto search = clientResult.client->Search(L"vscode-icons", 0, 1);
	ASSERT_TRUE(search.status) << search.status.message;
	const SOpenVsxSearchResult& searchResult = search.value;
	ASSERT_FALSE(searchResult.extensions.empty());

	// 別の配布物の sha256 を指すよう差し替える。中身と一致しなくなる
	SOpenVsxExtension ext = searchResult.extensions[0];
	ASSERT_FALSE(ext.sSha256Url.empty());
	ext.sSha256Url = L"https://open-vsx.org/api/-/search?query=eslint&size=1";

	CExtensionManager manager;
	EXPECT_FALSE(manager.Install(ext, *clientResult.client, errorMsg));
	EXPECT_FALSE(manager.GetBaseDir().empty());

	// 展開先を残していないこと
	EXPECT_FALSE(std::filesystem::exists(
		manager.GetBaseDir() / CExtensionManager::MakeInstallFolderName(ext)));

	::OleUninitialize();
}

//! 未導入の拡張を消そうとしても、失敗を返すだけで何も壊さないこと
TEST(CExtensionManager, Uninstall_NotInstalled)
{
	CExtensionManager manager;
	std::wstring errorMsg;
	EXPECT_FALSE(manager.Uninstall(L"no-such.extension", errorMsg));
	EXPECT_FALSE(errorMsg.empty());

	SInstalledExtension found;
	EXPECT_FALSE(manager.FindInstalled(L"no-such.extension", found));
}

TEST(CExtensionProfileState, MissingStateUsesProfileSpecificDefaults)
{
	TempDirectory directory;
	CExtensionProfileState state(directory.GetPath() / L"extensions.json");
	const auto missing = state.Load();

	EXPECT_EQ(missing.status, CExtensionProfileState::EStatus::Missing);
	EXPECT_TRUE(CExtensionProfileState::IsEnabled(missing, L"ms-toolsai.python", true));
	EXPECT_FALSE(CExtensionProfileState::IsEnabled(missing, L"ms-toolsai.python", false));
	EXPECT_TRUE(state.SetEnabled(L"MS-ToolsAI.Python", false));

	const auto disabled = state.Load();
	ASSERT_EQ(disabled.status, CExtensionProfileState::EStatus::Valid);
	EXPECT_FALSE(CExtensionProfileState::IsEnabled(disabled, L"ms-toolsai.python", true));
	EXPECT_TRUE(state.SetEnabled(L"ms-toolsai.python", true));
	EXPECT_TRUE(CExtensionProfileState::IsEnabled(state.Load(), L"ms-toolsai.python", false));
	EXPECT_TRUE(state.Remove(L"ms-toolsai.python"));
	EXPECT_EQ(state.Load().status, CExtensionProfileState::EStatus::Valid);
}

TEST(CExtensionProfileState, InvalidStateFailsClosedAndIsNotOverwritten)
{
	TempDirectory directory;
	const auto path = directory.GetPath() / L"extensions.json";
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << R"({"version":1,"extensions":{"bad/id":true}})";
	}
	CExtensionProfileState state(path);
	EXPECT_EQ(state.Load().status, CExtensionProfileState::EStatus::Invalid);
	EXPECT_FALSE(CExtensionProfileState::IsEnabled(state.Load(), L"ms-toolsai.python", true));
	EXPECT_FALSE(state.SetEnabled(L"ms-toolsai.python", true));
	EXPECT_EQ(state.Load().status, CExtensionProfileState::EStatus::Invalid);
}

TEST(CExtensionProfileState, RejectsUnsafeIds)
{
	EXPECT_TRUE(CExtensionProfileState::IsSafeExtensionId(L"publisher.extension"));
	EXPECT_TRUE(CExtensionProfileState::IsSafeExtensionId(L"publisher_extension-1"));
	EXPECT_FALSE(CExtensionProfileState::IsSafeExtensionId(L"publisher/extension"));
	EXPECT_FALSE(CExtensionProfileState::IsSafeExtensionId(L"..\\..\\secret"));
	EXPECT_FALSE(CExtensionProfileState::IsSafeExtensionId(L""));
}
