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
	"publisher":"sample","description":"Fixture","runtime":{"abi":"sakura:senp/extension@3.0.0","module":"module/extension.wasm"},
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
	EXPECT_EQ(L"sakura:senp/extension@3.0.0", catalog->runtime.abi);
	EXPECT_EQ((std::vector<std::wstring>{ L"onView:sample:view" }), catalog->runtime.activationEvents);
	ASSERT_EQ(1U, catalog->runtime.commands.size());
	EXPECT_EQ(L"sample.open", catalog->runtime.commands[0].command);
	EXPECT_EQ(L"Open", catalog->runtime.commands[0].title);
	EXPECT_EQ(L"sample:view", catalog->views[0].id);
	auto installedJson = Candidate();
	installedJson.insert(1, R"json("enabled":true,"compatible":true,"trust":"developer","readme":"","extensionPath":"C:/fixture",
	"modulePath":"C:/fixture/module/extension.wasm","moduleSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)json");
	const auto installed = senp::DecodeInstalledExtensions("[" + installedJson + "]");
	ASSERT_TRUE(installed);
	ASSERT_EQ(1U, installed->size());
	EXPECT_TRUE(installed->front().enabled);
	EXPECT_EQ(catalog->runtime, installed->front().runtime);
}

TEST(SenpManagementCodec, TakesRuntimeCompatibilityFromTheRustListingOnly)
{
	// sakura_senp decides whether an installed package's runtime ABI is the one
	// this build executes. A package built for another WIT world is still listed
	// (it can be refreshed or removed) and C++ neither re-derives nor overrides that.
	auto stale = Candidate();
	Replace(stale, "extension@3.0.0", "extension@2.0.0");
	stale.insert(1, R"json("enabled":true,"compatible":false,"trust":"builtin","readme":"","extensionPath":"C:/fixture",
	"modulePath":"C:/fixture/module/extension.wasm","moduleSha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",)json");
	const auto listed = senp::DecodeInstalledExtensions("[" + stale + "]");
	ASSERT_TRUE(listed);
	ASSERT_EQ(1U, listed->size());
	EXPECT_FALSE(listed->front().runtime.compatible);
	EXPECT_EQ(L"sakura:senp/extension@2.0.0", listed->front().runtime.abi);

	// The listing must say; an absent or non-boolean value fails closed.
	for (const auto& mutation : { std::string{ "\"compatible\":false," }, std::string{ "\"compatible\":false" } }) {
		auto json = stale;
		Replace(json, mutation, mutation.back() == ',' ? "" : "\"compatible\":\"no\"");
		EXPECT_FALSE(senp::DecodeInstalledExtensions("[" + json + "]")) << mutation;
	}
	// A built-in candidate comes from archive verification, which admits only
	// the current ABI, so it is compatible by construction.
	const auto candidate = senp::DecodeBuiltInExtension(Candidate());
	ASSERT_TRUE(candidate);
	EXPECT_TRUE(candidate->runtime.compatible);
}

TEST(SenpManagementCodec, PreservesLegacyMetadataAndRejectsCrossedRuntime)
{
	auto legacy = Candidate();
	Replace(legacy, "\"schemaVersion\":2", "\"schemaVersion\":1");
	Replace(legacy, "extension@3.0.0", "extension@1.0.0");
	Replace(legacy, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],", "");
	Replace(legacy, "onView:sample:view", "onStartupFinished");
	Replace(legacy, "\"workbench.views.tree\",\"workbench.commands\"", "\"editor.visibleText\",\"editor.decorations\"");
	const auto decoded = senp::DecodeBuiltInExtension(legacy);
	ASSERT_TRUE(decoded);
	EXPECT_EQ(1U, decoded->runtime.schemaVersion);
	EXPECT_TRUE(decoded->runtime.commands.empty());
	for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
		{ "extension@3.0.0", "extension@1.0.0" },
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
	Replace(legacy, "extension@3.0.0", "extension@1.0.0");
	Replace(legacy, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],", "\"menus\":{\"view/title\":[]},");
	Replace(legacy, "onView:sample:view", "onStartupFinished");
	Replace(legacy, "\"workbench.views.tree\",\"workbench.commands\"", "\"editor.visibleText\",\"editor.decorations\"");
	EXPECT_FALSE(senp::DecodeBuiltInExtension(legacy));
}

namespace {
std::string WithViewItem()
{
	auto json = Candidate();
	Replace(json, "\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"}],",
		"\"commands\":[{\"command\":\"sample.open\",\"title\":\"Open\"},"
		"{\"command\":\"sample.logs\",\"title\":\"View logs\",\"icon\":\"resources/icons/light/logs.svg\"}],"
		"\"menus\":{\"view/item/context\":[{\"command\":\"sample.logs\","
		"\"when\":\"viewItem =~ /job/ && viewItem =~ /completed/\",\"group\":\"inline\"}]},");
	return json;
}
}

TEST(SenpManagementCodec, RetainsInlineItemActionsMatchedByContextValue)
{
	const auto decoded = senp::DecodeBuiltInExtension(WithViewItem());
	ASSERT_TRUE(decoded);
	ASSERT_EQ(2U, decoded->runtime.commands.size());
	// A path icon is kept verbatim; only an inline row action can draw it.
	EXPECT_EQ(L"resources/icons/light/logs.svg", decoded->runtime.commands[1].icon);
	EXPECT_TRUE(decoded->runtime.viewTitle.empty());
	ASSERT_EQ(1U, decoded->runtime.viewItemContext.size());
	const auto& action = decoded->runtime.viewItemContext[0];
	EXPECT_EQ(L"sample.logs", action.command);
	EXPECT_TRUE(action.views.empty());
	EXPECT_EQ((std::vector<std::wstring>{ L"job", L"completed" }), action.contains);
	EXPECT_TRUE(action.equals.empty());
	auto scoped = WithViewItem();
	Replace(scoped, "viewItem =~ /job/ && viewItem =~ /completed/", "view == sample:view && viewItem == step");
	const auto equality = senp::DecodeBuiltInExtension(scoped);
	ASSERT_TRUE(equality);
	ASSERT_EQ(1U, equality->runtime.viewItemContext.size());
	EXPECT_EQ((std::vector<std::wstring>{ L"sample:view" }), equality->runtime.viewItemContext[0].views);
	EXPECT_TRUE(equality->runtime.viewItemContext[0].contains.empty());
	EXPECT_EQ((std::vector<std::wstring>{ L"step" }), equality->runtime.viewItemContext[0].equals);
}

TEST(SenpManagementCodec, RejectsInlineItemActionsARowCannotShow)
{
	for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
		{ "\"group\":\"inline\"", "\"group\":\"navigation\"" },
		{ ",\"group\":\"inline\"", "" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "viewItem =~ /job/ || viewItem =~ /completed/" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "!viewItem" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "viewItem =~ /jo.b/" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "view == sample:view" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "view == sample:view && view == other:view && viewItem == job" },
		{ "viewItem =~ /job/ && viewItem =~ /completed/", "viewItem =~ /job/ && viewItem =~ /job/" },
		{ "{\"command\":\"sample.logs\",\"when\"", "{\"command\":\"sample.open\",\"when\"" },
		{ "{\"command\":\"sample.logs\",\"when\"", "{\"command\":\"sample.missing\",\"when\"" },
		{ "\"resources/icons/light/logs.svg\"", "\"../logs.svg\"" },
		{ "\"group\":\"inline\"}]", "\"group\":\"inline\"},{\"command\":\"sample.logs\",\"when\":\"viewItem == job\",\"group\":\"inline\"}]" },
	}) {
		auto json = WithViewItem(); Replace(json, mutation.first, mutation.second);
		EXPECT_FALSE(senp::DecodeBuiltInExtension(json)) << mutation.second;
	}
	auto empty = Candidate();
	Replace(empty, "\"viewsContainers\"", "\"menus\":{\"view/item/context\":[]},\"viewsContainers\"");
	EXPECT_FALSE(senp::DecodeBuiltInExtension(empty));
	// A path icon cannot draw on a title button.
	auto title = WithViewTitle();
	Replace(title, "\"$(refresh)\"", "\"resources/icons/light/logs.svg\"");
	EXPECT_FALSE(senp::DecodeBuiltInExtension(title));
}
