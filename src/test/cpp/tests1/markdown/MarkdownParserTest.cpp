/*! @file */
#include "pch.h"

#include "markdown/MarkdownParser.h"

namespace markdown {
namespace {

TEST(MarkdownParser, RecognizesTheInitialNativePreviewBlockSubset)
{
	const auto document = ParseMarkdown(
		L"# Heading [site](https://example.test)\n"
		L"\n"
		L"A paragraph continues\n"
		L"on the next source line.\n"
		L"\n"
		L"- first bullet\n"
		L"1. first ordered item\n"
		L"> quoted text\n"
		L"\n"
		L"---\n"
		L"\n"
		L"```cpp\n"
		L"int value = 1;\n"
		L"```\n");

	ASSERT_EQ(7u, document.blocks.size());
	EXPECT_EQ(BlockKind::Heading, document.blocks[0].kind);
	EXPECT_EQ(1, document.blocks[0].level);
	EXPECT_EQ(L"Heading site", document.blocks[0].text);
	ASSERT_EQ(1u, document.blocks[0].inlineSpans.size());
	EXPECT_EQ(8u, document.blocks[0].inlineSpans[0].start);
	EXPECT_EQ(4u, document.blocks[0].inlineSpans[0].length);
	EXPECT_EQ(BlockKind::Paragraph, document.blocks[1].kind);
	EXPECT_EQ(L"A paragraph continues on the next source line.", document.blocks[1].text);
	EXPECT_EQ(BlockKind::BulletListItem, document.blocks[2].kind);
	EXPECT_EQ(L"\x2022 ", document.blocks[2].marker);
	EXPECT_EQ(BlockKind::OrderedListItem, document.blocks[3].kind);
	EXPECT_EQ(L"1. ", document.blocks[3].marker);
	EXPECT_EQ(BlockKind::BlockQuote, document.blocks[4].kind);
	EXPECT_EQ(BlockKind::HorizontalRule, document.blocks[5].kind);
	EXPECT_EQ(BlockKind::CodeBlock, document.blocks[6].kind);
	EXPECT_EQ(L"int value = 1;", document.blocks[6].text);
}

TEST(MarkdownParser, PreservesUnclosedFencedCodeAsCodeAndLeavesMalformedLinksLiteral)
{
	const auto document = ParseMarkdown(L"~~~\nline one\nline two\n\n[text](missing\n");

	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(BlockKind::CodeBlock, document.blocks.front().kind);
	EXPECT_EQ(L"line one\nline two\n\n[text](missing", document.blocks.front().text);
	EXPECT_TRUE(document.blocks.front().inlineSpans.empty());
}

} // namespace
} // namespace markdown
