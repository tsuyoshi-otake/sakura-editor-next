/*! @file */
#include "pch.h"

#include "markdown/MarkdownParser.h"

#include <algorithm>

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

TEST(MarkdownParser, ExposesUnsupportedNativeCapabilitiesAsTypedBoundaries)
{
	const auto document = ParseMarkdown(L"text");
	EXPECT_EQ(CapabilityStatus::Supported, document.capabilities.localImageProjection);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.linkActivation);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.editorPreviewScrollSync);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.rawHtmlExecution);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.mathTypesetting);
	EXPECT_EQ(CapabilityStatus::Unsupported, document.capabilities.mermaidDiagramRendering);
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
