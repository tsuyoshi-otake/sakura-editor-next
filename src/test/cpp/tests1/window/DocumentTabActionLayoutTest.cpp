/*! @file */
#include "pch.h"

#include "view/LineNumberLayout.h"
#include "view/RulerLayout.h"
#include "window/DocumentBreadcrumbs.h"
#include "window/DocumentTabActionLayout.h"

namespace tabbar {
namespace {

TEST(DocumentTabActionLayout, FirstTabStartsAtTheEditorPartContentEdge)
{
	EXPECT_EQ(0, CalculateDocumentTabControlLeftInset());
}

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

TEST(DocumentTabActionLayout, UsesFourDipRhythmWhenPreviewIsUnavailable)
{
	const auto layout = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, false, true);
	EXPECT_EQ(940, layout.tabControlRight);
	EXPECT_EQ(60, layout.reservedRight);
	EXPECT_EQ(944, layout.list.left);
	EXPECT_EQ(968, layout.list.right);
	EXPECT_EQ(972, layout.close.left);
	EXPECT_EQ(996, layout.close.right);
	ExpectOrderedWithoutOverlap(layout, false);
}

TEST(DocumentTabActionLayout, MarkdownAddsPreviewBeforeTheExistingRightActions)
{
	const auto layout = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true, true);
	EXPECT_EQ(912, layout.tabControlRight);
	EXPECT_EQ(88, layout.reservedRight);
	EXPECT_EQ(916, layout.preview.left);
	EXPECT_EQ(940, layout.preview.right);
	EXPECT_EQ(944, layout.list.left);
	EXPECT_EQ(972, layout.close.left);
	ExpectOrderedWithoutOverlap(layout, true);
}

TEST(DocumentTabActionLayout, ActionGeometryScalesAtSupportedDpis)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const int width = ScaleDocumentTabDip(1000, dpi);
		const int height = ScaleDocumentTabDip(32, dpi);
		const int button = ScaleDocumentTabDip(24, dpi);
		const int glyph = ScaleDocumentTabDip(16, dpi);
		const int gap = ScaleDocumentTabDip(4, dpi);
		const int outerPadding = ScaleDocumentTabDip(4, dpi);
		const auto normal = CalculateDocumentTabActionLayout(0, 0, width, height, dpi, false, true);
		const auto markdown = CalculateDocumentTabActionLayout(0, 0, width, height, dpi, true, true);

		// Each primitive is rounded independently before the action-cluster width
		// is composed, preserving the 4-DIP rhythm at fractional DPI.
		EXPECT_EQ(outerPadding * 2 + button * 2 + gap, normal.reservedRight);
		EXPECT_EQ(outerPadding * 2 + button * 3 + gap * 2, markdown.reservedRight);
		EXPECT_EQ(button, markdown.preview.right - markdown.preview.left);
		EXPECT_EQ(gap, markdown.list.left - markdown.preview.right);
		EXPECT_EQ(gap, markdown.close.left - markdown.list.right);
		const auto glyphBounds = CalculateDocumentTabActionGlyphBounds(markdown.preview, dpi);
		EXPECT_EQ(glyph, glyphBounds.Width());
		EXPECT_EQ(glyph, glyphBounds.Height());
		EXPECT_EQ((button - glyph) / 2, glyphBounds.left - markdown.preview.left);
		EXPECT_EQ((button - glyph) / 2, glyphBounds.top - markdown.preview.top);
		ExpectOrderedWithoutOverlap(normal, false);
		ExpectOrderedWithoutOverlap(markdown, true);
	}
}

TEST(DocumentTabActionLayout, HitTestingUsesHalfOpenButtonBounds)
{
	const auto layout = CalculateDocumentTabActionLayout(100, 20, 1100, 52, 96, true, true);
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
	const auto withoutSizeBox = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true, true, 0);
	const auto withSizeBox = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, true, true, 17);
	EXPECT_EQ(withoutSizeBox.tabControlRight - 17, withSizeBox.tabControlRight);
	EXPECT_EQ(withoutSizeBox.preview.left - 17, withSizeBox.preview.left);
	EXPECT_EQ(withoutSizeBox.list.left - 17, withSizeBox.list.left);
	EXPECT_EQ(withoutSizeBox.close.right - 17, withSizeBox.close.right);
	EXPECT_EQ(105, withSizeBox.reservedRight);
	ExpectOrderedWithoutOverlap(withSizeBox, true);
}

TEST(DocumentTabActionLayout, NarrowClientsNeverInvertOrOverlapBounds)
{
	for (const int width : { 0, 1, 8, 16, 31, 47, 69, 70 }) {
		for (const int sizeBox : { 0, 17, 80 }) {
			const auto layout = CalculateDocumentTabActionLayout(10, 4, 10 + width, 36, 192, true, true, sizeBox);
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

TEST(DocumentTabActionLayout, CurrentDocumentCloseDoesNotDuplicatePerTabClose)
{
	const auto layout = CalculateDocumentTabActionLayout(0, 0, 1000, 32, 96, false, false);
	EXPECT_EQ(968, layout.tabControlRight);
	EXPECT_EQ(32, layout.reservedRight);
	EXPECT_TRUE(layout.close.IsEmpty());
	EXPECT_EQ(972, layout.list.left);
	EXPECT_EQ(996, layout.list.right);
	EXPECT_EQ(DocumentTabAction::TabList,
		HitTestDocumentTabAction(layout, 990, layout.list.top));
}

TEST(DocumentTabActionLayout, UsesOfficialMarkdownPreviewIdentifiers)
{
	EXPECT_STREQ(L"markdown.showPreviewToSide", kMarkdownPreviewCommandId);
	EXPECT_STREQ(L"open-preview", kMarkdownPreviewCodiconId);
}

TEST(DocumentTabActionLayout, ResolvesNormalHoverAndPressedStatesExplicitly)
{
	EXPECT_EQ(DocumentTabActionVisualState::Normal,
		ResolveDocumentTabActionVisualState(false, false, false));
	EXPECT_EQ(DocumentTabActionVisualState::Hovered,
		ResolveDocumentTabActionVisualState(true, false, false));
	EXPECT_EQ(DocumentTabActionVisualState::Hovered,
		ResolveDocumentTabActionVisualState(true, true, false));
	EXPECT_EQ(DocumentTabActionVisualState::Pressed,
		ResolveDocumentTabActionVisualState(true, true, true));
}

TEST(DocumentTabContentLayout, NativeMeasurementPreservesCaptionAndCloseAtSupportedDpis)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		for (const bool showIcon : { false, true }) {
			for (const bool showClose : { false, true }) {
				const int captionWidth = ScaleDocumentTabDip(56, dpi);
				const int nativeImageWidth = showIcon ? ScaleDocumentTabDip(16, dpi) : 0;
				const int nativePadding = ScaleDocumentTabDip(
					CalculateDocumentTabNativeHorizontalPaddingDip(showIcon, showClose), dpi);
				const int tabWidth = captionWidth + nativeImageWidth + nativePadding * 2;
				const auto content = CalculateDocumentTabContentLayout(
					{ 0, 0, tabWidth, ScaleDocumentTabDip(32, dpi) }, dpi, showIcon, showClose);

				EXPECT_GE(content.text.Width(), captionWidth);
				EXPECT_EQ(showIcon, !content.icon.IsEmpty());
				EXPECT_EQ(showClose, !content.close.IsEmpty());
				if (!content.icon.IsEmpty()) EXPECT_LE(content.icon.right, content.text.left);
				if (!content.close.IsEmpty()) {
					EXPECT_LE(content.text.right, content.close.left);
					EXPECT_EQ(ScaleDocumentTabDip(24, dpi), content.close.Width());
					EXPECT_EQ(ScaleDocumentTabDip(24, dpi), content.close.Height());
				}
			}
		}
	}
	EXPECT_EQ(8, CalculateDocumentTabNativeHorizontalPaddingDip(false, false));
	EXPECT_EQ(10, CalculateDocumentTabNativeHorizontalPaddingDip(true, false));
	EXPECT_EQ(20, CalculateDocumentTabNativeHorizontalPaddingDip(false, true));
	EXPECT_EQ(22, CalculateDocumentTabNativeHorizontalPaddingDip(true, true));
}

TEST(DocumentTabContentLayout, NarrowTabsNeverOverlapCloseOrInvertText)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		for (int width = 0; width <= ScaleDocumentTabDip(80, dpi); ++width) {
			const auto content = CalculateDocumentTabContentLayout(
				{ 10, 4, 10 + width, 4 + ScaleDocumentTabDip(32, dpi) }, dpi, true, true);
			EXPECT_LE(content.icon.left, content.icon.right);
			EXPECT_LE(content.text.left, content.text.right);
			EXPECT_LE(content.close.left, content.close.right);
			if (!content.icon.IsEmpty()) EXPECT_LE(content.icon.right, content.text.left);
			if (!content.close.IsEmpty()) EXPECT_LE(content.text.right, content.close.left);
		}
	}
}

TEST(RulerLayout, MajorLabelsKeepNeutralFourDipSeparationAtSupportedDpis)
{
	for (const unsigned int dpi : { 96U, 144U, 192U }) {
		const int areaLeft = ScaleDocumentTabDip(50, dpi);
		const auto first = view::ruler::CalculateMajorLabelPosition(areaLeft, areaLeft, dpi);
		EXPECT_EQ(areaLeft + view::ruler::ScaleDip(4, dpi), first.x);
		EXPECT_EQ(0, first.y);

		const auto clippedTick = view::ruler::CalculateMajorLabelPosition(
			areaLeft - ScaleDocumentTabDip(20, dpi), areaLeft, dpi);
		EXPECT_EQ(first.x, clippedTick.x);
	}
}

TEST(DocumentTabActionLayout, InconsistentConfiguredWidthsStillPreserveReadableMinimum)
{
	EXPECT_EQ(120, CalculateDocumentTabItemWidth(1000, 1, 120, 48));
	EXPECT_EQ(120, CalculateDocumentTabItemWidth(40, 8, 120, 48));
	EXPECT_EQ(200, CalculateDocumentTabItemWidth(1000, 1, 120, 200));
	EXPECT_EQ(0, CalculateDocumentTabItemWidth(1000, 0, 120, 200));
}

TEST(DocumentBreadcrumbs, UsesWorkspaceRelativeFilesystemSegments)
{
	const auto result = breadcrumbs::BuildDocumentBreadcrumbs(
		LR"(C:\repo\src\main.cpp)", { LR"(C:\repo)" });
	EXPECT_TRUE(result.workspaceRelative);
	EXPECT_EQ((std::vector<std::wstring>{ L"src", L"main.cpp" }), result.segments);
}

TEST(DocumentBreadcrumbs, LongestMatchingMultiRootWinsCaseInsensitively)
{
	const auto result = breadcrumbs::BuildDocumentBreadcrumbs(
		LR"(C:\REPO\packages\editor\main.cpp)",
		{ LR"(c:\repo)", LR"(c:\repo\packages\editor)" });
	EXPECT_TRUE(result.workspaceRelative);
	EXPECT_EQ((std::vector<std::wstring>{ L"main.cpp" }), result.segments);
}

TEST(DocumentBreadcrumbs, OutsideWorkspaceNeverDisplaysAbsolutePath)
{
	const auto result = breadcrumbs::BuildDocumentBreadcrumbs(
		LR"(D:\external\notes.txt)", { LR"(C:\repo)" });
	EXPECT_FALSE(result.workspaceRelative);
	EXPECT_EQ((std::vector<std::wstring>{ L"notes.txt" }), result.segments);
}

TEST(DocumentBreadcrumbs, UntitledInputHasNoFilesystemBreadcrumbs)
{
	const auto result = breadcrumbs::BuildDocumentBreadcrumbs(L"", { LR"(C:\repo)" });
	EXPECT_FALSE(result.workspaceRelative);
	EXPECT_TRUE(result.segments.empty());
}

TEST(LineNumberLayout, UsesVsCodeMinimumAndDecorationDefaults)
{
	const auto layout = view::line_number::CalculateWorkbenchLineNumberLayout(42, 8, 96);
	EXPECT_EQ(5, layout.digitCount);
	EXPECT_EQ(40, layout.lineNumbersWidth);
	EXPECT_EQ(10, layout.decorationsWidth);
	EXPECT_EQ(50, layout.totalWidth);
	EXPECT_EQ(32, view::line_number::RightAlignedDigitX(layout, 1));
}

TEST(LineNumberLayout, ExpandsForActualDigitsAndScalesOnlyDecorations)
{
	const auto layout = view::line_number::CalculateWorkbenchLineNumberLayout(123456, 9, 144);
	EXPECT_EQ(6, layout.digitCount);
	EXPECT_EQ(54, layout.lineNumbersWidth);
	EXPECT_EQ(15, layout.decorationsWidth);
	EXPECT_EQ(69, layout.totalWidth);
	EXPECT_EQ(0, view::line_number::RightAlignedDigitX(layout, 6));
}

TEST(LineNumberLayout, TreatsZeroDpiAndEmptyDocumentsDeterministically)
{
	const auto layout = view::line_number::CalculateWorkbenchLineNumberLayout(0, 0, 0);
	EXPECT_EQ(5, layout.digitCount);
	EXPECT_EQ(1, layout.maxDigitWidth);
	EXPECT_EQ(10, layout.decorationsWidth);
	EXPECT_EQ(4, view::line_number::RightAlignedDigitX(layout, 1));
}

} // namespace
} // namespace tabbar
