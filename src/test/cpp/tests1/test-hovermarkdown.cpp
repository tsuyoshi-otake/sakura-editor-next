/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/hover/HoverMarkdown.h"

#include <string>

// workbench::hover::Parse() は、拡張機能が StatusBarItem.tooltip に設定した
// MarkdownString を、CHoverWidget が GDI で描けるブロックモデルへ変換する純粋関数で
// ある（実 VS Code の vs/base/browser/markdownRenderer.ts:renderMarkdown 相当）。
// ウィンドウを一切必要としないため、直接呼び出して検証する。
//
// 構造そのものを見たい検査はブロック列を直接読み、テキストの落とし込みだけを見たい
// 検査は ToPlainText() を通す。ToPlainText() は描画には使われず、アクセシビリティ用の
// ラベルと、このテストのための平文投影である。

using namespace workbench::hover;

namespace {

[[nodiscard]] std::wstring PlainText(std::wstring_view markdown, bool supportThemeIcons = false)
{
	return ToPlainText(Parse(markdown, { .supportThemeIcons = supportThemeIcons }));
}

//! 表の 1 セルぶんの平文。
[[nodiscard]] std::wstring CellText(const STableRow& row, std::size_t column)
{
	if (column >= row.cells.size()) return {};
	std::wstring text;
	for (const SInlineRun& run : row.cells[column]) text.append(run.text);
	return text;
}

} // namespace

TEST(HoverMarkdownParse, ReturnsEmptyDocumentForEmptyInput)
{
	const SDocument document = Parse(L"");
	EXPECT_TRUE(document.empty());
	EXPECT_EQ(L"", ToPlainText(document));
}

TEST(HoverMarkdownParse, KeepsBoldItalicAndInlineCodeAsStyledRunsInsteadOfFlatteningThem)
{
	// 旧実装は装飾を捨てて平文にしていた。ホバーは書式付きで描くので、装飾は
	// 構造として残らなければならない。
	{
		const SDocument document = Parse(L"**bold**");
		ASSERT_EQ(1u, document.blocks.size());
		ASSERT_EQ(1u, document.blocks[0].lines.size());
		ASSERT_EQ(1u, document.blocks[0].lines[0].size());
		EXPECT_EQ(L"bold", document.blocks[0].lines[0][0].text);
		EXPECT_TRUE(document.blocks[0].lines[0][0].bold);
		EXPECT_FALSE(document.blocks[0].lines[0][0].italic);
	}
	{
		const SDocument document = Parse(L"_italic_");
		ASSERT_EQ(1u, document.blocks.size());
		ASSERT_EQ(1u, document.blocks[0].lines[0].size());
		EXPECT_TRUE(document.blocks[0].lines[0][0].italic);
	}
	{
		const SDocument document = Parse(L"`code`");
		ASSERT_EQ(1u, document.blocks.size());
		ASSERT_EQ(1u, document.blocks[0].lines[0].size());
		EXPECT_EQ(L"code", document.blocks[0].lines[0][0].text);
		EXPECT_TRUE(document.blocks[0].lines[0][0].code);
	}
	// 平文投影では装飾記号だけが落ちる。
	EXPECT_EQ(L"bold", PlainText(L"__bold__"));
	EXPECT_EQ(L"italic", PlainText(L"*italic*"));
}

TEST(HoverMarkdownParse, NestedEmphasisAppliesBothStyles)
{
	const SDocument document = Parse(L"**bold _and italic_**");
	ASSERT_EQ(1u, document.blocks.size());
	const SInlineText& line = document.blocks[0].lines[0];
	ASSERT_EQ(2u, line.size());
	EXPECT_TRUE(line[0].bold);
	EXPECT_FALSE(line[0].italic);
	EXPECT_TRUE(line[1].bold);
	EXPECT_TRUE(line[1].italic);
	EXPECT_EQ(L"and italic", line[1].text);
}

TEST(HoverMarkdownParse, ReducesLinksToTheirLabelAndMarksThemAsLinkRuns)
{
	const SDocument document = Parse(L"[Run](command:my.command \"My Title\")");
	ASSERT_EQ(1u, document.blocks.size());
	ASSERT_EQ(1u, document.blocks[0].lines[0].size());
	const SInlineRun& run = document.blocks[0].lines[0][0];
	// URI もタイトルの引用符付き文字列も描かない。ラベルだけがリンク色で残る。
	EXPECT_EQ(L"Run", run.text);
	EXPECT_TRUE(run.link);
	EXPECT_FALSE(run.linkTarget.empty());
	EXPECT_EQ(L"label", PlainText(L"[label](https://example.com)"));
}

TEST(HoverMarkdownParse, ThemeIconsAreResolvedOnlyWhenSupportThemeIconsIsSet)
{
	// 実 VS Code は MarkdownString.supportThemeIcons が真のときだけ $(name) を
	// アイコンとして描き、偽ならリテラルの文字として描く。この判定はワイヤーを
	// 通って届くようになったので、旧実装のように常に除去してはならない。
	{
		const SDocument document = Parse(L"$(rocket) Launch", { .supportThemeIcons = true });
		ASSERT_EQ(1u, document.blocks.size());
		const SInlineText& line = document.blocks[0].lines[0];
		ASSERT_EQ(2u, line.size());
		EXPECT_EQ(L"rocket", line[0].iconId);
		EXPECT_TRUE(line[0].text.empty());
		EXPECT_EQ(L" Launch", line[1].text);
	}
	{
		const SDocument document = Parse(L"$(rocket) Launch", { .supportThemeIcons = false });
		ASSERT_EQ(1u, document.blocks.size());
		const SInlineText& line = document.blocks[0].lines[0];
		ASSERT_EQ(1u, line.size());
		EXPECT_TRUE(line[0].iconId.empty());
		EXPECT_EQ(L"$(rocket) Launch", line[0].text);
	}
}

TEST(HoverMarkdownParse, DropsThemeIconModifierSuffixExactlyLikeThemeIconFromString)
{
	// VS Code の ThemeIcon.fromString は "loading~spin" の "~spin" をアイコン名から
	// 切り離す。アイコン ID は "loading" でなければならない。
	const SDocument document = Parse(L"$(loading~spin)", { .supportThemeIcons = true });
	ASSERT_EQ(1u, document.blocks.size());
	ASSERT_EQ(1u, document.blocks[0].lines[0].size());
	EXPECT_EQ(L"loading", document.blocks[0].lines[0][0].iconId);
}

TEST(HoverMarkdownParse, StripsInlineHtmlAndConvertsBrToALineBreak)
{
	EXPECT_EQ(L"Before After\nNext line",
		PlainText(L"Before<img src=\"x.png\"> After<br>Next line"));
}

TEST(HoverMarkdownParse, HorizontalRuleBecomesItsOwnBlockButNeverLeadsOrTrails)
{
	const SDocument document = Parse(L"Above\n\n---\n\nBelow");
	ASSERT_EQ(3u, document.blocks.size());
	EXPECT_EQ(EBlockKind::Paragraph, document.blocks[0].kind);
	EXPECT_EQ(EBlockKind::HorizontalRule, document.blocks[1].kind);
	EXPECT_EQ(EBlockKind::Paragraph, document.blocks[2].kind);
	EXPECT_EQ(L"Above\n\nBelow", ToPlainText(document));

	// 先頭・末尾の水平線は見た目の区切りにならないので落とす。
	const SDocument edges = Parse(L"---\nOnly\n---");
	ASSERT_EQ(1u, edges.blocks.size());
	EXPECT_EQ(EBlockKind::Paragraph, edges.blocks[0].kind);
}

TEST(HoverMarkdownParse, HeadingKeepsItsLevelAsStructure)
{
	const SDocument document = Parse(L"## Usage");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(EBlockKind::Heading, document.blocks[0].kind);
	EXPECT_EQ(2, document.blocks[0].level);
	EXPECT_EQ(L"Usage", ToPlainText(document));
}

TEST(HoverMarkdownParse, ListItemsKeepTheirMarkerAndNestingLevel)
{
	const SDocument document = Parse(L"- first\n  - nested\n1. numbered");
	ASSERT_EQ(3u, document.blocks.size());
	EXPECT_EQ(EBlockKind::ListItem, document.blocks[0].kind);
	EXPECT_EQ(L"\x2022", document.blocks[0].marker);
	EXPECT_EQ(0, document.blocks[0].level);
	EXPECT_EQ(1, document.blocks[1].level);
	EXPECT_EQ(L"1.", document.blocks[2].marker);
	EXPECT_EQ(L"\x2022 first\n\x2022 nested\n1. numbered", ToPlainText(document));
}

TEST(HoverMarkdownParse, FencedCodeBlockKeepsItsLinesVerbatimAsCodeRuns)
{
	const SDocument document = Parse(L"```js\nconst x = **1**;\n```");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(EBlockKind::CodeBlock, document.blocks[0].kind);
	ASSERT_EQ(1u, document.blocks[0].lines.size());
	ASSERT_EQ(1u, document.blocks[0].lines[0].size());
	// コードブロックの中は Markdown として解釈しない。
	EXPECT_EQ(L"const x = **1**;", document.blocks[0].lines[0][0].text);
	EXPECT_TRUE(document.blocks[0].lines[0][0].code);
}

// --- テーブル ---------------------------------------------------------------
// 旧実装はテーブルを "cell | cell" の 1 行へ潰していた。ホバーは列を保った格子として
// 描くので、列は構造として残らなければならない。

TEST(HoverMarkdownParse, TableKeepsRowsCellsHeaderFlagAndColumnAlignments)
{
	const SDocument document = Parse(
		L"| Name | Value |\n"
		L"|:-----|------:|\n"
		L"| foo | 1 |\n"
		L"| bar | 2 |");
	ASSERT_EQ(1u, document.blocks.size());
	const SBlock& table = document.blocks[0];
	EXPECT_EQ(EBlockKind::Table, table.kind);
	ASSERT_EQ(3u, table.rows.size());
	// 区切り行そのものは行として残さない。
	EXPECT_TRUE(table.rows[0].header);
	EXPECT_FALSE(table.rows[1].header);
	ASSERT_EQ(2u, table.rows[0].cells.size());
	EXPECT_EQ(L"Name", CellText(table.rows[0], 0));
	EXPECT_EQ(L"Value", CellText(table.rows[0], 1));
	EXPECT_EQ(L"bar", CellText(table.rows[2], 0));
	ASSERT_EQ(2u, table.alignments.size());
	EXPECT_EQ(EColumnAlign::Left, table.alignments[0]);
	EXPECT_EQ(EColumnAlign::Right, table.alignments[1]);
	EXPECT_EQ(L"Name | Value\nfoo | 1\nbar | 2", ToPlainText(document));
}

TEST(HoverMarkdownParse, TableCellKeepsAThemeIconAndItsLabelAsSeparateRuns)
{
	// odangoo.otak-usage の実際の形。ブランドアイコンはセル内の $(name) として届く。
	const SDocument document = Parse(
		L"| $(otak-claude) Claude Code |\n"
		L"|---|\n"
		L"| 5h |",
		{ .supportThemeIcons = true });
	ASSERT_EQ(1u, document.blocks.size());
	const SBlock& table = document.blocks[0];
	ASSERT_EQ(2u, table.rows.size());
	ASSERT_EQ(1u, table.rows[0].cells.size());
	const SInlineText& cell = table.rows[0].cells[0];
	ASSERT_EQ(2u, cell.size());
	EXPECT_EQ(L"otak-claude", cell[0].iconId);
	EXPECT_EQ(L" Claude Code", cell[1].text);
}

TEST(HoverMarkdownParse, LiteralDividerColumnSurvivesAsItsOwnColumn)
{
	// otak-usage は中央列にリテラルの "│" (U+2502) を置いて 2 つのブランドを区切る。
	// 実 VS Code の Markdown ホバーは表に罫線を引かないので、この文字自体が区切りに
	// 見える。列として残さなければ、その見た目は再現できない。
	const SDocument document = Parse(
		L"| Claude Code | \x2502 | Codex CLI |\n"
		L"|---|---|---|\n"
		L"| 5h | \x2502 | 4% |");
	ASSERT_EQ(1u, document.blocks.size());
	const SBlock& table = document.blocks[0];
	ASSERT_EQ(2u, table.rows.size());
	ASSERT_EQ(3u, table.rows[0].cells.size());
	EXPECT_EQ(L"\x2502", CellText(table.rows[0], 1));
	EXPECT_EQ(L"\x2502", CellText(table.rows[1], 1));
	EXPECT_EQ(3u, table.alignments.size());
}

TEST(HoverMarkdownParse, NbspOnlyTableCellCollapsesLikeAGenuinelyEmptyCell)
{
	// odangoo.otak-usage の out/formatter.js は、埋めるべき値の無い列へリテラルの
	// "&nbsp;" を流し込む（`column.total ?? '&nbsp;'`）。実 VS Code は tooltip を
	// HTML として描くので、これは不可視の非改行スペースになりセルは空欄に見える。
	const std::wstring nbspOnly = PlainText(L"| A | &nbsp; | B |\n|---|---|---|\n| a | &nbsp; | b |");
	const std::wstring genuinelyEmpty = PlainText(L"| A |  | B |\n|---|---|---|\n| a |  | b |");
	EXPECT_EQ(genuinelyEmpty, nbspOnly);
	EXPECT_EQ(L"A | B\na | b", nbspOnly);
	EXPECT_EQ(std::wstring::npos, nbspOnly.find(L"&nbsp;"));
}

TEST(HoverMarkdownParse, FlattensOtakUsagePlaceholderRowsWithoutStrayPipesOrLiteralNbsp)
{
	const std::wstring text = PlainText(
		L"| Claude Code | &nbsp; | Codex CLI |\n"
		L"|---|---|---|\n"
		L"| \x5236\x9650 (max) | &nbsp; | &nbsp; |");
	EXPECT_EQ(L"Claude Code | Codex CLI\n\x5236\x9650 (max)", text);
	EXPECT_EQ(std::wstring::npos, text.find(L"&nbsp;"));
	EXPECT_EQ(std::wstring::npos, text.find(L"| |"));
	EXPECT_EQ(std::wstring::npos, text.find(L"||"));
}

TEST(HoverMarkdownParse, EncodedPipeInsideTableCellDoesNotCreateAnExtraColumn)
{
	// セル内の "&#124;"（符号化されたパイプ）は、列分割より後にだけデコードされる。
	// 先にデコードすると 1 行目が 4 列へ分裂し、3 列の 2 行目と食い違う。
	const SDocument document = Parse(L"| A | one &#124; two | B |\n|---|---|---|\n| C | D | E |");
	ASSERT_EQ(1u, document.blocks.size());
	ASSERT_EQ(2u, document.blocks[0].rows.size());
	EXPECT_EQ(3u, document.blocks[0].rows[0].cells.size());
	EXPECT_EQ(3u, document.blocks[0].rows[1].cells.size());
	EXPECT_EQ(L"one | two", CellText(document.blocks[0].rows[0], 1));
}

TEST(HoverMarkdownParse, PipeShapedLinesWithoutADelimiterRowStayParagraphs)
{
	// 区切り行が無ければテーブルではない。パイプを含むだけの散文を格子にしない。
	const SDocument document = Parse(L"a | b\nc | d");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(EBlockKind::Paragraph, document.blocks[0].kind);
}

// --- HTML 実体参照 -----------------------------------------------------------

TEST(HoverMarkdownParse, DecodesNamedHtmlEntitiesToTheirLiteralCharacters)
{
	EXPECT_EQ(L"A & B < C > D \"E\"", PlainText(L"A &amp; B &lt; C &gt; D &quot;E&quot;"));
}

TEST(HoverMarkdownParse, DecodesNumericAndNonBmpHtmlEntityReferences)
{
	EXPECT_EQ(L"A", PlainText(L"&#65;"));
	EXPECT_EQ(L"A", PlainText(L"&#x41;"));
	// U+1F600 (GRINNING FACE) は BMP 外なので、正しい UTF-16 サロゲートペアへ
	// エンコードされる必要がある。
	const std::wstring emoji = PlainText(L"&#128512;");
	ASSERT_EQ(2u, emoji.size());
	EXPECT_EQ(static_cast<wchar_t>(0xD83D), emoji[0]);
	EXPECT_EQ(static_cast<wchar_t>(0xDE00), emoji[1]);
}

TEST(HoverMarkdownParse, LeavesUnknownOrMalformedEntityReferencesLiteral)
{
	EXPECT_EQ(L"&bogus;", PlainText(L"&bogus;"));
	EXPECT_EQ(L"A & B", PlainText(L"A & B"));
	EXPECT_EQ(L"&#99999999;", PlainText(L"&#99999999;"));
}

// --- 信頼できない入力に対する境界 --------------------------------------------

TEST(HoverMarkdownParse, LeavesUnterminatedMarkupAsLiteralTextWithoutThrowing)
{
	// 閉じの "**" が無い。未終端の記法は解釈せず、開始記号を文字として残す。
	// 残りの文字列を飲み込んではならない。
	EXPECT_EQ(L"**bold", PlainText(L"**bold"));
	EXPECT_EQ(L"[label](unclosed", PlainText(L"[label](unclosed"));
}

TEST(HoverMarkdownParse, ReducesAStringOfOnlyMarkupWithoutThrowing)
{
	EXPECT_EQ(L"$(a)$(b)$(c)", PlainText(L"$(a)$(b)$(c)"));
	EXPECT_EQ(L"", PlainText(L"$(a)$(b)$(c)", /*supportThemeIcons*/ true));
}

TEST(HoverMarkdownParse, BoundsDisplayedCharactersWithAVisibleEllipsisBlock)
{
	// 実装の表示予算（4096 文字）ちょうどでは切り詰めない一方、それを超える入力は
	// 可視の "..." ブロックを足して打ち切る。黙って切ってはならない。
	constexpr std::size_t kDisplayBudget = 4096;

	const std::wstring exactBudget(kDisplayBudget, L'B');
	const SDocument fits = Parse(exactBudget);
	ASSERT_EQ(1u, fits.blocks.size());
	EXPECT_EQ(exactBudget, ToPlainText(fits));

	const std::wstring overBudget(kDisplayBudget + 200, L'A');
	const SDocument truncated = Parse(overBudget);
	ASSERT_EQ(2u, truncated.blocks.size());
	EXPECT_EQ(std::wstring(kDisplayBudget, L'A') + L"\n...", ToPlainText(truncated));
}

TEST(HoverMarkdownParse, DeeplyNestedEmphasisTerminatesWithoutUnboundedRecursion)
{
	// 敵対的な入力。ネスト深度は定数で上限を持つので、返ってくること自体が検証対象。
	std::wstring nested;
	for (int depth = 0; depth < 200; ++depth) nested.append(L"*");
	nested.append(L"x");
	for (int depth = 0; depth < 200; ++depth) nested.append(L"*");
	const SDocument document = Parse(nested);
	EXPECT_FALSE(document.empty());
}
