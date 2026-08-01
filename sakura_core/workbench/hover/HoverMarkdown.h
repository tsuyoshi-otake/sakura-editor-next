/*! @file
	@brief ホバー用 Markdown ブロックモデル（VS Code の markdownRenderer 相当）

	VS Code は `vscode.MarkdownString` を `vs/base/browser/markdownRenderer.ts` の
	`renderMarkdown()` で DOM へ変換し、`HoverWidget` の中に本物の書式付き内容として
	描画する。このモジュールはその「Markdown からレンダリング可能な構造へ」の段を
	ウィンドウ非依存の純粋関数として持つ。GDI 描画は CHoverWidget が受け持つ。

	平文へ落とし込む実装（旧 CMainStatusBar::ExtensionTooltipPlainText）の置き換え
	であり、テーブルは列を保ったまま、インライン装飾は装飾のまま構造に残す。
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::hover {

//! 1 つのインライン実行単位。装飾が変わるたびに区切られる。
struct SInlineRun {
	//! 表示テキスト。HTML 実体参照と Markdown のバックスラッシュエスケープは解決済み。
	std::wstring text;
	//! 非空なら `$(name)` 由来のテーマアイコン。このとき text は空。
	std::wstring iconId;
	bool bold = false;
	bool italic = false;
	//! インラインコード（`` `code` ``）。等幅フォント + 背景で描画される。
	bool code = false;
	//! `[label](target)` のラベル。リンク色で描画される。
	bool link = false;

	[[nodiscard]] bool operator==(const SInlineRun&) const = default;
};

//! 1 行分（あるいは 1 セル分）のインライン列。
using SInlineText = std::vector<SInlineRun>;

enum class EBlockKind : std::uint8_t {
	Paragraph,
	Heading,
	HorizontalRule,
	Table,
	CodeBlock,
	ListItem,
};

//! テーブルの区切り行（`| :--- | :---: | ---: |`）が持つ列ごとの寄せ。
enum class EColumnAlign : std::uint8_t {
	Left,
	Center,
	Right,
};

struct STableRow {
	std::vector<SInlineText> cells;
	//! 区切り行より前の行、すなわち見出し行。
	bool header = false;

	[[nodiscard]] bool operator==(const STableRow&) const = default;
};

struct SBlock {
	EBlockKind kind = EBlockKind::Paragraph;
	//! Heading では見出しレベル (1-6)、ListItem ではネストの深さ (0 起点)。
	int level = 0;
	//! ListItem の行頭マーカー（"•" または "1."）。他の種別では空。
	std::wstring marker;
	//! Paragraph / Heading / CodeBlock / ListItem の本文。1 要素が 1 行。
	std::vector<SInlineText> lines;
	//! Table の行。
	std::vector<STableRow> rows;
	//! Table の列ごとの寄せ。要素数は最大列数と一致する。
	std::vector<EColumnAlign> alignments;

	[[nodiscard]] bool operator==(const SBlock&) const = default;
};

struct SDocument {
	std::vector<SBlock> blocks;

	[[nodiscard]] bool empty() const noexcept { return blocks.empty(); }
	[[nodiscard]] bool operator==(const SDocument&) const = default;
};

struct SParseOptions {
	/*!
		`vscode.MarkdownString.supportThemeIcons` と同じ意味。

		真のときだけ `$(name)` をテーマアイコンとして解釈する。偽のときは
		VS Code と同じくリテラルの文字列として扱う。この値はワイヤー
		（`serializeThemeValue`）から `SExtensionStatusBarItem::tooltipSupportsThemeIcons`
		を経由して渡される。
	*/
	bool supportThemeIcons = false;
};

/*!
	@brief 拡張機能が渡した Markdown を、描画可能なブロック列へ変換する

	信頼できない入力に対して例外を投げず、未終端の記法があっても残りの文字列を
	飲み込まない。入力長・ブロック数・テーブルの行/列数・インラインの先読み幅と
	ネスト深度はいずれも定数で上限を持ち、上限に達した場合は可視の "..." を末尾に
	置いて打ち切る（黙って切らない）。

	@param [in] markdown 拡張機能が設定した Markdown 原文
	@param [in] options 解釈オプション
	@return 描画可能なブロック列
*/
[[nodiscard]] SDocument Parse(std::wstring_view markdown, const SParseOptions& options = {});

/*!
	@brief 解析結果の平文投影

	描画には使わない。アクセシビリティ用のラベルと、構造を言葉で検証するテストの
	ために、ブロック構造を素直な行へ落とす。テーブルはセルを " | " で連結する。
*/
[[nodiscard]] std::wstring ToPlainText(const SDocument& document);

} // namespace workbench::hover
