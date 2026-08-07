/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionManager.h"
#include "extension/ExtensionUntrustedWorkspaceOverride.h"

#include <filesystem>
#include <fstream>
#include <map>
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
	@brief CExtensionManager::ParseRestrictedConfigurations の単体検証

	ParseUntrustedWorkspaceSupport と同じ picojson 経路を辿る純粋関数なので、同じ流儀
	（package.json 本文の文字列だけを組み立てて検証する）で確かめる。読めない形は
	すべて「制限なし」ではなく空の一覧として fail closed する。
 */

TEST(CExtensionManagerParseRestrictedConfigurations, HappyPathReadsDeclaredKeys)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":[\"editor.tabSize\",\"files.autoSave\"]}}}";
	const std::vector<std::string> result = CExtensionManager::ParseRestrictedConfigurations(sJson);
	ASSERT_EQ(2u, result.size());
	EXPECT_EQ("editor.tabSize", result[0]);
	EXPECT_EQ("files.autoSave", result[1]);
}

TEST(CExtensionManagerParseRestrictedConfigurations, MissingCapabilitiesIsEmpty)
{
	const std::string sJson = "{\"name\":\"withcode\",\"main\":\"./out/extension.js\"}";
	EXPECT_TRUE(CExtensionManager::ParseRestrictedConfigurations(sJson).empty());
}

TEST(CExtensionManagerParseRestrictedConfigurations, CapabilitiesWithoutUntrustedWorkspacesIsEmpty)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"virtualWorkspaces\":true}}";
	EXPECT_TRUE(CExtensionManager::ParseRestrictedConfigurations(sJson).empty());
}

TEST(CExtensionManagerParseRestrictedConfigurations, UntrustedWorkspacesWithoutRestrictedConfigurationsIsEmpty)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\"}}}";
	EXPECT_TRUE(CExtensionManager::ParseRestrictedConfigurations(sJson).empty());
}

TEST(CExtensionManagerParseRestrictedConfigurations, RestrictedConfigurationsNotArrayIsEmpty)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":\"editor.tabSize\"}}}";
	EXPECT_TRUE(CExtensionManager::ParseRestrictedConfigurations(sJson).empty());
}

TEST(CExtensionManagerParseRestrictedConfigurations, NonStringMembersAreDroppedIndividually)
{
	// 文字列でない要素はその要素だけを読み捨て、一覧全体は打ち切らない。
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":[\"editor.tabSize\",42,null,true,[],{},\"files.autoSave\"]}}}";
	const std::vector<std::string> result = CExtensionManager::ParseRestrictedConfigurations(sJson);
	ASSERT_EQ(2u, result.size());
	EXPECT_EQ("editor.tabSize", result[0]);
	EXPECT_EQ("files.autoSave", result[1]);
}

TEST(CExtensionManagerParseRestrictedConfigurations, NonCanonicalKeyIsDropped)
{
	// アンダースコア・ドット無し・先頭/末尾ドット・空セグメント・数字始まりは、いずれも
	// config::CConfigurationService.cpp の IsCanonicalKey と同じ規則で非正準として落ちる。
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":[\"editor.tabSize\",\"editor.tab_size\",\"nodot\","
		"\".editor.tabSize\",\"editor.tabSize.\",\"editor..tabSize\",\"1editor.tabSize\"]}}}";
	const std::vector<std::string> result = CExtensionManager::ParseRestrictedConfigurations(sJson);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ("editor.tabSize", result[0]);
}

TEST(CExtensionManagerParseRestrictedConfigurations, DuplicatesCollapsePreservingFirstSeenOrder)
{
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":[\"files.autoSave\",\"editor.tabSize\",\"files.autoSave\"]}}}";
	const std::vector<std::string> result = CExtensionManager::ParseRestrictedConfigurations(sJson);
	ASSERT_EQ(2u, result.size());
	EXPECT_EQ("files.autoSave", result[0]);
	EXPECT_EQ("editor.tabSize", result[1]);
}

TEST(CExtensionManagerParseRestrictedConfigurations, OverLongArrayDropsEntriesPastTheBound)
{
	// kMaxRestrictedConfigurationEntries を超える分は静かに切り捨てられる。マジック
	// ナンバーを再定義せず、テストからも参照できる公開境界そのものを使って確かめる。
	std::string sArray = "[";
	const std::size_t entryCount = CExtensionManager::kMaxRestrictedConfigurationEntries + 5;
	for (std::size_t index = 0; index < entryCount; ++index) {
		if (index != 0) {
			sArray += ",";
		}
		sArray += "\"test.key" + std::to_string(index) + "\"";
	}
	sArray += "]";
	const std::string sJson =
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":" + sArray + "}}}";

	const std::vector<std::string> result = CExtensionManager::ParseRestrictedConfigurations(sJson);
	ASSERT_EQ(CExtensionManager::kMaxRestrictedConfigurationEntries, result.size());
	EXPECT_EQ("test.key0", result.front());
	EXPECT_EQ("test.key" + std::to_string(CExtensionManager::kMaxRestrictedConfigurationEntries - 1), result.back());
}

TEST(CExtensionManagerParseRestrictedConfigurations, MalformedJsonIsEmpty)
{
	const std::string sJson = "{ this is not valid json";
	EXPECT_TRUE(CExtensionManager::ParseRestrictedConfigurations(sJson).empty());
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

/*!
	@brief CExtensionManager::EnumInstalled() が restrictedConfigurations を実ファイルから配線する検証

	上の ParseRestrictedConfigurations 単体テスト群は文字列 in / vector out だけを見ており、
	EnumInstalled が同じ sManifestBody から SInstalledExtension::restrictedConfigurations を
	詰める配線は通っていない。EnumInstalledTempDirectory / WriteInstalledExtensionManifest /
	FindByUniqueId は上の匿名名前空間のものをそのまま再利用する。
 */

TEST(CExtensionManagerEnumInstalledRestrictedConfigurations, DeclaredRestrictedConfigurationsArePopulated)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.limited", L"1.0.0",
		"{\"name\":\"limited\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":\"limited\","
		"\"restrictedConfigurations\":[\"editor.tabSize\",\"files.autoSave\"]}}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.limited");
	ASSERT_NE(nullptr, pFound);
	ASSERT_EQ(2u, pFound->restrictedConfigurations.size());
	EXPECT_EQ("editor.tabSize", pFound->restrictedConfigurations[0]);
	EXPECT_EQ("files.autoSave", pFound->restrictedConfigurations[1]);
}

TEST(CExtensionManagerEnumInstalledRestrictedConfigurations, ExtensionDeclaringNoneHasEmptyList)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.withcode", L"1.0.0",
		"{\"name\":\"withcode\",\"main\":\"./out/extension.js\","
		"\"capabilities\":{\"untrustedWorkspaces\":{\"supported\":true}}}");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.withcode");
	ASSERT_NE(nullptr, pFound);
	EXPECT_TRUE(pFound->restrictedConfigurations.empty());
}

//! 壊れた package.json は untrustedWorkspaceSupport 同様、restrictedConfigurations も fail closed で空になり、一覧から消えない
TEST(CExtensionManagerEnumInstalledRestrictedConfigurations, BrokenManifestHasEmptyListAndStillEnumerates)
{
	const EnumInstalledTempDirectory directory;
	WriteInstalledExtensionManifest(directory.GetPath(), L"acme.broken", L"1.0.0",
		"{ this is not valid json");

	CExtensionManager manager(directory.GetPath());
	const std::vector<SInstalledExtension> installed = manager.EnumInstalled();

	const SInstalledExtension* pFound = FindByUniqueId(installed, L"acme.broken");
	ASSERT_NE(nullptr, pFound) << L"壊れたマニフェストでも一覧から消えてはならない";
	EXPECT_TRUE(pFound->restrictedConfigurations.empty());
}

/*!
	@brief ParseExtensionUntrustedWorkspaceOverrides の単体検証

	extensions.supportUntrustedWorkspaces は JSONC から既にデコードされた
	config::ConfigurationValue 経路で届くため、ParseUntrustedWorkspaceSupport /
	ParseRestrictedConfigurations の picojson 文字列組み立てとは違い、
	config::ConfigurationValue::Object をそのまま組み立てて検証する
	（config/ConfigurationServiceTest.cpp の組み立て方に合わせた）。
	読めない形はすべて「その項目だけを落とす」で fail closed する。
 */

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, NonObjectSettingValueIsEmpty)
{
	const config::ConfigurationValue value(true);
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, NullSettingValueIsEmpty)
{
	const config::ConfigurationValue value; // 既定構築 == JSON null / 未設定相当
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, SupportedTrueIsSupported)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(true) },
		}) },
	});
	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	ASSERT_EQ(1u, result.size());
	const auto it = result.find(L"acme.widget");
	ASSERT_NE(result.end(), it);
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported, it->second.supported);
	EXPECT_TRUE(it->second.version.empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, SupportedFalseIsNotSupported)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(false) },
		}) },
	});
	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported, result.at(L"acme.widget").supported);
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, SupportedLimitedStringIsLimited)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(L"limited") },
		}) },
	});
	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	ASSERT_EQ(1u, result.size());
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Limited, result.at(L"acme.widget").supported);
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, SupportedWrongTypeDropsEntry)
{
	// bool でも "limited" 文字列でもない値は読み取れない申告として、その項目を落とす。
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(static_cast<std::int64_t>(1)) },
		}) },
	});
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, SupportedWrongCasedLimitedDropsEntry)
{
	// capabilities.untrustedWorkspaces.supported の "limited" と同じ大文字小文字の区別を踏襲する。
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(L"Limited") },
		}) },
	});
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, MissingSupportedDropsEntry)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"version", config::ConfigurationValue(L"1.0.0") },
		}) },
	});
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, StringVersionIsStored)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(true) },
			{ L"version", config::ConfigurationValue(L"1.2.3") },
		}) },
	});
	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	ASSERT_EQ(1u, result.size());
	EXPECT_STREQ(L"1.2.3", result.at(L"acme.widget").version.c_str());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, NonStringVersionDropsWholeEntry)
{
	// version が読めないまま全バージョンへ適用すると、書いていない免除を勝手に広げて
	// しまうため、supported が正しく読めていても version ごとこの項目を落とす。
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(true) },
			{ L"version", config::ConfigurationValue(static_cast<std::int64_t>(1)) },
		}) },
	});
	EXPECT_TRUE(ParseExtensionUntrustedWorkspaceOverrides(value).empty());
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, NonObjectMemberIsDroppedWithoutAffectingOthers)
{
	const config::ConfigurationValue value(config::ConfigurationValue::Object {
		{ L"acme.broken", config::ConfigurationValue(true) },
		{ L"acme.widget", config::ConfigurationValue(config::ConfigurationValue::Object {
			{ L"supported", config::ConfigurationValue(true) },
		}) },
	});
	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	ASSERT_EQ(1u, result.size());
	EXPECT_NE(result.end(), result.find(L"acme.widget"));
	EXPECT_EQ(result.end(), result.find(L"acme.broken"));
}

TEST(ParseExtensionUntrustedWorkspaceOverridesTest, OverLongMapDropsEntriesPastTheBound)
{
	// kMaxExtensionUntrustedWorkspaceOverrideEntries を超える分は静かに切り捨てられる。
	// マジックナンバーを再定義せず、テストからも参照できる公開境界そのものを使う。
	config::ConfigurationValue::Object root;
	const std::size_t entryCount = kMaxExtensionUntrustedWorkspaceOverrideEntries + 5;
	for (std::size_t index = 0; index < entryCount; ++index) {
		root.emplace(L"acme.ext" + std::to_wstring(index),
			config::ConfigurationValue(config::ConfigurationValue::Object {
				{ L"supported", config::ConfigurationValue(true) },
			}));
	}
	const config::ConfigurationValue value(std::move(root));

	const auto result = ParseExtensionUntrustedWorkspaceOverrides(value);
	EXPECT_EQ(kMaxExtensionUntrustedWorkspaceOverrideEntries, result.size());
}

/*!
	@brief ResolveUntrustedWorkspaceSupport の単体検証

	上書きが無ければマニフェスト値をそのまま返すこと、上書きは緩める方向にも厳しく
	する方向にも効くこと、バージョン一致の優先順位、拡張 ID の大文字小文字を無視した
	一致を確かめる。
 */

TEST(ResolveUntrustedWorkspaceSupportTest, NoOverrideReturnsManifestValue)
{
	const std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::Supported, L"acme.widget", L"1.0.0", overrides));
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Limited,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::Limited, L"acme.widget", L"1.0.0", overrides));
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, OverrideWithNoVersionApplies)
{
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"" });

	// バージョン指定なしは、問い合わせのバージョンによらず適用される。
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"9.9.9", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, OverrideWithMatchingVersionApplies)
{
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"1.0.0" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, OverrideWithMismatchedVersionFallsBackToManifestValue)
{
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"1.0.0" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"2.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, OverrideWidensNotSupportedToSupported)
{
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, OverrideNarrowsSupportedToNotSupported)
{
	// 上書きは緩める方向にも厳しくする方向にも効く。VS Code 自身がそう動くため、
	// 片方向だけ効かせる作りにしてはならない。
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::NotSupported, L"" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::Supported, L"acme.widget", L"1.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, IdMatchingIsCaseInsensitive)
{
	// CExtensionManager::FindInstalled が sUniqueId を大文字小文字を無視して比較しており、
	// VS Code 自身も拡張識別子を大文字小文字を無視して比較するため、これに合わせる。
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"Acme.Widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
}

TEST(ResolveUntrustedWorkspaceSupportTest, VersionSpecificMatchWinsOverVersionAgnosticMatchForSameId)
{
	// 同じ拡張 ID に対して、バージョン指定なしの申告とバージョン指定ありの申告が
	// （大文字小文字違いの 2 つの設定キーとして）両方存在するとき、より具体的な
	// バージョン一致を優先する。
	std::map<std::wstring, ExtensionUntrustedWorkspaceOverride, std::less<>> overrides;
	overrides.emplace(L"acme.widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::NotSupported, L"" });
	overrides.emplace(L"Acme.Widget", ExtensionUntrustedWorkspaceOverride{ EExtensionUntrustedWorkspaceSupport::Supported, L"1.0.0" });

	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::Supported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::NotSupported, L"acme.widget", L"1.0.0", overrides));
	// バージョンが一致しない問い合わせは、バージョン指定なしの申告へフォールバックする。
	EXPECT_EQ(EExtensionUntrustedWorkspaceSupport::NotSupported,
		ResolveUntrustedWorkspaceSupport(EExtensionUntrustedWorkspaceSupport::Supported, L"acme.widget", L"9.9.9", overrides));
}
