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

namespace {
std::string WithViewTitle()
{
	auto json = Candidate();
	Replace(json, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],",
		"\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"},"
		"{\"command\":\"sample.refresh\",\"title\":\"Refresh\",\"icon\":\"$(refresh)\"}],"
		"\"menus\":{\"view/title\":[{\"command\":\"sample.refresh\","
		"\"when\":\"view == sample:view || view==other:view\",\"group\":\"navigation\"}]},");
	return json;
}
}

TEST(SenpManagementCodec, RetainsViewTitleActionsPlacedByViewEquality)
{
	const auto decoded = senp::DecodeBuiltInExtension(WithViewTitle());
	ASSERT_TRUE(decoded);
	ASSERT_EQ(2U, decoded->runtime.commands.size());
	EXPECT_TRUE(decoded->runtime.commands[0].icon.empty());
	EXPECT_EQ(L"$(refresh)", decoded->runtime.commands[1].icon);
	ASSERT_EQ(1U, decoded->runtime.viewTitle.size());
	EXPECT_EQ(L"sample.refresh", decoded->runtime.viewTitle[0].command);
	EXPECT_EQ((std::vector<std::wstring>{ L"sample:view", L"other:view" }), decoded->runtime.viewTitle[0].views);
	// Absent menus are an empty set, not a schema error.
	const auto plain = senp::DecodeBuiltInExtension(Candidate());
	ASSERT_TRUE(plain);
	EXPECT_TRUE(plain->runtime.viewTitle.empty());
}

TEST(SenpManagementCodec, RejectsViewTitleActionsATitleButtonCannotShow)
{
	for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
		{ "\"group\":\"navigation\"", "\"group\":\"inline\"" },
		{ ",\"group\":\"navigation\"", "" },
		{ "view == sample:view || view==other:view", "view != sample:view" },
		{ "view == sample:view || view==other:view", "view == sample:view && focusedView == sample:view" },
		{ "view == sample:view || view==other:view", "view == sample:view || view == sample:view" },
		{ "view == sample:view || view==other:view", "view == " },
		{ "{\"command\":\"sample.refresh\",\"when\"", "{\"command\":\"sample.open\",\"when\"" },
		{ "{\"command\":\"sample.refresh\",\"when\"", "{\"command\":\"sample.missing\",\"when\"" },
		{ "\"$(refresh)\"", "\"refresh\"" },
		{ "\"$(refresh)\"", "\"$(re fresh)\"" },
		{ "\"menus\":{\"view/title\"", "\"menus\":{\"editor/title\"" },
		{ "\"group\":\"navigation\"}]", "\"group\":\"navigation\"},{\"command\":\"sample.refresh\",\"when\":\"view == other:view\",\"group\":\"navigation\"}]" },
	}) {
		auto json = WithViewTitle(); Replace(json, mutation.first, mutation.second);
		EXPECT_FALSE(senp::DecodeBuiltInExtension(json)) << mutation.second;
	}
	auto empty = Candidate();
	Replace(empty, "\"viewsContainers\"", "\"menus\":{\"view/title\":[]},\"viewsContainers\"");
	EXPECT_FALSE(senp::DecodeBuiltInExtension(empty));
	// Schema 1 has no command surface for a title action to name.
	auto legacy = Candidate();
	Replace(legacy, "\"schemaVersion\":2", "\"schemaVersion\":1");
	Replace(legacy, "extension@2.0.0", "extension@1.0.0");
	Replace(legacy, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],", "\"menus\":{\"view/title\":[]},");
	Replace(legacy, "onView:sample:view", "onStartupFinished");
	Replace(legacy, "\"workbench.views.tree\",\"workbench.commands\"", "\"editor.visibleText\",\"editor.decorations\"");
	EXPECT_FALSE(senp::DecodeBuiltInExtension(legacy));
}
