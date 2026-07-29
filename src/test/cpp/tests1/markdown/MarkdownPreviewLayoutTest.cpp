/*! @file */
#include "pch.h"

#include "markdown/MarkdownPreviewLayout.h"

namespace markdown {
namespace {

TEST(MarkdownPreviewLayout, HiddenPreviewLeavesTheWholeCentralRegionToTheEditor)
{
	const auto layout = CalculateMarkdownPreviewLayout(120, 1000, 96, false);
	EXPECT_EQ(120, layout.editorLeft);
	EXPECT_EQ(1000, layout.editorRight);
	EXPECT_EQ(0, layout.PreviewWidth());
	EXPECT_EQ(1000, layout.previewLeft);
}

TEST(MarkdownPreviewLayout, VisiblePreviewIsAnEditorSiblingBeforeTheRightBoundary)
{
	const auto layout = CalculateMarkdownPreviewLayout(120, 1600, 144, true);
	EXPECT_EQ(120, layout.editorLeft);
	EXPECT_GT(layout.EditorWidth(), 0);
	EXPECT_GT(layout.PreviewWidth(), 0);
	EXPECT_EQ(layout.editorRight, layout.dividerLeft);
	EXPECT_EQ(layout.dividerRight, layout.previewLeft);
	EXPECT_EQ(1600, layout.previewRight);
}

TEST(MarkdownPreviewLayout, ReservesExactlyOneThemedDividerDipBetweenTheSiblingPanes)
{
	const auto layout96 = CalculateMarkdownPreviewLayout(0, 1200, 96, true);
	const auto layout192 = CalculateMarkdownPreviewLayout(0, 2400, 192, true);
	EXPECT_EQ(1, layout96.dividerRight - layout96.dividerLeft);
	EXPECT_EQ(2, layout192.dividerRight - layout192.dividerLeft);
	EXPECT_EQ(layout96.editorRight, layout96.dividerLeft);
	EXPECT_EQ(layout192.previewLeft, layout192.dividerRight);
}

TEST(MarkdownPreviewLayout, VeryNarrowRegionsNeverProduceInvertedBounds)
{
	for (const int width : { 0, 1, 2, 7, 31 }) {
		const auto layout = CalculateMarkdownPreviewLayout(10, 10 + width, 192, true);
		EXPECT_LE(layout.editorLeft, layout.editorRight);
		EXPECT_LE(layout.dividerLeft, layout.dividerRight);
		EXPECT_LE(layout.previewLeft, layout.previewRight);
		EXPECT_EQ(10 + width, layout.previewRight);
	}
}

} // namespace
} // namespace markdown
