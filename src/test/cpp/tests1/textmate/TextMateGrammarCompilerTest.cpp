/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "textmate/TextMateGrammarCompiler.h"
#include "textmate/TextMateGrammarModel.h"
#include "textmate/TextMateGrammarValue.h"

#include <initializer_list>
#include <utility>

namespace {

using textmate::CaptureRule;
using textmate::EIncludeKind;
using textmate::ERuleKind;
using textmate::Grammar;
using textmate::GrammarCompileResult;
using textmate::IncludeReference;
using textmate::kInvalidRuleId;
using textmate::PatternRef;
using textmate::ResolveInclude;
using textmate::RuleId;
using textmate::TextMateGrammarCompiler;
using textmate::TextMateGrammarValue;
using textmate::TextMateRule;

// Small builders so each test reads close to the JSON/plist shape it stands
// in for, without depending on either format-specific loader (this test
// exercises `TextMateGrammarCompiler` directly against hand-built
// `TextMateGrammarValue` trees, the same boundary both loaders target).

TextMateGrammarValue Str(std::wstring value)
{
	return TextMateGrammarValue(std::move(value));
}

TextMateGrammarValue Obj(std::initializer_list<std::pair<std::wstring, TextMateGrammarValue>> members)
{
	TextMateGrammarValue::Object object(members.begin(), members.end());
	return TextMateGrammarValue(std::move(object));
}

TextMateGrammarValue Arr(std::initializer_list<TextMateGrammarValue> elements)
{
	TextMateGrammarValue::Array array(elements);
	return TextMateGrammarValue(std::move(array));
}

TextMateGrammarValue Bool(bool value)
{
	return TextMateGrammarValue(value);
}

const TextMateRule* PatternRule(const Grammar& grammar, const std::vector<PatternRef>& patterns, std::size_t index)
{
	const auto* ruleId = std::get_if<RuleId>(&patterns.at(index));
	if (!ruleId) return nullptr;
	return grammar.Rule(*ruleId);
}

const IncludeReference* PatternInclude(const std::vector<PatternRef>& patterns, std::size_t index)
{
	return std::get_if<IncludeReference>(&patterns.at(index));
}

TEST(TextMateGrammarCompilerTest, Compile_MissingScopeName_Fails)
{
	const TextMateGrammarValue root = Obj({});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_FALSE(result.diagnostics.empty());
}

TEST(TextMateGrammarCompilerTest, Compile_MinimalGrammar_ProducesEmptyRootRule)
{
	const TextMateGrammarValue root = Obj({{L"scopeName", Str(L"source.test")}});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	EXPECT_EQ(L"source.test", result.grammar->scopeName);
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	ASSERT_NE(nullptr, rootRule);
	EXPECT_EQ(ERuleKind::IncludeOnly, rootRule->kind);
	EXPECT_TRUE(rootRule->patterns.empty());
}

TEST(TextMateGrammarCompilerTest, Compile_MatchRule_SetsNameAndSource)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({{L"match", Str(L"foo")}, {L"name", Str(L"keyword.foo")}})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	ASSERT_EQ(1u, rootRule->patterns.size());
	const TextMateRule* matchRule = PatternRule(*result.grammar, rootRule->patterns, 0);
	ASSERT_NE(nullptr, matchRule);
	EXPECT_EQ(ERuleKind::Match, matchRule->kind);
	EXPECT_EQ(L"foo", matchRule->matchSource);
	EXPECT_EQ(L"keyword.foo", matchRule->name);
}

TEST(TextMateGrammarCompilerTest, Compile_BeginEndRule_WithExplicitBeginCaptures)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"begin", Str(L"\"")},
			{L"end", Str(L"\"")},
			{L"name", Str(L"string.quoted.double")},
			{L"beginCaptures", Obj({{L"0", Obj({{L"name", Str(L"punctuation.definition.string.begin")}})}})},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	const TextMateRule* rule = PatternRule(*result.grammar, rootRule->patterns, 0);
	ASSERT_NE(nullptr, rule);
	EXPECT_EQ(ERuleKind::BeginEnd, rule->kind);
	EXPECT_EQ(L"\"", rule->beginSource);
	EXPECT_EQ(L"\"", rule->endSource);
	EXPECT_EQ(L"string.quoted.double", rule->name);
	ASSERT_EQ(1u, rule->beginCaptures.size());
	EXPECT_EQ(0, rule->beginCaptures[0].groupIndex);
	EXPECT_EQ(L"punctuation.definition.string.begin", rule->beginCaptures[0].name);
	// No explicit `endCaptures` and no shared `captures` fallback: end side
	// stays empty rather than inheriting `beginCaptures`.
	EXPECT_TRUE(rule->endCaptures.empty());
}

TEST(TextMateGrammarCompilerTest, Compile_SharedCaptures_AppliesToBothBeginAndEnd)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"begin", Str(L"<")},
			{L"end", Str(L">")},
			{L"captures", Obj({{L"0", Obj({{L"name", Str(L"punctuation.bracket")}})}})},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rule = PatternRule(*result.grammar, result.grammar->Rule(result.grammar->rootRuleId)->patterns, 0);
	ASSERT_NE(nullptr, rule);
	ASSERT_EQ(1u, rule->beginCaptures.size());
	ASSERT_EQ(1u, rule->endCaptures.size());
	EXPECT_EQ(L"punctuation.bracket", rule->beginCaptures[0].name);
	EXPECT_EQ(L"punctuation.bracket", rule->endCaptures[0].name);
}

TEST(TextMateGrammarCompilerTest, Compile_BeginWhileRule_SetsWhileSourceNotEnd)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"begin", Str(L"^\\s*#")},
			{L"while", Str(L"^\\s*#")},
			{L"name", Str(L"comment.line.continuation")},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rule = PatternRule(*result.grammar, result.grammar->Rule(result.grammar->rootRuleId)->patterns, 0);
	ASSERT_NE(nullptr, rule);
	EXPECT_EQ(ERuleKind::BeginWhile, rule->kind);
	EXPECT_EQ(L"^\\s*#", rule->whileSource);
	EXPECT_TRUE(rule->endSource.empty());
}

TEST(TextMateGrammarCompilerTest, Compile_ApplyEndPatternLast_DefaultsFalse)
{
	const TextMateGrammarValue withoutFlag = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({{L"begin", Str(L"a")}, {L"end", Str(L"b")}})})},
	});
	const GrammarCompileResult withoutResult = TextMateGrammarCompiler::Compile(withoutFlag);
	ASSERT_TRUE(withoutResult.Succeeded());
	EXPECT_FALSE(PatternRule(*withoutResult.grammar, withoutResult.grammar->Rule(withoutResult.grammar->rootRuleId)->patterns, 0)->applyEndPatternLast);

	const TextMateGrammarValue withFlag = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({{L"begin", Str(L"a")}, {L"end", Str(L"b")}, {L"applyEndPatternLast", Bool(true)}})})},
	});
	const GrammarCompileResult withResult = TextMateGrammarCompiler::Compile(withFlag);
	ASSERT_TRUE(withResult.Succeeded());
	EXPECT_TRUE(PatternRule(*withResult.grammar, withResult.grammar->Rule(withResult.grammar->rootRuleId)->patterns, 0)->applyEndPatternLast);
}

TEST(TextMateGrammarCompilerTest, Compile_DisabledPattern_IsSkippedEntirely)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({
			Obj({{L"match", Str(L"x")}, {L"disabled", Bool(true)}}),
			Obj({{L"match", Str(L"y")}}),
		})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	ASSERT_EQ(1u, rootRule->patterns.size());
	EXPECT_EQ(L"y", PatternRule(*result.grammar, rootRule->patterns, 0)->matchSource);
}

TEST(TextMateGrammarCompilerTest, Compile_Include_RepositoryEntry_ResolvesThroughRepository)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"repository", Obj({{L"foo", Obj({{L"match", Str(L"x")}, {L"name", Str(L"n")}})}})},
		{L"patterns", Arr({Obj({{L"include", Str(L"#foo")}})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	ASSERT_EQ(1u, rootRule->patterns.size());
	const IncludeReference* include = PatternInclude(rootRule->patterns, 0);
	ASSERT_NE(nullptr, include);
	EXPECT_EQ(EIncludeKind::RepositoryEntry, include->kind);
	EXPECT_EQ(L"foo", include->primaryName);

	const TextMateRule* resolved = ResolveInclude(*result.grammar, *include, nullptr);
	ASSERT_NE(nullptr, resolved);
	EXPECT_EQ(L"x", resolved->matchSource);
}

TEST(TextMateGrammarCompilerTest, Compile_Include_UnknownRepositoryEntry_ResolvesToNull)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({{L"include", Str(L"#missing")}})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const IncludeReference* include = PatternInclude(result.grammar->Rule(result.grammar->rootRuleId)->patterns, 0);
	ASSERT_NE(nullptr, include);
	EXPECT_EQ(nullptr, ResolveInclude(*result.grammar, *include, nullptr));
}

TEST(TextMateGrammarCompilerTest, Compile_Include_SelfAndBase_BothResolveToRootRule)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({
			Obj({{L"include", Str(L"$self")}}),
			Obj({{L"include", Str(L"$base")}}),
		})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	const IncludeReference* selfRef = PatternInclude(rootRule->patterns, 0);
	const IncludeReference* baseRef = PatternInclude(rootRule->patterns, 1);
	ASSERT_NE(nullptr, selfRef);
	ASSERT_NE(nullptr, baseRef);
	EXPECT_EQ(EIncludeKind::Self, selfRef->kind);
	// Documented divergence (see `EIncludeKind::Base` and `textmate/CLAUDE.md`):
	// `$base` resolves identically to `$self` because grammar injection is not
	// implemented yet.
	EXPECT_EQ(EIncludeKind::Base, baseRef->kind);
	EXPECT_EQ(rootRule, ResolveInclude(*result.grammar, *selfRef, nullptr));
	EXPECT_EQ(rootRule, ResolveInclude(*result.grammar, *baseRef, nullptr));
}

TEST(TextMateGrammarCompilerTest, Compile_CaptureWithNestedPatterns_AllocatesRecursiveRule)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"match", Str(L"(a)(b)")},
			{L"captures", Obj({
				{L"1", Obj({{L"patterns", Arr({Obj({{L"match", Str(L"z")}})})}})},
			})},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	const TextMateRule* matchRule = PatternRule(*result.grammar, result.grammar->Rule(result.grammar->rootRuleId)->patterns, 0);
	ASSERT_NE(nullptr, matchRule);
	ASSERT_EQ(1u, matchRule->captures.size());
	const CaptureRule& capture = matchRule->captures[0];
	EXPECT_EQ(1, capture.groupIndex);
	EXPECT_NE(kInvalidRuleId, capture.nestedPatternsRuleId);

	const TextMateRule* nested = result.grammar->Rule(capture.nestedPatternsRuleId);
	ASSERT_NE(nullptr, nested);
	ASSERT_EQ(1u, nested->patterns.size());
	EXPECT_EQ(L"z", PatternRule(*result.grammar, nested->patterns, 0)->matchSource);
}

TEST(TextMateGrammarCompilerTest, Compile_NonNumericCaptureKey_SkippedWithDiagnostic)
{
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"match", Str(L"a")},
			{L"captures", Obj({{L"not-a-number", Obj({{L"name", Str(L"x")}})}})},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	EXPECT_TRUE(PatternRule(*result.grammar, result.grammar->Rule(result.grammar->rootRuleId)->patterns, 0)->captures.empty());
	EXPECT_FALSE(result.diagnostics.empty());
}

TEST(TextMateGrammarCompilerTest, Compile_RepositoryNestedInsidePatternRule_IsNotMerged)
{
	// Documented gap (see `Grammar::repository` and `textmate/CLAUDE.md`
	// "Known gaps"): a `repository` object nested inside a non-root rule is a
	// legal but rare TextMate shape this compiler does not merge into
	// `Grammar::repository`.
	const TextMateGrammarValue root = Obj({
		{L"scopeName", Str(L"source.test")},
		{L"patterns", Arr({Obj({
			{L"patterns", Arr({})},
			{L"repository", Obj({{L"nested", Obj({{L"match", Str(L"x")}})}})},
		})})},
	});
	const GrammarCompileResult result = TextMateGrammarCompiler::Compile(root);

	ASSERT_TRUE(result.Succeeded());
	EXPECT_EQ(0u, result.grammar->repository.count(L"nested"));
}

} // namespace
