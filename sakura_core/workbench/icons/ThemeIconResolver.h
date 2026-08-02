/*! @file
	@brief `$(icon-id)` を描画方法へ解決する単一の窓口

	実 VS Code の `ThemeIcon.fromString("$(id)")` は、拡張が `contributes.icons` で
	寄与したアイコンも組み込み codicon も、単一のグローバルな `IconRegistry` から
	id だけで引く。ステータスバーの `StatusBarItem.text` と、ホバーが描く
	`MarkdownString`（supportThemeIcons）は同じ記法・同じ解決順でなければならない
	ので、その語彙をここ 1 か所に置く。

	組み込み codicon の語彙（CodiconGlyphTable.h = 同梱 codicon.ttf の全 746 名）と
	拡張寄与の語彙（CExtensionIconFontRegistry）は別々の所有者のままにし、この
	ヘッダーは両者を上から順に引くだけにとどめる。

	このヘッダーは純粋に保つ。同梱フォントの所有者（CCodiconFont）は include せず、
	登録済みの書体名を引数で受け取るだけにする。書体名が空なら同梱フォントは
	使えないという意味で、取り込み済みベクター（CodiconsActivityIcons.h）へ落ちる。
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/IconMetrics.h"
#include "workbench/icons/CExtensionIconFont.h"
#include "workbench/icons/CodiconGlyphTable.h"
#include "workbench/icons/CodiconsActivityIcons.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::icons {

//! `$(icon-id)` 1 個の解決結果。3 つの描き方のいずれか 1 つだけが有効になる。
struct SResolvedThemeIcon {
	/*!
		@brief フォントのグリフとして描くアイコン。真なら fontIcon を使う。

		拡張の contributes.icons も、同梱 codicon.ttf の組み込みアイコンも、
		どちらも「登録済みの書体名 + 1 文字」というこの同じ形になる。実 VS Code が
		両者を同じ font-family/content の仕組みで描くのと同じで、描画側に 2 通りの
		経路を持たせない。
	*/
	bool font = false;
	SExtensionContributedIcon fontIcon;
	//! font が真のとき、そのグリフが拡張の contributes.icons 由来なら真。
	bool contributed = false;
	//! `$(extensions)`。同梱フォントが使えないときだけの 2x2 タイル合成アイコン。
	//! 取り込み済みベクター codicon。同梱フォントが使えないときの最後の砦。
	codicons::Icon builtin = codicons::Icon::RecordSmall;
};

/*!
	@brief 組み込み codicon の名前を、取り込み済みのベクターアイコンへ写す

	同梱 codicon.ttf が登録できなかったときだけ通る縮退経路である。通常は
	CodiconGlyphTable.h の 746 名がフォントのグリフとして解決されるので、ここへは
	来ない。未取り込みの名前は代替の点（RecordSmall）へ落ちる。リテラルの
	"$(name)" を漏らすよりは安定した代替マーカーのほうがよい、という既存の方針を
	そのまま引き継ぐ。
*/
[[nodiscard]] inline codicons::Icon BuiltinCodiconFor(std::wstring_view name) noexcept
{
	using Icon = codicons::Icon;
	if (name == L"git-branch") return Icon::GitBranch;
	if (name == L"source-control") return Icon::SourceControl;
	if (name == L"account") return Icon::Account;
	if (name == L"gear" || name == L"settings") return Icon::Gear;
	if (name == L"target") return Icon::Target;
	if (name == L"newline") return Icon::Newline;
	if (name == L"code") return Icon::Code;
	if (name == L"file-binary") return Icon::FileBinary;
	if (name == L"record-small" || name == L"circle-filled") return Icon::RecordSmall;
	if (name == L"insert") return Icon::Insert;
	if (name == L"zoom-in") return Icon::ZoomIn;
	if (name == L"file") return Icon::File;
	if (name == L"open-preview") return Icon::OpenPreview;
	if (name == L"chevron-down") return Icon::ChevronDown;
	if (name == L"chevron-right") return Icon::ChevronRight;
	if (name == L"extensions") return Icon::Extensions;
	if (name == L"warning") return Icon::Warning;
	if (name == L"error") return Icon::Error;
	if (name == L"info") return Icon::Info;
	if (name == L"close") return Icon::Close;
	if (name == L"close-all") return Icon::CloseAll;
	if (name == L"loading") return Icon::Loading;
	return Icon::RecordSmall;
}

/*!
	@brief `$(icon-id)` の id を描画方法へ解決する

	実 VS Code の IconRegistry と同じく、寄与アイコンは拡張をまたいだ 1 つの id 空間に
	入る。よってまず寄与アイコンを id だけで引き、無ければ組み込みへ落とす。これを
	拡張ごとの解決へ「直して」はならない。ある拡張が別の拡張の寄与した id を使うのは
	上流仕様どおりの正当な状態である。

	@param builtinFontFaceName 登録済みの同梱 codicon.ttf の書体名
	       （workbench::icons::CCodiconFont::Instance().FaceName()）。空を渡すと
	       組み込みアイコンは取り込み済みベクターへ落ちる。

	@note レジストリ検索は文字列を確保し得るので noexcept にはできない。
*/
[[nodiscard]] inline SResolvedThemeIcon ResolveThemeIcon(
	std::wstring_view iconId,
	const CExtensionIconFontRegistry* contributedIcons,
	std::wstring_view builtinFontFaceName = {})
{
	SResolvedThemeIcon resolved;
	if (contributedIcons != nullptr) {
		if (auto found = contributedIcons->Find(iconId); found.has_value()) {
			resolved.fontIcon = std::move(*found);
			resolved.font = true;
			resolved.contributed = true;
			return resolved;
		}
	}
	// 実 VS Code と同じく、組み込みアイコンは codicon.ttf の 1 グリフとして描く。
	// 取り込み済みベクターの有無で名前ごとに描き方が変わってはならない。
	if (!builtinFontFaceName.empty()) {
		if (const auto glyph = FindCodiconGlyph(iconId); glyph.has_value()) {
			resolved.fontIcon.faceName.assign(builtinFontFaceName);
			resolved.fontIcon.glyph.assign(1, *glyph);
			resolved.font = true;
			return resolved;
		}
	}
	resolved.builtin = BuiltinCodiconFor(iconId);
	return resolved;
}

//! `renderLabelWithIcons` が返す 1 断片。icon が真ならアイコン、偽ならテキスト。
struct SLabelRun {
	//! 真なら resolved を描く。偽なら text を描く。
	bool icon = false;
	//! アイコン断片では空。テキスト断片では描画する文字列。
	std::wstring text;
	//! テキスト断片では未使用。
	SResolvedThemeIcon resolved;
	//! `$(loading~spin)` の `spin` 等。modifier が無ければ空。
	std::wstring modifier;
};

//! VS Code の `ThemeIcon.iconNameExpression`（`[A-Za-z0-9-]+`）。
[[nodiscard]] inline constexpr bool IsIconNameChar(wchar_t ch) noexcept
{
	return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')
		|| (ch >= L'0' && ch <= L'9') || ch == L'-';
}

//! VS Code の `ThemeIcon.iconModifierExpression`（`~[A-Za-z]+`）の本体部。
[[nodiscard]] inline constexpr bool IsIconModifierChar(wchar_t ch) noexcept
{
	return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

/*!
	@brief ラベル中の `$(name)` をすべてインライン断片へ分解する

	実 VS Code の `vs/base/browser/ui/iconLabel/iconLabels.ts` の
	`renderLabelWithIcons` と同じ規則をそのまま写す。

	- 正規表現は `(\\)?\$\(([A-Za-z0-9-]+(?:~[A-Za-z]+)?)\)`。
	- 直前の `\` はエスケープで、`$(name)` をリテラル文字として出す。
	- 名前が上の字種に合わないものはアイコンではなく、ただの文字として残る。
	- 一致は**位置そのまま**で、前後のテキストと交互に並ぶ。先頭 1 個だけを
	  特別扱いしてはならない。`StatusBarItem.text` は上流でも複数アイコンを
	  取り、`$(otak-claude) 46% $(otak-openai) 100%` はその代表例である。

	@note modifier（`~spin` 等）はアイコン id から切り離して別に返す。id 解決には
	      使わないが、呼び出し側がアニメーションの有無を判断できるようにする。
*/
[[nodiscard]] inline std::vector<SLabelRun> ParseLabelWithIcons(
	std::wstring_view label,
	const CExtensionIconFontRegistry* contributedIcons,
	std::wstring_view builtinFontFaceName = {})
{
	std::vector<SLabelRun> runs;
	std::wstring literal;
	const auto flushLiteral = [&runs, &literal]() {
		if (literal.empty()) return;
		runs.push_back(SLabelRun{ .icon = false, .text = std::move(literal) });
		literal.clear();
	};

	std::size_t cursor = 0;
	while (cursor < label.size()) {
		const bool escaped = label[cursor] == L'\\'
			&& cursor + 2 < label.size() && label[cursor + 1] == L'$' && label[cursor + 2] == L'(';
		const std::size_t tokenStart = escaped ? cursor + 1 : cursor;
		if (label[tokenStart] != L'$' || tokenStart + 1 >= label.size() || label[tokenStart + 1] != L'(') {
			literal.push_back(label[cursor]);
			++cursor;
			continue;
		}

		std::size_t scan = tokenStart + 2;
		const std::size_t nameStart = scan;
		while (scan < label.size() && IsIconNameChar(label[scan])) ++scan;
		const std::wstring_view name = label.substr(nameStart, scan - nameStart);
		std::wstring_view modifier;
		if (!name.empty() && scan < label.size() && label[scan] == L'~') {
			const std::size_t modifierStart = scan + 1;
			std::size_t modifierScan = modifierStart;
			while (modifierScan < label.size() && IsIconModifierChar(label[modifierScan])) ++modifierScan;
			if (modifierScan > modifierStart) {
				modifier = label.substr(modifierStart, modifierScan - modifierStart);
				scan = modifierScan;
			}
		}
		if (name.empty() || scan >= label.size() || label[scan] != L')') {
			// 一致しないものは記法ではない。1 文字だけ進めて素のテキストへ落とす。
			literal.push_back(label[cursor]);
			++cursor;
			continue;
		}

		if (escaped) {
			literal.append(label.substr(tokenStart, scan + 1 - tokenStart));
		} else {
			flushLiteral();
			SLabelRun run;
			run.icon = true;
			run.resolved = ResolveThemeIcon(name, contributedIcons, builtinFontFaceName);
			run.modifier.assign(modifier);
			runs.push_back(std::move(run));
		}
		cursor = scan + 1;
	}
	flushLiteral();
	return runs;
}

//! `$(extensions)` の 2x2 タイル。ベクターパスを持たないので矩形 4 枚で描く。
} // namespace workbench::icons
