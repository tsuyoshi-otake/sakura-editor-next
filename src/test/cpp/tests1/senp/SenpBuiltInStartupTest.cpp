/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpManagementService.h"

#include <set>

// Every embedded built-in is installed by default (#337), so startup brings a
// built-in the profile lacks into a fresh profile and into one an update
// reaches alike: both arrive here with no installed copy and no tombstone.
TEST(SenpBuiltInStartup, InstallsEveryEmbeddedBuiltInTheProfileLacks)
{
	for (const auto id : senp::EmbeddedBuiltInExtensionIds()) {
		EXPECT_TRUE(senp::StartupInstallsBuiltIn(id, false, false)) << std::wstring(id);
		EXPECT_TRUE(senp::StartupInstallsBuiltIn(id, false, true)) << std::wstring(id);
	}
}

// The GitHub packages are named so that moving them back to offered fails here
// rather than only in the loop above.
TEST(SenpBuiltInStartup, InstallsTheGitHubBuiltInsWithoutAGitHubConnection)
{
	EXPECT_TRUE(senp::StartupInstallsBuiltIn(L"sakura-github-pull-requests", false, false));
	EXPECT_TRUE(senp::StartupInstallsBuiltIn(L"sakura-github-actions", false, false));
}

// An explicit uninstall survives product updates; only an explicit reinstall
// clears its tombstone.
TEST(SenpBuiltInStartup, HonorsAnUninstallTombstoneForEveryEmbeddedBuiltIn)
{
	for (const auto id : senp::EmbeddedBuiltInExtensionIds()) {
		EXPECT_FALSE(senp::StartupInstallsBuiltIn(id, true, false)) << std::wstring(id);
		EXPECT_FALSE(senp::StartupInstallsBuiltIn(id, true, true)) << std::wstring(id);
	}
}

TEST(SenpBuiltInStartup, NeverInstallsAnIdTheExecutableDoesNotEmbed)
{
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"sample.extension", false, false));
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"sample.extension", false, true));
	EXPECT_FALSE(senp::StartupInstallsBuiltIn(L"", false, true));
}

namespace {

std::string ManifestJson(std::string_view id)
{
	return std::string(R"json({"schemaVersion":1,"id":")json") + std::string(id)
		+ R"json(","displayName":")json" + std::string(id)
		+ R"json(","version":"1.0.0","publisher":"sakura.builtin","description":"x",)json"
		+ R"json("engines":{"sakura":">=0.0.0"},)json"
		+ R"json("activationEvents":[],"capabilities":[],)json"
		+ R"json("contributes":{"editorDecorations":[],"languages":[],"grammars":[],)json"
		+ R"json("viewsContainers":{"activitybar":[]},"views":{}}})json";
}

std::string CatalogJson(std::string_view id)
{
	return std::string(R"json({"archiveSha256":")json") + std::string(64, 'a')
		+ R"json(","signed":false,"manifest":)json" + ManifestJson(id) + "}";
}

std::string InstalledLine(std::string_view id)
{
	return std::string(R"json({"kind":"installed","archiveSha256":")json") + std::string(64, 'a')
		+ R"json(","enabled":true,"signed":false,"trust":"builtin","compatible":true,)json"
		+ R"json("extensionPath":"C:/senp/content","moduleSha256":null,"manifest":)json"
		+ ManifestJson(id) + "}\n";
}

//! The staging path `<installRoot>/staging/<id>.<pid>.senp` carries the id.
std::wstring StagedId(const std::wstring& path)
{
	const auto name = path.substr(path.find_last_of(L"\\/") + 1);
	const auto first = name.find(L'.');
	return name.substr(0, first == std::wstring::npos ? first : first);
}

struct FakeTool final {
	senp::detail::ToolResult operator()(const std::vector<std::wstring>& arguments)
	{
		senp::detail::ToolResult result;
		result.launched = true;
		result.exitCode = 0;
		const auto& verb = arguments.at(0);
		if (verb == L"inspect-builtin") {
			result.output = CatalogJson(WideToUtf8(StagedId(arguments.at(1))));
			return result;
		}
		if (verb == L"install-builtin") {
			const auto id = StagedId(arguments.at(1));
			installCalls.push_back(id);
			if (id == failId) {
				result.exitCode = 1;
				result.error = "install failed";
				return result;
			}
			installedIds.insert(WideToUtf8(id));
			return result;
		}
		if (verb == L"list") {
			for (const auto& id : installedIds) result.output += InstalledLine(id);
			return result;
		}
		if (verb == L"list-uninstalled") {
			result.output = "[]";
			return result;
		}
		result.exitCode = 2;
		return result;
	}

	static std::string WideToUtf8(std::wstring_view value)
	{
		std::string text;
		for (const wchar_t ch : value) text.push_back(static_cast<char>(ch));
		return text;
	}

	std::wstring failId;
	std::set<std::string> installedIds;
	std::vector<std::wstring> installCalls;
};

TEST(SenpBuiltInStartup, OneFailedInstallStopsNoOtherAndTheNextStartRetriesOnlyIt)
{
	auto fake = std::make_shared<FakeTool>();
	fake->failId = L"sakura-shell-language-basics";
	senp::CWin32SenpManagementService service(L"C:\\senp-test-profile", {
		.runTool = [fake](const auto& args) { return (*fake)(args); },
		.embeddedResource = [](int id) {
			// Package resource ids are even, hash resource ids odd; the staged
			// hash must be exactly 64 hex characters.
			if (id % 2 == 1) {
				std::vector<std::byte> hash(64, std::byte{ 'a' });
				return std::optional(std::move(hash));
			}
			return std::optional(std::vector<std::byte>{ std::byte{ 0x7f } });
		},
	});
	const auto first = service.Start();
	EXPECT_TRUE(first.Succeeded());
	EXPECT_EQ(senp::EManagementState::ReadyWithDiagnostics, first.snapshot.state);
	EXPECT_NE(std::wstring::npos,
		first.snapshot.diagnostic.find(L"sakura-shell-language-basics"));
	// Every other built-in was still attempted and installed.
	const auto builtInCount = senp::EmbeddedBuiltInExtensionIds().size();
	EXPECT_EQ(builtInCount, fake->installCalls.size());
	EXPECT_EQ(builtInCount - 1, fake->installedIds.size());
	EXPECT_TRUE(std::ranges::none_of(first.snapshot.extensions,
		[](const auto& extension) { return extension.id == L"sakura-shell-language-basics"
			&& extension.installed; }));

	// A later Start retries exactly the failed id, never answering AlreadyReady.
	fake->installCalls.clear();
	const auto second = service.Start();
	EXPECT_EQ(senp::EManagementOperationStatus::Succeeded, second.status);
	ASSERT_EQ(1U, fake->installCalls.size());
	EXPECT_EQ(L"sakura-shell-language-basics", fake->installCalls.front());
	EXPECT_EQ(senp::EManagementState::ReadyWithDiagnostics, second.snapshot.state);

	// Once the failure clears, reconciliation completes and Start settles.
	fake->failId.clear();
	fake->installCalls.clear();
	const auto third = service.Start();
	EXPECT_EQ(senp::EManagementOperationStatus::Succeeded, third.status);
	EXPECT_EQ(senp::EManagementState::Ready, third.snapshot.state);
	EXPECT_EQ(1U, fake->installCalls.size());
	const auto fourth = service.Start();
	EXPECT_EQ(senp::EManagementOperationStatus::AlreadyReady, fourth.status);
}

TEST(SenpBuiltInStartup, ATruncatedListingIsATypedFailureThatKeepsTheSnapshot)
{
	auto fake = std::make_shared<FakeTool>();
	bool truncate = false;
	senp::CWin32SenpManagementService service(L"C:\\senp-test-profile", {
		.runTool = [fake, &truncate](const auto& args) {
			auto result = (*fake)(args);
			if (truncate && args.at(0) == L"list") result.outputTruncated = true;
			return result;
		},
		.embeddedResource = [](int) { return std::optional<std::vector<std::byte>>{}; },
	});
	// Establish a previous snapshot through the public refresh path.
	fake->installedIds.insert("sakura-indent-rainbow");
	const auto ready = service.Refresh();
	ASSERT_TRUE(ready.Succeeded());
	ASSERT_EQ(1U, ready.snapshot.extensions.size());

	truncate = true;
	const auto truncated = service.Refresh();
	EXPECT_EQ(senp::EManagementOperationStatus::Failed, truncated.status);
	EXPECT_EQ(senp::EManagementState::ReadyWithDiagnostics, truncated.snapshot.state);
	EXPECT_NE(std::wstring::npos, truncated.snapshot.diagnostic.find(L"4 MiB"));
	// The previous snapshot survives; no cut stream was parsed.
	ASSERT_EQ(1U, truncated.snapshot.extensions.size());
	EXPECT_EQ(L"sakura-indent-rainbow", truncated.snapshot.extensions.front().id);
}

} // namespace
