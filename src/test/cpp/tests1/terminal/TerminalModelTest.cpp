/*! @file */
#include "pch.h"
#include "terminal/model/TerminalModel.h"

#include <unordered_set>

namespace {

std::wstring RowText( const terminal::TerminalRow& row )
{
	std::wstring result;
	for( const auto& cell : row.cells ) if( !cell.continuation ) result.append(cell.Text());
	return result;
}

TEST(TerminalModel, StoresWideAndCombiningGraphemesWithoutPerCellHeapStorage)
{
	terminal::TerminalModel model(10, 2);
	static_assert(sizeof(terminal::TerminalCell) <= sizeof(wchar_t) * terminal::TerminalCell::kMaxCodeUnits + 8);
	model.Print(U'日');
	model.Print(U'e');
	model.Print(U'\u0301');
	EXPECT_EQ(3u, model.CursorColumn());
	EXPECT_EQ(2u, model.Rows()[0].cells[0].width);
	EXPECT_TRUE(model.Rows()[0].cells[1].continuation);
	EXPECT_EQ(L"e\u0301", model.Rows()[0].cells[2].Text());
}

TEST(TerminalModel, KeepsCommonEmojiZwjSequenceInOneWideCell)
{
	terminal::TerminalModel model(10, 2);
	model.Print(U'\U0001F469');
	model.Print(U'\u200D');
	model.Print(U'\U0001F4BB');
	EXPECT_EQ(2u, model.CursorColumn());
	EXPECT_EQ(5u, model.Rows()[0].cells[0].length);
	EXPECT_TRUE(model.Rows()[0].cells[1].continuation);
}

TEST(TerminalModel, KeepsFlagAndSkinToneSequencesInSingleWideCells)
{
	terminal::TerminalModel model(10, 2);
	model.Print(U'\U0001F1EF');
	model.Print(U'\U0001F1F5');
	model.Print(U'\U0001F44D');
	model.Print(U'\U0001F3FD');
	EXPECT_EQ(4u, model.CursorColumn());
	EXPECT_EQ(4u, model.Rows()[0].cells[0].length);
	EXPECT_EQ(4u, model.Rows()[0].cells[2].length);
	EXPECT_TRUE(model.Rows()[0].cells[1].continuation);
	EXPECT_TRUE(model.Rows()[0].cells[3].continuation);
}

TEST(TerminalModel, EmojiVariationSelectorCanGrowPreviousCellWidth)
{
	terminal::TerminalModel model(10, 2);
	model.Print(U'\u2764');
	model.Print(U'\uFE0F');
	EXPECT_EQ(2u, model.CursorColumn());
	EXPECT_EQ(2u, model.Rows()[0].cells[0].length);
	EXPECT_EQ(2u, model.Rows()[0].cells[0].width);
	EXPECT_TRUE(model.Rows()[0].cells[1].continuation);
}

TEST(TerminalModel, CapsScrollbackAndStopsGrowingAtConfiguredLimit)
{
	terminal::TerminalModel model(4, 2, 3);
	for( int i = 0; i < 10; ++i ) {
		model.Print(static_cast<char32_t>(L'0' + i));
		model.ExecuteControl(L'\r');
		model.ExecuteControl(L'\n');
	}
	EXPECT_EQ(3u, model.ScrollbackSize());
	model.SetScrollbackLimit(1);
	EXPECT_EQ(1u, model.ScrollbackSize());
	model.SetScrollbackLimit(terminal::TerminalModel::kMaxScrollbackLines + 50);
	EXPECT_EQ(terminal::TerminalModel::kMaxScrollbackLines, model.ScrollbackLimit());
}

TEST(TerminalModel, EvictsOldestScrollbackRowAtCapacity)
{
	terminal::TerminalModel model(1, 1, 2);
	for( const auto character : { U'a', U'b', U'c', U'd' } ) {
		model.Print(character);
		model.ExecuteControl(L'\r');
		model.ExecuteControl(L'\n');
	}
	ASSERT_EQ(2u, model.ScrollbackSize());
	EXPECT_EQ(L"c", RowText(model.Scrollback()[0]));
	EXPECT_EQ(L"d", RowText(model.Scrollback()[1]));
}

TEST(TerminalModel, FullScreenScrollMovesRowsToScrollbackAndReusesBoundedRowBuffers)
{
	terminal::TerminalModel model(3, 1, 2);
	// Fill both rows, then scroll until the bounded history has reached steady
	// state. The screen and scrollback must retain the correct chronological
	// order while all later rows reuse one of the existing cell buffers.
	for( const auto character : { U'a', U'b', U'c', U'd' } ) {
		model.Print(character);
		model.ExecuteControl(L'\r');
		model.ExecuteControl(L'\n');
	}
	ASSERT_EQ(2u, model.ScrollbackSize());
	EXPECT_EQ(L"c", RowText(model.Scrollback()[0]));
	EXPECT_EQ(L"d", RowText(model.Scrollback()[1]));

	std::unordered_set<const terminal::TerminalCell*> cellBuffers;
	for( const auto& row : model.Rows() ) cellBuffers.insert(row.cells.data());
	for( const auto& row : model.Scrollback() ) cellBuffers.insert(row.cells.data());
	for( const auto character : { U'e', U'f', U'g', U'h' } ) {
		model.Print(character);
		model.ExecuteControl(L'\r');
		model.ExecuteControl(L'\n');
	}
	EXPECT_EQ(L"g", RowText(model.Scrollback()[0]));
	EXPECT_EQ(L"h", RowText(model.Scrollback()[1]));
	for( const auto& row : model.Rows() ) EXPECT_TRUE(cellBuffers.contains(row.cells.data()));
	for( const auto& row : model.Scrollback() ) EXPECT_TRUE(cellBuffers.contains(row.cells.data()));
}

TEST(TerminalModel, PartialScrollRegionRotatesOnlyItsRowsAndKeepsOuterRows)
{
	terminal::TerminalModel model(2, 4);
	for( std::size_t row = 0; row < model.RowCount(); ++row ) {
		model.SetCursorPosition(0, row);
		model.Print(static_cast<char32_t>(U'A' + row));
	}
	model.SetScrollRegion(1, 2);
	model.ScrollUp(1);
	EXPECT_EQ(L"A", RowText(model.Rows()[0]));
	EXPECT_EQ(L"C", RowText(model.Rows()[1]));
	EXPECT_TRUE(RowText(model.Rows()[2]).empty());
	EXPECT_EQ(L"D", RowText(model.Rows()[3]));
	EXPECT_EQ(0u, model.ScrollbackSize());
}

TEST(TerminalModel, AlternateScreenPreservesMainScreenAndHasNoScrollback)
{
	terminal::TerminalModel model(8, 2);
	model.Print(U'M');
	model.SetAlternateScreen(true);
	EXPECT_TRUE(model.IsAlternateScreen());
	model.Print(U'A');
	for( int i = 0; i < 5; ++i ) model.ExecuteControl(L'\n');
	EXPECT_EQ(0u, model.ScrollbackSize());
	model.SetAlternateScreen(false);
	EXPECT_FALSE(model.IsAlternateScreen());
	EXPECT_EQ(L"M", RowText(model.Rows()[0]));
}

TEST(TerminalModel, AlternateScreenRestoresMainRenditionAndSavedCursorState)
{
	terminal::TerminalModel model(8, 3);
	model.SetForeground(terminal::TerminalColor::Indexed(2));
	model.SetBackground(terminal::TerminalColor::Indexed(4));
	model.SetCursorPosition(3, 1);
	model.SaveCursor();
	model.SetAlternateScreen(true);
	model.SetForeground(terminal::TerminalColor::Indexed(0));
	model.SetBackground(terminal::TerminalColor::Indexed(0));
	model.SetCursorPosition(7, 2);
	model.SaveCursor();

	model.SetAlternateScreen(false);
	EXPECT_EQ(terminal::TerminalColor::Indexed(2), model.CurrentAttributes().foreground);
	EXPECT_EQ(terminal::TerminalColor::Indexed(4), model.CurrentAttributes().background);
	model.SetCursorPosition(0, 0);
	model.RestoreCursor();
	EXPECT_EQ(3u, model.CursorColumn());
	EXPECT_EQ(1u, model.CursorRow());
}

TEST(TerminalModel, ResizesSavedMainScreenWhileAlternateScreenIsActive)
{
	terminal::TerminalModel model(4, 2);
	model.Print(U'M');
	model.SetAlternateScreen(true);
	model.Resize(12, 4);
	model.SetAlternateScreen(false);

	ASSERT_EQ(4u, model.Rows().size());
	for( const auto& row : model.Rows() ) {
		EXPECT_EQ(12u, row.cells.size());
		EXPECT_EQ(12u, row.cellAttributes.size());
	}
	model.SetCursorPosition(11, 3);
	model.Print(U'Z');
	EXPECT_EQ(L"Z", model.Rows()[3].cells[11].Text());
}

TEST(TerminalModel, EraseAndScrollBlanksUseCurrentBackgroundRendition)
{
	terminal::TerminalModel model(5, 2);
	const auto background = terminal::TerminalColor::Rgb(12, 34, 56);
	model.SetBackground(background);
	model.Print(U'X');
	model.EraseLine(2);
	for( std::size_t column = 0; column < model.Columns(); ++column ) {
		EXPECT_EQ(background, model.Rows()[0].AttributesAt(column).background);
	}
	model.SetScrollRegion(0, 1);
	model.ScrollUp(1);
	for( std::size_t column = 0; column < model.Columns(); ++column ) {
		EXPECT_EQ(background, model.Rows()[1].AttributesAt(column).background);
	}
}

TEST(TerminalModel, ReportsOnlyDirtyVisibleRowsAndClearsTheSet)
{
	terminal::TerminalModel model(8, 3);
	EXPECT_EQ(3u, model.ConsumeDirtyRows().size());
	EXPECT_TRUE(model.ConsumeDirtyRows().empty());
	model.SetCursorPosition(0, 1);
	model.Print(U'x');
	const auto dirty = model.ConsumeDirtyRows();
	ASSERT_EQ(1u, dirty.size());
	EXPECT_EQ(1u, dirty[0]);
}

TEST(TerminalModel, KeepsAttributesAsContiguousRuns)
{
	terminal::TerminalModel model(6, 1);
	model.SetForeground(terminal::TerminalColor::Indexed(1));
	model.Print(U'a');
	model.Print(U'b');
	model.ResetAttributes();
	model.Print(U'c');
	ASSERT_EQ(2u, model.Rows()[0].attributeRuns.size());
	EXPECT_EQ(2u, model.Rows()[0].attributeRuns[0].length);
	EXPECT_EQ(terminal::TerminalColor::Indexed(1), model.Rows()[0].AttributesAt(0).foreground);
	EXPECT_EQ(terminal::TerminalColorKind::Default, model.Rows()[0].AttributesAt(2).foreground.kind);
}

TEST(TerminalModel, AlternatingAttributesRemainContiguousRunsWithConstantTimeCellLookup)
{
	constexpr std::size_t columns = 120;
	terminal::TerminalModel model(columns, 1);
	for( std::size_t column = 0; column < columns; ++column ) {
		model.SetForeground(terminal::TerminalColor::Indexed(static_cast<std::uint8_t>(column % 2 == 0 ? 1 : 2)));
		model.Print(U'x');
	}

	const auto& row = model.Rows()[0];
	ASSERT_EQ(columns, row.attributeRuns.size());
	ASSERT_EQ(columns, row.cellAttributes.size());
	for( std::size_t column = 0; column < columns; ++column ) {
		EXPECT_EQ(column, row.attributeRuns[column].start);
		EXPECT_EQ(1u, row.attributeRuns[column].length);
		EXPECT_EQ(terminal::TerminalColor::Indexed(static_cast<std::uint8_t>(column % 2 == 0 ? 1 : 2)),
			row.AttributesAt(column).foreground);
	}
}

TEST(TerminalModel, CapsFiveThousandRowsOfOneHundredTwentyColumnAttributeStorage)
{
	constexpr std::size_t columns = 120;
	constexpr std::size_t scrollbackLines = 5000;
	terminal::TerminalModel model(columns, 1, scrollbackLines);
	for( std::size_t line = 0; line <= scrollbackLines; ++line ) {
		model.Print(U'x');
		model.ExecuteControl(L'\r');
		model.ExecuteControl(L'\n');
	}

	ASSERT_EQ(scrollbackLines, model.ScrollbackSize());
	EXPECT_EQ(scrollbackLines, model.ScrollbackLimit());
	for( const auto& row : model.Scrollback() ) {
		EXPECT_EQ(columns, row.cells.size());
		EXPECT_EQ(columns, row.cellAttributes.size());
		EXPECT_EQ(1u, row.attributeRuns.size());
		EXPECT_EQ(columns, row.attributeRuns.front().length);
	}
}

} // namespace
