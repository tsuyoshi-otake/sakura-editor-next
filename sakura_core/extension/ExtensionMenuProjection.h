/*! @file
	@brief `contributes.menus` を、描画側がそのまま並べられる形へ畳む
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CExtensionCommandPalette;
class CExtensionContextKeys;
class CExtensionContributionRegistry;

namespace extension::menus {

/*!
	VS Code のメニュー面の名前。文字列を各描画箇所に直書きすると、
	綴りがずれた面が黙って空になる（誰も気付けない壊れ方をする）。
*/
inline constexpr std::wstring_view kEditorTitle = L"editor/title";
inline constexpr std::wstring_view kEditorContext = L"editor/context";
inline constexpr std::wstring_view kExplorerContext = L"explorer/context";
inline constexpr std::wstring_view kCommandPalette = L"commandPalette";

//! 入れ子メニューの深さ上限。循環参照は別途検出するが、深すぎる入れ子も操作できない。
inline constexpr std::size_t kMaxSubmenuDepth = 4;

/*!
	@brief 投影済みのメニュー 1 項目

	描画側は `when` も `group@order` も解釈しない。ここまで畳んだ結果だけを見る。
	そうしないと「メニューによって when の解釈が違う」という直しようのない差が生まれる。
*/
struct SProjectedMenuItem {
	std::wstring					commandId;			//!< 入れ子見出しのときは空
	std::wstring					altCommandId;		//!< Alt 押下時に差し替わるコマンド。無ければ空
	std::wstring					label;
	std::wstring					submenuId;			//!< 入れ子見出しのときのみ非空
	bool							enabled = true;		//!< `enablement` の評価結果。灰色表示に使う
	bool							separatorBefore = false;	//!< 直前の項目と group が変わった
	std::vector<SProjectedMenuItem>	children;			//!< 入れ子見出しの中身
};

/*!
	@brief 1 つのメニュー面を投影する

	`when` が偽の項目、コマンドが未登録の項目、中身が空になった入れ子は落とす。
	落とすのは描画時ではなくここ。空の項目を並べてから消すと、区切り線だけが残る。
*/
[[nodiscard]] std::vector<SProjectedMenuItem> ProjectMenu(
	const CExtensionContributionRegistry&	contributions,
	const CExtensionCommandPalette&			commands,
	const CExtensionContextKeys&			contextKeys,
	std::wstring_view						location);

/*!
	@brief `ProjectMenu` がコマンド 1 件について描画側に返す最小限の情報

	`title`/`category` を結んだ表示名と、`enablement` を既に評価し終えた可否だけを運ぶ。
	投影そのものは呼び出し元の具象コマンドストアを知らないため、これ以上は持たない。
*/
struct SProjectedMenuCommandInfo {
	std::wstring	label;
	bool			enabled = true;
};

//! `item.whenClause` を評価する。空節は真、というこのリポジトリ共通の規約は呼び出し先が守る。
using WhenClauseEvaluator = std::function<bool(std::wstring_view)>;
//! `commandId` からコマンド情報を引く。未登録なら `nullopt`。
using CommandInfoLookup = std::function<std::optional<SProjectedMenuCommandInfo>(std::wstring_view)>;

/*!
	@brief `ProjectMenu` の追加オーバーロード。具象の `CExtensionCommandPalette`／`CExtensionContextKeys`
	を持たない呼び出し元向けに、同じ投影規則を関数オブジェクト越しに提供する。

	`CExtensionService`（`sakura_core/extension/CExtensionService.h`）は自身が内包する
	`CExtensionCommandPalette`/`CExtensionContextKeys` を外部へ公開しない。実運用の `CEditWnd` は
	この既存の公開 API（`EvaluateWhenClause`、`SearchCommands`）だけから `evaluateWhenClause`／
	`lookupCommand` を組み立てて、この面を計算する。上の元のオーバーロードとは完全に独立しており、
	既存のテストが検証している挙動・シグネチャには一切手を入れていない。
*/
[[nodiscard]] std::vector<SProjectedMenuItem> ProjectMenu(
	const CExtensionContributionRegistry&	contributions,
	const WhenClauseEvaluator&				evaluateWhenClause,
	const CommandInfoLookup&				lookupCommand,
	std::wstring_view						location);

/*!
	@brief `commandPalette` 面によるコマンド 1 件の可視性

	VS Code では `contributes.menus.commandPalette` は「パレットに出す／出さない」の
	上書きにだけ使われ、宣言が無いコマンドは既定で出る。宣言があるのに `when` が偽の
	ときだけ隠す。ここを「宣言のあるものだけ出す」と読むと、大半のコマンドが消える。
*/
[[nodiscard]] bool IsVisibleInCommandPalette(
	const CExtensionContributionRegistry&	contributions,
	const CExtensionContextKeys&			contextKeys,
	std::wstring_view						commandId);

} // namespace extension::menus
