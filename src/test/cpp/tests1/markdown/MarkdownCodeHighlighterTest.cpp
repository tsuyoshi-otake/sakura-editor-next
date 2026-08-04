/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "markdown/MarkdownCodeHighlighter.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace markdown {
namespace {

bool HasKind(const CodeHighlightResult& result, CodeTokenKind kind)
{
	return std::any_of(result.tokens.begin(), result.tokens.end(),
		[kind](const CodeHighlightToken& token) { return token.kind == kind; });
}

bool HasTokenText(
	const CodeHighlightResult& result,
	std::wstring_view source,
	CodeTokenKind kind,
	std::wstring_view text)
{
	return std::any_of(result.tokens.begin(), result.tokens.end(),
		[=](const CodeHighlightToken& token) {
			return token.kind == kind
				&& source.substr(token.start, token.length) == text;
		});
}

void ExpectValidTokens(const CodeHighlightResult& result, std::size_t sourceLength)
{
	std::size_t previousEnd = 0;
	for (const auto& token : result.tokens) {
		EXPECT_GT(token.length, 0U);
		EXPECT_GE(token.start, previousEnd);
		EXPECT_LE(token.start, sourceLength);
		EXPECT_LE(token.length, sourceLength - std::min(token.start, sourceLength));
		previousEnd = token.start + token.length;
	}
}

TEST(MarkdownCodeHighlighter, NormalizesFenceLanguageAliases)
{
	struct AliasFixture {
		std::wstring_view input;
		std::wstring_view expected;
	};
	constexpr std::array fixtures{
		AliasFixture{L" shell ", L"sh"},
		AliasFixture{L"PY3", L"python"},
		AliasFixture{L"tsx", L"jsx"},
		AliasFixture{L"TypeScriptReact", L"jsx"},
		AliasFixture{L"json5", L"json"},
		AliasFixture{L"jsonc", L"json"},
		AliasFixture{L"c#", L"cs"},
		AliasFixture{L"CSharp", L"cs"},
		AliasFixture{L"c++", L"cpp"},
		AliasFixture{L"javascript", L"js"},
		AliasFixture{L"pwsh", L"powershell"},
		AliasFixture{L"md", L"markdown"},
		AliasFixture{L"unsupported-active-language", L"plain"},
	};

	for (const auto& fixture : fixtures) {
		EXPECT_EQ(fixture.expected, NormalizeMarkdownCodeLanguage(fixture.input))
			<< "input=" << testing::PrintToString(fixture.input);
	}
}

TEST(MarkdownCodeHighlighter, HighlightsEverySupportedNativeLanguageFixture)
{
	struct SyntaxFixture {
		std::wstring_view language;
		std::wstring_view source;
		std::array<CodeTokenKind, 3> expectedKinds;
	};
	constexpr std::array fixtures{
		SyntaxFixture{L"c", L"int value = 1; // note",
			{CodeTokenKind::Type, CodeTokenKind::Number, CodeTokenKind::Comment}},
		SyntaxFixture{L"cpp", L"class Item { const char* value = \"x\"; };",
			{CodeTokenKind::Keyword, CodeTokenKind::Type, CodeTokenKind::String}},
		SyntaxFixture{L"cs", L"string Value() { return @\"x\"; }",
			{CodeTokenKind::Type, CodeTokenKind::Keyword, CodeTokenKind::String}},
		SyntaxFixture{L"js", L"const ready = true; // note",
			{CodeTokenKind::Keyword, CodeTokenKind::Literal, CodeTokenKind::Comment}},
		SyntaxFixture{L"ts", L"interface Item { value: number; enabled: false; }",
			{CodeTokenKind::Keyword, CodeTokenKind::Type, CodeTokenKind::Literal}},
		SyntaxFixture{L"jsx", L"<Button title=\"x\">{true}</Button>",
			{CodeTokenKind::Tag, CodeTokenKind::Attribute, CodeTokenKind::String}},
		SyntaxFixture{L"json", L"{ \"ready\": true, \"count\": 1 }",
			{CodeTokenKind::String, CodeTokenKind::Literal, CodeTokenKind::Number}},
		SyntaxFixture{L"python", L"def value():\n    return \"x\" # note",
			{CodeTokenKind::Keyword, CodeTokenKind::String, CodeTokenKind::Comment}},
		SyntaxFixture{L"sh", L"if [ \"$value\" ]; then echo ok; fi # note",
			{CodeTokenKind::Keyword, CodeTokenKind::String, CodeTokenKind::Comment}},
		SyntaxFixture{L"powershell", L"function Test { $value = \"x\" # note\n}",
			{CodeTokenKind::Keyword, CodeTokenKind::Variable, CodeTokenKind::Comment}},
		SyntaxFixture{L"css", L".item { color: #fff; width: 10px; }",
			{CodeTokenKind::Attribute, CodeTokenKind::Literal, CodeTokenKind::Number}},
		SyntaxFixture{L"html", L"<div class=\"x\">&amp;</div><!-- note -->",
			{CodeTokenKind::Tag, CodeTokenKind::Attribute, CodeTokenKind::Comment}},
		SyntaxFixture{L"markdown", L"# Title\nUse **bold**, [link](a.md), and `code`.",
			{CodeTokenKind::Heading, CodeTokenKind::Link, CodeTokenKind::Code}},
	};

	for (const auto& fixture : fixtures) {
		const auto result = HighlightMarkdownCode(fixture.language, fixture.source);
		EXPECT_EQ(CodeHighlightTerminalState::Completed, result.terminalState)
			<< "language=" << testing::PrintToString(fixture.language);
		EXPECT_EQ(fixture.source.size(), result.scannedLength);
		ExpectValidTokens(result, fixture.source.size());
		for (const auto kind : fixture.expectedKinds) {
			EXPECT_TRUE(HasKind(result, kind))
				<< "language=" << testing::PrintToString(fixture.language)
				<< "kind=" << static_cast<int>(kind);
		}
	}

	const auto plain = HighlightMarkdownCode(L"plain", L"int value = 1;");
	EXPECT_EQ(CodeHighlightTerminalState::Completed, plain.terminalState);
	EXPECT_TRUE(plain.tokens.empty());
}

TEST(MarkdownCodeHighlighter, ReportsUtf16OffsetsWithoutChangingInput)
{
	std::wstring source;
	source.push_back(static_cast<wchar_t>(0xd83d));
	source.push_back(static_cast<wchar_t>(0xde80));
	source.append(L" int value = 1;");
	const std::wstring original = source;

	const auto result = HighlightMarkdownCode(L"c", source);

	EXPECT_EQ(original, source);
	EXPECT_TRUE(HasTokenText(result, source, CodeTokenKind::Type, L"int"));
	const auto type = std::find_if(result.tokens.begin(), result.tokens.end(),
		[](const CodeHighlightToken& token) { return token.kind == CodeTokenKind::Type; });
	ASSERT_NE(result.tokens.end(), type);
	EXPECT_EQ(3U, type->start);
	EXPECT_EQ(3U, type->length);
	ExpectValidTokens(result, source.size());
}

TEST(MarkdownCodeHighlighter, UnterminatedQuotesAndCommentsReachExplicitTerminals)
{
	const std::wstring cppString = L"const char* value = \"unterminated";
	const auto stringResult = HighlightMarkdownCode(L"cpp", cppString);
	EXPECT_EQ(CodeHighlightTerminalState::UnterminatedString, stringResult.terminalState);
	EXPECT_EQ(cppString.size(), stringResult.scannedLength);
	EXPECT_TRUE(HasKind(stringResult, CodeTokenKind::String));

	const std::wstring cComment = L"/* unterminated";
	const auto commentResult = HighlightMarkdownCode(L"c", cComment);
	EXPECT_EQ(CodeHighlightTerminalState::UnterminatedComment, commentResult.terminalState);
	EXPECT_EQ(cComment.size(), commentResult.scannedLength);
	EXPECT_TRUE(HasKind(commentResult, CodeTokenKind::Comment));

	const std::wstring powershellComment = L"<# unterminated";
	const auto powershellResult = HighlightMarkdownCode(L"powershell", powershellComment);
	EXPECT_EQ(CodeHighlightTerminalState::UnterminatedComment, powershellResult.terminalState);
	EXPECT_EQ(powershellComment.size(), powershellResult.scannedLength);
}

TEST(MarkdownCodeHighlighter, TokenLimitIsBoundedAndStillScansToTheEnd)
{
	std::wstring source;
	for (std::size_t index = 0; index < 20'000; ++index) {
		source.append(L"if value = 1; ");
	}
	constexpr std::size_t maximumTokens = 32;

	const auto result = HighlightMarkdownCode(L"cpp", source, maximumTokens);

	EXPECT_EQ(CodeHighlightTerminalState::TokenLimitReached, result.terminalState);
	EXPECT_LE(result.tokens.size(), maximumTokens);
	EXPECT_EQ(source.size(), result.scannedLength);
	ExpectValidTokens(result, source.size());
}

TEST(MarkdownCodeHighlighter, MaliciousLongLinesRemainLinearAndTerminate)
{
	constexpr std::size_t length = 1U << 20;
	const std::wstring unmatchedLinks(length, L'[');
	const auto markdownResult = HighlightMarkdownCode(L"markdown", unmatchedLinks, 64);
	EXPECT_EQ(CodeHighlightTerminalState::UnterminatedConstruct,
		markdownResult.terminalState);
	EXPECT_EQ(unmatchedLinks.size(), markdownResult.scannedLength);
	EXPECT_LE(markdownResult.workUnits, unmatchedLinks.size() * 8 + 128);
	EXPECT_LE(markdownResult.tokens.size(), 64U);

	std::wstring unterminatedString;
	unterminatedString.reserve(length);
	unterminatedString.push_back(L'"');
	unterminatedString.append(length - 1, L'x');
	const auto stringResult = HighlightMarkdownCode(L"cpp", unterminatedString, 64);
	EXPECT_EQ(CodeHighlightTerminalState::UnterminatedString, stringResult.terminalState);
	EXPECT_EQ(unterminatedString.size(), stringResult.scannedLength);
	EXPECT_LE(stringResult.workUnits, unterminatedString.size() * 8 + 128);
}

} // namespace
} // namespace markdown
