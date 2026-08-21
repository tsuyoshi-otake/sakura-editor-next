/*! @file */
#include "pch.h"
#include "terminal/window/TerminalRenderMapping.h"
#include "terminal/window/TerminalScrollbarLayout.h"
#include "terminal/window/TerminalViewportGeometry.h"

#include <array>
#include <utility>

namespace {

void WriteLine( terminal::TerminalModel& model, std::wstring_view text )
{
	for( const auto character : text ) model.Print(character);
	model.ExecuteControl(L'\r');
	model.ExecuteControl(L'\n');
}

TEST(TerminalRenderMapping, CalculatesViewportFromScrollbackWithoutWalkingTheTree)
{
	terminal::TerminalModel model(6, 2, 10);
	WriteLine(model, L"one");
	WriteLine(model, L"two");
	WriteLine(model, L"three");
	ASSERT_GT(model.ScrollbackSize(), 0u);
	const auto bottom = terminal::CalculateTerminalViewport(model, 2, 0);
	EXPECT_EQ(model.ScrollbackSize() + model.RowCount(), bottom.totalRows);
	EXPECT_EQ(bottom.totalRows - 2, bottom.topRow);
	const auto scrolled = terminal::CalculateTerminalViewport(model, 2, 1);
	EXPECT_EQ(bottom.topRow - 1, scrolled.topRow);
}

TEST(TerminalRenderMapping, AlternateScreenIgnoresRememberedMainScrollbackViewport)
{
	terminal::TerminalModel model(6, 2, 10);
	WriteLine(model, L"main-one");
	WriteLine(model, L"main-two");
	WriteLine(model, L"main-three");
	ASSERT_GT(model.ScrollbackSize(), 0u);
	const auto mainViewport = terminal::CalculateTerminalViewport(model, 2, 1);
	ASSERT_GT(mainViewport.topRow, 0u);

	model.SetAlternateScreen(true);
	model.Print(U'A');
	const auto alternateViewport = terminal::CalculateTerminalViewport(model, 2, 999);
	EXPECT_EQ(model.RowCount(), alternateViewport.totalRows);
	EXPECT_EQ(2u, alternateViewport.visibleRows);
	EXPECT_EQ(0u, alternateViewport.topRow);
	const RECT client{ 0, 0, 640, 320 };
	EXPECT_FALSE(terminal::CalculateTerminalScrollbarLayout(client, alternateViewport.totalRows,
		alternateViewport.visibleRows, alternateViewport.topRow, 96).scrollable);
	const auto* alternateFirstRow = terminal::GetTerminalRow(model, 0);
	ASSERT_NE(nullptr, alternateFirstRow);
	EXPECT_EQ(L"A", alternateFirstRow->cells[0].Text());
	const std::vector<std::size_t> expectedDirtyRows{ 0, 1 };
	EXPECT_EQ(expectedDirtyRows, terminal::MapDirtyRowsToViewport(model, alternateViewport, { 0, 1 }));
	EXPECT_EQ(L"A", terminal::ExtractTerminalSelection(model, { 0, 0 }, { 0, 1 }));

	model.SetAlternateScreen(false);
	const auto restoredViewport = terminal::CalculateTerminalViewport(model, 2, 1);
	EXPECT_EQ(mainViewport.topRow, restoredViewport.topRow);
	const auto* restoredFirstRow = terminal::GetTerminalRow(model, 0);
	ASSERT_NE(nullptr, restoredFirstRow);
	EXPECT_NE(L"A", restoredFirstRow->cells[0].Text());
}

TEST(TerminalRenderMapping, MapsOnlyDirtyScreenRowsThatAreActuallyVisible)
{
	terminal::TerminalModel model(8, 3);
	const auto viewport = terminal::CalculateTerminalViewport(model, 2, 0);
	const auto mapped = terminal::MapDirtyRowsToViewport(model, viewport, { 0, 1, 2 });
	ASSERT_EQ(2u, mapped.size());
	EXPECT_EQ(0u, mapped[0]);
	EXPECT_EQ(1u, mapped[1]);
}

TEST(TerminalRenderMapping, MapsPixelsToClampedGlobalCells)
{
	terminal::TerminalModel model(8, 3);
	const auto viewport = terminal::CalculateTerminalViewport(model, 3, 0);
	EXPECT_EQ((terminal::TerminalSelectionPoint{ viewport.topRow + 1, 2 }),
		terminal::TerminalCellFromPoint(viewport, 19, 18, 8, 16, model.Columns()));
	EXPECT_EQ(model.Columns(), terminal::TerminalCellFromPoint(viewport, 9999, 0, 8, 16, model.Columns()).column);
}

TEST(TerminalRenderMapping, UsesApportionedPaddingAtEachSupportedDpi)
{
	constexpr std::array cases{
		std::pair{ 96u, 5 },
		std::pair{ 144u, 8 },
		std::pair{ 192u, 10 },
	};
	for( const auto [dpi, padding] : cases ) {
		const auto geometry = terminal::TerminalViewportGeometry::FromDpi(dpi);
		EXPECT_EQ(padding, geometry.padding) << dpi;
		EXPECT_EQ(padding, geometry.GridOriginX()) << dpi;
		EXPECT_EQ(padding, geometry.GridOriginY()) << dpi;
		EXPECT_EQ(100 - padding * 2, geometry.GridWidth(100)) << dpi;
		EXPECT_EQ(100 - padding * 2, geometry.GridHeight(100)) << dpi;
		const auto grid = geometry.GridRect({ 0, 7, 100, 27 });
		EXPECT_EQ(padding, grid.left) << dpi;
		EXPECT_EQ(7 + padding, grid.top) << dpi;
		EXPECT_EQ(100 - padding, grid.right) << dpi;
		EXPECT_EQ(27 - padding, grid.bottom) << dpi;
	}
	const auto clipped = terminal::TerminalViewportGeometry::FromDpi(192).GridRect({ 0, 0, 16, 12 });
	EXPECT_EQ(10, clipped.left);
	EXPECT_EQ(10, clipped.top);
	EXPECT_EQ(10, clipped.right);
	EXPECT_EQ(10, clipped.bottom);
	EXPECT_EQ(0, terminal::TerminalViewportGeometry::FromDpi(192).GridWidth(16));
	EXPECT_EQ(0, terminal::TerminalViewportGeometry::FromDpi(192).GridHeight(12));
}

TEST(TerminalRenderMapping, ClampsHitTestingInsideAndBeforeThePadding)
{
	const terminal::TerminalViewport viewport{ 4, 2, 1 };
	const auto geometry = terminal::TerminalViewportGeometry::FromDpi(144);

	EXPECT_EQ((terminal::TerminalSelectionPoint{ 1, 0 }),
		terminal::TerminalCellFromPoint(viewport, 0, 0, 8, 16, 10, geometry));
	EXPECT_EQ((terminal::TerminalSelectionPoint{ 1, 0 }),
		terminal::TerminalCellFromPoint(viewport, geometry.padding, geometry.padding, 8, 16, 10, geometry));
	EXPECT_EQ((terminal::TerminalSelectionPoint{ 1, 1 }),
		terminal::TerminalCellFromPoint(viewport, geometry.padding + 8, geometry.padding, 8, 16, 10, geometry));
	EXPECT_EQ((terminal::TerminalSelectionPoint{ 2, 0 }),
		terminal::TerminalCellFromPoint(viewport, geometry.padding, geometry.padding + 16, 8, 16, 10, geometry));
	EXPECT_EQ((terminal::TerminalSelectionPoint{ 3, 10 }),
		terminal::TerminalCellFromPoint(viewport, 9999, 9999, 8, 16, 10, geometry));
}

TEST(TerminalRenderMapping, ImeWindowPositionUsesTheSameGridCellAsTheCaret)
{
	const auto geometry = terminal::TerminalViewportGeometry::FromDpi(144);
	const auto position = geometry.ImeWindowPosition(10, 3, 8, 16);

	EXPECT_EQ(88, position.caret.x);
	EXPECT_EQ(56, position.caret.y);
	EXPECT_EQ(88, position.composition.x);
	EXPECT_EQ(72, position.composition.y);
	EXPECT_EQ(88, position.candidateArea.left);
	EXPECT_EQ(56, position.candidateArea.top);
	EXPECT_EQ(96, position.candidateArea.right);
	EXPECT_EQ(72, position.candidateArea.bottom);
}

TEST(TerminalRenderMapping, NormalizesSelectionToIncludeBothEndpointsAndWideCells)
{
	terminal::TerminalModel model(8, 1);
	model.Print(U'\u65e5');
	model.Print(U'A');

	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 0 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 0 }, { 0, 0 }));
	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 2 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 0 }, { 0, 1 }));
	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 3 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 0 }, { 0, 2 }));
	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 3 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 2 }, { 0, 0 }));
}

TEST(TerminalRenderMapping, ExtractsWideAndCombiningCellsWithoutContinuationDuplicates)
{
	terminal::TerminalModel model(8, 2);
	model.Print(U'\u65e5');
	model.Print(U'e');
	model.Print(U'\u0301');
	const auto text = terminal::ExtractTerminalSelection(model, { 0, 0 }, { 0, 3 });
	EXPECT_EQ(L"\u65e5e\u0301", text);
}

TEST(TerminalRenderMapping, UsesCrLfBetweenUnwrappedSelectedRows)
{
	terminal::TerminalModel model(8, 2);
	model.Print(U'a');
	model.ExecuteControl(L'\r');
	model.ExecuteControl(L'\n');
	model.Print(U'b');
	EXPECT_EQ(L"a\r\nb", terminal::ExtractTerminalSelection(model, { 0, 0 }, { 1, 1 }));
}

TEST(TerminalRenderMapping, NormalizesAndExtractsSelectionAcrossAnAstralSurrogatePairEmoji)
{
	// Unlike the single-UTF-16-unit CJK cell above, this codepoint is above
	// U+FFFF: TerminalModel::AppendCodepoint stores it as a real UTF-16
	// surrogate pair (two code units) inside one wide cell. The selection
	// endpoint math must treat that cell as one grapheme of width 2, the same
	// as any other wide cell, and extraction must hand back the complete pair
	// rather than truncating to its high surrogate.
	terminal::TerminalModel model(8, 1);
	model.Print(U'\U0001F600');
	model.Print(U'A');

	const auto* row = terminal::GetTerminalRow(model, 0);
	ASSERT_NE(nullptr, row);
	ASSERT_EQ(2u, row->cells[0].width);
	EXPECT_TRUE(row->cells[1].continuation);
	EXPECT_EQ(2u, row->cells[0].Text().size());

	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 2 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 0 }, { 0, 1 }));
	EXPECT_EQ(std::wstring(L"\U0001F600"),
		terminal::ExtractTerminalSelection(model, { 0, 0 }, { 0, 2 }));
}

TEST(TerminalRenderMapping, ExtractsSpacesForCellsATabJumpedOverWithoutPrinting)
{
	// A horizontal tab only moves the cursor; it never prints into the cells it
	// skips. Those cells stay at their default zero-length state, but each one
	// still occupies exactly one selected column, so extraction must yield a
	// space per skipped column instead of silently gluing the surrounding text
	// together.
	terminal::TerminalModel model(12, 1);
	model.Print(U'a');
	model.ExecuteControl(L'\t');
	model.Print(U'b');

	EXPECT_EQ(L"a" + std::wstring(7, L' ') + L"b",
		terminal::ExtractTerminalSelection(model, { 0, 0 }, { 0, 9 }));
}

TEST(TerminalRenderMapping, NormalizesSelectionEndAtExactRowWidthWithTrailingWideCell)
{
	// The active endpoint can already sit at the row width (selecting through
	// end-of-line) when the last visible cell is the trailing half of a wide
	// grapheme. Normalization must clamp there instead of indexing past the
	// last cell while looking for a width to extend by.
	terminal::TerminalModel model(4, 1);
	model.Print(U'A');
	model.Print(U'B');
	model.Print(U'日');

	const auto* row = terminal::GetTerminalRow(model, 0);
	ASSERT_NE(nullptr, row);
	ASSERT_EQ(4u, row->cells.size());
	EXPECT_TRUE(row->cells[3].continuation);

	EXPECT_EQ((terminal::TerminalSelectionRange{ { 0, 0 }, { 0, 4 } }),
		terminal::NormalizeTerminalSelection(model, { 0, 0 }, { 0, 4 }));
	EXPECT_EQ(L"AB日", terminal::ExtractTerminalSelection(model, { 0, 0 }, { 0, 4 }));
}

} // namespace
