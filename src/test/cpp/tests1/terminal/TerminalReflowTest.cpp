/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/model/TerminalModel.h"
#include "terminal/model/TerminalReflow.h"
#include "terminal/parser/TerminalParser.h"
#include "terminal/runtime/TerminalCaptureExtraction.h"
#include "terminal/window/TerminalLink.h"
#include "terminal/window/TerminalRenderMapping.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using terminal::TerminalModel;
using terminal::TerminalParser;

// A row's written cells end at the last cell that holds text.  A written
// space counts; unwritten and erased cells after it do not; a wide cell
// includes its continuation.  This is the one join rule every consumer
// must share (issue #445 step 2).
std::size_t WrittenEnd( const terminal::TerminalRow& row )
{
	for( std::size_t column = row.cells.size(); column > 0; --column ) {
		if( row.cells[column - 1].length != 0 ) return column;
	}
	return 0;
}

std::wstring RowWrittenText( const terminal::TerminalRow& row, bool wholeRow )
{
	const auto end = wholeRow ? row.cells.size() : WrittenEnd(row);
	std::wstring text;
	for( std::size_t column = 0; column < end; ++column ) {
		const auto& cell = row.cells[column];
		if( cell.continuation ) continue;
		if( cell.length == 0 ) text.push_back(L' ');
		else text.append(cell.Text());
	}
	return text;
}

// Logical lines over [scrollback | screen], joined while the leader row is
// wrapped.  Each row contributes cells up to its last written cell.
std::vector<std::wstring> LogicalLines( const TerminalModel& model, bool includeHistory = true )
{
	std::vector<std::wstring> lines;
	std::wstring current;
	auto visit = [&]( const terminal::TerminalRow& row ) {
		current += RowWrittenText(row, false);
		if( !row.wrapped ) {
			lines.push_back(std::move(current));
			current.clear();
		}
	};
	if( includeHistory ) for( const auto& row : model.Scrollback() ) visit(row);
	for( const auto& row : model.Rows() ) visit(row);
	if( !current.empty() ) lines.push_back(std::move(current));
	return lines;
}

std::u32string ToU32( const std::wstring& text )
{
	return { text.begin(), text.end() };
}

void Print( TerminalModel& model, std::u32string_view text )
{
	for( const auto ch : text ) {
		if( ch < 32 ) model.ExecuteControl(static_cast<wchar_t>(ch));
		else model.Print(ch);
	}
}

// "M<n> " followed by cycling digits and spaces ending in END; the length
// is chosen so the line wraps at every width used here.
std::wstring MarkerText( std::size_t index, std::size_t length )
{
	std::wstring text = L"M" + std::to_wstring(index) + L" ";
	while( text.size() + 4 < length ) text.push_back(L'0' + static_cast<wchar_t>((index + text.size()) % 10));
	while( text.size() < length ) text.push_back(L' ');
	text.replace(text.size() - 3, 3, L"END");
	return text;
}

struct MarkerCount final {
	std::size_t intact{};
	std::size_t damaged{};
	std::size_t glued{};
	std::size_t gone{};
	// Occurrences of a marker beyond the first, counted separately: an
	// "intact" verdict is only honest when each marker exists exactly once.
	std::size_t extra{};
};

MarkerCount CountMarkers( const std::vector<std::wstring>& lines, std::size_t markers )
{
	MarkerCount result;
	for( std::size_t index = 0; index < markers; ++index ) {
		const auto head = L"M" + std::to_wstring(index) + L" ";
		const auto expected = MarkerText(index, 192);
		std::vector<const std::wstring*> hits;
		for( const auto& line : lines )
			if( line.find(head) != std::wstring::npos ) hits.push_back(&line);
		if( hits.empty() ) { ++result.gone; continue; }
		result.extra += hits.size() - 1;
		if( hits.size() == 1 && *hits.front() == expected ) { ++result.intact; continue; }
		// A line that holds this marker's head plus another marker's head
		// glues two markers into one logical line.
		const auto shares = std::any_of(hits.begin(), hits.end(), [&]( const std::wstring* hit ) {
			for( std::size_t other = 0; other < markers; ++other ) {
				if( other != index
					&& hit->find(L"M" + std::to_wstring(other) + L" ") != std::wstring::npos )
					return true;
			}
			return false;
		});
		if( shares || (hits.size() == 1
			&& hits.front()->find(expected) != std::wstring::npos && *hits.front() != expected) )
			++result.glued;
		else
			++result.damaged;
	}
	return result;
}

void AppendUtf8( std::string& bytes, const std::wstring& text )
{
	for( const auto ch : text ) {
		if( ch < 0x80 ) bytes.push_back(static_cast<char>(ch));
		else if( ch < 0x800 ) {
			bytes.push_back(static_cast<char>(0xC0 | (ch >> 6)));
			bytes.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		} else {
			bytes.push_back(static_cast<char>(0xE0 | (ch >> 12)));
			bytes.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
			bytes.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
		}
	}
}

// The synthetic ConPTY repaint, shaped after the resize-* captures.
// ConPTY keeps only a screen: a line that left it never comes back, so
// this emits the fixture's own retained screen lines (they only shrink
// across resizes).  The whole screen is repainted - each line is written
// whole so the host autowraps it, an EL and CR LF between lines, the rows
// below the content are erased too, and the cursor lands where the
// application left it: the first column of the row after its content.
std::string SyntheticRepaint( const TerminalModel& model, std::size_t columns, std::size_t rows )
{
	std::vector<std::wstring> lines;
	std::wstring current;
	for( const auto& row : model.Rows() ) {
		current += RowWrittenText(row, false);
		if( !row.wrapped ) {
			lines.push_back(std::move(current));
			current.clear();
		}
	}
	if( !current.empty() ) lines.push_back(std::move(current));
	std::size_t fitting = 0, needed = 0;
	for( auto it = lines.rbegin(); it != lines.rend(); ++it ) {
		const auto height = std::max<std::size_t>(1, (it->size() + columns - 1) / columns);
		if( needed + height > rows ) break;
		needed += height;
		++fitting;
	}
	std::string bytes = "\x1b[H";
	std::size_t covered = 0;
	for( std::size_t line = lines.size() - fitting; line < lines.size(); ++line ) {
		AppendUtf8(bytes, lines[line]);
		bytes += "\x1b[K";
		if( line + 1 != lines.size() ) bytes += "\r\n";
		covered += std::max<std::size_t>(1, (lines[line].size() + columns - 1) / columns);
	}
	while( covered < rows ) {
		bytes += "\r\n\x1b[K";
		++covered;
	}
	bytes += "\x1b[" + std::to_string(std::min(needed + 1, rows)) + ";1H";
	return bytes;
}

std::vector<std::wstring> FeedMarkers( TerminalParser& parser,
	std::size_t markers, std::size_t length )
{
	std::vector<std::wstring> lines;
	lines.reserve(markers);
	std::string bytes;
	for( std::size_t index = 0; index < markers; ++index ) {
		lines.push_back(MarkerText(index, length));
		AppendUtf8(bytes, lines.back());
		bytes += "\r\n";
	}
	parser.Feed(bytes);
	return lines;
}

TEST(TerminalReflow, RepaintReplayKeepsEveryMarkerLineIntact)
{
	constexpr std::size_t kMarkers = 60;
	TerminalModel model(120, 30);
	TerminalParser parser(model);
	FeedMarkers(parser, kMarkers, 192);

	for( int cycle = 0; cycle < 3; ++cycle ) {
		model.Resize(40, 30);
		parser.Feed(SyntheticRepaint(model, 40, 30));
		model.Resize(120, 30);
		parser.Feed(SyntheticRepaint(model, 120, 30));
	}
	const auto counts = CountMarkers(LogicalLines(model), kMarkers);
	EXPECT_EQ(kMarkers, counts.intact);
	EXPECT_EQ(0u, counts.damaged);
	EXPECT_EQ(0u, counts.gone);
	EXPECT_EQ(0u, counts.glued);
	EXPECT_EQ(0u, counts.extra);
}

TEST(TerminalReflow, NarrowingMovesScreenRowsIntoHistoryWithoutTruncation)
{
	TerminalModel model(30, 4);
	Print(model, U"012345678901234567890123456789012345678901234567890123456789\r\n");
	Print(model, U"abcdefghijklmnopqrstuvwxyz");
	model.Resize(10, 4);
	// The 60-cell line reflows to six 10-cell rows; the top five move to
	// history.  The 26-cell line becomes three rows.  Nothing is truncated.
	const auto lines = LogicalLines(model);
	ASSERT_EQ(2u, lines.size());
	EXPECT_EQ(L"012345678901234567890123456789012345678901234567890123456789", lines[0]);
	EXPECT_EQ(L"abcdefghijklmnopqrstuvwxyz", lines[1]);
	EXPECT_EQ(5u, model.ScrollbackSize());
}

TEST(TerminalReflow, WideningNeverMovesHistoryRowsOntoTheScreen)
{
	TerminalModel model(20, 4);
	for( std::size_t index = 0; index < 8; ++index )
		Print(model, ToU32(MarkerText(index, 39)) + U"\r\n");
	model.Resize(10, 4);
	ASSERT_GT(model.ScrollbackSize(), 0u);
	const auto before = LogicalLines(model);
	model.Resize(40, 4);
	// Every logical line survives the widen exactly; history rows never come
	// back, so a screen row never resumes a history line either.  Blank tail
	// rows differ (the screen must keep `rows` rows), so compare content.
	const auto after = LogicalLines(model);
	const auto nonEmpty = []( std::vector<std::wstring> lines ) {
		lines.erase(std::remove_if(lines.begin(), lines.end(),
			[]( const std::wstring& line ) { return line.empty(); }), lines.end());
		return lines;
	};
	EXPECT_EQ(nonEmpty(before), nonEmpty(after));
	for( const auto& row : model.Rows() ) EXPECT_EQ(40u, row.cells.size());
}

TEST(TerminalReflow, OneRowLineThatStillFitsKeepsCellsAndAttributesExactly)
{
	TerminalModel model(30, 2);
	model.SetForeground(terminal::TerminalColor::Indexed(3));
	Print(model, U"fit     "); // trailing written spaces keep the color
	model.Resize(20, 2);
	const auto& row = model.Rows()[0];
	for( std::size_t column = 0; column < 8; ++column )
		EXPECT_EQ(3, row.AttributesAt(column).foreground.value);
	// Padding added by the truncate/pad path carries default attributes.
	EXPECT_EQ(terminal::TerminalAttributes{}, row.AttributesAt(19));
	model.Resize(30, 2);
	EXPECT_EQ(3, model.Rows()[0].AttributesAt(7).foreground.value);
	EXPECT_EQ(terminal::TerminalAttributes{}, model.Rows()[0].AttributesAt(29));
}

TEST(TerminalReflow, ExplicitLineFeedBreaksTheLogicalLine)
{
	TerminalModel model(10, 4);
	// The model defers autowrap: the flag only sets when the next printable
	// character arrives, so 'X' fires row 1's wrap before the explicit LF.
	Print(model, U"0123456789abcdefghijX\r\nrest");
	// Row 0 wraps onto row 1 and row 1 wraps onto row 2.  Reprint row 1
	// shorter, then leave it with an explicit LF: its old continuation must
	// not glue row 2 to it.  Row 0's leader flag is untouched, so the
	// reprinted row stays part of the first logical line.
	model.SetCursorPosition(0, 1);
	Print(model, U"XY\r\n");
	Print(model, U"next");
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 3u);
	EXPECT_EQ(L"0123456789XYcdefghij", lines[0]);
	EXPECT_EQ(L"next", lines[1]);
	EXPECT_EQ(L"rest", lines[2]);
}

TEST(TerminalReflow, ExplicitLineFeedAtScrollBottomClearsTheLeftRowFlag)
{
	TerminalModel model(10, 3);
	Print(model, U"0123456789AB\r\n");
	model.SetCursorPosition(0, 2);
	Print(model, U"0123456789"); // fills the row; autowrap marks it
	model.ExecuteControl(L'\n'); // explicit LF scrolls; the flag must clear
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(L"0123456789AB", lines[0]);
	EXPECT_EQ(L"0123456789", lines[1]);
}

TEST(TerminalReflow, EraseLineZeroAtColumnZeroUngluesThePreviousRow)
{
	TerminalModel model(10, 3);
	Print(model, U"0123456789abcdefghij");
	// Row 0 wraps onto row 1.  EL 0 at column 0 on row 1 erases the whole
	// continuation row: it stops being row 0's continuation.
	model.SetCursorPosition(0, 1);
	model.EraseLine(0);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"", lines[1]);
}

TEST(TerminalReflow, EraseLineTwoUngluesThePreviousRow)
{
	TerminalModel model(10, 3);
	Print(model, U"0123456789abcdefghij");
	model.SetCursorPosition(5, 1);
	model.EraseLine(2);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"", lines[1]);
}

TEST(TerminalReflow, EraseLineOneKeepsThePreviousRowFlag)
{
	TerminalModel model(10, 3);
	Print(model, U"0123456789abcdefghij");
	model.SetCursorPosition(5, 1);
	model.EraseLine(1); // erases columns 0-5; the row stays a continuation
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 1u);
	EXPECT_EQ(L"0123456789      ghij", lines[0]);
}

TEST(TerminalReflow, EraseDisplayBelowUngluesErasedRows)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789abc\r\n");
	Print(model, U"0123456789def");
	// ED 0 from row 1: every erased row's leader flag drops and the
	// cursor's previous row unglues because the column is 0.
	model.SetCursorPosition(0, 1);
	model.EraseDisplay(0);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"", lines[1]);
}

TEST(TerminalReflow, EraseDisplayAboveKeepsRowsBelowSeparate)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789abc\r\n0123456789def\r\n");
	model.SetCursorPosition(9, 1);
	model.EraseDisplay(1); // column + 1 >= columns: the cursor row clears too
	// The newest history row lost its leader flag: the erased rows above
	// the cursor were its continuations, so no line glues onto history.
	ASSERT_FALSE(model.Scrollback().empty());
	EXPECT_FALSE(model.Scrollback()[model.ScrollbackSize() - 1].wrapped);
	const auto lines = LogicalLines(model);
	ASSERT_FALSE(lines.empty());
	EXPECT_EQ(L"0123456789", lines.front());
	EXPECT_NE(lines.end(), std::find(lines.begin(), lines.end(), L"def"));
}

TEST(TerminalReflow, InsertLinesBreaksAdjacencyAtBothSeams)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789AB\r\nrowtwo\r\nrowthree");
	// IL at row 1 pushes content down: the flag of the row before the
	// insertion point and of the row that now ends the region must clear.
	model.SetCursorPosition(0, 1);
	model.InsertLines(1);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 3u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"", lines[1]);
	EXPECT_EQ(L"AB", lines[2]);
}

TEST(TerminalReflow, DeleteLinesBreaksAdjacencyAtBothSeams)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789AB\r\nrowtwo\r\nrowthree");
	model.SetCursorPosition(0, 1);
	model.DeleteLines(1);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 3u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"rowtwo", lines[1]);
}

TEST(TerminalReflow, PartialScrollUpBreaksBothRegionSeams)
{
	TerminalModel model(10, 5);
	Print(model, U"AB\r\n0123456789cd\r\nthree\r\nfour");
	// Region rows 2-3 scroll up: "cd" is destroyed, so row 1's flag must
	// clear and "three" must not glue to the blanked row 3.
	model.SetScrollRegion(2, 3);
	model.ScrollUp(1);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 5u);
	EXPECT_EQ(L"AB", lines[0]);
	EXPECT_EQ(L"0123456789", lines[1]);
	EXPECT_EQ(L"three", lines[2]);
	EXPECT_EQ(L"", lines[3]);
	EXPECT_EQ(L"four", lines[4]);
}

TEST(TerminalReflow, ScrollDownBreaksBothRegionSeams)
{
	TerminalModel model(10, 5);
	Print(model, U"0123456789AB\r\n0123456789cd\r\nthree");
	// Region rows 1-3 scroll down: the blank at row 1 follows row 0 (row 0's
	// flag clears) and the moved leader at row 3 loses its flag onto row 4.
	model.SetScrollRegion(1, 3);
	model.ScrollDown(1);
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 5u);
	EXPECT_EQ(L"0123456789", lines[0]);
	EXPECT_EQ(L"", lines[1]);
	EXPECT_EQ(L"AB", lines[2]);
	EXPECT_EQ(L"0123456789", lines[3]);
	EXPECT_EQ(L"three", lines[4]);
}

TEST(TerminalReflow, FullScreenScrollKeepsFlagsSoChainsSurviveHistory)
{
	TerminalModel model(10, 3);
	Print(model, U"0123456789ABCDEFGHIJ\r\nnext");
	const auto lines = LogicalLines(model);
	ASSERT_GE(lines.size(), 2u);
	EXPECT_EQ(L"0123456789ABCDEFGHIJ", lines[0]);
	EXPECT_EQ(L"next", lines[1]);
}

TEST(TerminalReflow, CopyKeepsWrittenSpacesAtTheWrapPoint)
{
	TerminalModel model(10, 3);
	Print(model, U"hello     world");
	// The first row ends in written spaces and wraps.  Copy keeps them.
	const auto text = terminal::ExtractTerminalSelection(model, { 0, 0 }, { 1, 5 });
	EXPECT_EQ(L"hello     world", text);
}

TEST(TerminalReflow, CaptureJoinDropsTheUnwrittenTailOfAWrappedRow)
{
	TerminalModel model(10, 3);
	// The wide cell does not fit in the last column: row 0 autowraps with
	// its last cell left unwritten, and the gap is not content.
	Print(model, U"012345678\u65e5cd\r\n");
	Print(model, U"x\r\n"); // scrolls the leader row into history
	ASSERT_GT(model.ScrollbackSize(), 0u);
	terminal::TerminalCaptureExtractionRequest request;
	request.joinWrappedLines = true;
	request.startLine = -1; // include the wrapped history row the line starts on
	const auto extracted = terminal::ExtractTerminalCapture(model, request);
	ASSERT_EQ(terminal::TerminalCaptureResultCode::Succeeded, extracted.code);
	// A wrapped row contributes up to its last written cell: the unwritten
	// gap at the wrap point never enters the joined text.  The line's final
	// row keeps its cell positions (join pads its unwritten tail), so the
	// joined text is the line followed by that row's padding.
	bool joined = false;
	for( const auto& line : extracted.lines ) {
		EXPECT_EQ(std::u16string::npos, line.text.find(u" \u65e5"));
		if( line.text == u"012345678\u65e5cd      " ) joined = true;
	}
	EXPECT_TRUE(joined);
}

TEST(TerminalReflow, LinkJoinsAcrossAWideCellGapAtTheRightEdge)
{
	TerminalModel model(10, 3);
	Print(model, U"https://a\u65e5bc.de/x"); // the wide char wraps, leaving a gap
	const auto link = terminal::DetectTerminalWebLink(model, { 1, 2 });
	ASSERT_TRUE(link);
	EXPECT_EQ(L"https://a\u65e5bc.de/x", link->uri);
}

TEST(TerminalReflow, WideCellThatDoesNotFitStartsTheNextRowLeavingAGap)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789\u65e5\u65e5XY\r\n");
	model.Resize(12, 4);
	const auto lines = LogicalLines(model);
	ASSERT_FALSE(lines.empty());
	EXPECT_EQ(L"0123456789\u65e5\u65e5XY", lines[0]);
	bool continuationWithoutLead = false;
	for( const auto& row : model.Rows() ) {
		for( std::size_t column = 1; column < row.cells.size(); ++column ) {
			if( row.cells[column].continuation && row.cells[column - 1].Text() != L"\u65e5" )
				continuationWithoutLead = true;
		}
	}
	EXPECT_FALSE(continuationWithoutLead);
}

TEST(TerminalReflow, OneColumnTerminatesAWideCell)
{
	TerminalModel model(10, 3);
	Print(model, U"ab\u65e5cd\r\n");
	model.Resize(1, 3);
	const auto lines = LogicalLines(model);
	ASSERT_FALSE(lines.empty());
	EXPECT_EQ(L"ab\u65e5cd", lines[0]);
	// One column cannot hold a two-column cell: like Print's own clamp the
	// reflow stores it width 1, with no dangling continuation cell.  The
	// shrink pushes the line's leading rows into history, so scan both.
	bool found = false;
	const auto checkRow = [&found]( const terminal::TerminalRow& row ) {
		EXPECT_FALSE(row.cells[0].continuation);
		if( row.cells[0].Text() == L"\u65e5" ) {
			EXPECT_EQ(1u, row.cells[0].width);
			found = true;
		}
	};
	for( const auto& row : model.Scrollback() ) checkRow(row);
	for( const auto& row : model.Rows() ) checkRow(row);
	EXPECT_TRUE(found);
}

TEST(TerminalReflow, ScrollbackLimitIsEnforcedAfterTheReflow)
{
	TerminalModel model(30, 4, 20);
	model.SetScrollbackLimit(10);
	for( std::size_t index = 0; index < 10; ++index )
		Print(model, ToU32(MarkerText(index, 50)) + U"\r\n");
	model.Resize(10, 4);
	EXPECT_LE(model.ScrollbackSize(), 10u);
	EXPECT_TRUE(model.ConsumeScrollbackChange().Changed());
}

TEST(TerminalReflow, LimitZeroStillReflowsTheScreenAndDropsRows)
{
	TerminalModel model(30, 4, 0);
	Print(model, U"0123456789012345678901234567890123456789\r\n");
	Print(model, U"abcdefghijklmnopqrstuvwxyz");
	model.Resize(10, 4);
	EXPECT_EQ(0u, model.ScrollbackSize());
	EXPECT_EQ(4u, model.Rows().size());
	const auto lines = LogicalLines(model);
	EXPECT_EQ(L"abcdefghijklmnopqrstuvwxyz", lines.back());
}

TEST(TerminalReflow, AlternateScreenIsNotReflowedAndSavedBuffersAre)
{
	TerminalModel model(20, 3);
	Print(model, U"0123456789012345678901\r\nsecond\r\nthird");
	model.SetAlternateScreen(true);
	Print(model, U"ALT0123456789");
	model.Resize(10, 3);
	EXPECT_TRUE(model.IsAlternateScreen());
	// Alternate screen rows are truncated, not reflowed.
	const auto alt = LogicalLines(model, false);
	ASSERT_GE(alt.size(), 1u);
	EXPECT_EQ(L"ALT0123456", alt[0]);
	model.SetAlternateScreen(false);
	const auto main = LogicalLines(model);
	ASSERT_GE(main.size(), 2u);
	EXPECT_EQ(L"0123456789012345678901", main[0]);
}

TEST(TerminalReflow, CursorMapsByCellOffsetThroughASplit)
{
	TerminalModel model(20, 4);
	Print(model, U"0123456789abcdefghij");
	// The model defers autowrap: the cursor parks at column 20 on row 0
	// (the pending wrap), which is the line's end offset.
	ASSERT_EQ(0u, model.CursorRow());
	ASSERT_EQ(20u, model.CursorColumn());
	model.Resize(10, 4);
	EXPECT_EQ(L"0123456789abcdefghij", LogicalLines(model)[0]);
	// The cursor's cell offset (20) is the end of the joined content: it
	// stays on the line's last output row, clamped to columns - 1.
	EXPECT_EQ(1u, model.CursorRow());
	EXPECT_EQ(9u, model.CursorColumn());
}

TEST(TerminalReflow, CursorPastTheContentStaysOnTheLastRow)
{
	TerminalModel model(40, 4);
	Print(model, U"short");
	model.SetCursorPosition(39, 0);
	model.Resize(10, 4);
	EXPECT_EQ(0u, model.CursorRow());
	EXPECT_EQ(9u, model.CursorColumn());
}

TEST(TerminalReflow, SavedCursorMapsByCellOffsetToo)
{
	TerminalModel model(20, 4);
	Print(model, U"0123456789abcdefghij");
	model.SaveCursor(); // offset 20 = end of the joined line
	model.SetCursorPosition(0, 0);
	model.Resize(10, 4);
	model.RestoreCursor();
	EXPECT_EQ(1u, model.CursorRow());
	EXPECT_EQ(9u, model.CursorColumn());
}

TEST(TerminalReflow, SavedMainCursorFollowsWhileAlternateIsShown)
{
	TerminalModel model(20, 3);
	Print(model, U"0123456789abcdefghij");
	model.SetAlternateScreen(true);
	model.Resize(10, 3);
	model.SetAlternateScreen(false);
	EXPECT_EQ(1u, model.CursorRow());
	EXPECT_EQ(9u, model.CursorColumn());
}

TEST(TerminalReflow, RoundTripKeepsEveryLogicalLine)
{
	std::mt19937 random(445);
	TerminalModel model(60, 20);
	std::uniform_int_distribution<int> printable(0x20, 0x7E);
	std::uniform_int_distribution<int> wide(0, 20);
	std::u32string text;
	for( std::size_t line = 0; line < 40; ++line ) {
		const auto length = 1 + random() % 160;
		for( std::size_t column = 0; column < length; ++column )
			text.push_back(wide(random) == 0 ? U'\u65e5' : static_cast<char32_t>(printable(random)));
		text += U"\r\n";
	}
	Print(model, text);
	const auto before = LogicalLines(model);
	// Compare content only: a reflow that frees rows pads the screen bottom
	// with blank rows, so trailing empty logical lines differ legitimately.
	const auto nonEmpty = []( std::vector<std::wstring> lines ) {
		lines.erase(std::remove_if(lines.begin(), lines.end(),
			[]( const std::wstring& line ) { return line.empty(); }), lines.end());
		return lines;
	};
	model.Resize(30, 20);
	EXPECT_EQ(nonEmpty(before), nonEmpty(LogicalLines(model)));
	model.Resize(60, 20);
	EXPECT_EQ(nonEmpty(before), nonEmpty(LogicalLines(model)));
}

// The logical scrollback texts, oldest first (D2/D3 assert contents and
// order, not just the row count).
std::vector<std::wstring> ScrollbackLineTexts( const TerminalModel& model )
{
	std::vector<std::wstring> lines;
	std::wstring current;
	for( const auto& row : model.Scrollback() ) {
		current += RowWrittenText(row, false);
		if( !row.wrapped ) {
			lines.push_back(std::move(current));
			current.clear();
		}
	}
	if( !current.empty() ) lines.push_back(std::move(current));
	return lines;
}

TEST(TerminalReflow, RowsOnlyShrinkKeepsRingOrderWithFullHistory)
{
	TerminalModel model(10, 4, 3);
	Print(model, U"A\r\nB\r\nC\r\nD\r\nE\r\nF\r\nG\r\nH\r\n");
	// The bounded scrollback holds the last three pushed rows.
	ASSERT_EQ(3u, model.ScrollbackSize());
	// A rows-only shrink pushes the top screen row into the full ring; the
	// retained rows must keep their oldest-first order.
	model.Resize(10, 3);
	EXPECT_EQ((std::vector<std::wstring>{ L"D", L"E", L"F" }), ScrollbackLineTexts(model));
	// One more line must evict the oldest retained row, not the newest.
	Print(model, U"I\r\n");
	EXPECT_EQ((std::vector<std::wstring>{ L"E", L"F", L"G" }), ScrollbackLineTexts(model));
}

TEST(TerminalReflow, ColumnReflowNearTheLimitKeepsContentsAndOrder)
{
	TerminalModel model(10, 4, 3);
	Print(model, U"L0abcdef\r\nL1abcdef\r\nL2abcdef\r\nL3abcdef\r\n");
	Print(model, U"L4abcdef\r\nL5abcdef\r\nL6abcdef\r\nL7abcdef\r\n");
	// History holds L2..L4 (the cap evicted L0, L1); the screen shows
	// L5..L7 plus a blank.  Reflowing to half width splits every row in
	// two and pushes the reflowed screen's top rows into history.
	ASSERT_EQ(3u, model.ScrollbackSize());
	model.Resize(5, 4);
	// Only the newest three rows may be retained, in order: L5's two
	// chunks and the chunk of L6 pushed off the reflowed screen.
	EXPECT_EQ((std::vector<std::wstring>{ L"L5abcdef", L"L6abc" }), ScrollbackLineTexts(model));
	const auto change = model.ConsumeScrollbackChange();
	ASSERT_TRUE(change.Reflow());
	// A retained screen row maps onto its own text: old screen row 0
	// (global 3, L5) begins the line that now starts the scrollback.
	std::size_t newRow = 0, newColumn = 0;
	ASSERT_TRUE(change.Reflow()->MapPoint(3, 0, model.Columns(), newRow, newColumn));
	EXPECT_EQ(0u, newColumn);
	EXPECT_EQ(L"L5abc", RowWrittenText(model.Scrollback()[newRow], false));
}

TEST(TerminalReflow, AutowrapAtRegionBottomKeepsTheJoinedLine)
{
	TerminalModel model(10, 5);
	Print(model, U"top");
	model.SetScrollRegion(1, 3);
	model.SetCursorPosition(0, 3);
	// The line is longer than the width: its autowrap scrolls the region.
	// R2(d) exempts the autowrap path from the seam clear, so the moved
	// leader keeps its flag and the whole line stays joined.
	Print(model, U"0123456789AB");
	EXPECT_TRUE(model.Rows()[2].wrapped);
	const auto lines = LogicalLines(model);
	EXPECT_NE(lines.end(), std::find(lines.begin(), lines.end(), L"0123456789AB"));
}

TEST(TerminalReflow, ScrollingAWholeTopRegionClearsNoFlagBelow)
{
	TerminalModel model(10, 30);
	for( std::size_t line = 0; line < 14; ++line ) Print(model, U"filler\r\n");
	Print(model, U"01234567890123456789ABCDE");
	// Rows 14-16 form one joined line; row 15 is a wrapped leader.
	ASSERT_TRUE(model.Rows()[15].wrapped);
	model.SetScrollRegion(0, 9);
	model.ScrollUp(10); // the whole region scrolls; nothing below it may change
	EXPECT_TRUE(model.Rows()[15].wrapped);
	// DeleteLines through the region bottom has the same underflow.
	model.SetCursorPosition(0, 0);
	model.DeleteLines(999);
	EXPECT_TRUE(model.Rows()[15].wrapped);
	const auto lines = LogicalLines(model);
	EXPECT_NE(lines.end(), std::find(lines.begin(), lines.end(), L"01234567890123456789ABCDE"));
}

TEST(TerminalReflow, WrappedRowTailPositionCountsAsTheJoinedLength)
{
	TerminalModel model(10, 4);
	Print(model, U"AB");
	model.SetCursorPosition(9, 0);
	// The wide cell does not fit at column 9: row 0 wraps with an
	// unwritten tail, so positions in it count as the joined length (R3).
	Print(model, U"\u65e5");
	Print(model, U"cdefgh");
	model.Resize(5, 4);
	const auto change = model.ConsumeScrollbackChange();
	ASSERT_TRUE(change.Reflow());
	const auto screenBase = model.ScrollbackSize();
	std::size_t tailRow = 0, tailColumn = 0, edgeRow = 0, edgeColumn = 0;
	ASSERT_TRUE(change.Reflow()->MapPoint(screenBase, 7, model.Columns(), tailRow, tailColumn));
	ASSERT_TRUE(change.Reflow()->MapPoint(screenBase, 2, model.Columns(), edgeRow, edgeColumn));
	EXPECT_EQ(edgeRow, tailRow);
	EXPECT_EQ(edgeColumn, tailColumn);
}

// D8: a half-open boundary after the rightmost cell (column == columns)
// maps to a boundary after the same cell, not to the last cell inside.
TEST(TerminalReflow, RightEdgeBoundaryMapsToABoundaryAfterTheSameCell)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789");
	// Move the cursor off the pending wrap so the line stays a complete
	// non-wrapped line of exactly one row.
	model.SetCursorPosition(0, 1);
	Print(model, U"AB");
	model.Resize(5, 4);
	const auto change = model.ConsumeScrollbackChange();
	ASSERT_TRUE(change.Reflow());
	const auto screenBase = model.ScrollbackSize();
	std::size_t newRow = 0, newColumn = 0;
	// The boundary after the last cell of the first line (itself a
	// boundary, not a cell) must land after the same cell at the new
	// width: the second split row's right-edge boundary, not cell 4.
	ASSERT_TRUE(change.Reflow()->MapPoint(screenBase, 10, model.Columns(), newRow, newColumn, true));
	EXPECT_EQ(1u, newRow);
	EXPECT_EQ(model.Columns(), newColumn);
	// The same position as a cell keeps the clamped last-column answer.
	ASSERT_TRUE(change.Reflow()->MapPoint(screenBase, 10, model.Columns(), newRow, newColumn, false));
	EXPECT_EQ(1u, newRow);
	EXPECT_EQ(model.Columns() - 1, newColumn);
}

TEST(TerminalReflow, EraseDisplayClearsThroughTheLastRow)
{
	TerminalModel model(10, 3);
	model.SetScrollRegion(0, 1); // leave row 2 outside the region
	model.SetCursorPosition(0, 2);
	Print(model, U"0123456789");
	// The next printable character fires the autowrap on the last row.
	// The region scroll it triggers leaves the flag standing there.
	Print(model, U"A");
	ASSERT_TRUE(model.Rows()[2].wrapped);
	model.SetScrollRegion(0, 2);
	model.EraseDisplay(2);
	EXPECT_FALSE(model.Rows()[2].wrapped);
}

TEST(TerminalReflow, ReflowDropsTheColoredUnwrittenTailOfAWrappedRow)
{
	TerminalModel model(10, 4);
	Print(model, U"01234");
	model.SetCursorPosition(9, 0);
	Print(model, U"\u65e5"); // wraps; row 0 = "01234" + unwritten tail
	model.SetCursorPosition(5, 0);
	// An erase with a background color marks the unwritten tail as content
	// even though nothing was ever written there (R5 uses the written end).
	model.SetBackground(terminal::TerminalColor::Indexed(4));
	model.EraseLine(0);
	model.SetCursorPosition(2, 1);
	Print(model, U"cd");
	model.Resize(7, 4);
	// The colored tail must not re-enter the stream: the joined line is
	// exact, without blank cells glued into its middle.
	std::wstring joined;
	for( const auto& line : LogicalLines(model) ) {
		if( line.find(L"01234") != std::wstring::npos ) joined = line;
	}
	EXPECT_EQ(L"01234\u65e5cd", joined);
}

TEST(TerminalReflow, ColumnReflowRetiresEveryOldOrdinal)
{
	TerminalModel model(20, 4, 5);
	Print(model, U"A\r\nB\r\nC\r\nD\r\nE\r\nF\r\nG\r\nH\r\nI\r\n");
	ASSERT_GT(model.ScrollbackSize(), 0u);
	const auto appendedBefore = model.HistoryAppendedTotal();
	model.Resize(10, 4);
	// The reflow renumbered every retained row: all old ordinals retire
	// (new evicted = old appended) and the retained rows take fresh
	// ordinals above them, so both totals stay monotonic.
	EXPECT_EQ(appendedBefore, model.HistoryEvictedTotal());
	EXPECT_EQ(model.HistoryEvictedTotal() + model.ScrollbackSize(), model.HistoryAppendedTotal());
}

// Phase 18 item 1 (a): narrowing keeps the distance a cursor sits past
// the content on the line's last output row.
TEST(TerminalReflow, CursorPastAWrappedLineKeepsItsDistanceNarrowing)
{
	TerminalModel model(30, 4);
	Print(model, U"abcdefghijklmnopqrstuvwxy"); // 25 chars, no wrap yet
	ASSERT_EQ(0u, model.CursorRow());
	ASSERT_EQ(25u, model.CursorColumn());
	model.Resize(10, 4);
	// "uvwxy" is the last piece (5 cells); the cursor sat right after the
	// content, so it lands right after it on the last output row.
	EXPECT_EQ(2u, model.CursorRow());
	EXPECT_EQ(5u, model.CursorColumn());
}

// Phase 18 item 1 (b): widening keeps the distance a cursor sits past
// the content of a wrapped row.
TEST(TerminalReflow, CursorPastAWrappedLineKeepsItsDistanceWidening)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789abcde"); // row 0 wraps, "abcde" on row 1
	ASSERT_EQ(1u, model.CursorRow());
	ASSERT_EQ(5u, model.CursorColumn());
	model.Resize(20, 4);
	EXPECT_EQ(0u, model.CursorRow());
	EXPECT_EQ(15u, model.CursorColumn());
}

// Phase 18 item 1 (c): the DECSC saved cursor follows the same rule.
TEST(TerminalReflow, SavedCursorPastAWrappedLineKeepsItsDistanceWidening)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789abcde");
	model.SaveCursor(); // (1, 5)
	model.SetCursorPosition(0, 0);
	model.Resize(20, 4);
	model.RestoreCursor();
	EXPECT_EQ(0u, model.CursorRow());
	EXPECT_EQ(15u, model.CursorColumn());
}

// Phase 18 item 1 (d): the saved main cursor and the saved main DECSC
// follow the same rule when the resize happens under the alternate
// screen.
TEST(TerminalReflow, SavedMainPositionsKeepTheDistancePastAWrappedLine)
{
	TerminalModel model(10, 4);
	Print(model, U"0123456789abcde"); // cursor (1, 5)
	model.SaveCursor();
	model.SetAlternateScreen(true); // saves main cursor (1,5) + DECSC (1,5)
	model.Resize(20, 4);
	model.SetAlternateScreen(false);
	EXPECT_EQ(0u, model.CursorRow());
	EXPECT_EQ(15u, model.CursorColumn());
	model.RestoreCursor();
	EXPECT_EQ(0u, model.CursorRow());
	EXPECT_EQ(15u, model.CursorColumn());
}

// Phase 18 item 2: a history line that continues onto screen row 0 is
// wrapped on its last history row, so that row contributes cells up to
// its written end (R5), not its content end.  A colored unwritten tail
// must not re-enter the stream as a blank-only history row.
TEST(TerminalReflow, HistoryLineEndingOnTheScreenUsesTheWrittenEnd)
{
	TerminalModel model(10, 2);
	TerminalParser parser(model);
	// Color the whole row (EL2 under a red background), print 9 chars,
	// then a wide character that wraps and leaves column 9 as a colored
	// unwritten gap.
	parser.Feed("\x1b[41m\x1b[2K\x1b[0m");
	parser.Feed("abcdefghi");
	parser.Feed("\xE6\x97\xA5"); // U+65E5
	parser.Feed("\r\n");         // row 0 -> history, wrapped onto screen row 0
	ASSERT_EQ(1u, model.ScrollbackSize());
	model.Resize(9, 2);
	// With the written end the colored gap contributes nothing: the line
	// is "abcdefghi" + the wide character, one history row.
	EXPECT_EQ(1u, model.ScrollbackSize());
	const auto lines = LogicalLines(model);
	ASSERT_FALSE(lines.empty());
	EXPECT_EQ(L"abcdefghi\u65e5", lines.front());
}

} // namespace
