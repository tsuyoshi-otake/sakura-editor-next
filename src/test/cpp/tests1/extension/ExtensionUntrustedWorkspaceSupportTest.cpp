/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionManager.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "util/file.h"

/*!
	@brief CExtensionManager::ParseUntrustedWorkspaceSupport の単体検証

	通信・ファイル操作を一切伴わない純粋関数なので、package.json 本文の文字列だけを
	組み立てて検証する。決定規則は upstream VS Code の ExtensionManifestPropertiesService
	に合わせてあり、「コードを実行しない拡張は常に Supported」を最優先で判定し、
	それ以外は capabilities.untrustedWorkspaces.supported を厳密に読む。
	規則から外れる入力（不正 JSON、想定外の型など）はすべて fail closed で NotSupported。
 */

TEST(ExtensionUntrustedWorkspaceSupport, NoMainMeansSupportedEvenWhenCapabilitiesSaysFalse)
{
	// 宣言的な拡張（main が無い）は capabilities の宣言に関わらず常に Supported。
	const std::string sJson =
		"{\"name\":\"declarative\",\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, MainWithSupportedTrueIsSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":true}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, MainWithSupportedLimitedIsLimited)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\"}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Limited,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, MainWithSupportedFalseIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, MainWithNoCapabilitiesKeyIsNotSupported)
{
	const std::string sJson = "{\"name\":\"withcode\",\"main\":\"./out/extension.js\"}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, MainWithCapabilitiesButNoUntrustedWorkspacesIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"virtualWorkspaces\":true}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, UntrustedWorkspacesWithoutSupportedKeyIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"description\":\"needs a real workspace\"}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, SupportedAsNumberIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":1}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, SupportedAsNullIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":null}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, SupportedAsArrayIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":[]}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, SupportedAsObjectIsNotSupported)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":{}}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, SupportedAsWrongCaseLimitedStringIsNotSupported)
{
	const std::string sJsonTitleCase =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"Limited\"}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJsonTitleCase));

	const std::string sJsonUpperCase =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"LIMITED\"}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJsonUpperCase));
}

TEST(ExtensionUntrustedWorkspaceSupport, MalformedJsonIsNotSupported)
{
	const std::string sJson = "{ this is not valid json";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, ArrayRootIsNotSupported)
{
	const std::string sJson = "[1, 2, 3]";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, EmptyMainStringIsSupported)
{
	// main が空文字列は「実質エントリーポイントを持たない」ので宣言的拡張と同じ扱い。
	const std::string sJson =
		"{\"name\":\"emptymain\",\"main\":\"\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

TEST(ExtensionUntrustedWorkspaceSupport, BrowserOnlyEntryPointCountsAsCode)
{
	// main が無くても browser があればコードを実行するとみなす。
	const std::string sJson =
		"{\"name\":\"webextonly\",\"browser\":\"./dist/web/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		CExtensionManager::ParseUntrustedWorkspaceSupport(sJson));
}

/*!
	@brief CExtensionManager::EnumInstalled() が untrustedWorkspaceSupport を実ファイルから配線する検証

	上の TEST 群は ParseUntrustedWorkspaceSupport 単体（文字列 in / enum out）だけを見ており、
	EnumInstalled が「実際のディスク上の package.json を読んで SInstalledExtension に詰める」経路は
	通っていない。CExtensionService::LoadInstalledExtensionRootsWorker が Workspace Trust の
	ゲートとして依存しているのはこの配線そのものなので、ここで実ファイルシステムに対して検証する。
 */
namespace {

//! 導入先を実プロファイルから隔離する。CExtensionManagerStreamedInstallTest.cpp と同じ構成。
class EnumInstalledTempDirectory {
public:
	EnumInstalledTempDirectory()
		: m_path(GetTempFilePath(L"untrustws"))
	{
		std::error_code ec;
		std::filesystem::remove(m_path, ec);
		std::filesystem::create_directory(m_path, ec);
	}
	~EnumInstalledTempDirectory()
	{
		std::error_code ec;
		std::filesystem::remove_all(m_path, ec);
	}
	EnumInstalledTempDirectory(const EnumInstalledTempDirectory&) = delete;
	EnumInstalledTempDirectory& operator = (const EnumInstalledTempDirectory&) = delete;

	const std::filesystem::path& GetPath() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

/*!
	@brief EnumInstalled が認識する導入済み拡張のレイアウトをそのまま作る

	CExtensionManager::MakeInstallFolderName は "namespace.name-version" というフォルダー名を
	作り、EnumInstalled はそれを最後の '-' で sUniqueId / sVersion に分割し直す。マニフェストは
	その下の "extension/package.json"（kVsixContentDir / kManifestFileName）に置かれている必要が
	ある。ここでは Install() を経由せず、その最終形をそのまま組み立てる。

	@param[in] sUniqueId "namespace.name" 相当。ハイフンを含めないこと（含めると
		EnumInstalled 側の最後の '-' 分割が sVersion 側にずれて壊れる）。
*/
void WriteInstalledExtensionManifest(
	const std::filesystem::path& baseDir,
	const std::wstring& sUniqueId,
	const std::wstring& sVersion,
	const std::string& sManifestJson)
{
	const std::filesystem::path extensionDir =
		baseDir / (sUniqueId + L"-" + sVersion) / CExtensionManager::kVsixContentDir;
	std::error_code ec;
	std::filesystem::create_directories(extensionDir, ec);
	ASSERT_FALSE(ec) << ec.message().c_str();

	std::ofstream out(extensionDir / CExtensionManager::kManifestFileName, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(out.is_open());
	out << sManifestJson;
}

//! sUniqueId で探す。EnumInstalled の列挙順はファイルシステム依存なので、添字ではなく ID で照合する。
const SInstalledExtension* FindByUniqueId(
	const std::vector<SInstalledExtension>& installed,
	const std::wstring& sUniqueId)
{
	for (const auto& entry : installed) {
		if (entry.sUniqueId == sUniqueId) {
			return &entry;
		}
	}
	return nullptr;
}

} // namespace

TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, CodeExtensionDeclaringSupportedTrueIsSupported)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.withcode", L"1.0.0",
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":true}}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.withcode");
	ASSERT_NE(nullptr, pFound) << L"EnumInstalled がフィクスチャのレイアウトを認識できていない";
	EXPECT_STREQ(L"1.0.0", pFound->sVersion.c_str());
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported, pFound->untrustedWorkspaceSupport);
}

TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, CodeExtensionDeclaringSupportedLimitedIsLimited)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.withcode", L"1.0.0",
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\"}}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.withcode");
	ASSERT_NE(nullptr, pFound);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Limited, pFound->untrustedWorkspaceSupport);
}

TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, CodeExtensionDeclaringSupportedFalseIsNotSupported)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.withcode", L"1.0.0",
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.withcode");
	ASSERT_NE(nullptr, pFound);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, pFound->untrustedWorkspaceSupport);
}

TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, CodeExtensionDeclaringNothingIsNotSupported)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.withcode", L"1.0.0",
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\"}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.withcode");
	ASSERT_NE(nullptr, pFound);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, pFound->untrustedWorkspaceSupport);
}

TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, DeclarativeExtensionWithNoMainIsSupported)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.declarative", L"2.1.0",
		"{\"name\":\"declarative\",\"contributes\":{\"themes\":[]}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.declarative");
	ASSERT_NE(nullptr, pFound);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported, pFound->untrustedWorkspaceSupport);
}

//! 壊れた package.json は、EnumInstalled から消え去るのではなく fail closed で NotSupported として残ること
TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, UnparsableManifestIsNotSupportedAndStillEnumerated)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.broken", L"1.0.0",
		"{ this is not valid json");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.broken");
	ASSERT_NE(nullptr, pFound) << L"壊れたマニフェストでも一覧から消えてはならない";
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, pFound->untrustedWorkspaceSupport);
	// ReadDisplayNameFromBody も同じ本文を解析できないため、表示名は sUniqueId にフォールバックする
	// （CExtensionManager::EnumInstalled の実装どおり）。
	EXPECT_STREQ(L"acme.broken", pFound->sDisplayName.c_str());
}

//! 1 回の EnumInstalled で複数拡張を混在させ、ループのある反復が別の反復へ値を持ち越さないことを示す
TEST(CExtensionManagerEnumInstalledUntrustedWorkspaceSupport, MultipleExtensionsEachKeepTheirOwnValue)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.supported", L"1.0.0",
		"{\"name\":\"supported\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":true}}}");
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.limited", L"1.0.0",
		"{\"name\":\"limited\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\"}}}");
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.notsupported", L"1.0.0",
		"{\"name\":\"notsupported\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":false}}}");
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.declarative", L"1.0.0",
		"{\"name\":\"declarative\"}");
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.broken", L"1.0.0",
		"{ this is not valid json");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();
	ASSERT_EQ(5u, installed.size());

	const SInstalledExtension* pSupported = FindByUniqueId(installed, L"acme.supported");
	const SInstalledExtension* pLimited = FindByUniqueId(installed, L"acme.limited");
	const SInstalledExtension* pNotSupported = FindByUniqueId(installed, L"acme.notsupported");
	const SInstalledExtension* pDeclarative = FindByUniqueId(installed, L"acme.declarative");
	const SInstalledExtension* pBroken = FindByUniqueId(installed, L"acme.broken");

	ASSERT_NE(nullptr, pSupported);
	ASSERT_NE(nullptr, pLimited);
	ASSERT_NE(nullptr, pNotSupported);
	ASSERT_NE(nullptr, pDeclarative);
	ASSERT_NE(nullptr, pBroken);

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported, pSupported->untrustedWorkspaceSupport);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Limited, pLimited->untrustedWorkspaceSupport);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, pNotSupported->untrustedWorkspaceSupport);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported, pDeclarative->untrustedWorkspaceSupport);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, pBroken->untrustedWorkspaceSupport);
}
