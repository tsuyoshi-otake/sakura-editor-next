/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionCommandPalette.h"
#include "extension/CExtensionContextKeys.h"
#include "extension/CExtensionNotificationCenter.h"
#include "extension/CExtensionStatusBar.h"

TEST(CExtensionContextKeys, EvaluatesCommonWhenClausesAndFailsClosed)
{
	CExtensionContextKeys context;
	ASSERT_TRUE(context.Set(L"editorLangId", std::wstring(L"markdown"), L"sample.extension"));
	ASSERT_TRUE(context.Set(L"editorHasSelection", true, L"sample.extension"));
	ASSERT_TRUE(context.Set(L"resourceExtname", std::wstring(L".MD"), L"sample.extension"));

	EXPECT_TRUE(context.Evaluate(L"editorLangId == markdown && editorHasSelection"));
	EXPECT_TRUE(context.Evaluate(L"editorLangId != plaintext && !missingKey"));
	EXPECT_TRUE(context.Evaluate(L"resourceExtname =~ /\\.md$/i"));
	EXPECT_FALSE(context.Evaluate(L"editorLangId == markdown && (editorHasSelection"));
	EXPECT_FALSE(context.Evaluate(L"resourceExtname =~ /[/"));
	EXPECT_TRUE(context.Evaluate(L""));
}
TEST(CExtensionContextKeys, RemovesOnlyKeysOwnedByTheDeactivatedExtension)
{
	CExtensionContextKeys context;
	ASSERT_TRUE(context.Set(L"first.key", true, L"first.extension"));
	ASSERT_TRUE(context.Set(L"second.key", true, L"second.extension"));
	context.RemoveOwnedBy(L"first.extension");
	EXPECT_FALSE(context.Contains(L"first.key"));
	EXPECT_TRUE(context.Contains(L"second.key"));
	EXPECT_FALSE(context.Remove(L"second.key", L"first.extension"));
	EXPECT_TRUE(context.Remove(L"second.key", L"second.extension"));
}

TEST(CExtensionCommandPalette, SearchesBuiltInAndExtensionCommandsWithContext)
{
	CExtensionContextKeys context;
	ASSERT_TRUE(context.Set(L"editorLangId", std::wstring(L"markdown")));
	ASSERT_TRUE(context.Set(L"workspaceTrusted", false));
	CExtensionCommandPalette palette;
	ASSERT_TRUE(palette.Register({
		.id = L"sakura.file.open", .title = L"Open File", .category = L"File", .builtIn = true }));
	ASSERT_TRUE(palette.Register({
		.id = L"markdownlint.fixAll", .title = L"Fix all supported markdownlint violations",
		.category = L"Markdownlint", .whenClause = L"editorLangId == markdown",
		.enablementClause = L"workspaceTrusted", .extensionId = L"davidanson.vscode-markdownlint", .generation = 7 }));

	const auto all = palette.Search(L"", context);
	ASSERT_EQ(2u, all.size());
	const auto filtered = palette.Search(L"md fix", context);
	ASSERT_EQ(1u, filtered.size());
	EXPECT_EQ(L"markdownlint.fixAll", filtered[0].id);
	EXPECT_FALSE(filtered[0].enabled);
	EXPECT_FALSE(palette.Register({
		.id = L"markdownlint.fixAll", .title = L"Hijack", .extensionId = L"evil.extension", .generation = 1 }));
}

TEST(CExtensionCommandPalette, RemovesOnlyTheRequestedHostGeneration)
{
	CExtensionContextKeys context;
	CExtensionCommandPalette palette;
	ASSERT_TRUE(palette.Register({
		.id = L"first.command", .title = L"First", .extensionId = L"sample.extension", .generation = 1 }));
	ASSERT_TRUE(palette.Register({
		.id = L"second.command", .title = L"Second", .extensionId = L"sample.extension", .generation = 2 }));
	palette.RemoveOwnedBy(L"sample.extension", 1);
	EXPECT_FALSE(palette.Contains(L"first.command"));
	EXPECT_TRUE(palette.Contains(L"second.command"));
}

TEST(CExtensionStatusBar, SortsVisibleItemsByAlignmentAndPriority)
{
	CExtensionStatusBar status;
	ASSERT_TRUE(status.Upsert({ .handle = L"right-low", .extensionId = L"sample.extension", .generation = 1,
		.alignment = EExtensionStatusBarAlignment::Right, .priority = 1, .text = L"R", .visible = true }));
	ASSERT_TRUE(status.Upsert({ .handle = L"left-low", .extensionId = L"sample.extension", .generation = 1,
		.alignment = EExtensionStatusBarAlignment::Left, .priority = 1, .text = L"L1", .visible = true }));
	ASSERT_TRUE(status.Upsert({ .handle = L"left-high", .extensionId = L"sample.extension", .generation = 1,
		.alignment = EExtensionStatusBarAlignment::Left, .priority = 100, .text = L"L2",
		.command = L"sample.run", .visible = true }));
	ASSERT_TRUE(status.Upsert({ .handle = L"hidden", .extensionId = L"sample.extension", .generation = 1,
		.text = L"hidden", .visible = false }));

	const auto items = status.Snapshot();
	ASSERT_EQ(3u, items.size());
	EXPECT_EQ(L"left-high", items[0].handle);
	EXPECT_EQ(L"left-low", items[1].handle);
	EXPECT_EQ(L"right-low", items[2].handle);
	EXPECT_EQ(std::optional<std::wstring>(L"sample.run"), status.CommandFor(L"left-high"));
	EXPECT_FALSE(status.CommandFor(L"hidden").has_value());
}

TEST(CExtensionStatusBar, EnforcesOwnershipAndCleansUpOnHostLoss)
{
	CExtensionStatusBar status;
	ASSERT_TRUE(status.Upsert({ .handle = L"owned", .extensionId = L"sample.extension", .generation = 4,
		.text = L"ready", .visible = true }));
	EXPECT_FALSE(status.Upsert({ .handle = L"owned", .extensionId = L"other.extension", .generation = 4,
		.text = L"replace", .visible = true }));
	EXPECT_FALSE(status.Remove(L"owned", L"sample.extension", 3));
	status.RemoveOwnedBy(L"sample.extension", 4);
	EXPECT_TRUE(status.Snapshot().empty());
}

TEST(CExtensionNotificationCenter, ResolvesEachRequestExactlyOnce)
{
	CExtensionNotificationCenter center;
	auto id = center.Show({ .extensionId = L"sample.extension", .generation = 2,
		.severity = EExtensionNotificationSeverity::Warning, .message = L"Continue?",
		.actions = { L"Yes", L"No" } });
	ASSERT_TRUE(id.has_value());
	EXPECT_TRUE(center.Resolve(*id, 1));
	EXPECT_FALSE(center.Resolve(*id, 0));
	auto completion = center.TakeCompletion(*id);
	ASSERT_TRUE(completion.has_value());
	EXPECT_EQ(EExtensionNotificationState::Resolved, completion->state);
	EXPECT_EQ(1u, *completion->selectedAction);
	EXPECT_FALSE(center.TakeCompletion(*id).has_value());
}

TEST(CExtensionNotificationCenter, BoundsPendingWorkAndTerminatesOnHostLoss)
{
	CExtensionNotificationCenter center(2);
	auto first = center.Show({ .extensionId = L"sample.extension", .generation = 9, .message = L"first" });
	auto modal = center.Show({ .extensionId = L"sample.extension", .generation = 9, .message = L"modal", .modal = true });
	ASSERT_TRUE(first && modal);
	auto third = center.Show({ .extensionId = L"sample.extension", .generation = 9, .message = L"third" });
	ASSERT_TRUE(third.has_value());
	auto evicted = center.TakeCompletion(*first);
	ASSERT_TRUE(evicted.has_value());
	EXPECT_EQ(EExtensionNotificationState::Dismissed, evicted->state);

	center.NotifyHostLost(L"sample.extension", 9);
	EXPECT_TRUE(center.Pending().empty());
	auto lost = center.TakeCompletion(*modal);
	ASSERT_TRUE(lost.has_value());
	EXPECT_EQ(EExtensionNotificationState::HostLost, lost->state);
}
