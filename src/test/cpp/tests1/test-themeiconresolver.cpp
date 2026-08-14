/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "workbench/icons/ThemeIconResolver.h"

#include <string>

// workbench::icons::ParseLabelWithIcons() は、ワークベンチのラベルを
// インラインに描ける断片列へ分解する純粋関数である
// （実 VS Code の vs/base/browser/ui/iconLabel/iconLabels.ts:renderLabelWithIcons 相当）。
//
// 既定の Runs() は同梱フォントの書体名を渡さないので、
// アイコン断片は取り込み済みベクター（CodiconsActivityIcons.h）へ縮退する。ここで
// 見たいのは「どこがアイコンで、どこがテキストか」という分解そのものであって、
// グリフの選択ではない。同梱フォントの実登録は test-ccodiconfont.cpp が担当する。
//
// 同梱 codicon.ttf が使える通常経路は FontRuns() で確かめる。GDI へ触らずに済むよう
// 書体名はダミーで、見たいのは「名前が表に載っていればフォントのグリフになる」こと。

using namespace workbench::icons;

namespace {

//! 同梱フォントが登録できなかった縮退状態
[[nodiscard]] std::vector<SLabelRun> Runs(std::wstring_view label)
{
	return ParseLabelWithIcons(label);
}

//! 同梱 codicon.ttf が登録できている通常状態
[[nodiscard]] std::vector<SLabelRun> FontRuns(std::wstring_view label)
{
	return ParseLabelWithIcons(label, L"codicon");
}

} // namespace

TEST(ThemeIconResolverLabel, EmptyLabelProducesNoRuns)
{
	EXPECT_TRUE(Runs(L"").empty());
}

TEST(ThemeIconResolverLabel, PlainTextStaysOneTextRun)
{
	const auto runs = Runs(L"CPU: 46%");
	ASSERT_EQ(1u, runs.size());
	EXPECT_FALSE(runs[0].icon);
	EXPECT_EQ(L"CPU: 46%", runs[0].text);
}

TEST(ThemeIconResolverLabel, LeadingIconSplitsIntoIconThenText)
{
	const auto runs = Runs(L"$(gear) Settings");
	ASSERT_EQ(2u, runs.size());
	EXPECT_TRUE(runs[0].icon);
	EXPECT_EQ(codicons::Icon::Gear, runs[0].resolved.builtin);
	EXPECT_FALSE(runs[1].icon);
	EXPECT_EQ(L" Settings", runs[1].text);
}

TEST(ThemeIconResolverLabel, EveryIconInTheLabelSurvivesInItsOriginalPosition)
{
	const auto runs = Runs(L"$(warning) first $(info) second");
	ASSERT_EQ(4u, runs.size());
	EXPECT_TRUE(runs[0].icon);
	EXPECT_FALSE(runs[1].icon);
	EXPECT_EQ(L" first ", runs[1].text);
	EXPECT_TRUE(runs[2].icon);
	EXPECT_FALSE(runs[3].icon);
	EXPECT_EQ(L" second", runs[3].text);
}

TEST(ThemeIconResolverLabel, IconInTheMiddleKeepsTheTextOnBothSides)
{
	const auto runs = Runs(L"before $(close) after");
	ASSERT_EQ(3u, runs.size());
	EXPECT_EQ(L"before ", runs[0].text);
	EXPECT_TRUE(runs[1].icon);
	EXPECT_EQ(L" after", runs[2].text);
}

TEST(ThemeIconResolverLabel, TrailingIconNeedsNoTextAfterIt)
{
	const auto runs = Runs(L"done $(check)");
	ASSERT_EQ(2u, runs.size());
	EXPECT_EQ(L"done ", runs[0].text);
	EXPECT_TRUE(runs[1].icon);
}

TEST(ThemeIconResolverLabel, AdjacentIconsProduceTwoIconRunsWithNoEmptyTextBetween)
{
	const auto runs = Runs(L"$(check)$(close)");
	ASSERT_EQ(2u, runs.size());
	EXPECT_TRUE(runs[0].icon);
	EXPECT_TRUE(runs[1].icon);
}

// ThemeIcon.fromString と同じく modifier は id から切り離す。id 解決には使わないが、
// 呼び出し側がアニメーションの有無を判断できるよう別に返す。
TEST(ThemeIconResolverLabel, ModifierIsSeparatedFromTheIconIdAndKept)
{
	const auto runs = Runs(L"$(loading~spin) usage");
	ASSERT_EQ(2u, runs.size());
	ASSERT_TRUE(runs[0].icon);
	EXPECT_EQ(codicons::Icon::Loading, runs[0].resolved.builtin);
	EXPECT_EQ(L"spin", runs[0].modifier);
	EXPECT_EQ(L" usage", runs[1].text);
}

// 上流の正規表現は (\\)?\$\(...\)。直前の \ はエスケープで、リテラルの "$(name)" を出す。
TEST(ThemeIconResolverLabel, BackslashEscapesTheIconSyntaxIntoLiteralText)
{
	const auto runs = Runs(L"\\$(gear) literal");
	ASSERT_EQ(1u, runs.size());
	EXPECT_FALSE(runs[0].icon);
	EXPECT_EQ(L"$(gear) literal", runs[0].text);
}

// 名前が [A-Za-z0-9-]+ に合わないものは記法ではない。上流でも素の文字として残る。
TEST(ThemeIconResolverLabel, NameOutsideTheAllowedCharacterSetStaysLiteralText)
{
	const auto runs = Runs(L"$(not a name) x");
	ASSERT_EQ(1u, runs.size());
	EXPECT_FALSE(runs[0].icon);
	EXPECT_EQ(L"$(not a name) x", runs[0].text);
}

TEST(ThemeIconResolverLabel, EmptyIconNameStaysLiteralText)
{
	const auto runs = Runs(L"$() x");
	ASSERT_EQ(1u, runs.size());
	EXPECT_EQ(L"$() x", runs[0].text);
}

TEST(ThemeIconResolverLabel, UnterminatedIconSyntaxStaysLiteralTextWithoutThrowing)
{
	const auto runs = Runs(L"$(gear");
	ASSERT_EQ(1u, runs.size());
	EXPECT_FALSE(runs[0].icon);
	EXPECT_EQ(L"$(gear", runs[0].text);
}

TEST(ThemeIconResolverLabel, ModifierWithoutABodyIsNotAModifierAndBreaksTheMatch)
{
	const auto runs = Runs(L"$(gear~) x");
	ASSERT_EQ(1u, runs.size());
	EXPECT_FALSE(runs[0].icon);
	EXPECT_EQ(L"$(gear~) x", runs[0].text);
}

TEST(ThemeIconResolverLabel, DollarWithoutAParenthesisIsOrdinaryText)
{
	const auto runs = Runs(L"$367.31");
	ASSERT_EQ(1u, runs.size());
	EXPECT_EQ(L"$367.31", runs[0].text);
}

// 以下 2 件は同梱フォントが登録できなかったときの縮退経路。通常経路は
// ThemeIconResolverBundledFont が確かめる。
TEST(ThemeIconResolverLabel, WithoutTheBundledFontUsesCanonicalVectorsForCommonBuiltinIds)
{
	const auto runs = Runs(L"$(source-control)$(warning)$(error)$(info)$(chevron-right)");
	ASSERT_EQ(5u, runs.size());
	EXPECT_EQ(codicons::Icon::SourceControl, runs[0].resolved.builtin);
	EXPECT_EQ(codicons::Icon::Warning, runs[1].resolved.builtin);
	EXPECT_EQ(codicons::Icon::Error, runs[2].resolved.builtin);
	EXPECT_EQ(codicons::Icon::Info, runs[3].resolved.builtin);
	EXPECT_EQ(codicons::Icon::ChevronRight, runs[4].resolved.builtin);
}

// 取り込んでいないベクター名は代替の点へ落ちる。リテラルの "$(name)" を漏らすより
// 安定した代替マーカーのほうがよい、という既存方針をそのまま引き継ぐ。
TEST(ThemeIconResolverLabel, WithoutTheBundledFontANameOutsideTheVectorSubsetFallsBackToTheSubstituteDot)
{
	const auto runs = Runs(L"$(rocket)");
	ASSERT_EQ(1u, runs.size());
	ASSERT_TRUE(runs[0].icon);
	EXPECT_FALSE(runs[0].resolved.font);
	EXPECT_EQ(codicons::Icon::RecordSmall, runs[0].resolved.builtin);
}

// 実 VS Code は codicon.ttf を丸ごと同梱し、`$(name)` をその 1 書体のグリフとして描く。
// 通常経路ではベクターの取り込み有無で名前ごとに描き方が変わってはならない。
TEST(ThemeIconResolverBundledFont, BuiltinNameResolvesToAGlyphOfTheBundledFont)
{
	const auto runs = FontRuns(L"$(rocket)");
	ASSERT_EQ(1u, runs.size());
	ASSERT_TRUE(runs[0].icon);
	ASSERT_TRUE(runs[0].resolved.font);
	EXPECT_EQ(L"codicon", runs[0].resolved.fontIcon.faceName);
	EXPECT_EQ(std::wstring(1, L'\uEB44'), runs[0].resolved.fontIcon.glyph);
}

// codicon.csv には無いが codiconsLibrary.ts にはある別名。
TEST(ThemeIconResolverBundledFont, AliasOnlyNameZapResolves)
{
	const auto runs = FontRuns(L"$(zap)");
	ASSERT_EQ(1u, runs.size());
	ASSERT_TRUE(runs[0].resolved.font);
	EXPECT_EQ(std::wstring(1, L'\uEA86'), runs[0].resolved.fontIcon.glyph);
}

// modifier は id から切り離してから引く。`$(loading~spin)` は loading のグリフ。
TEST(ThemeIconResolverBundledFont, ModifierIsStrippedBeforeTheGlyphLookup)
{
	const auto runs = FontRuns(L"$(loading~spin)");
	ASSERT_EQ(1u, runs.size());
	ASSERT_TRUE(runs[0].resolved.font);
	EXPECT_EQ(std::wstring(1, L'\uEB19'), runs[0].resolved.fontIcon.glyph);
	EXPECT_EQ(L"spin", runs[0].modifier);
}

// 上流 VS Code の Codicon にも無い名前は、同梱フォントでも解決できない。
TEST(ThemeIconResolverBundledFont, NameAbsentFromTheLibraryStillFallsBackToTheSubstituteDot)
{
	const auto runs = FontRuns(L"$(no-such-codicon-name)");
	ASSERT_EQ(1u, runs.size());
	ASSERT_TRUE(runs[0].icon);
	EXPECT_FALSE(runs[0].resolved.font);
	EXPECT_EQ(codicons::Icon::RecordSmall, runs[0].resolved.builtin);
}

TEST(CodiconGlyphTable, FindAnswersNulloptForANameTheLibraryDoesNotDeclare)
{
	EXPECT_FALSE(FindCodiconGlyph(L"").has_value());
	EXPECT_FALSE(FindCodiconGlyph(L"no-such-codicon-name").has_value());
	// 前方一致でも後方一致でもなく、完全一致だけが当たる。
	EXPECT_FALSE(FindCodiconGlyph(L"gea").has_value());
	EXPECT_FALSE(FindCodiconGlyph(L"gearx").has_value());
}

TEST(CodiconGlyphTable, SpotChecksMatchTheUpstreamCodepoints)
{
	EXPECT_EQ(L'\uEAF8', FindCodiconGlyph(L"gear").value_or(0));
	EXPECT_EQ(L'\uEAB2', FindCodiconGlyph(L"check").value_or(0));
	EXPECT_EQ(L'\uEA6C', FindCodiconGlyph(L"warning").value_or(0));
	EXPECT_EQ(L'\uEBCC', FindCodiconGlyph(L"copy").value_or(0));
	EXPECT_EQ(L'\uEA68', FindCodiconGlyph(L"source-control").value_or(0));
	EXPECT_EQ(L'\uEACD', FindCodiconGlyph(L"dashboard").value_or(0));
	EXPECT_EQ(L'\uEABD', FindCodiconGlyph(L"circle-slash").value_or(0));
}

// codicon.ttf の私用領域は BMP に収まる。表に BMP 外の値が紛れ込むと wchar_t 1 文字では
// 描けないので、生成器が壊れたことをここで捕まえる。
TEST(CodiconGlyphTable, EveryCodepointIsInThePrivateUseArea)
{
	ASSERT_FALSE(std::empty(kCodiconGlyphs));
	for (const auto& entry : kCodiconGlyphs) {
		EXPECT_GE(entry.character, L'\uE000');
		EXPECT_LE(entry.character, L'\uF8FF');
		EXPECT_FALSE(entry.name.empty());
	}
}
