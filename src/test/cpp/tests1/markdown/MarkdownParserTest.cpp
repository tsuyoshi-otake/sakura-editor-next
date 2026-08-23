/*! @file */
#include "pch.h"

#include "markdown/MarkdownParser.h"

#include <algorithm>
#include <cstdio>

namespace markdown {
namespace {

[[nodiscard]] const InlineSpan* FindInline(const Block& block, InlineKind kind)
{
	const auto found = std::find_if(block.inlineSpans.begin(), block.inlineSpans.end(),
		[kind](const InlineSpan& span) { return span.kind == kind; });
	return found == block.inlineSpans.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t CountInline(const Block& block, InlineKind kind)
{
	return static_cast<std::size_t>(std::count_if(block.inlineSpans.begin(), block.inlineSpans.end(),
		[kind](const InlineSpan& span) { return span.kind == kind; }));
}

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
	EXPECT_EQ(InlineKind::Link, document.blocks[0].inlineSpans[0].kind);
	EXPECT_EQ(8u, document.blocks[0].inlineSpans[0].start);
	EXPECT_EQ(4u, document.blocks[0].inlineSpans[0].length);
	ASSERT_TRUE(document.blocks[0].inlineSpans[0].resource.has_value());
	EXPECT_EQ(ResourceDisposition::ExternalBlocked,
		document.blocks[0].inlineSpans[0].resource->disposition);
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
	EXPECT_EQ(L"cpp", document.blocks[6].language);
}

TEST(MarkdownParser, PreservesUnclosedFencedCodeAsCodeAndLeavesMalformedLinksLiteral)
{
	const auto document = ParseMarkdown(L"~~~\nline one\nline two\n\n[text](missing\n");

	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(BlockKind::CodeBlock, document.blocks.front().kind);
	EXPECT_EQ(L"line one\nline two\n\n[text](missing", document.blocks.front().text);
	EXPECT_TRUE(document.blocks.front().inlineSpans.empty());
}

TEST(MarkdownParser, ProjectsInlineFormattingAndTablesIntoTheNativeModel)
{
	const auto document = ParseMarkdown(
		L"Plain *emphasis*, **strong**, `code`, and [relative](guide.md).\n"
		L"\n"
		L"| Name | Value |\n"
		L"| :--- | ---: |\n"
		L"| **alpha** | `1` |\n",
		{ L"C:\\workspace\\docs\\readme.md", L"C:\\workspace" });

	ASSERT_EQ(2u, document.blocks.size());
	const auto& paragraph = document.blocks[0];
	EXPECT_EQ(BlockKind::Paragraph, paragraph.kind);
	EXPECT_NE(nullptr, FindInline(paragraph, InlineKind::Emphasis));
	EXPECT_NE(nullptr, FindInline(paragraph, InlineKind::Strong));
	EXPECT_NE(nullptr, FindInline(paragraph, InlineKind::Code));
	const auto* link = FindInline(paragraph, InlineKind::Link);
	ASSERT_NE(nullptr, link);
	ASSERT_TRUE(link->resource.has_value());
	EXPECT_EQ(ResourceDisposition::ResolvedLocal, link->resource->disposition);
	EXPECT_EQ(L"C:\\workspace\\docs\\guide.md", link->resource->resolvedPath);

	const auto& table = document.blocks[1];
	EXPECT_EQ(BlockKind::Table, table.kind);
	ASSERT_EQ(2u, table.tableRows.size());
	ASSERT_EQ(2u, table.tableAlignments.size());
	EXPECT_EQ(TableAlignment::Left, table.tableAlignments[0]);
	EXPECT_EQ(TableAlignment::Right, table.tableAlignments[1]);
	EXPECT_TRUE(table.tableRows[0].header);
	ASSERT_EQ(2u, table.tableRows[1].cells.size());
	ASSERT_EQ(1u, table.tableRows[1].cells[0].inlineSpans.size());
	EXPECT_EQ(InlineKind::Strong, table.tableRows[1].cells[0].inlineSpans[0].kind);
}

TEST(MarkdownParser, ConvertsSafeHtmlWrappersWithoutExposingOrExecutingRawTags)
{
	const auto document = ParseMarkdown(
		L"<h1 align=\"center\"><img src=\"./hero.png\" alt=\"Hero\" onerror=\"boom()\"></h1>\n"
		L"<p onclick=\"boom()\">Safe <strong>bold</strong> and <em>emphasis</em>. "
		L"<a href=\"javascript:boom()\">blocked link</a></p>\n"
		L"<script>secretScriptText()</script><style>secretStyleText{}</style>\n"
		L"<table><tr><th>A</th><th>B</th></tr><tr><td>one</td><td>two</td></tr></table>\n",
		{ L"C:\\workspace\\README.md", L"C:\\workspace" });

	ASSERT_FALSE(document.blocks.empty());
	ASSERT_EQ(BlockKind::Image, document.blocks.front().kind);
	ASSERT_TRUE(document.blocks.front().image.has_value());
	EXPECT_EQ(L"Hero", document.blocks.front().image->altText);
	EXPECT_EQ(ResourceDisposition::ResolvedLocal, document.blocks.front().image->source.disposition);
	EXPECT_EQ(L"C:\\workspace\\hero.png", document.blocks.front().image->source.resolvedPath);

	bool foundTable = false;
	bool foundUnsafeLink = false;
	std::wstring allVisibleText;
	for (const auto& block : document.blocks) {
		allVisibleText.append(block.text);
		if (block.kind == BlockKind::Table) foundTable = true;
		for (const auto& span : block.inlineSpans) {
			if (span.kind == InlineKind::Link && span.resource.has_value()
				&& span.resource->disposition == ResourceDisposition::UnsafeSchemeBlocked) {
				foundUnsafeLink = true;
			}
		}
	}
	EXPECT_TRUE(foundTable);
	EXPECT_TRUE(foundUnsafeLink);
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L'<'));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"secretScriptText"));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"secretStyleText"));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"onclick"));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"onerror"));
}

TEST(MarkdownParser, ProjectsAdditionalSafeHtmlElementsIntoNativeMarkdown)
{
	const auto document = ParseMarkdown(
		L"<p>safe <kbd>Ctrl+C</kbd> <del>old</del> <s>stale</s> <mark>new</mark></p>\n"
		L"<dl><dt>Term</dt><dd>Definition</dd></dl>\n"
		L"<table><caption>Caption</caption><thead><tr><th>Name</th></tr></thead>"
		L"<tbody><tr><td>Value</td></tr></tbody></table>\n"
		L"<button>must not appear</button><textarea>nor this</textarea>\n");

	ASSERT_FALSE(document.blocks.empty());
	std::wstring allVisibleText;
	bool foundTable = false;
	for (const auto& block : document.blocks) {
		allVisibleText.append(block.text);
		if (block.kind == BlockKind::Table) foundTable = true;
	}
	EXPECT_NE(std::wstring::npos, allVisibleText.find(L"safe Ctrl+C old stale new"));
	EXPECT_NE(std::wstring::npos, allVisibleText.find(L"Term"));
	EXPECT_NE(std::wstring::npos, allVisibleText.find(L"Definition"));
	EXPECT_NE(std::wstring::npos, allVisibleText.find(L"Caption"));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"must not appear"));
	EXPECT_EQ(std::wstring::npos, allVisibleText.find(L"nor this"));
	EXPECT_TRUE(foundTable);

	const auto& paragraph = document.blocks.front();
	EXPECT_EQ(1u, CountInline(paragraph, InlineKind::Code));
	EXPECT_EQ(2u, CountInline(paragraph, InlineKind::Strikethrough));
}

TEST(MarkdownParser, PreservesHtmlBreaksPreformattedBackticksAndLinkLabels)
{
	const auto document = ParseMarkdown(
		L"<p>one<br>two<br/>three</p>\n"
		L"<pre><code>&lt;b&gt; ``` inside</code></pre>\n"
		L"<a href=\"#section\">label [x]</a>\n");

	const Block* paragraph = nullptr;
	const Block* code = nullptr;
	const Block* link = nullptr;
	for (const auto& block : document.blocks) {
		if (block.kind == BlockKind::Paragraph && paragraph == nullptr) paragraph = &block;
		if (block.kind == BlockKind::CodeBlock) code = &block;
		if (block.kind == BlockKind::Paragraph && FindInline(block, InlineKind::Link) != nullptr) link = &block;
	}
	ASSERT_NE(nullptr, paragraph);
	ASSERT_NE(nullptr, code);
	ASSERT_NE(nullptr, link);
	EXPECT_EQ(L"one\ntwo\nthree", paragraph->text);
	EXPECT_EQ(L"<b> ``` inside", code->text);
	EXPECT_EQ(L"label [x]", link->text);
	ASSERT_EQ(1u, CountInline(*link, InlineKind::Link));
}

TEST(MarkdownParser, PreservesLiteralHtmlInsideMarkdownCodeBeforeSanitizingRawHtml)
{
	const auto document = ParseMarkdown(
		L"`<b>inline</b>`\n\n"
		L"```html\n"
		L"<script>alert(1)</script>\n"
		L"<div onclick=\"boom()\">literal</div>\n"
		L"```\n\n"
		L"<script>outside()</script><p>safe</p>\n");

	ASSERT_EQ(3u, document.blocks.size());
	EXPECT_EQ(BlockKind::Paragraph, document.blocks[0].kind);
	EXPECT_EQ(L"<b>inline</b>", document.blocks[0].text);
	ASSERT_EQ(1u, document.blocks[0].inlineSpans.size());
	EXPECT_EQ(InlineKind::Code, document.blocks[0].inlineSpans[0].kind);

	EXPECT_EQ(BlockKind::CodeBlock, document.blocks[1].kind);
	EXPECT_EQ(L"html", document.blocks[1].language);
	EXPECT_EQ(L"<script>alert(1)</script>\n<div onclick=\"boom()\">literal</div>",
		document.blocks[1].text);

	EXPECT_EQ(BlockKind::Paragraph, document.blocks[2].kind);
	EXPECT_EQ(L"safe", document.blocks[2].text);
}

TEST(MarkdownParser, ResolvesOnlyLocalResourcesInsideTheApprovedRoot)
{
	const auto document = ParseMarkdown(
		L"![local](images/local.png)\n\n"
		L"![root](/assets/root.png)\n\n"
		L"![traversal](../../outside.png)\n\n"
		L"![absolute](C:\\outside\\outside.png)\n\n"
		L"![external](https://example.test/image.png)\n\n"
		L"![data](data:image/png;base64,AAAA)\n\n"
		L"[fragment](#section) [script](javascript:boom()) [file](file:///C:/secret.txt)\n",
		{ L"C:\\workspace\\docs\\README.md", L"C:\\workspace" });

	ASSERT_EQ(7u, document.blocks.size());
	ASSERT_TRUE(document.blocks[0].image.has_value());
	EXPECT_EQ(ResourceDisposition::ResolvedLocal, document.blocks[0].image->source.disposition);
	EXPECT_EQ(L"C:\\workspace\\docs\\images\\local.png", document.blocks[0].image->source.resolvedPath);
	ASSERT_TRUE(document.blocks[1].image.has_value());
	EXPECT_EQ(ResourceDisposition::ResolvedLocal, document.blocks[1].image->source.disposition);
	EXPECT_EQ(L"C:\\workspace\\assets\\root.png", document.blocks[1].image->source.resolvedPath);
	ASSERT_TRUE(document.blocks[2].image.has_value());
	EXPECT_EQ(ResourceDisposition::OutsideAllowedRoots, document.blocks[2].image->source.disposition);
	ASSERT_TRUE(document.blocks[3].image.has_value());
	EXPECT_EQ(ResourceDisposition::OutsideAllowedRoots, document.blocks[3].image->source.disposition);
	ASSERT_TRUE(document.blocks[4].image.has_value());
	EXPECT_EQ(ResourceDisposition::ExternalBlocked, document.blocks[4].image->source.disposition);
	ASSERT_TRUE(document.blocks[5].image.has_value());
	EXPECT_EQ(ResourceDisposition::UnsafeSchemeBlocked, document.blocks[5].image->source.disposition);

	const auto& links = document.blocks[6];
	ASSERT_EQ(3u, links.inlineSpans.size());
	EXPECT_EQ(ResourceDisposition::Fragment, links.inlineSpans[0].resource->disposition);
	EXPECT_EQ(ResourceDisposition::UnsafeSchemeBlocked, links.inlineSpans[1].resource->disposition);
	EXPECT_EQ(ResourceDisposition::UnsafeSchemeBlocked, links.inlineSpans[2].resource->disposition);
}

TEST(MarkdownParser, ProjectsExtendedGfmAndNativeOnlyFallbackNodesFromOneFixture)
{
	const auto document = ParseMarkdown(
		L"---\n"
		L"title: Native preview\n"
		L"tags:\n"
		L"  - editor\n"
		L"---\n"
		L"# Extended syntax\n\n"
		L"- [ ] unchecked\n"
		L"- [X] checked\n\n"
		L"~~removed~~ ~also~ ~~~plain~~~ <https://example.test/angle> https://example.test/bare "
		L"<irc://chat.example.test/room> <mail@example.test> www.example.test mail@example.test $z$\n\n"
		L"```C++ title=sample\n"
		L"int value = 1;\n"
		L"```\n\n"
		L"$$\n"
		L"x^2 + y^2\n"
		L"$$\n\n"
		L"```math\n"
		L"a/b\n"
		L"```\n\n"
		L"```mermaid\n"
		L"graph TD; A-->B\n"
		L"```\n\n"
		L":::mermaid\n"
		L"sequenceDiagram\n"
		L":::\n");

	ASSERT_EQ(10u, document.blocks.size());
	const auto& frontMatter = document.blocks[0];
	EXPECT_EQ(BlockKind::FrontMatter, frontMatter.kind);
	EXPECT_EQ(FrontMatterMode::Table, frontMatter.frontMatterMode);
	EXPECT_EQ(NativeFallbackKind::None, frontMatter.fallbackKind);
	ASSERT_EQ(3u, frontMatter.frontMatterFields.size());
	EXPECT_EQ(L"title", frontMatter.frontMatterFields[0].name);
	EXPECT_EQ(L"Native preview", frontMatter.frontMatterFields[0].value);
	EXPECT_EQ(L"  -", frontMatter.frontMatterFields[2].name);
	EXPECT_EQ(L"editor", frontMatter.frontMatterFields[2].value);

	EXPECT_EQ(BlockKind::Heading, document.blocks[1].kind);
	EXPECT_EQ(5u, document.blocks[1].sourceLine);
	EXPECT_EQ(TaskListState::Unchecked, document.blocks[2].taskListState);
	EXPECT_EQ(L"unchecked", document.blocks[2].text);
	EXPECT_EQ(TaskListState::Checked, document.blocks[3].taskListState);
	EXPECT_EQ(L"checked", document.blocks[3].text);

	const auto& inlineSyntax = document.blocks[4];
	EXPECT_EQ(L"removed also ~~~plain~~~ https://example.test/angle https://example.test/bare "
		L"irc://chat.example.test/room mail@example.test www.example.test mail@example.test z", inlineSyntax.text);
	EXPECT_EQ(2u, CountInline(inlineSyntax, InlineKind::Strikethrough));
	EXPECT_EQ(6u, CountInline(inlineSyntax, InlineKind::Autolink));
	EXPECT_EQ(1u, CountInline(inlineSyntax, InlineKind::Math));
	std::size_t blockedExternal = 0;
	std::size_t blockedUnsafeScheme = 0;
	for (const auto& span : inlineSyntax.inlineSpans) {
		if (span.kind == InlineKind::Autolink) {
			ASSERT_TRUE(span.resource.has_value());
			if (span.resource->disposition == ResourceDisposition::ExternalBlocked) ++blockedExternal;
			if (span.resource->disposition == ResourceDisposition::UnsafeSchemeBlocked) ++blockedUnsafeScheme;
		}
	}
	EXPECT_EQ(5u, blockedExternal);
	EXPECT_EQ(1u, blockedUnsafeScheme);

	EXPECT_EQ(BlockKind::CodeBlock, document.blocks[5].kind);
	EXPECT_EQ(L"c++", document.blocks[5].language);
	EXPECT_EQ(NativeFallbackKind::None, document.blocks[5].fallbackKind);
	for (std::size_t index = 6; index < document.blocks.size(); ++index) {
		EXPECT_EQ(NativeFallbackKind::LiteralSource, document.blocks[index].fallbackKind);
	}
	EXPECT_EQ(BlockKind::Math, document.blocks[6].kind);
	EXPECT_EQ(BlockKind::Math, document.blocks[7].kind);
	EXPECT_EQ(L"math", document.blocks[7].language);
	EXPECT_EQ(BlockKind::MermaidDiagram, document.blocks[8].kind);
	EXPECT_EQ(BlockKind::MermaidDiagram, document.blocks[9].kind);
}

TEST(MarkdownParser, FrontMatterModesAndUnsupportedYamlRemainExplicitAndInert)
{
	constexpr auto source = L"---\ntitle: Demo\ndefaults: &defaults\ncopy: *defaults\n---\nbody\n";

	ParseOptions hiddenOptions;
	hiddenOptions.frontMatterMode = FrontMatterMode::Hide;
	const auto hidden = ParseMarkdown(source, hiddenOptions);
	ASSERT_FALSE(hidden.blocks.empty());
	EXPECT_EQ(BlockKind::FrontMatter, hidden.blocks[0].kind);
	EXPECT_TRUE(hidden.blocks[0].text.empty());
	EXPECT_EQ(NativeFallbackKind::UnsupportedSyntax, hidden.blocks[0].fallbackKind);

	ParseOptions tableOptions;
	tableOptions.frontMatterMode = FrontMatterMode::Table;
	const auto table = ParseMarkdown(source, tableOptions);
	ASSERT_FALSE(table.blocks.empty());
	EXPECT_EQ(FrontMatterMode::Table, table.blocks[0].frontMatterMode);
	ASSERT_EQ(1u, table.blocks[0].frontMatterFields.size());
	EXPECT_EQ(L"title", table.blocks[0].frontMatterFields[0].name);

	ParseOptions codeOptions;
	codeOptions.frontMatterMode = FrontMatterMode::CodeBlock;
	const auto code = ParseMarkdown(source, codeOptions);
	ASSERT_FALSE(code.blocks.empty());
	EXPECT_EQ(FrontMatterMode::CodeBlock, code.blocks[0].frontMatterMode);
	EXPECT_NE(std::wstring::npos, code.blocks[0].text.find(L"defaults: &defaults"));
	EXPECT_EQ(BlockKind::Paragraph, code.blocks[1].kind);
	EXPECT_EQ(L"body", code.blocks[1].text);
}

TEST(MarkdownParser, EnforcesInputBlockImageHtmlAndFrontMatterLimits)
{
	ParseOptions inputOptions;
	inputOptions.limits.maximumInputCharacters = 4;
	const auto inputLimited = ParseMarkdown(L"abcdef", inputOptions);
	EXPECT_EQ(ParseCompletion::InputLimitReached, inputLimited.completion);
	ASSERT_EQ(1u, inputLimited.blocks.size());
	EXPECT_EQ(L"abcd", inputLimited.blocks[0].text);

	ParseOptions blockOptions;
	blockOptions.limits.maximumBlocks = 2;
	const auto blockLimited = ParseMarkdown(L"one\n\ntwo\n\nthree\n", blockOptions);
	EXPECT_EQ(ParseCompletion::BlockLimitReached, blockLimited.completion);
	ASSERT_EQ(2u, blockLimited.blocks.size());

	ParseOptions imageOptions;
	imageOptions.documentPath = L"C:\\workspace\\README.md";
	imageOptions.workspaceRoot = L"C:\\workspace";
	imageOptions.limits.maximumImages = 1;
	const auto imageLimited = ParseMarkdown(L"![one](one.png)\n\n![two](two.png)\n", imageOptions);
	ASSERT_EQ(2u, imageLimited.blocks.size());
	ASSERT_TRUE(imageLimited.blocks[1].image.has_value());
	EXPECT_EQ(ResourceDisposition::LimitExceeded, imageLimited.blocks[1].image->source.disposition);
	EXPECT_EQ(L"two.png", imageLimited.blocks[1].image->source.original);

	ParseOptions htmlOptions;
	htmlOptions.limits.maximumHtmlDepth = 1;
	const auto htmlLimited = ParseMarkdown(L"<div>outer<span>deep</span>end</div>", htmlOptions);
	ASSERT_EQ(1u, htmlLimited.blocks.size());
	EXPECT_EQ(L"outerend", htmlLimited.blocks[0].text);
	EXPECT_EQ(std::wstring::npos, htmlLimited.blocks[0].text.find(L"deep"));

	ParseOptions inlineDepthOptions;
	inlineDepthOptions.limits.maximumInlineDepth = 0;
	const auto inlineDepthLimited = ParseMarkdown(L"*literal emphasis*", inlineDepthOptions);
	ASSERT_EQ(1u, inlineDepthLimited.blocks.size());
	EXPECT_EQ(L"*literal emphasis*", inlineDepthLimited.blocks[0].text);
	EXPECT_EQ(NativeFallbackKind::LimitExceeded, inlineDepthLimited.blocks[0].fallbackKind);

	ParseOptions frontLineOptions;
	frontLineOptions.limits.maximumFrontMatterLines = 1;
	const auto frontLineLimited = ParseMarkdown(L"---\na: 1\nb: 2\n---\nbody\n", frontLineOptions);
	ASSERT_FALSE(frontLineLimited.blocks.empty());
	EXPECT_EQ(BlockKind::FrontMatter, frontLineLimited.blocks[0].kind);
	EXPECT_EQ(NativeFallbackKind::LimitExceeded, frontLineLimited.blocks[0].fallbackKind);

	ParseOptions frontFieldOptions;
	frontFieldOptions.limits.maximumFrontMatterFields = 1;
	const auto frontFieldLimited = ParseMarkdown(L"---\na: 1\nb: 2\n---\n", frontFieldOptions);
	ASSERT_FALSE(frontFieldLimited.blocks.empty());
	EXPECT_EQ(NativeFallbackKind::LimitExceeded, frontFieldLimited.blocks[0].fallbackKind);
	ASSERT_EQ(1u, frontFieldLimited.blocks[0].frontMatterFields.size());
}

TEST(MarkdownParser, BoundsAdversarialInlineSearchAndKeepsLongUtf16ScanSemantics)
{
	std::wstring unmatched(8192, L'[');
	const auto bounded = ParseMarkdown(unmatched);
	ASSERT_EQ(1u, bounded.blocks.size());
	EXPECT_EQ(NativeFallbackKind::LimitExceeded, bounded.blocks[0].fallbackKind);
	EXPECT_EQ(unmatched, bounded.blocks[0].text);

	std::wstring unterminatedHtml(8192, L'<');
	const auto boundedHtml = ParseMarkdown(unterminatedHtml);
	ASSERT_EQ(1u, boundedHtml.blocks.size());
	EXPECT_EQ(NativeFallbackKind::LimitExceeded, boundedHtml.blocks[0].fallbackKind);
	EXPECT_EQ(unterminatedHtml, boundedHtml.blocks[0].text);

	std::wstring parenthesizedUrl = L"https://example.test/";
	parenthesizedUrl.append(8192, L')');
	const auto boundedUrl = ParseMarkdown(parenthesizedUrl);
	ASSERT_EQ(1u, boundedUrl.blocks.size());
	EXPECT_EQ(1u, CountInline(boundedUrl.blocks[0], InlineKind::Autolink));
	EXPECT_EQ(NativeFallbackKind::None, boundedUrl.blocks[0].fallbackKind);

	std::wstring longSource(80, L'a');
	longSource.append(L" 日本語 ~~removed~~ <https://example.test/long>");
	const auto longDocument = ParseMarkdown(longSource);
	ASSERT_EQ(1u, longDocument.blocks.size());
	EXPECT_EQ(1u, CountInline(longDocument.blocks[0], InlineKind::Strikethrough));
	EXPECT_EQ(1u, CountInline(longDocument.blocks[0], InlineKind::Autolink));
	EXPECT_EQ(NativeFallbackKind::None, longDocument.blocks[0].fallbackKind);
}

TEST(MarkdownParser, Utf16DispatchCallerPreservesMixedLineModel)
{
	// The CR is code unit 63 and the LF is code unit 64, so the pair crosses
	// every currently supported UTF-16 vector width (8, 16, and 32 units).
	std::wstring boundaryHeading = L"# ";
	boundaryHeading.append(61, L'x');
	ASSERT_EQ(63u, boundaryHeading.size());

	std::wstring surrogateHeading = L"# ";
	surrogateHeading.push_back(static_cast<wchar_t>(0xd83d));
	surrogateHeading.push_back(static_cast<wchar_t>(0xde80));
	surrogateHeading.append(L" lone ");
	surrogateHeading.push_back(static_cast<wchar_t>(0xd800));

	std::wstring source = boundaryHeading;
	source.append(L"\r\n");
	source.append(L"\n");
	source.append(L"# 日本語\r");
	source.append(surrogateHeading);
	source.append(L"\r\n");
	source.append(L"\r");
	source.append(L"# last\n");

	const auto document = ParseMarkdown(source);
	ASSERT_EQ(4u, document.blocks.size());
	for (const auto& block : document.blocks) {
		EXPECT_EQ(BlockKind::Heading, block.kind);
	}
	EXPECT_EQ(std::wstring(61, L'x'), document.blocks[0].text);
	EXPECT_EQ(L"日本語", document.blocks[1].text);

	std::wstring expectedSurrogateText;
	expectedSurrogateText.push_back(static_cast<wchar_t>(0xd83d));
	expectedSurrogateText.push_back(static_cast<wchar_t>(0xde80));
	expectedSurrogateText.append(L" lone ");
	expectedSurrogateText.push_back(static_cast<wchar_t>(0xd800));
	EXPECT_EQ(expectedSurrogateText, document.blocks[2].text);
	EXPECT_EQ(L"last", document.blocks[3].text);

	EXPECT_EQ(0u, document.blocks[0].sourceLine);
	EXPECT_EQ(2u, document.blocks[1].sourceLine);
	EXPECT_EQ(3u, document.blocks[2].sourceLine);
	EXPECT_EQ(5u, document.blocks[3].sourceLine);
	EXPECT_TRUE(ParseMarkdown(L"").blocks.empty());
}

TEST(MarkdownParser, Utf16DispatchCallerCoversSpecialPositionsAndLargeCorpora)
{
	// One literal paragraph begins with a special, ends with a special, and
	// contains all ten scanner kinds. None is paired into Markdown syntax, so
	// the exact external model remains the source text.
	const std::wstring everySpecial = L"\\ ` ! [ * _ ~ < & $";
	const auto specialDocument = ParseMarkdown(everySpecial);
	ASSERT_EQ(1u, specialDocument.blocks.size());
	EXPECT_EQ(everySpecial, specialDocument.blocks[0].text);
	EXPECT_TRUE(specialDocument.blocks[0].inlineSpans.empty());

	const std::wstring noSpecial(160, L'x');
	const auto noSpecialDocument = ParseMarkdown(noSpecial);
	ASSERT_EQ(1u, noSpecialDocument.blocks.size());
	EXPECT_EQ(noSpecial, noSpecialDocument.blocks[0].text);

	std::wstring lastSpecial(160, L'x');
	lastSpecial.push_back(L'$');
	const auto lastSpecialDocument = ParseMarkdown(lastSpecial);
	ASSERT_EQ(1u, lastSpecialDocument.blocks.size());
	EXPECT_EQ(lastSpecial, lastSpecialDocument.blocks[0].text);

	constexpr std::size_t longLength = 3U * 1024U * 1024U;
	const std::wstring longLine(longLength, L'x');
	const auto longDocument = ParseMarkdown(longLine);
	ASSERT_EQ(1u, longDocument.blocks.size());
	EXPECT_EQ(ParseCompletion::Complete, longDocument.completion);
	EXPECT_EQ(longLine, longDocument.blocks[0].text);

	constexpr std::size_t shortLineCount = 4096;
	std::wstring shortLines;
	shortLines.reserve(shortLineCount * 4);
	for (std::size_t line = 0; line < shortLineCount; ++line) {
		shortLines.append(L"# x\n");
	}
	const auto manyLinesDocument = ParseMarkdown(shortLines);
	ASSERT_EQ(shortLineCount, manyLinesDocument.blocks.size());
	EXPECT_EQ(L"x", manyLinesDocument.blocks.front().text);
	EXPECT_EQ(L"x", manyLinesDocument.blocks.back().text);
	EXPECT_EQ(0u, manyLinesDocument.blocks.front().sourceLine);
	EXPECT_EQ(shortLineCount - 1, manyLinesDocument.blocks.back().sourceLine);
}

TEST(MarkdownParser, ExposesUnsupportedNativeCapabilitiesAsTypedBoundaries)
{
	const auto document = ParseMarkdown(L"text");
	EXPECT_EQ(CapabilityStatus::Supported, document.capabilities.localImageProjection);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.linkActivation);
	EXPECT_EQ(CapabilityStatus::Supported, document.capabilities.scrollPreviewWithEditor);
	EXPECT_EQ(CapabilityStatus::Supported, document.capabilities.scrollEditorWithPreview);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.rawHtmlExecution);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.mathTypesetting);
	EXPECT_EQ(CapabilityStatus::Supported, document.capabilities.mermaidFlowchartRendering);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.mermaidNonFlowchartRendering);
}

TEST(MarkdownParser, LiveUpdateWaitsForAStableRevisionAndEveryBranchTerminates)
{
	PreviewLiveUpdateModel model;
	EXPECT_EQ(LiveUpdateAction::AwaitStableRevision, model.Observe(10));
	EXPECT_TRUE(model.HasPendingUpdate());
	EXPECT_EQ(LiveUpdateAction::Render, model.Observe(10));
	model.Commit(10);
	EXPECT_EQ(LiveUpdateAction::None, model.Observe(10));
	EXPECT_FALSE(model.HasPendingUpdate());

	EXPECT_EQ(LiveUpdateAction::AwaitStableRevision, model.Observe(11));
	EXPECT_EQ(LiveUpdateAction::AwaitStableRevision, model.Observe(12));
	EXPECT_EQ(LiveUpdateAction::AwaitStableRevision, model.Observe(13));
	EXPECT_EQ(LiveUpdateAction::Render, model.Observe(13));
	model.Commit(13);
	EXPECT_EQ(13, model.RenderedRevision());
	EXPECT_EQ(LiveUpdateAction::None, model.Observe(13));

	model.Reset();
	EXPECT_EQ(-1, model.RenderedRevision());
	EXPECT_FALSE(model.HasPendingUpdate());
}

} // namespace
} // namespace markdown

namespace markdown {
namespace {

//! The concatenated text of the first block, which is what the preview draws.
[[nodiscard]] std::wstring FirstText(std::wstring_view source)
{
	const auto document = ParseMarkdown(source);
	return document.blocks.empty() ? std::wstring{} : document.blocks.front().text;
}

TEST(MarkdownParserGfm, PairsEmphasisWithTheCommonMarkDelimiterStack)
{
	// Forward pairing cannot resolve these: the closing run of `***all***` has
	// to be split between the strong and the emphasis span.
	const auto document = ParseMarkdown(L"***all*** and **a *b* c** and _**x**_");
	ASSERT_EQ(1u, document.blocks.size());
	const auto& block = document.blocks.front();
	EXPECT_EQ(L"all and a b c and x", block.text);
	EXPECT_EQ(3u, CountInline(block, InlineKind::Strong));
	EXPECT_EQ(3u, CountInline(block, InlineKind::Emphasis));
}

TEST(MarkdownParserGfm, LeavesIntrawordUnderscoresAlone)
{
	EXPECT_EQ(L"a_b_c", FirstText(L"a_b_c"));
	const auto document = ParseMarkdown(L"snake_case_name and _real_");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(L"snake_case_name and real", document.blocks.front().text);
	EXPECT_EQ(1u, CountInline(document.blocks.front(), InlineKind::Emphasis));
}

TEST(MarkdownParserGfm, KeepsStrikethroughToOneOrTwoTildes)
{
	const auto document = ParseMarkdown(L"~~gone~~ but ~~~kept~~~");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(1u, CountInline(document.blocks.front(), InlineKind::Strikethrough));
	EXPECT_NE(std::wstring::npos, document.blocks.front().text.find(L"~~~kept~~~"));
}

TEST(MarkdownParserGfm, DecodesNamedAndNumericEntities)
{
	EXPECT_EQ(L"\u00a9 \u2014 \u2026 \u00a0 \u00a9 \u2014",
		FirstText(L"&copy; &mdash; &hellip; &nbsp; &#169; &#x2014;"));
	// An unknown name stays literal rather than becoming a replacement character.
	EXPECT_EQ(L"&notanentity;", FirstText(L"&notanentity;"));
}

TEST(MarkdownParserGfm, ReplacesEmojiShortcodes)
{
	EXPECT_EQ(L"Ship it \U0001f680 \U0001f44d", FirstText(L"Ship it :rocket: :+1:"));
	// A shortcode inside a code span is verbatim, as on GitHub.
	const auto document = ParseMarkdown(L"`:rocket:` stays");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(L":rocket: stays", document.blocks.front().text);
	EXPECT_EQ(1u, CountInline(document.blocks.front(), InlineKind::Code));
	EXPECT_EQ(L"a :notanemoji: b", FirstText(L"a :notanemoji: b"));
}

TEST(MarkdownParserGfm, ReadsIndentedCodeBlocks)
{
	const auto document = ParseMarkdown(L"paragraph\n\n    int x = 1;\n    int y = 2;\n");
	ASSERT_EQ(2u, document.blocks.size());
	EXPECT_EQ(BlockKind::Paragraph, document.blocks[0].kind);
	EXPECT_EQ(BlockKind::CodeBlock, document.blocks[1].kind);
	EXPECT_EQ(L"int x = 1;\nint y = 2;", document.blocks[1].text);
}

TEST(MarkdownParserGfm, DoesNotLetAnIndentedCodeBlockInterruptAParagraph)
{
	const auto document = ParseMarkdown(L"paragraph\n    still the paragraph\n");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(BlockKind::Paragraph, document.blocks[0].kind);
}

TEST(MarkdownParserGfm, KeepsHardLineBreaks)
{
	// Two trailing spaces and a trailing backslash are both GFM hard breaks;
	// a plain line ending is not.
	EXPECT_EQ(L"one\ntwo\nthree", FirstText(L"one  \ntwo" + std::wstring(1, L'\\') + L"\nthree"));
	EXPECT_EQ(L"one two", FirstText(L"one\ntwo"));
}

TEST(MarkdownParserGfm, AcceptsShortTableDelimiterCells)
{
	// GFM needs only one dash per column, so `|:-|-:|` is a valid delimiter row.
	const auto document = ParseMarkdown(L"| a | b |\n|:-|-:|\n| 1 | 2 |\n");
	ASSERT_EQ(1u, document.blocks.size());
	EXPECT_EQ(BlockKind::Table, document.blocks.front().kind);
	ASSERT_EQ(2u, document.blocks.front().tableAlignments.size());
	EXPECT_EQ(TableAlignment::Left, document.blocks.front().tableAlignments[0]);
	EXPECT_EQ(TableAlignment::Right, document.blocks.front().tableAlignments[1]);
}

} // namespace
} // namespace markdown
