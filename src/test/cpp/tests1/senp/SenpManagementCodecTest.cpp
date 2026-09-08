/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "senp/SenpManagementService.h"

namespace {
std::string Candidate()
{
	return R"json({"archiveSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","signed":false,
	"manifest":{"schemaVersion":2,"id":"sample.extension","displayName":"Sample","version":"1.0.0",
	"publisher":"sample","description":"Fixture","runtime":{"abi":"sakura:senp/extension@2.0.0","module":"module/extension.wasm"},
	"activationEvents":["onView:sample:view"],"capabilities":["workbench.views.tree","workbench.commands"],
	"contributes":{"editorDecorations":[],"languages":[],"grammars":[],
	"commands":[{"command":"sample.open","title":"Open"}],
	"viewsContainers":{"activitybar":[{"id":"sample.container","title":"Sample","icon":"$(beaker)","order":1}]},
	"views":{"sample.container":[{"id":"sample:view","name":"View","provider":"senp.tree","order":1}]}}}})json";
}
void Replace(std::string& text, std::string_view from, std::string_view to)
{
	const auto offset = text.find(from);
	ASSERT_NE(std::string::npos, offset);
	text.replace(offset, from.size(), to);
}
}

TEST(SenpManagementCodec, RetainsRuntimeMetadataForCatalogAndInstalledAuthorities)
{
	const auto catalog = senp::DecodeBuiltInExtension(Candidate());
	ASSERT_TRUE(catalog);
	EXPECT_FALSE(catalog->installed);
	EXPECT_FALSE(catalog->enabled);
	EXPECT_EQ(2U, catalog->runtime.schemaVersion);
	EXPECT_EQ(L"sakura:senp/extension@2.0.0", catalog->runtime.abi);
	EXPECT_EQ((std::vector<std::wstring>{ L"onView:sample:view" }), catalog->runtime.activationEvents);
	ASSERT_EQ(1U, catalog->runtime.commands.size());
	EXPECT_EQ(L"sample.open", catalog->runtime.commands[0].command);
	EXPECT_EQ(L"Open", catalog->runtime.commands[0].title);
	EXPECT_EQ(L"sample:view", catalog->views[0].id);
	auto installedJson = Candidate();
	installedJson.insert(1, R"json("enabled":true,"trust":"developer","readme":"","extensionPath":"C:/fixture",
	"modulePath":"C:/fixture/module/extension.wasm","moduleSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)json");
	const auto installed = senp::DecodeInstalledExtensions("[" + installedJson + "]");
	ASSERT_TRUE(installed);
	ASSERT_EQ(1U, installed->size());
	EXPECT_TRUE(installed->front().enabled);
	EXPECT_EQ(catalog->runtime, installed->front().runtime);
}

TEST(SenpManagementCodec, PreservesLegacyMetadataAndRejectsCrossedRuntime)
{
	auto legacy = Candidate();
	Replace(legacy, "\"schemaVersion\":2", "\"schemaVersion\":1");
	Replace(legacy, "extension@2.0.0", "extension@1.0.0");
	Replace(legacy, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],", "");
	Replace(legacy, "onView:sample:view", "onStartupFinished");
	Replace(legacy, "\"workbench.views.tree\",\"workbench.commands\"", "\"editor.visibleText\",\"editor.decorations\"");
	const auto decoded = senp::DecodeBuiltInExtension(legacy);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(1U, decoded->runtime.schemaVersion);
	EXPECT_TRUE(decoded->runtime.commands.empty());
	for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
		{ "extension@2.0.0", "extension@1.0.0" },
		{ "module/extension.wasm", "elsewhere.wasm" },
		{ "\"schemaVersion\":2", "\"schemaVersion\":3" },
		{ "\"command\":\"sample.open\"", "\"command\":null" },
		{ "\"activationEvents\":[\"onView:sample:view\"]", "\"activationEvents\":null" },
	}) {
		auto json = Candidate(); Replace(json, mutation.first, mutation.second);
		EXPECT_FALSE(senp::DecodeBuiltInExtension(json)) << mutation.first;
	}
}
