/*! @file */
#include "pch.h"

#include "markdown/MarkdownPreviewScrollMap.h"

#include <vector>

namespace markdown {
namespace {

struct RenderLine final {
	std::size_t sourceLine = 0;
	int top = 0;
};

//! Source lines 0, 2, 5 and 9 render at four ascending rows. The gaps are the
//! blank/continuation lines a Markdown block absorbs.
[[nodiscard]] std::vector<RenderLine> SampleLines()
{
	return { { 0, 0 }, { 2, 40 }, { 5, 90 }, { 9, 150 } };
}

TEST(MarkdownPreviewScrollMap, EmptyLayoutMapsNeitherDirection)
{
	const std::vector<RenderLine> empty;
	EXPECT_FALSE(PreviewTopForSourceLine(empty, 0).has_value());
	EXPECT_FALSE(PreviewTopForSourceLine(empty, 7).has_value());
	EXPECT_FALSE(SourceLineForPreviewScroll(empty, 0).has_value());
	EXPECT_FALSE(SourceLineForPreviewScroll(empty, 500).has_value());
}

TEST(MarkdownPreviewScrollMap, EditorLineMapsToTheFirstRenderedRowAtOrAfterIt)
{
	const auto lines = SampleLines();
	EXPECT_EQ(0, PreviewTopForSourceLine(lines, 0));
	EXPECT_EQ(40, PreviewTopForSourceLine(lines, 1));
	EXPECT_EQ(40, PreviewTopForSourceLine(lines, 2));
	EXPECT_EQ(90, PreviewTopForSourceLine(lines, 3));
	EXPECT_EQ(90, PreviewTopForSourceLine(lines, 5));
	EXPECT_EQ(150, PreviewTopForSourceLine(lines, 6));
	EXPECT_EQ(150, PreviewTopForSourceLine(lines, 9));
}

TEST(MarkdownPreviewScrollMap, EditorLinePastTheLastRenderedRowClampsToIt)
{
	const auto lines = SampleLines();
	EXPECT_EQ(150, PreviewTopForSourceLine(lines, 10));
	EXPECT_EQ(150, PreviewTopForSourceLine(lines, 1000000));
}

TEST(MarkdownPreviewScrollMap, PreviewPositionMapsToTheSourceLineOwningTheTopRow)
{
	const auto lines = SampleLines();
	EXPECT_EQ(0u, SourceLineForPreviewScroll(lines, 0));
	EXPECT_EQ(0u, SourceLineForPreviewScroll(lines, 39));
	EXPECT_EQ(2u, SourceLineForPreviewScroll(lines, 40));
	EXPECT_EQ(2u, SourceLineForPreviewScroll(lines, 89));
	EXPECT_EQ(5u, SourceLineForPreviewScroll(lines, 90));
	EXPECT_EQ(9u, SourceLineForPreviewScroll(lines, 150));
}

TEST(MarkdownPreviewScrollMap, PreviewPositionOutsideTheLayoutClampsInsideTheDocument)
{
	const auto lines = SampleLines();
	EXPECT_EQ(0u, SourceLineForPreviewScroll(lines, -1));
	EXPECT_EQ(0u, SourceLineForPreviewScroll(lines, -100000));
	EXPECT_EQ(9u, SourceLineForPreviewScroll(lines, 100000));
}

//! The two directions must agree at every mapped row, or a synchronized scroll
//! would drift a little further on each round trip.
TEST(MarkdownPreviewScrollMap, RoundTripFromARenderedRowIsStable)
{
	const auto lines = SampleLines();
	for (const auto& line : lines) {
		const auto top = PreviewTopForSourceLine(lines, line.sourceLine);
		ASSERT_TRUE(top.has_value());
		const auto back = SourceLineForPreviewScroll(lines, *top);
		ASSERT_TRUE(back.has_value());
		EXPECT_EQ(line.sourceLine, *back);
		EXPECT_EQ(top, PreviewTopForSourceLine(lines, *back));
	}
}

TEST(MarkdownPreviewScrollMap, ASingleRenderedRowAbsorbsEveryPosition)
{
	const std::vector<RenderLine> lines = { { 3, 12 } };
	EXPECT_EQ(12, PreviewTopForSourceLine(lines, 0));
	EXPECT_EQ(12, PreviewTopForSourceLine(lines, 3));
	EXPECT_EQ(12, PreviewTopForSourceLine(lines, 99));
	EXPECT_EQ(3u, SourceLineForPreviewScroll(lines, -5));
	EXPECT_EQ(3u, SourceLineForPreviewScroll(lines, 12));
	EXPECT_EQ(3u, SourceLineForPreviewScroll(lines, 5000));
}

} // namespace
} // namespace markdown
