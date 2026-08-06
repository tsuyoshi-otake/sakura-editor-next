/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

// NOTE: unlike the other tests in this directory, these tests exercise the
// real Onigmo regex engine through OnigmoPattern::Compile()/Search() (see
// TextMateTokenizer.cpp), not just the JSON/plist-to-Grammar compilation
// boundary. As of this writing, Onigmo is not yet wired into either the
// MSBuild (sakura.vcxproj) or CMake (sakura.cmake) build of sakura_core --
// see textmate/CLAUDE.md for the exact build-configuration changes this
// still requires. This file is therefore expected to fail to LINK (not just
// "fail to pass") until that wiring lands; it has been traced by hand
// against TextMateTokenizer.cpp's actual control flow, but has not been
// compiled or run.

#include "pch.h"

#include "textmate/TextMateJsonGrammarLoader.h"
#include "textmate/TextMateTokenizer.h"

namespace {

using textmate::GrammarCompileResult;
using textmate::RuleStackHandle;
using textmate::TextMateJsonGrammarLoader;
using textmate::TextMateLineTokenizeResult;
using textmate::TextMateToken;
using textmate::TextMateTokenizer;

TEST(TextMateTokenizerTest, TokenizeLine_MatchRule_ProducesKeywordTokenAndPlainRemainder)
{
	constexpr std::string_view source = R"JSON(
	{
		"scopeName": "source.demo",
		"patterns": [
			{ "match": "\\bif\\b", "name": "keyword.control.demo" }
		]
	}
	)JSON";
	const GrammarCompileResult compiled = TextMateJsonGrammarLoader::Load(source);
	ASSERT_TRUE(compiled.Succeeded());

	const TextMateTokenizer tokenizer(*compiled.grammar);
	RuleStackHandle nextState;
	const TextMateLineTokenizeResult result = tokenizer.TokenizeLine(L"if (x)", tokenizer.InitialState(), nextState);

	ASSERT_EQ(2u, result.tokens.size());

	const TextMateToken& keyword = result.tokens[0];
	EXPECT_EQ(0u, keyword.utf16Start);
	EXPECT_EQ(2u, keyword.utf16End);
	ASSERT_EQ(1u, keyword.scopes.size());
	EXPECT_EQ(L"keyword.control.demo", keyword.scopes[0]);

	const TextMateToken& remainder = result.tokens[1];
	EXPECT_EQ(2u, remainder.utf16Start);
	EXPECT_EQ(6u, remainder.utf16End);
	EXPECT_TRUE(remainder.scopes.empty());

	// The root rule carries no scope name of its own (see
	// TextMateGrammarCompiler::Compile: the compiled root IncludeOnly rule's
	// `name` is never set from the grammar's `scopeName`), so a plain-text
	// span outside any begin/end frame has an empty scope list rather than
	// carrying "source.demo" the way real vscode-textmate would. This is a
	// documented divergence, not a test mistake -- see textmate/CLAUDE.md
	// "Known gaps".
	ASSERT_NE(nullptr, nextState);
	EXPECT_EQ(nullptr, nextState->parent);
}

// Exercises begin/end state carried forward across two TokenizeLine() calls
// via RuleStackHandle, plus beginCaptures/endCaptures group-1 scoping (group
// 0, the whole match, is intentionally never re-emitted as its own capture
// scope -- see EmitCaptureTokens's early "group 0 == whole match" skip).
TEST(TextMateTokenizerTest, TokenizeLine_BeginEndRule_CarriesStackAcrossLines)
{
	constexpr std::string_view source = R"JSON(
	{
		"scopeName": "source.demo",
		"patterns": [
			{
				"begin": "(\")",
				"end": "(\")",
				"name": "string.quoted.double.demo",
				"beginCaptures": { "1": { "name": "punctuation.definition.string.begin.demo" } },
				"endCaptures": { "1": { "name": "punctuation.definition.string.end.demo" } }
			}
		]
	}
	)JSON";
	const GrammarCompileResult compiled = TextMateJsonGrammarLoader::Load(source);
	ASSERT_TRUE(compiled.Succeeded());

	const TextMateTokenizer tokenizer(*compiled.grammar);

	RuleStackHandle afterLine1;
	const TextMateLineTokenizeResult line1 = tokenizer.TokenizeLine(L"\"abc", tokenizer.InitialState(), afterLine1);

	ASSERT_EQ(2u, line1.tokens.size());
	EXPECT_EQ(0u, line1.tokens[0].utf16Start);
	EXPECT_EQ(1u, line1.tokens[0].utf16End);
	ASSERT_EQ(2u, line1.tokens[0].scopes.size());
	EXPECT_EQ(L"string.quoted.double.demo", line1.tokens[0].scopes[0]);
	EXPECT_EQ(L"punctuation.definition.string.begin.demo", line1.tokens[0].scopes[1]);

	EXPECT_EQ(1u, line1.tokens[1].utf16Start);
	EXPECT_EQ(4u, line1.tokens[1].utf16End);
	ASSERT_EQ(1u, line1.tokens[1].scopes.size());
	EXPECT_EQ(L"string.quoted.double.demo", line1.tokens[1].scopes[0]);

	// The stack handed back after line 1 must still be inside the string
	// frame (not popped back to root): the closing quote never appeared.
	ASSERT_NE(nullptr, afterLine1);
	ASSERT_NE(nullptr, afterLine1->parent);
	EXPECT_EQ(nullptr, afterLine1->parent->parent);
	EXPECT_EQ(L"string.quoted.double.demo", afterLine1->name);

	RuleStackHandle afterLine2;
	const TextMateLineTokenizeResult line2 = tokenizer.TokenizeLine(L"end\"", afterLine1, afterLine2);

	ASSERT_EQ(2u, line2.tokens.size());
	EXPECT_EQ(0u, line2.tokens[0].utf16Start);
	EXPECT_EQ(3u, line2.tokens[0].utf16End);
	ASSERT_EQ(1u, line2.tokens[0].scopes.size());
	EXPECT_EQ(L"string.quoted.double.demo", line2.tokens[0].scopes[0]);

	EXPECT_EQ(3u, line2.tokens[1].utf16Start);
	EXPECT_EQ(4u, line2.tokens[1].utf16End);
	ASSERT_EQ(2u, line2.tokens[1].scopes.size());
	EXPECT_EQ(L"string.quoted.double.demo", line2.tokens[1].scopes[0]);
	EXPECT_EQ(L"punctuation.definition.string.end.demo", line2.tokens[1].scopes[1]);

	// The end match closed the frame: state after line 2 is back at root.
	ASSERT_NE(nullptr, afterLine2);
	EXPECT_EQ(nullptr, afterLine2->parent);
}

} // namespace
