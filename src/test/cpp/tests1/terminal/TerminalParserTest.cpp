/*! @file */
#include "pch.h"
#include "terminal/parser/TerminalParser.h"

#include <algorithm>
#include <array>

namespace {

std::wstring RowText( const terminal::TerminalRow& row )
{
	std::wstring result;
	for( const auto& cell : row.cells ) if( !cell.continuation ) result.append(cell.Text());
	return result;
}

TEST(TerminalParser, StreamsSplitUtf8AndSplitVtSequences)
{
	terminal::TerminalModel model(12, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\xE6");
	parser.Feed("\x97");
	parser.Feed("\xA5");
	parser.Feed("\x1b[3");
	parser.Feed("1mX");
	EXPECT_EQ(L"日X", RowText(model.Rows()[0]));
	EXPECT_EQ(terminal::TerminalColor::Indexed(1), model.Rows()[0].AttributesAt(2).foreground);
}

TEST(TerminalParser, StreamsWideAndCombiningText)
{
	terminal::TerminalModel model(12, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\xE6\x97\xA5" "e\xCC");
	parser.Feed("\x81");
	EXPECT_EQ(3u, model.CursorColumn());
	EXPECT_EQ(L"e\u0301", model.Rows()[0].cells[2].Text());
}

TEST(TerminalParser, AppliesSixteenIndexedAndTrueColors)
{
	terminal::TerminalModel model(12, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b[91mA\x1b[38;5;202mB\x1b[48;2;1;2;3mC");
	EXPECT_EQ(terminal::TerminalColor::Indexed(9), model.Rows()[0].AttributesAt(0).foreground);
	EXPECT_EQ(terminal::TerminalColor::Indexed(202), model.Rows()[0].AttributesAt(1).foreground);
	EXPECT_EQ(terminal::TerminalColor::Rgb(1, 2, 3), model.Rows()[0].AttributesAt(2).background);
}

TEST(TerminalParser, ResetsPowerShellAnsiBlackToThemeDefault)
{
	terminal::TerminalModel model(8, 1);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b[30mP\x1b[0mS");
	EXPECT_EQ(terminal::TerminalColor::Indexed(0), model.Rows()[0].AttributesAt(0).foreground);
	EXPECT_EQ(terminal::TerminalColorKind::Default, model.Rows()[0].AttributesAt(1).foreground.kind);
}

TEST(TerminalParser, StreamsColonDelimitedExtendedSgrColorsAcrossFeedBoundaries)
{
	terminal::TerminalModel model(12, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b[1;38:2::12:34");
	parser.Feed(":56mF\x1b[48:2::7:8");
	parser.Feed(":9mB\x1b[38:5:201mI\x1b[48:5:202mJ");

	const auto& foreground = model.Rows()[0].AttributesAt(0);
	EXPECT_TRUE(foreground.bold);
	EXPECT_EQ(terminal::TerminalColor::Rgb(12, 34, 56), foreground.foreground);
	EXPECT_EQ(terminal::TerminalColor::Rgb(7, 8, 9), model.Rows()[0].AttributesAt(1).background);
	EXPECT_EQ(terminal::TerminalColor::Indexed(201), model.Rows()[0].AttributesAt(2).foreground);
	EXPECT_EQ(terminal::TerminalColor::Indexed(202), model.Rows()[0].AttributesAt(3).background);
}

TEST(TerminalParser, LeavesUnsupportedSgrSubparametersUndispatched)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b[1;4:3mX");
	const auto& attributes = model.Rows()[0].AttributesAt(0);
	EXPECT_FALSE(attributes.bold);
	EXPECT_FALSE(attributes.underline);
}

TEST(TerminalParser, AppliesAndResetsBoldUnderlineAndInverse)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b[1;4;7mA\x1b[0mB");
	const auto& emphasized = model.Rows()[0].AttributesAt(0);
	EXPECT_TRUE(emphasized.bold);
	EXPECT_TRUE(emphasized.underline);
	EXPECT_TRUE(emphasized.inverse);
	const auto& reset = model.Rows()[0].AttributesAt(1);
	EXPECT_FALSE(reset.bold);
	EXPECT_FALSE(reset.underline);
	EXPECT_FALSE(reset.inverse);
}

TEST(TerminalParser, HandlesCarriageReturnLineFeedBackspaceAndTab)
{
	terminal::TerminalModel model(16, 3);
	terminal::TerminalParser parser(model);
	parser.Feed("ab\bZ\rQ\n\tX");
	EXPECT_EQ(L"QZ", RowText(model.Rows()[0]));
	EXPECT_EQ(L"X", model.Rows()[1].cells[8].Text());
}

TEST(TerminalParser, HandlesCursorEraseSaveRestoreAndScrollRegion)
{
	terminal::TerminalModel model(8, 4);
	terminal::TerminalParser parser(model);
	parser.Feed("abc\x1b[2D\x1b[sZ\x1b[u\x1b[1CY\x1b[2;3r");
	EXPECT_EQ(L"aZY", RowText(model.Rows()[0]));
	parser.Feed("\x1b[2K");
	EXPECT_TRUE(RowText(model.Rows()[0]).empty());
}

TEST(TerminalParser, TogglesAlternateScreenBracketedPasteAndMouseModes)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("M\x1b[?1049hA\x1b[?2004h\x1b[?1000h\x1b[?1006h");
	EXPECT_TRUE(model.IsAlternateScreen());
	EXPECT_TRUE(model.Modes().bracketedPaste);
	EXPECT_TRUE(model.Modes().mouseButtonTracking);
	EXPECT_TRUE(model.Modes().mouseSgrEncoding);
	parser.Feed("\x1b[?1049l");
	EXPECT_EQ(L"M", RowText(model.Rows()[0]));
}

TEST(TerminalParser, SanitizesAndLimitsSplitOscTitle)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b]2;safe");
	parser.Feed(std::string("\x01", 1));
	parser.Feed(std::string(300, 'x'));
	parser.Feed("\x1b\\");
	ASSERT_EQ(256u, model.Title().size());
	EXPECT_EQ(L"safex", model.Title().substr(0, 5));
}

TEST(TerminalParser, DecodesUtf8OscTitleAcrossFeedBoundaries)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\x1b]2;\xE6");
	parser.Feed("\x97");
	parser.Feed("\xA5\x07");
	EXPECT_EQ(L"日", model.Title());
}

TEST(TerminalParser, DisablesOsc52SixelAndArbitraryStringCommands)
{
	terminal::TerminalModel model(20, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("base\x1b]52;c;SGVsbG8=\x07");
	parser.Feed("\x1bPqSIXEL-DATA\x1b\\");
	parser.Feed("\x1b_arbitrary-command\x1b\\ok");
	EXPECT_TRUE(model.Title().empty());
	EXPECT_EQ(L"baseok", RowText(model.Rows()[0]));
}

TEST(TerminalParser, ReplacesIncompleteUtf8OnlyWhenFlushed)
{
	terminal::TerminalModel model(8, 2);
	terminal::TerminalParser parser(model);
	parser.Feed("\xE3\x81");
	EXPECT_TRUE(RowText(model.Rows()[0]).empty());
	parser.Flush();
	EXPECT_EQ(L"\xFFFD", RowText(model.Rows()[0]));
}

TEST(TerminalParser, StreamsTenMiBOfAnsiTextWithoutCorruptingTheTerminalModel)
{
	constexpr std::size_t totalBytes = 10u * 1024u * 1024u;
	constexpr std::size_t scrollbackLimit = 64;
	const std::string record = "\x1b[31m\xE6\x97\xA5R\x1b[0m ordinary text\r\n";
	const std::string finalRecord = "\r\nFINAL-\xE6\x97\xA5-\x1b[32mG\x1b[0m\r\n";

	std::string stream;
	stream.reserve(totalBytes);
	while( stream.size() + record.size() + finalRecord.size() <= totalBytes ) {
		stream.append(record);
	}
	stream.append(totalBytes - finalRecord.size() - stream.size(), 'x');
	stream.append(finalRecord);
	ASSERT_EQ(totalBytes, stream.size());

	terminal::TerminalModel model(80, 4, scrollbackLimit);
	terminal::TerminalParser parser(model);
	const std::array<std::size_t, 8> chunkSizes = { 1, 2, 4, 17, 64, 257, 1024, 4093 };
	for( std::size_t offset = 0, chunk = 0; offset < stream.size(); ++chunk ) {
		const auto size = std::min(chunkSizes[chunk % chunkSizes.size()], stream.size() - offset);
		parser.Feed(std::string_view(stream).substr(offset, size));
		offset += size;
	}
	parser.Flush();

	EXPECT_EQ(scrollbackLimit, model.ScrollbackLimit());
	EXPECT_EQ(scrollbackLimit, model.ScrollbackSize());
	EXPECT_EQ(4u, model.Rows().size());

	bool foundFinalRecord = false;
	for( const auto& row : model.Rows() ) {
		const auto text = RowText(row);
		if( text.find(L"FINAL-\u65E5-G") == std::wstring::npos ) continue;
		foundFinalRecord = true;
		EXPECT_EQ(terminal::TerminalColor::Indexed(2), row.AttributesAt(9).foreground);
	}
	EXPECT_TRUE(foundFinalRecord);
}

} // namespace
