/*! @file */
#include "pch.h"

#include "window/DocumentTabActionLayout.h"

namespace tabbar {
namespace {

void ExpectOrderedWithoutOverlap(const DocumentTabActionLayout& layout, bool expectPreview)
{
	EXPECT_LE(layout.tabControlRight, expectPreview ? layout.preview.left : layout.list.left);
	if (expectPreview) {
		EXPECT_FALSE(layout.preview.IsEmpty());
		EXPECT_LE(layout.preview.right, layout.list.left);
	} else {
		EXPECT_TRUE(layout.preview.IsEmpty());
	}
	EXPECT_LE(layout.list.right, layout.close.left);
}

TEST(DocumentTabActionLayout, PreservesLegacyMarginWhenPreviewIsUnavailable)
{
	const auto layout = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, false);
	EXPECT_EQ(953, layout.tabControlRight);
	EXPECT_EQ(47, layout.reservedRight);
	EXPECT_EQ(957, layout.list.left);
	EXPECT_EQ(973, layout.list.right);
	EXPECT_EQ(980, layout.close.left);
	EXPECT_EQ(996, layout.close.right);
	ExpectOrderedWithoutOverlap(layout, false);
}

TEST(DocumentTabActionLayout, MarkdownAddsPreviewBeforeTheExistingRightActions)
{
	const auto layout = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true);
	EXPECT_EQ(930, layout.tabControlRight);
	EXPECT_EQ(70, layout.reservedRight);
	EXPECT_EQ(934, layout.preview.left);
	EXPECT_EQ(950, layout.preview.right);
	EXPECT_EQ(957, layout.list.left);
	EXPECT_EQ(980, layout.close.left);
	ExpectOrderedWithoutOverlap(layout, true);
}

TEST(DocumentTabActionLayout, ActionGeometryScalesAtSupportedDpis)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const int width = ScaleDocumentTabDip(1000, dpi);
		const int height = ScaleDocumentTabDip(32, dpi);
		const auto normal = CalculateDocumentTabActionLayout(0, 0, width, height, dpi, false);
		const auto markdown = CalculateDocumentTabActionLayout(0, 0, width, height, dpi, true);

		EXPECT_EQ(ScaleDocumentTabDip(47, dpi), normal.reservedRight);
		EXPECT_EQ(ScaleDocumentTabDip(70, dpi), markdown.reservedRight);
		EXPECT_EQ(ScaleDocumentTabDip(16, dpi), markdown.preview.right - markdown.preview.left);
		EXPECT_EQ(ScaleDocumentTabDip(7, dpi), markdown.list.left - markdown.preview.right);
		EXPECT_EQ(ScaleDocumentTabDip(7, dpi), markdown.close.left - markdown.list.right);
		ExpectOrderedWithoutOverlap(normal, false);
		ExpectOrderedWithoutOverlap(markdown, true);
	}
}

TEST(DocumentTabActionLayout, HitTestingUsesHalfOpenButtonBounds)
{
	const auto layout = CalculateDocumentTabActionLayout(100, 20, 1100, 52, 96, true);
	EXPECT_EQ(DocumentTabAction::MarkdownPreview,
		HitTestDocumentTabAction(layout, layout.preview.left, layout.preview.top));
	EXPECT_EQ(DocumentTabAction::MarkdownPreview,
		HitTestDocumentTabAction(layout, layout.preview.right - 1, layout.preview.bottom - 1));
	EXPECT_EQ(DocumentTabAction::None,
		HitTestDocumentTabAction(layout, layout.preview.right, layout.preview.top));
	EXPECT_EQ(DocumentTabAction::None,
		HitTestDocumentTabAction(layout, layout.preview.left, layout.preview.bottom));
	EXPECT_EQ(DocumentTabAction::TabList,
		HitTestDocumentTabAction(layout, layout.list.left, layout.list.top));
	EXPECT_EQ(DocumentTabAction::Close,
		HitTestDocumentTabAction(layout, layout.close.right - 1, layout.close.bottom - 1));
}

TEST(DocumentTabActionLayout, SizeBoxShiftsTheWholeActionClusterWithoutOverlap)
{
	const auto withoutSizeBox = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true, 0);
	const auto withSizeBox = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true, 17);
	EXPECT_EQ(withoutSizeBox.tabControlRight - 17, withSizeBox.tabControlRight);
	EXPECT_EQ(withoutSizeBox.preview.left - 17, withSizeBox.preview.left);
	EXPECT_EQ(withoutSizeBox.list.left - 17, withSizeBox.list.left);
	EXPECT_EQ(withoutSizeBox.close.right - 17, withSizeBox.close.right);
	EXPECT_EQ(87, withSizeBox.reservedRight);
	ExpectOrderedWithoutOverlap(withSizeBox, true);
}

TEST(DocumentTabActionLayout, NarrowClientsNeverInvertOrOverlapBounds)
{
	for (const int width : { 0, 1, 8, 16, 31, 47, 69, 70 }) {
		for (const int sizeBox : { 0, 17, 80 }) {
			const auto layout = CalculateDocumentTabActionLayout(10, 4, 10 + width, 36, 192, true, sizeBox);
			EXPECT_GE(layout.tabControlRight, 10);
			EXPECT_LE(layout.tabControlRight, 10 + width);
			EXPECT_LE(layout.preview.left, layout.preview.right);
			EXPECT_LE(layout.list.left, layout.list.right);
			EXPECT_LE(layout.close.left, layout.close.right);
			if (!layout.preview.IsEmpty() && !layout.list.IsEmpty()) {
				EXPECT_LE(layout.preview.right, layout.list.left);
			}
			if (!layout.list.IsEmpty() && !layout.close.IsEmpty()) {
				EXPECT_LE(layout.list.right, layout.close.left);
			}
		}
	}
}

} // namespace
} // namespace tabbar
