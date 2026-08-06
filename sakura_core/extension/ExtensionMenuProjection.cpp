/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "extension/ExtensionMenuProjection.h"

#include "extension/CExtensionCommandPalette.h"
#include "extension/CExtensionContextKeys.h"
#include "extension/CExtensionContributionRegistry.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace extension::menus {

namespace {

//! コマンドの表示名。`category` があれば VS Code と同じ `Category: Title` に組む。
[[nodiscard]] std::wstring LabelOf(const SExtensionCommandDescriptor& command)
{
	return command.category.empty() ? command.title : command.category + L": " + command.title;
}

/*!
	@brief 1 面を投影する本体

	`visitedSubmenus` は循環参照よけ。`contributes.submenus` は自分自身や
	祖先を指せてしまうので、これが無いと depth 上限に当たるまで同じ枝を掘る。
*/
[[nodiscard]] std::vector<SProjectedMenuItem> ProjectLocation(
	const CExtensionContributionRegistry& contributions,
	const CExtensionCommandPalette& commands,
	const CExtensionContextKeys& contextKeys,
	const std::vector<SExtensionSubmenuDescriptor>& submenus,
	const std::wstring_view location,
	std::vector<std::wstring>& visitedSubmenus,
	const std::size_t depth)
{
	std::vector<SProjectedMenuItem> projected;
	if (depth > kMaxSubmenuDepth) return projected;

	const auto declared = contributions.MenuItems(location);
	// 区切り線は「group が変わったところ」に入る。最初の group の前には入れない。
	std::wstring previousGroup;
	bool anyEmitted = false;
	for (const auto& item : declared) {
		if (!contextKeys.Evaluate(item.whenClause)) continue;

		SProjectedMenuItem entry;
		if (!item.submenuId.empty()) {
			if (std::ranges::find(visitedSubmenus, item.submenuId) != visitedSubmenus.end()) continue;
			const auto descriptor = std::ranges::find(submenus, item.submenuId, &SExtensionSubmenuDescriptor::id);
			if (descriptor == submenus.end()) continue;

			visitedSubmenus.push_back(item.submenuId);
			auto children = ProjectLocation(
				contributions, commands, contextKeys, submenus, item.submenuId, visitedSubmenus, depth + 1);
			visitedSubmenus.pop_back();
			// 中身が空になった入れ子は見出しごと落とす。開いて何も無いのは壊れて見える。
			if (children.empty()) continue;

			entry.submenuId = item.submenuId;
			entry.label = descriptor->label.empty() ? descriptor->id : descriptor->label;
			entry.children = std::move(children);
		} else {
			// 未登録のコマンドを指す項目は、押しても何も起きないうえ表示名も無い。
			const auto command = commands.Find(item.commandId);
			if (!command) continue;
			entry.commandId = item.commandId;
			entry.altCommandId = commands.Contains(item.altCommandId) ? item.altCommandId : std::wstring();
			entry.label = LabelOf(*command);
			entry.enabled = contextKeys.Evaluate(command->enablementClause);
		}

		entry.separatorBefore = anyEmitted && item.groupName != previousGroup;
		previousGroup = item.groupName;
		anyEmitted = true;
		projected.push_back(std::move(entry));
	}
	return projected;
}

/*!
	@brief `ProjectLocation` と同じ規則を、具象の `CExtensionCommandPalette`／`CExtensionContextKeys`
	ではなく関数オブジェクトに対して適用する版。

	差分は評価手段だけ: `when` は `evaluateWhenClause`、コマンドの存在と表示名/可否は
	`lookupCommand` に問う。区切り線・循環参照・深さ上限・空入れ子の扱いは一字一句そろえてあり、
	わざと共通化していない（テンプレート化して既存 `ProjectLocation` に波及させるより、
	既存テストへのリスクをゼロに保つほうを優先した）。
*/
[[nodiscard]] std::vector<SProjectedMenuItem> ProjectLocationGeneric(
	const CExtensionContributionRegistry& contributions,
	const WhenClauseEvaluator& evaluateWhenClause,
	const CommandInfoLookup& lookupCommand,
	const std::vector<SExtensionSubmenuDescriptor>& submenus,
	const std::wstring_view location,
	std::vector<std::wstring>& visitedSubmenus,
	const std::size_t depth)
{
	std::vector<SProjectedMenuItem> projected;
	if (depth > kMaxSubmenuDepth) return projected;

	const auto declared = contributions.MenuItems(location);
	std::wstring previousGroup;
	bool anyEmitted = false;
	for (const auto& item : declared) {
		if (!evaluateWhenClause(item.whenClause)) continue;

		SProjectedMenuItem entry;
		if (!item.submenuId.empty()) {
			if (std::ranges::find(visitedSubmenus, item.submenuId) != visitedSubmenus.end()) continue;
			const auto descriptor = std::ranges::find(submenus, item.submenuId, &SExtensionSubmenuDescriptor::id);
			if (descriptor == submenus.end()) continue;

			visitedSubmenus.push_back(item.submenuId);
			auto children = ProjectLocationGeneric(
				contributions, evaluateWhenClause, lookupCommand, submenus, item.submenuId, visitedSubmenus, depth + 1);
			visitedSubmenus.pop_back();
			if (children.empty()) continue;

			entry.submenuId = item.submenuId;
			entry.label = descriptor->label.empty() ? descriptor->id : descriptor->label;
			entry.children = std::move(children);
		} else {
			const auto command = lookupCommand(item.commandId);
			if (!command) continue;
			entry.commandId = item.commandId;
			entry.altCommandId = (!item.altCommandId.empty() && lookupCommand(item.altCommandId))
				? item.altCommandId : std::wstring();
			entry.label = command->label;
			entry.enabled = command->enabled;
		}

		entry.separatorBefore = anyEmitted && item.groupName != previousGroup;
		previousGroup = item.groupName;
		anyEmitted = true;
		projected.push_back(std::move(entry));
	}
	return projected;
}

} // namespace

std::vector<SProjectedMenuItem> ProjectMenu(
	const CExtensionContributionRegistry& contributions,
	const CExtensionCommandPalette& commands,
	const CExtensionContextKeys& contextKeys,
	const std::wstring_view location)
{
	if (location.empty()) return {};
	const auto submenus = contributions.Submenus();
	std::vector<std::wstring> visitedSubmenus;
	return ProjectLocation(contributions, commands, contextKeys, submenus, location, visitedSubmenus, 0);
}

std::vector<SProjectedMenuItem> ProjectMenu(
	const CExtensionContributionRegistry& contributions,
	const WhenClauseEvaluator& evaluateWhenClause,
	const CommandInfoLookup& lookupCommand,
	const std::wstring_view location)
{
	if (location.empty() || !evaluateWhenClause || !lookupCommand) return {};
	const auto submenus = contributions.Submenus();
	std::vector<std::wstring> visitedSubmenus;
	return ProjectLocationGeneric(contributions, evaluateWhenClause, lookupCommand, submenus, location, visitedSubmenus, 0);
}

bool IsVisibleInCommandPalette(
	const CExtensionContributionRegistry& contributions,
	const CExtensionContextKeys& contextKeys,
	const std::wstring_view commandId)
{
	if (commandId.empty()) return false;
	bool declared = false;
	for (const auto& item : contributions.MenuItems(kCommandPalette)) {
		if (item.commandId != commandId) continue;
		declared = true;
		// 同じコマンドに複数の宣言があるときは、どれか 1 本でも真なら出す。
		if (contextKeys.Evaluate(item.whenClause)) return true;
	}
	return !declared;
}

} // namespace extension::menus
