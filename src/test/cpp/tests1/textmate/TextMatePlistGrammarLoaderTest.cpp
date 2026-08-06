/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "textmate/TextMateGrammarModel.h"
#include "textmate/TextMatePlistGrammarLoader.h"

namespace {

using textmate::EPlistDiagnosticCode;
using textmate::ERuleKind;
using textmate::GrammarCompileResult;
using textmate::PatternRef;
using textmate::RuleId;
using textmate::TextMateGrammarValue;
using textmate::TextMatePlistGrammarLoader;
using textmate::TextMateRule;

const TextMateRule* PatternRule(const textmate::Grammar& grammar, const std::vector<PatternRef>& patterns, std::size_t index)
{
	const auto* ruleId = std::get_if<RuleId>(&patterns.at(index));
	return ruleId ? grammar.Rule(*ruleId) : nullptr;
}

// A full grammar shape (dict/array/begin-end/beginCaptures/repository/include)
// mirroring TextMateJsonGrammarLoaderTest's equivalent JSON fixture, to prove
// the plist loader reaches the same compiled Grammar shape as the JSON path.
constexpr std::string_view kValidGrammarPlist = R"PLIST(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>scopeName</key>
	<string>source.demo</string>
	<key>fileTypes</key>
	<array>
		<string>demo</string>
	</array>
	<key>patterns</key>
	<array>
		<dict>
			<key>match</key>
			<string>\bif\b</string>
			<key>name</key>
			<string>keyword.control.demo</string>
		</dict>
		<dict>
			<key>include</key>
			<string>#string</string>
		</dict>
	</array>
	<key>repository</key>
	<dict>
		<key>string</key>
		<dict>
			<key>begin</key>
			<string>"</string>
			<key>end</key>
			<string>"</string>
			<key>name</key>
			<string>string.quoted.double.demo</string>
			<key>beginCaptures</key>
			<dict>
				<key>0</key>
				<dict>
					<key>name</key>
					<string>punctuation.definition.string.begin.demo</string>
				</dict>
			</dict>
		</dict>
	</dict>
</dict>
</plist>
)PLIST";

TEST(TextMatePlistGrammarLoaderTest, Load_ValidGrammar_ProducesScopeNameAndRootPatterns)
{
	const GrammarCompileResult result = TextMatePlistGrammarLoader::Load(kValidGrammarPlist);

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

TEST(TextMatePlistGrammarLoaderTest, Parse_SelfClosingRoot_ProducesEmptyObject)
{
	constexpr std::string_view source = R"PLIST(<plist version="1.0"/>)PLIST";

	const TextMatePlistGrammarLoader::ParseResult result = TextMatePlistGrammarLoader::Parse(source);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_TRUE(result.value->IsObject());
	EXPECT_TRUE(result.value->AsObject()->empty());
}

TEST(TextMatePlistGrammarLoaderTest, Load_SelfClosingRoot_FailsWithMissingScopeName)
{
	constexpr std::string_view source = R"PLIST(<plist version="1.0"/>)PLIST";

	const GrammarCompileResult result = TextMatePlistGrammarLoader::Load(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_FALSE(result.diagnostics.empty());
}

// XXE-defense: a `<!ENTITY xxe ...>` declared inside the `<!DOCTYPE>` internal
// subset must never be registered anywhere, so referencing it via `&xxe;`
// fails closed as an *unknown* entity reference -- exactly the same failure
// as any other unrecognized `&name;`, never a silent expansion of the
// declared replacement text. This is the behavioral proof behind the
// XXE-safety claim documented on TextMatePlistGrammarLoader's class comment.
TEST(TextMatePlistGrammarLoaderTest, Parse_ReferencingDeclaredDoctypeEntity_FailsAsUnknownEntityReference)
{
	constexpr std::string_view source =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!DOCTYPE plist [\n"
		"<!ENTITY xxe SYSTEM \"file:///etc/passwd\">\n"
		"]>\n"
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"<key>scopeName</key>\n"
		"<string>&xxe;</string>\n"
		"</dict>\n"
		"</plist>\n";

	const TextMatePlistGrammarLoader::ParseResult result = TextMatePlistGrammarLoader::Parse(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EPlistDiagnosticCode::UnknownEntityReference, result.diagnostic->code);
}

// The internal subset itself is skipped as opaque bytes regardless of whether
// any of its declared entities are ever referenced: a document that declares
// an entity but never uses it must parse exactly as if the <!DOCTYPE ...>
// were absent, and the declared replacement text must never leak into the
// document by any other means either.
TEST(TextMatePlistGrammarLoaderTest, Parse_DoctypeWithUnreferencedEntity_IsInertAndHarmless)
{
	constexpr std::string_view source =
		"<!DOCTYPE plist [\n"
		"<!ENTITY unused \"this text must never appear anywhere\">\n"
		"]>\n"
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"<key>scopeName</key>\n"
		"<string>source.demo</string>\n"
		"</dict>\n"
		"</plist>\n";

	const TextMatePlistGrammarLoader::ParseResult result = TextMatePlistGrammarLoader::Parse(source);

	ASSERT_TRUE(result.Succeeded());
	const TextMateGrammarValue::Object* object = result.value->AsObject();
	ASSERT_NE(nullptr, object);
	ASSERT_EQ(1u, object->size());
	EXPECT_EQ(L"scopeName", object->at(0).first);
	ASSERT_TRUE(object->at(0).second.IsString());
	EXPECT_EQ(L"source.demo", *object->at(0).second.AsString());
}

TEST(TextMatePlistGrammarLoaderTest, Parse_PredefinedAndNumericEntities_DecodeCorrectly)
{
	constexpr std::string_view source =
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"<key>scopeName</key>\n"
		"<string>source.demo</string>\n"
		"<key>fileTypes</key>\n"
		"<array>\n"
		"<string>a &amp;&lt;b&gt; &quot;q&quot; &apos;s&apos; &#65; &#x42;</string>\n"
		"</array>\n"
		"</dict>\n"
		"</plist>\n";

	const GrammarCompileResult result = TextMatePlistGrammarLoader::Load(source);

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1u, result.grammar->fileTypes.size());
	EXPECT_EQ(L"a &<b> \"q\" 's' A B", result.grammar->fileTypes[0]);
}

TEST(TextMatePlistGrammarLoaderTest, Parse_MismatchedEndTag_FailsAsMalformedXml)
{
	constexpr std::string_view source =
		"<plist version=\"1.0\">\n"
		"<dict>\n"
		"<key>scopeName</key>\n"
		"<string>source.demo</string>\n"
		"</array>\n"
		"</plist>\n";

	const TextMatePlistGrammarLoader::ParseResult result = TextMatePlistGrammarLoader::Parse(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EPlistDiagnosticCode::MalformedXml, result.diagnostic->code);
}

TEST(TextMatePlistGrammarLoaderTest, Parse_NonPlistRoot_FailsAsMalformedXml)
{
	constexpr std::string_view source = "<dict></dict>";

	const TextMatePlistGrammarLoader::ParseResult result = TextMatePlistGrammarLoader::Parse(source);

	EXPECT_FALSE(result.Succeeded());
	ASSERT_TRUE(result.diagnostic.has_value());
	EXPECT_EQ(EPlistDiagnosticCode::MalformedXml, result.diagnostic->code);
}

} // namespace
