/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include <Windows.h>
#include <limits>
#include "extension/COpenVsxClient.h"
#include "extension/openvsx/OpenVsxProtocol.h"

/*!
	@brief 通信を伴わない部分だけを検証する

	Search / DownloadVsix はネットワークに依存するため対象外。
	URL の組み立てと応答の解析は純粋な変換なので、ここで固定する。
 */

TEST(COpenVsxClient, BuildSearchUrl_WithQuery)
{
	const COpenVsxClient client(L"https://open-vsx.org");
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=25&query=eslint",
		client.BuildSearchUrl(L"eslint", 0, 25).c_str());
}

TEST(COpenVsxClient, BuildSearchUrl_EmptyQueryFallsBackToPopularityOrder)
{
	const COpenVsxClient client(L"https://open-vsx.org");
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=50&size=10&sortBy=downloadCount&sortOrder=desc",
		client.BuildSearchUrl(L"", 50, 10).c_str());
}

//! 検索語はレジストリに素の文字列として渡してはならない
TEST(COpenVsxClient, BuildSearchUrl_EncodesQuery)
{
	const COpenVsxClient client(L"https://open-vsx.org");

	// & や = で追加のパラメーターを注入できないこと
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=25&query=a%26size%3D999",
		client.BuildSearchUrl(L"a&size=999", 0, 25).c_str());

	// 非 ASCII は UTF-8 バイト単位で符号化される（"日" == E6 97 A5）
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=25&query=%E6%97%A5",
		client.BuildSearchUrl(L"日", 0, 25).c_str());

	// 空白とスラッシュ
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=25&query=a%20b%2Fc",
		client.BuildSearchUrl(L"a b/c", 0, 25).c_str());
}

TEST(COpenVsxClient, BuildSearchUrl_ClampsRange)
{
	const COpenVsxClient client(L"https://open-vsx.org");

	// 負の開始位置は 0 に、件数は [1, 100] に収める
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=1&query=x",
		client.BuildSearchUrl(L"x", -1, 0).c_str());
	EXPECT_STREQ(
		L"https://open-vsx.org/api/-/search?offset=0&size=100&query=x",
		client.BuildSearchUrl(L"x", 0, 1000).c_str());
}

//! 末尾の '/' を含む指定でも URL が二重の区切りにならないこと
TEST(COpenVsxClient, Constructor_StripsTrailingSlash)
{
	const COpenVsxClient client(L"https://example.test///");
	EXPECT_STREQ(
		L"https://example.test/api/-/search?offset=0&size=25&query=x",
		client.BuildSearchUrl(L"x", 0, 25).c_str());
}

//! protocol は legacy HTTP client を生成せず URL の入力境界を固定できること
TEST(OpenVsxProtocol, BuildSearchUrl_NormalizesAndClampsWithoutTransport)
{
	using extension::openvsx::OpenVsxProtocol;
	const auto registryUrl = OpenVsxProtocol::NormalizeRegistryUrl(L"https://example.test///");

	EXPECT_STREQ(L"https://example.test", registryUrl.c_str());
	EXPECT_STREQ(
		L"https://example.test/api/-/search?offset=0&size=100&query=a%26b",
		OpenVsxProtocol::BuildSearchUrl(registryUrl, L"a&b", -1, 101).c_str());
}

//! 実際のレジストリ応答と同じ形を解析できること
TEST(COpenVsxClient, ParseSearchResponse_RealShape)
{
	// https://open-vsx.org/api/-/search?query=eslint&size=1 の応答から項目を抜粋したもの
	const std::string sJson = R"({
		"offset": 0,
		"totalSize": 42,
		"extensions": [
			{
				"url": "https://open-vsx.org/api/dbaeumer/vscode-eslint",
				"files": {
					"download": "https://open-vsx.org/api/dbaeumer/vscode-eslint/3.0.10/file/dbaeumer.vscode-eslint-3.0.10.vsix",
					"signature": "https://example.test/sig",
					"icon": "https://example.test/icon.png",
					"sha256": "https://example.test/hash.sha256",
					"readme": "https://example.test/readme.md",
					"changelog": "https://example.test/changelog.md",
					"publicKey": "https://example.test/key"
				},
				"name": "vscode-eslint",
				"namespace": "dbaeumer",
				"version": "3.0.10",
				"timestamp": "2024-06-24T13:14:57.352339Z",
				"verified": true,
				"averageRating": 4.5,
				"reviewCount": 2,
				"downloadCount": 1234567,
				"displayName": "ESLint",
				"description": "Integrates ESLint JavaScript into VS Code.",
				"deprecated": false
			}
		]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(sJson, result, errorMsg)) << errorMsg;

	EXPECT_EQ(0, result.nOffset);
	EXPECT_EQ(42, result.nTotalSize);
	ASSERT_EQ(1u, result.extensions.size());

	const SOpenVsxExtension& ext = result.extensions[0];
	EXPECT_STREQ(L"dbaeumer", ext.sNamespace.c_str());
	EXPECT_STREQ(L"vscode-eslint", ext.sName.c_str());
	EXPECT_STREQ(L"ESLint", ext.sDisplayName.c_str());
	EXPECT_STREQ(L"3.0.10", ext.sVersion.c_str());
	EXPECT_STREQ(L"dbaeumer.vscode-eslint", ext.GetUniqueId().c_str());
	EXPECT_STREQ(
		L"https://open-vsx.org/api/dbaeumer/vscode-eslint/3.0.10/file/dbaeumer.vscode-eslint-3.0.10.vsix",
		ext.sDownloadUrl.c_str());
	EXPECT_STREQ(L"https://example.test/hash.sha256", ext.sSha256Url.c_str());
	EXPECT_STREQ(L"https://example.test/icon.png", ext.sIconUrl.c_str());
	EXPECT_STREQ(L"https://example.test/readme.md", ext.sReadmeUrl.c_str());
	EXPECT_STREQ(L"https://example.test/changelog.md", ext.sChangelogUrl.c_str());
	EXPECT_EQ(1234567LL, ext.nDownloadCount);
	EXPECT_TRUE(ext.HasRating());
	EXPECT_DOUBLE_EQ(4.5, ext.dAverageRating);
	EXPECT_TRUE(ext.bVerified);
	EXPECT_FALSE(ext.bDeprecated);
}

//! 非 ASCII は UTF-8 として解釈されること
TEST(COpenVsxClient, ParseSearchResponse_DecodesUtf8)
{
	// 応答は UTF-8 で届く。このソースの実行文字セットは Shift_JIS なので、
	// 「日本語」をそのまま書くと UTF-8 にならない。バイト列を明示する。
	const std::string sJson = std::string(R"({
		"extensions": [
			{
				"namespace": "ns", "name": "n", "version": "1.0.0",
				"displayName": ")") + "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" + R"(",
				"files": { "download": "https://example.test/a.vsix" }
			}
		]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(sJson, result, errorMsg)) << errorMsg;
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_STREQ(L"日本語", result.extensions[0].sDisplayName.c_str());
}

//! 表示名を持たない拡張は拡張名で代替されること
TEST(COpenVsxClient, ParseSearchResponse_FallsBackToNameAsDisplayName)
{
	const std::string sJson = R"({
		"extensions": [
			{
				"namespace": "ns", "name": "the-name", "version": "1.0.0",
				"files": { "download": "https://example.test/a.vsix" }
			}
		]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(sJson, result, errorMsg)) << errorMsg;
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_STREQ(L"the-name", result.extensions[0].sDisplayName.c_str());
	EXPECT_FALSE(result.extensions[0].HasRating());
}

//! 必須項目を欠く要素は、解析全体を失敗させずに取り除かれること
TEST(COpenVsxClient, ParseSearchResponse_SkipsIncompleteEntries)
{
	const std::string sJson = R"({
		"extensions": [
			{ "name": "no-namespace", "version": "1.0.0", "files": { "download": "https://example.test/a.vsix" } },
			{ "namespace": "ns", "version": "1.0.0", "files": { "download": "https://example.test/a.vsix" } },
			{ "namespace": "ns", "name": "no-version", "files": { "download": "https://example.test/a.vsix" } },
			{ "namespace": "ns", "name": "no-files", "version": "1.0.0" },
			{ "namespace": "ns", "name": "empty-files", "version": "1.0.0", "files": {} },
			"not an object",
			{ "namespace": "ns", "name": "ok", "version": "1.0.0", "files": { "download": "https://example.test/a.vsix" } }
		]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(sJson, result, errorMsg)) << errorMsg;
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_STREQ(L"ok", result.extensions[0].sName.c_str());
}

//! 型が想定と違う項目は既定値に落ち、解析は続行されること
TEST(COpenVsxClient, ParseSearchResponse_ToleratesWrongTypes)
{
	const std::string sJson = R"({
		"offset": "not a number",
		"totalSize": null,
		"extensions": [
			{
				"namespace": "ns", "name": "n", "version": "1.0.0",
				"displayName": 123,
				"downloadCount": "many",
				"averageRating": null,
				"verified": "yes",
				"files": { "download": "https://example.test/a.vsix", "sha256": 5 }
			}
		]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(sJson, result, errorMsg)) << errorMsg;
	EXPECT_EQ(0, result.nOffset);
	EXPECT_EQ(0, result.nTotalSize);
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_STREQ(L"n", result.extensions[0].sDisplayName.c_str());
	EXPECT_EQ(0LL, result.extensions[0].nDownloadCount);
	EXPECT_FALSE(result.extensions[0].HasRating());
	EXPECT_FALSE(result.extensions[0].bVerified);
	EXPECT_TRUE(result.extensions[0].sSha256Url.empty());
}

//! 該当なしの応答では extensions が省略され得る
TEST(COpenVsxClient, ParseSearchResponse_MissingExtensionsIsEmptyResult)
{
	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(R"({"offset":0,"totalSize":0})", result, errorMsg)) << errorMsg;
	EXPECT_TRUE(result.extensions.empty());
}

TEST(COpenVsxClient, ParseSearchResponse_RejectsMalformedInput)
{
	SOpenVsxSearchResult result;
	std::wstring errorMsg;

	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse("", result, errorMsg));
	EXPECT_FALSE(errorMsg.empty());

	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse("{ this is not json", result, errorMsg));
	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse("[1,2,3]", result, errorMsg));
	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse(R"({"extensions": "not an array"})", result, errorMsg));
}

/*!
	@brief 実際の Open VSX に問い合わせる

	外部サービスに依存するので既定では実行しない。
	実装を変更したときは以下で明示的に確認する。
	@code
	tests1.exe --gtest_also_run_disabled_tests --gtest_filter=*Live*
	@endcode
 */
TEST(COpenVsxClient, DISABLED_Search_Live)
{
	COpenVsxClient client;
	ASSERT_TRUE(client.IsOk());

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(client.Search(L"eslint", 0, 5, result, errorMsg)) << errorMsg;

	EXPECT_GT(result.nTotalSize, 0);
	ASSERT_FALSE(result.extensions.empty());
	for (const SOpenVsxExtension& ext : result.extensions) {
		EXPECT_FALSE(ext.sNamespace.empty());
		EXPECT_FALSE(ext.sName.empty());
		EXPECT_FALSE(ext.sVersion.empty());
		EXPECT_EQ(0u, ext.sDownloadUrl.find(L"https://")) << ext.sDownloadUrl;
	}
}

//! 実サービスの download URL が返す HTTPS リダイレクトを追跡して保存できること
TEST(COpenVsxClient, DISABLED_DownloadVsix_Live)
{
	COpenVsxClient client;
	ASSERT_TRUE(client.IsOk());

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(client.Search(L"eslint", 0, 1, result, errorMsg)) << errorMsg;
	ASSERT_FALSE(result.extensions.empty());

	std::error_code ec;
	const std::filesystem::path outPath = std::filesystem::temp_directory_path() /
		(L"sakura-editor-next-openvsx-" + std::to_wstring(::GetCurrentProcessId()) + L".vsix");
	std::filesystem::remove(outPath, ec);
	struct TempFileGuard {
		std::filesystem::path path;
		~TempFileGuard() { std::error_code ignored; std::filesystem::remove(path, ignored); }
	} guard{ outPath };

	ASSERT_TRUE(client.DownloadVsix(result.extensions[0].sDownloadUrl, outPath, errorMsg)) << errorMsg;
	EXPECT_TRUE(std::filesystem::is_regular_file(outPath, ec));
	EXPECT_GT(std::filesystem::file_size(outPath, ec), 0u);
}

//! 平文通信が拒否されること
TEST(COpenVsxClient, Search_RejectsPlainHttpBeforeConnecting)
{
	COpenVsxClient client(L"http://127.0.0.1:1");
	ASSERT_TRUE(client.IsOk());

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	EXPECT_FALSE(client.Search(L"eslint", 0, 5, result, errorMsg));
	EXPECT_NE(std::wstring::npos, errorMsg.find(L"https")) << errorMsg;
}

//! 事前に公開された取消しは接続前に失敗として観測できること
TEST(COpenVsxClient, Search_HonorsPreCancellation)
{
	COpenVsxClient client(L"https://127.0.0.1:1");
	ASSERT_TRUE(client.IsOk());

	std::atomic<bool> cancelled{ true };
	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	EXPECT_FALSE(client.Search(L"eslint", 0, 5, result, errorMsg, &cancelled));
	EXPECT_NE(std::wstring::npos, errorMsg.find(L"cancelled")) << errorMsg;
	EXPECT_TRUE(result.extensions.empty());
}

//! 失敗時に前回の結果が残らないこと
TEST(COpenVsxClient, ParseSearchResponse_ClearsResultOnFailure)
{
	SOpenVsxSearchResult result;
	result.nTotalSize = 99;
	result.extensions.emplace_back();

	std::wstring errorMsg;
	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse("{", result, errorMsg));
	EXPECT_EQ(0, result.nTotalSize);
	EXPECT_TRUE(result.extensions.empty());
}

//! レジストリが要求件数を無視して巨大な配列を返しても無制限に確保しないこと
TEST(COpenVsxClient, ParseSearchResponse_RejectsOversizedPage)
{
	std::string json = R"({"extensions":[)";
	for (int i = 0; i < 101; ++i) {
		if (i != 0) {
			json += ',';
		}
		json += "{}";
	}
	json += "]}";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	EXPECT_FALSE(COpenVsxClient::ParseSearchResponse(json, result, errorMsg));
	EXPECT_FALSE(errorMsg.empty());
	EXPECT_TRUE(result.extensions.empty());
}

//! 範囲外の数値を整数へ変換して未定義動作を起こさないこと
TEST(COpenVsxClient, ParseSearchResponse_ClampsUntrustedCounts)
{
	const std::string json = R"({
		"offset": 1e100,
		"totalSize": -5,
		"extensions": [{
			"namespace": "safe",
			"name": "sample",
			"version": "1.0.0",
			"downloadCount": 1e100,
			"files": {"download": "https://example.test/sample.vsix"}
		}]
	})";

	SOpenVsxSearchResult result;
	std::wstring errorMsg;
	ASSERT_TRUE(COpenVsxClient::ParseSearchResponse(json, result, errorMsg)) << errorMsg;
	EXPECT_EQ((std::numeric_limits<int>::max)(), result.nOffset);
	EXPECT_EQ(0, result.nTotalSize);
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_EQ((std::numeric_limits<long long>::max)(), result.extensions[0].nDownloadCount);
}

//! protocol の解析結果は legacy client の静的 API と同じ public model を返すこと
TEST(OpenVsxProtocol, ParseSearchResponse_PreservesLegacyPublicModel)
{
	const std::string json = R"({
		"offset": 1,
		"totalSize": 1,
		"extensions": [{
			"namespace": "sample",
			"name": "tool",
			"version": "1.0.0",
			"files": { "download": "https://example.test/tool.vsix" }
		}]
	})";
	SOpenVsxSearchResult result;
	std::wstring errorMsg;

	ASSERT_TRUE(extension::openvsx::OpenVsxProtocol::ParseSearchResponse(json, result, errorMsg)) << errorMsg;
	ASSERT_EQ(1u, result.extensions.size());
	EXPECT_EQ(L"sample.tool", result.extensions.front().GetUniqueId());
	EXPECT_EQ(L"tool", result.extensions.front().sDisplayName);
}

//! 成功結果に、同じ呼び出し元が保持していた古い失敗診断を混在させないこと
TEST(OpenVsxProtocol, ParseSearchResponse_ClearsPriorErrorOnSuccess)
{
	SOpenVsxSearchResult result;
	std::wstring errorMsg = L"stale failure";

	ASSERT_TRUE(extension::openvsx::OpenVsxProtocol::ParseSearchResponse(
		R"({"offset":0,"totalSize":0})", result, errorMsg));
	EXPECT_TRUE(errorMsg.empty());
}
