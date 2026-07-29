/*! @file */
#include "pch.h"
#include "terminal/window/TerminalRenderMapping.h"
#include "terminal/window/TerminalScrollbarLayout.h"

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

} // namespace
