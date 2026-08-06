/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "textmate/TextMateGrammarModel.h"
#include "textmate/TextMateJsonGrammarLoader.h"

namespace {

using textmate::ERuleKind;
using textmate::GrammarCompileResult;
using textmate::PatternRef;
using textmate::RuleId;
using textmate::TextMateJsonGrammarLoader;
using textmate::TextMateRule;

const TextMateRule* PatternRule(const textmate::Grammar& grammar, const std::vector<PatternRef>& patterns, std::size_t index)
{
	const auto* ruleId = std::get_if<RuleId>(&patterns.at(index));
	return ruleId ? grammar.Rule(*ruleId) : nullptr;
}

TEST(TextMateJsonGrammarLoaderTest, Load_ValidGrammar_ProducesScopeNameAndRootPatterns)
{
	constexpr std::string_view source = R"JSON(
	{
		"scopeName": "source.demo",
		"fileTypes": ["demo"],
		"patterns": [
			{ "match": "\\bif\\b", "name": "keyword.control.demo" },
			{ "include": "#string" }
		],
		"repository": {
			"string": {
				"begin": "\"",
				"end": "\"",
				"name": "string.quoted.double.demo",
				"beginCaptures": {
					"0": { "name": "punctuation.definition.string.begin.demo" }
				}
			}
		}
	}
	)JSON";

	const GrammarCompileResult result = TextMateJsonGrammarLoader::Load(source);

	ASSERT_TRUE(result.Succeeded());
	EXPECT_EQ(L"source.demo", result.grammar->scopeName);
	ASSERT_EQ(1u, result.grammar->fileTypes.size());
	EXPECT_EQ(L"demo", result.grammar->fileTypes[0]);

	const TextMateRule* rootRule = result.grammar->Rule(result.grammar->rootRuleId);
	ASSERT_NE(nullptr, rootRule);
	ASSERT_EQ(2u, rootRule->patterns.size());

	const TextMateRule* keywordRule = PatternRule(*result.grammar, rootRule->patterns, 0);
	ASSERT_NE(nullptr, keywordRule);
	EXPECT_EQ(ERuleKind::Match, keywordRule->kind);
	EXPECT_EQ(L"\\bif\\b", keywordRule->matchSource);

	ASSERT_EQ(1u, result.grammar->repository.count(L"string"));
	const TextMateRule* stringRule = result.grammar->Rule(result.grammar->repository.at(L"string"));
	ASSERT_NE(nullptr, stringRule);
	EXPECT_EQ(ERuleKind::BeginEnd, stringRule->kind);
	EXPECT_EQ(L"string.quoted.double.demo", stringRule->name);
	ASSERT_EQ(1u, stringRule->beginCaptures.size());
	EXPECT_EQ(L"punctuation.definition.string.begin.demo", stringRule->beginCaptures[0].name);
}

TEST(TextMateJsonGrammarLoaderTest, Load_PreservesPatternsArrayOrder)
{
	constexpr std::string_view source = R"JSON(
	{
		"scopeName": "source.order",
		"patterns": [
			{ "match": "third", "name": "a" },
			{ "match": "first", "name": "b" },
			{ "match": "second", "name": "c" }
		]
	}
	)JSON";

	const GrammarCompileResult result = TextMateJsonGrammarLoader::Load(source);

	ASSERT_TRUE(result.Succeeded());
	const auto& patterns = result.grammar->Rule(result.grammar->rootRuleId)->patterns;
	ASSERT_EQ(3u, patterns.size());
	EXPECT_EQ(L"third", PatternRule(*result.grammar, patterns, 0)->matchSource);
	EXPECT_EQ(L"first", PatternRule(*result.grammar, patterns, 1)->matchSource);
	EXPECT_EQ(L"second", PatternRule(*result.grammar, patterns, 2)->matchSource);
}

TEST(TextMateJsonGrammarLoaderTest, Load_ApplyEndPatternLastAsJsonBoolean)
{
	constexpr std::string_view source = R"JSON(
	{
		"scopeName": "source.demo",
		"patterns": [
			{ "begin": "a", "end": "b", "applyEndPatternLast": true }
		]
	}
	)JSON";

	const GrammarCompileResult result = TextMateJsonGrammarLoader::Load(source);

	ASSERT_TRUE(result.Succeeded());
	const auto& patterns = result.grammar->Rule(result.grammar->rootRuleId)->patterns;
	ASSERT_EQ(1u, patterns.size());
	EXPECT_TRUE(PatternRule(*result.grammar, patterns, 0)->applyEndPatternLast);
}

TEST(TextMateJsonGrammarLoaderTest, Load_MalformedJson_FailsWithDiagnostic)
{
	constexpr std::string_view source = R"JSON({ "scopeName": "source.demo", )JSON"; // truncated, unterminated object

	const GrammarCompileResult result = TextMateJsonGrammarLoader::Load(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_FALSE(result.diagnostics.empty());
	EXPECT_FALSE(result.diagnostics[0].message.empty());
}

TEST(TextMateJsonGrammarLoaderTest, Load_MissingScopeName_FailsWithDiagnostic)
{
	constexpr std::string_view source = R"JSON({ "patterns": [] })JSON";

	const GrammarCompileResult result = TextMateJsonGrammarLoader::Load(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_FALSE(result.diagnostics.empty());
}

} // namespace
