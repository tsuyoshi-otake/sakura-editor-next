/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "extension/CExtensionCommandPalette.h"
#include "extension/CExtensionContextKeys.h"
#include "extension/CExtensionContributionRegistry.h"
#include "extension/ExtensionMenuProjection.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using extension::menus::CommandInfoLookup;
using extension::menus::IsVisibleInCommandPalette;
using extension::menus::kCommandPalette;
using extension::menus::kEditorTitle;
using extension::menus::ProjectMenu;
using extension::menus::SProjectedMenuCommandInfo;
using extension::menus::SProjectedMenuItem;
using extension::menus::WhenClauseEvaluator;

//! 投影の入力 3 点を 1 つに束ねただけの足場。テストごとに組み直すのは無駄が多い。
struct MenuFixture {
	CExtensionContributionRegistry	contributions;
	CExtensionCommandPalette		commands;
	CExtensionContextKeys			contextKeys;

	void DeclareCommand(std::wstring id, std::wstring title,
		std::wstring category = {}, std::wstring enablement = {})
	{
		commands.Register({
			.id = std::move(id),
			.title = std::move(title),
			.category = std::move(category),
			.enablementClause = std::move(enablement),
			.extensionId = L"vendor.tool",
			.generation = 1,
		});
	}

	void Register(SExtensionContributions declared)
	{
		contributions.Register({ .extensionId = L"vendor.tool", .generation = 1 }, std::move(declared));
	}

	[[nodiscard]] std::vector<SProjectedMenuItem> Project(std::wstring_view location) const
	{
		return ProjectMenu(contributions, commands, contextKeys, location);
	}
};

/*!
	`ProjectMenu(contributions, WhenClauseEvaluator, CommandInfoLookup, location)` の追加オーバーロード用の足場。

	本番の呼び出し元 `CEditWnd::ProjectExtensionEditorTitleMenu` は `CExtensionCommandPalette`／
	`CExtensionContextKeys` を持たず、`CExtensionService` が公開する `EvaluateWhenClause`／
	`SearchCommands` だけから 2 つの関数オブジェクトを組み立てる。このフィクスチャもわざと同じ形にし、
	素の `std::map`／`std::set` だけで評価器とコマンド情報の引き当てを行う。`MenuFixture` とは
	独立させてあり、既存テストの `contributions` 組み立て規約（`Register`）だけを再利用する。
*/
struct GenericMenuFixture {
	CExtensionContributionRegistry						contributions;
	std::map<std::wstring, SProjectedMenuCommandInfo>	commandInfo;
	std::set<std::wstring>								trueWhenClauses;

	void DeclareCommand(std::wstring id, std::wstring label, bool enabled = true)
	{
		commandInfo.emplace(std::move(id), SProjectedMenuCommandInfo{ std::move(label), enabled });
	}

	void Register(SExtensionContributions declared)
	{
		contributions.Register({ .extensionId = L"vendor.tool", .generation = 1 }, std::move(declared));
	}

	//! 空節は真、というこのリポジトリ共通の規約をここで守る（呼び出し先の責務、ヘッダの doc comment参照）。
	[[nodiscard]] bool EvaluateWhenClause(std::wstring_view clause) const
	{
		return clause.empty() || trueWhenClauses.contains(std::wstring(clause));
	}

	[[nodiscard]] std::optional<SProjectedMenuCommandInfo> LookupCommand(std::wstring_view commandId) const
	{
		const auto found = commandInfo.find(std::wstring(commandId));
		if (found == commandInfo.end()) return std::nullopt;
		return found->second;
	}

	[[nodiscard]] std::vector<SProjectedMenuItem> Project(std::wstring_view location) const
	{
		const WhenClauseEvaluator evaluateWhenClause = [this](std::wstring_view clause) {
			return EvaluateWhenClause(clause);
		};
		const CommandInfoLookup lookupCommand = [this](std::wstring_view commandId) {
			return LookupCommand(commandId);
		};
		return ProjectMenu(contributions, evaluateWhenClause, lookupCommand, location);
	}
};

} // namespace

TEST(ExtensionMenuProjection, UsesTheCommandTitleAndItsCategoryAsTheLabel)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run", L"Vendor");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_EQ(L"Vendor: Run", projected[0].label);
	EXPECT_EQ(L"vendor.run", projected[0].commandId);
	EXPECT_TRUE(projected[0].enabled);
	EXPECT_FALSE(projected[0].separatorBefore);
}

/*!
	未登録のコマンドを指す項目は落とす。表示名が無いうえ、押しても何も起きない。
	描画側で拾わせると、面ごとに「空ラベルの項目」の扱いがばらつく。
*/
TEST(ExtensionMenuProjection, DropsItemsWhoseCommandWasNeverRegistered)
{
	MenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.missing" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

TEST(ExtensionMenuProjection, HidesItemsWhoseWhenClauseIsFalseAndShowsThemWhenItTurnsTrue)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.run", .whenClause = L"vendor.ready" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());

	fixture.contextKeys.Set(L"vendor.ready", true);
	EXPECT_EQ(1u, fixture.Project(kEditorTitle).size());
}

/*!
	`enablement` が偽でも項目は出す。VS Code は灰色で見せる。
	隠してしまうと「なぜ出ないのか」がユーザーから分からない。
*/
TEST(ExtensionMenuProjection, ShowsButDisablesAnItemWhoseEnablementIsFalse)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run", {}, L"vendor.hasTarget");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	ASSERT_EQ(1u, fixture.Project(kEditorTitle).size());
	EXPECT_FALSE(fixture.Project(kEditorTitle)[0].enabled);

	fixture.contextKeys.Set(L"vendor.hasTarget", true);
	EXPECT_TRUE(fixture.Project(kEditorTitle)[0].enabled);
}

TEST(ExtensionMenuProjection, MarksASeparatorWhereTheGroupChangesButNeverAtTheTop)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.a", L"A");
	fixture.DeclareCommand(L"vendor.b", L"B");
	fixture.DeclareCommand(L"vendor.c", L"C");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.a",
		.groupName = L"navigation", .groupOrder = 1 });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.b",
		.groupName = L"navigation", .groupOrder = 2 });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.c", .groupName = L"z_config" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(3u, projected.size());
	EXPECT_FALSE(projected[0].separatorBefore);
	EXPECT_FALSE(projected[1].separatorBefore);
	EXPECT_TRUE(projected[2].separatorBefore);
}

/*!
	`when` で消えた項目のぶんの区切り線を残さないこと。
	残すと、区切り線だけが 2 本並んだメニューになる。
*/
TEST(ExtensionMenuProjection, DoesNotLeaveASeparatorAtTheTopWhenTheFirstGroupIsHidden)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.a", L"A");
	fixture.DeclareCommand(L"vendor.c", L"C");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.a",
		.whenClause = L"never", .groupName = L"navigation" });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.c", .groupName = L"z_config" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_FALSE(projected[0].separatorBefore);
}

TEST(ExtensionMenuProjection, ExpandsASubmenuIntoItsChildren)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.child", L"Child");
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.more", .label = L"More" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.more" });
	declared.menuItems.push_back({ .location = L"vendor.more", .commandId = L"vendor.child" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_EQ(L"More", projected[0].label);
	EXPECT_EQ(L"vendor.more", projected[0].submenuId);
	EXPECT_TRUE(projected[0].commandId.empty());
	ASSERT_EQ(1u, projected[0].children.size());
	EXPECT_EQ(L"vendor.child", projected[0].children[0].commandId);
}

TEST(ExtensionMenuProjection, DropsASubmenuThatWouldOpenEmpty)
{
	MenuFixture fixture;
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.more", .label = L"More" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.more" });
	// 中身が指すコマンドは未登録なので、入れ子は空になる。
	declared.menuItems.push_back({ .location = L"vendor.more", .commandId = L"vendor.missing" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

TEST(ExtensionMenuProjection, DropsASubmenuReferenceThatWasNeverDeclared)
{
	MenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.undeclared" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

/*!
	入れ子が自分自身（や祖先）を指しても止まること。
	`contributes.submenus` はこれを禁じていないので、ホスト側で止めるしかない。
*/
TEST(ExtensionMenuProjection, TerminatesOnASubmenuThatReferencesItself)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.child", L"Child");
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.loop", .label = L"Loop" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.loop" });
	declared.menuItems.push_back({ .location = L"vendor.loop", .submenuId = L"vendor.loop" });
	declared.menuItems.push_back({ .location = L"vendor.loop", .commandId = L"vendor.child" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	// 自分自身への参照だけが落ち、実体のある子は残る。
	ASSERT_EQ(1u, projected[0].children.size());
	EXPECT_EQ(L"vendor.child", projected[0].children[0].commandId);
}

TEST(ExtensionMenuProjection, KeepsAnAlternateCommandOnlyWhenItActuallyExists)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	fixture.DeclareCommand(L"vendor.runAll", L"Run All");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.run", .altCommandId = L"vendor.runAll" });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.runAll", .altCommandId = L"vendor.ghost" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(2u, projected.size());
	EXPECT_EQ(L"vendor.runAll", projected[0].altCommandId);
	EXPECT_TRUE(projected[1].altCommandId.empty());
}

TEST(ExtensionMenuProjection, ReturnsNothingForAnUnknownLocation)
{
	MenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(L"explorer/context").empty());
	EXPECT_TRUE(fixture.Project(L"").empty());
}

/*!
	`commandPalette` は「出す／出さない」の上書き専用。
	宣言の無いコマンドまで隠すと、拡張のコマンドがほぼ全部消える。
*/
TEST(ExtensionMenuProjectionCommandPalette, ShowsCommandsThatDeclareNothing)
{
	MenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kCommandPalette), .commandId = L"vendor.declared", .whenClause = L"never" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(IsVisibleInCommandPalette(fixture.contributions, fixture.contextKeys, L"vendor.undeclared"));
	EXPECT_FALSE(IsVisibleInCommandPalette(fixture.contributions, fixture.contextKeys, L"vendor.declared"));
	EXPECT_FALSE(IsVisibleInCommandPalette(fixture.contributions, fixture.contextKeys, L""));
}

TEST(ExtensionMenuProjectionCommandPalette, ShowsACommandWhenAnyOfItsDeclarationsIsTrue)
{
	MenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kCommandPalette), .commandId = L"vendor.run", .whenClause = L"vendor.a" });
	declared.menuItems.push_back({
		.location = std::wstring(kCommandPalette), .commandId = L"vendor.run", .whenClause = L"vendor.b" });
	fixture.Register(std::move(declared));

	EXPECT_FALSE(IsVisibleInCommandPalette(fixture.contributions, fixture.contextKeys, L"vendor.run"));
	fixture.contextKeys.Set(L"vendor.b", true);
	EXPECT_TRUE(IsVisibleInCommandPalette(fixture.contributions, fixture.contextKeys, L"vendor.run"));
}

/*!
	`ProjectMenu` の追加オーバーロード（`WhenClauseEvaluator`／`CommandInfoLookup` 版）。

	`CEditWnd::ProjectExtensionEditorTitleMenu` が実運用で組み立てる形をそのまま模した
	`GenericMenuFixture` を使い、`MenuFixture` 版のテストが検証している規則
	（ラベル、未登録コマンドの除去、when の表示/非表示、enablement の灰色表示、
	group ごとの区切り線、入れ子メニューの展開/空削除/未宣言削除/循環終端、
	alt コマンドの存在チェック、未知の面）が、この呼び出し経路でも一字一句成り立つことを
	確かめる。`MenuFixture` 側の既存テストは変更していない。
*/
TEST(ExtensionMenuProjectionGeneric, UsesTheSuppliedCommandInfoLabelAndEnabledStateAsIs)
{
	GenericMenuFixture fixture;
	// 本番の CommandInfoLookup は「Category: Title」を既に結び終えた表示名しか渡してこないので、
	// ここでも結合済みの文字列をそのまま与える。
	fixture.DeclareCommand(L"vendor.run", L"Vendor: Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_EQ(L"Vendor: Run", projected[0].label);
	EXPECT_EQ(L"vendor.run", projected[0].commandId);
	EXPECT_TRUE(projected[0].enabled);
	EXPECT_FALSE(projected[0].separatorBefore);
}

TEST(ExtensionMenuProjectionGeneric, DropsItemsWhoseCommandInfoLookupReturnsNullopt)
{
	GenericMenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.missing" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

TEST(ExtensionMenuProjectionGeneric, HidesItemsWhoseWhenClauseIsFalseAndShowsThemWhenItTurnsTrue)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.run", .whenClause = L"vendor.ready" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());

	fixture.trueWhenClauses.insert(L"vendor.ready");
	EXPECT_EQ(1u, fixture.Project(kEditorTitle).size());
}

//! `enabled=false` を返す `CommandInfoLookup` でも項目は出す。灰色表示は描画側の責務。
TEST(ExtensionMenuProjectionGeneric, ShowsButDisablesAnItemWhoseCommandInfoIsDisabled)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run", /* enabled */ false);
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_FALSE(projected[0].enabled);
}

TEST(ExtensionMenuProjectionGeneric, MarksASeparatorWhereTheGroupChangesButNeverAtTheTop)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.a", L"A");
	fixture.DeclareCommand(L"vendor.b", L"B");
	fixture.DeclareCommand(L"vendor.c", L"C");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.a",
		.groupName = L"navigation", .groupOrder = 1 });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.b",
		.groupName = L"navigation", .groupOrder = 2 });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.c", .groupName = L"z_config" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(3u, projected.size());
	EXPECT_FALSE(projected[0].separatorBefore);
	EXPECT_FALSE(projected[1].separatorBefore);
	EXPECT_TRUE(projected[2].separatorBefore);
}

TEST(ExtensionMenuProjectionGeneric, ExpandsASubmenuIntoItsChildren)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.child", L"Child");
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.more", .label = L"More" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.more" });
	declared.menuItems.push_back({ .location = L"vendor.more", .commandId = L"vendor.child" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	EXPECT_EQ(L"More", projected[0].label);
	EXPECT_EQ(L"vendor.more", projected[0].submenuId);
	EXPECT_TRUE(projected[0].commandId.empty());
	ASSERT_EQ(1u, projected[0].children.size());
	EXPECT_EQ(L"vendor.child", projected[0].children[0].commandId);
}

TEST(ExtensionMenuProjectionGeneric, DropsASubmenuThatWouldOpenEmpty)
{
	GenericMenuFixture fixture;
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.more", .label = L"More" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.more" });
	declared.menuItems.push_back({ .location = L"vendor.more", .commandId = L"vendor.missing" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

TEST(ExtensionMenuProjectionGeneric, DropsASubmenuReferenceThatWasNeverDeclared)
{
	GenericMenuFixture fixture;
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.undeclared" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(kEditorTitle).empty());
}

TEST(ExtensionMenuProjectionGeneric, TerminatesOnASubmenuThatReferencesItself)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.child", L"Child");
	SExtensionContributions declared;
	declared.submenus.push_back({ .id = L"vendor.loop", .label = L"Loop" });
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .submenuId = L"vendor.loop" });
	declared.menuItems.push_back({ .location = L"vendor.loop", .submenuId = L"vendor.loop" });
	declared.menuItems.push_back({ .location = L"vendor.loop", .commandId = L"vendor.child" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(1u, projected.size());
	ASSERT_EQ(1u, projected[0].children.size());
	EXPECT_EQ(L"vendor.child", projected[0].children[0].commandId);
}

TEST(ExtensionMenuProjectionGeneric, KeepsAnAlternateCommandOnlyWhenItsInfoActuallyResolves)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	fixture.DeclareCommand(L"vendor.runAll", L"Run All");
	SExtensionContributions declared;
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.run", .altCommandId = L"vendor.runAll" });
	declared.menuItems.push_back({
		.location = std::wstring(kEditorTitle), .commandId = L"vendor.runAll", .altCommandId = L"vendor.ghost" });
	fixture.Register(std::move(declared));

	const auto projected = fixture.Project(kEditorTitle);
	ASSERT_EQ(2u, projected.size());
	EXPECT_EQ(L"vendor.runAll", projected[0].altCommandId);
	EXPECT_TRUE(projected[1].altCommandId.empty());
}

TEST(ExtensionMenuProjectionGeneric, ReturnsNothingForAnUnknownOrEmptyLocation)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	EXPECT_TRUE(fixture.Project(L"explorer/context").empty());
	EXPECT_TRUE(fixture.Project(L"").empty());
}

/*!
	`ProjectMenu(..., WhenClauseEvaluator{}, ...)` / `ProjectMenu(..., CommandInfoLookup{}, ...)` は
	既定構築（=空）の `std::function` を「まだ呼び出し元が組み立てられていない」の印として扱い、
	呼び出さずに空を返す。`CEditWnd::ProjectExtensionEditorTitleMenu` はこの経路には入らない
	（`m_extensionService` が無ければ `ProjectMenu` 自体を呼ばずに空を返す）が、この関数単体としての
	契約を独立に保証しておく。実装側のガードは `if (location.empty() || !evaluateWhenClause ||
	!lookupCommand) return {};`（`ExtensionMenuProjection.cpp`）。
*/
TEST(ExtensionMenuProjectionGeneric, ReturnsNothingWhenEitherCallableIsDefaultConstructed)
{
	GenericMenuFixture fixture;
	fixture.DeclareCommand(L"vendor.run", L"Run");
	SExtensionContributions declared;
	declared.menuItems.push_back({ .location = std::wstring(kEditorTitle), .commandId = L"vendor.run" });
	fixture.Register(std::move(declared));

	const WhenClauseEvaluator evaluateWhenClause = [](std::wstring_view) { return true; };
	const CommandInfoLookup lookupCommand = [&fixture](std::wstring_view commandId) {
		return fixture.LookupCommand(commandId);
	};

	EXPECT_TRUE(ProjectMenu(fixture.contributions, WhenClauseEvaluator{}, lookupCommand, kEditorTitle).empty());
	EXPECT_TRUE(ProjectMenu(fixture.contributions, evaluateWhenClause, CommandInfoLookup{}, kEditorTitle).empty());
	// 対照実験: 両方とも本物を渡せば、いつもどおり投影される。
	EXPECT_EQ(1u, ProjectMenu(fixture.contributions, evaluateWhenClause, lookupCommand, kEditorTitle).size());
}
