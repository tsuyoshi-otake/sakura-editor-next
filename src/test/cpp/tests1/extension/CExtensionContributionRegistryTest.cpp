/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionContributionRegistry.h"

namespace {

SExtensionContributions ClaudeCodeLikeContributions()
{
	SExtensionContributions contributions;
	contributions.containerPresentations.push_back({ .id = L"claude-code", .codicon = L"comment-discussion" });
	contributions.viewPresentations.push_back({
		.id = L"claude-code.sidebar",
		.whenClause = L"claude-code.enabled",
		.contextualTitle = L"Claude Code",
		.kind = EExtensionViewKind::Webview,
	});
	contributions.menuItems.push_back({
		.location = L"editor/title", .commandId = L"claude-code.runInTerminal",
		.groupName = L"navigation", .groupOrder = 1 });
	contributions.menuItems.push_back({
		.location = L"editor/title", .commandId = L"claude-code.openSettings", .groupName = L"z_config" });
	contributions.menuItems.push_back({
		.location = L"commandPalette", .commandId = L"claude-code.focus", .whenClause = L"editorFocus" });
	contributions.submenus.push_back({ .id = L"claude-code.more", .label = L"More" });
	contributions.keybindings.push_back({
		.commandId = L"claude-code.focus", .keyChord = L"ctrl+escape", .argumentsJson = "{\"from\":\"key\"}" });
	contributions.languages.push_back({
		.id = L"claude-plan",
		.extensions = { L".plan" },
		.filenames = { L"CLAUDE.md" },
	});
	contributions.snippets.push_back({ .languageId = L"claude-plan", .path = L"C:\\ext\\snippets\\plan.json" });
	contributions.acknowledged = { L"jsonValidation", L"walkthroughs" };
	return contributions;
}

TEST(CExtensionContributionRegistry, StampsOwnershipAndExposesEveryContributionKind)
{
	CExtensionContributionRegistry registry;
	registry.Register({ .extensionId = L"Anthropic.claude-code", .generation = 3 }, ClaudeCodeLikeContributions());

	const auto container = registry.ContainerPresentation(L"claude-code");
	EXPECT_EQ(L"claude-code", container.id);
	EXPECT_EQ(L"comment-discussion", container.codicon);
	EXPECT_EQ(L"Anthropic.claude-code", container.extensionId);
	EXPECT_EQ(3u, container.generation);

	// `"type": "webview"` を落とすとサイドバーは空のツリーになる。ここは実害に直結する。
	const auto view = registry.ViewPresentation(L"claude-code.sidebar");
	EXPECT_EQ(EExtensionViewKind::Webview, view.kind);
	EXPECT_EQ(L"claude-code.enabled", view.whenClause);
	EXPECT_EQ(L"Claude Code", view.contextualTitle);

	EXPECT_EQ(std::vector<std::wstring>({ L"commandPalette", L"editor/title" }), registry.MenuLocations());
	ASSERT_EQ(1u, registry.Keybindings().size());
	EXPECT_EQ("{\"from\":\"key\"}", registry.Keybindings()[0].argumentsJson);
	ASSERT_EQ(1u, registry.Submenus().size());
	EXPECT_EQ(L"More", registry.Submenus()[0].label);
	EXPECT_EQ(std::vector<std::wstring>({ L"jsonValidation", L"walkthroughs" }),
		registry.AcknowledgedContributions(L"Anthropic.claude-code"));
	EXPECT_TRUE(registry.AcknowledgedContributions(L"other.extension").empty());
}

TEST(CExtensionContributionRegistry, OrdersMenuItemsWithNavigationFirstThenGroupNameAndOrder)
{
	CExtensionContributionRegistry registry;
	SExtensionContributions contributions;
	contributions.menuItems.push_back({ .location = L"editor/title", .commandId = L"c", .groupName = L"z_config" });
	contributions.menuItems.push_back({ .location = L"editor/title", .commandId = L"b", .groupName = L"navigation", .groupOrder = 2 });
	contributions.menuItems.push_back({ .location = L"editor/title", .commandId = L"a", .groupName = L"navigation", .groupOrder = 1 });
	// グループ未指定は VS Code と同じく末尾へ落とす。
	contributions.menuItems.push_back({ .location = L"editor/title", .commandId = L"d" });
	registry.Register({ .extensionId = L"vendor.tool", .generation = 1 }, std::move(contributions));

	const auto items = registry.MenuItems(L"editor/title");
	ASSERT_EQ(4u, items.size());
	EXPECT_EQ(L"a", items[0].commandId);
	EXPECT_EQ(L"b", items[1].commandId);
	EXPECT_EQ(L"c", items[2].commandId);
	EXPECT_EQ(L"d", items[3].commandId);
	EXPECT_TRUE(registry.MenuItems(L"explorer/context").empty());
}

TEST(CExtensionContributionRegistry, ReplacesEveryContributionOfTheSameOwnerOnReRegistration)
{
	CExtensionContributionRegistry registry;
	const SExtensionContributionOwner owner{ .extensionId = L"vendor.tool", .generation = 1 };
	registry.Register(owner, ClaudeCodeLikeContributions());

	// 更新後のマニフェスト再送。差分ではなく全置換なので、古い項目は残ってはいけない。
	SExtensionContributions replacement;
	replacement.menuItems.push_back({ .location = L"editor/context", .commandId = L"vendor.only" });
	registry.Register(owner, std::move(replacement));

	EXPECT_EQ(std::vector<std::wstring>({ L"editor/context" }), registry.MenuLocations());
	EXPECT_TRUE(registry.Keybindings().empty());
	EXPECT_TRUE(registry.Languages().empty());
	EXPECT_TRUE(registry.SnippetFiles().empty());
	EXPECT_TRUE(registry.ContainerPresentation(L"claude-code").id.empty());
	EXPECT_TRUE(registry.AcknowledgedContributions(L"vendor.tool").empty());
}

TEST(CExtensionContributionRegistry, ReplacesTheOlderGenerationOfTheSameExtensionOnReconnect)
{
	CExtensionContributionRegistry registry;
	registry.Register({ .extensionId = L"vendor.tool", .generation = 1 }, ClaudeCodeLikeContributions());

	// 拡張ホストが落ちて繋ぎ直すと、同じ拡張が新しい世代で登録し直してくる。
	// 前の世代を残すと、アクティビティバーのアイコンもメニューも二重に出る。
	SExtensionContributions reconnected;
	reconnected.containerPresentations.push_back({ .id = L"claude-code", .codicon = L"rocket" });
	registry.Register({ .extensionId = L"vendor.tool", .generation = 2 }, std::move(reconnected));

	const auto containers = registry.ContainerPresentations();
	ASSERT_EQ(1u, containers.size());
	EXPECT_EQ(L"rocket", containers[0].codicon);
	EXPECT_EQ(2u, containers[0].generation);
	EXPECT_TRUE(registry.MenuLocations().empty());
	EXPECT_TRUE(registry.Keybindings().empty());
	EXPECT_TRUE(registry.ViewPresentations().empty());

	// 新しい世代で消せば残らない。
	registry.RemoveOwnedBy(L"vendor.tool", 2);
	EXPECT_TRUE(registry.ContainerPresentations().empty());
}

TEST(CExtensionContributionRegistry, RemovesOnlyTheMatchingOwnerGeneration)
{
	CExtensionContributionRegistry registry;
	registry.Register({ .extensionId = L"vendor.tool", .generation = 1 }, ClaudeCodeLikeContributions());
	SExtensionContributions other;
	other.menuItems.push_back({ .location = L"editor/title", .commandId = L"other.command" });
	registry.Register({ .extensionId = L"other.tool", .generation = 9 }, std::move(other));

	// 世代違いは他人。ここを generation 無視で消すと、再接続直後の登録を巻き添えにする。
	registry.RemoveOwnedBy(L"vendor.tool", 2);
	EXPECT_FALSE(registry.ContainerPresentation(L"claude-code").id.empty());

	registry.RemoveOwnedBy(L"vendor.tool", 1);
	EXPECT_TRUE(registry.ContainerPresentation(L"claude-code").id.empty());
	ASSERT_EQ(1u, registry.MenuItems(L"editor/title").size());
	EXPECT_EQ(L"other.command", registry.MenuItems(L"editor/title")[0].commandId);

	registry.Clear();
	EXPECT_TRUE(registry.MenuLocations().empty());
}

TEST(CExtensionContributionRegistry, ResolvesLanguageIdByFilenameBeforeExtensionAndIgnoresCase)
{
	CExtensionContributionRegistry registry;
	SExtensionContributions contributions;
	contributions.languages.push_back({ .id = L"markdown", .extensions = { L".md" } });
	contributions.languages.push_back({ .id = L"claude-instructions", .filenames = { L"CLAUDE.md" } });
	registry.Register({ .extensionId = L"vendor.tool", .generation = 1 }, std::move(contributions));

	// 完全なファイル名の一致が拡張子より強い。逆にすると CLAUDE.md が markdown に落ちる。
	EXPECT_EQ(L"claude-instructions", registry.ResolveLanguageId(L"C:\\repo\\CLAUDE.md"));
	EXPECT_EQ(L"claude-instructions", registry.ResolveLanguageId(L"claude.MD"));
	EXPECT_EQ(L"markdown", registry.ResolveLanguageId(L"C:/repo/README.md"));
	EXPECT_TRUE(registry.ResolveLanguageId(L"main.cpp").empty());
	EXPECT_TRUE(registry.ResolveLanguageId(L"").empty());
	// 拡張子だけのファイル名を拡張子一致にしない（`.md` は「md という名前」ではない）。
	EXPECT_TRUE(registry.ResolveLanguageId(L".md").empty());
}

TEST(CExtensionContributionRegistry, FiltersSnippetFilesByLanguage)
{
	CExtensionContributionRegistry registry;
	SExtensionContributions contributions;
	contributions.snippets.push_back({ .languageId = L"cpp", .path = L"C:\\ext\\cpp.json" });
	contributions.snippets.push_back({ .languageId = L"markdown", .path = L"C:\\ext\\md.json" });
	registry.Register({ .extensionId = L"vendor.tool", .generation = 1 }, std::move(contributions));

	EXPECT_EQ(2u, registry.SnippetFiles().size());
	ASSERT_EQ(1u, registry.SnippetFiles(L"markdown").size());
	EXPECT_EQ(L"C:\\ext\\md.json", registry.SnippetFiles(L"markdown")[0].path);
	EXPECT_TRUE(registry.SnippetFiles(L"python").empty());
}

} // namespace
