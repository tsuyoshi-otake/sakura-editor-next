/*! @file */
#include "pch.h"
#include "terminal/unicode/TerminalGraphemeWidth.h"

namespace {

void ExpectOneGrapheme(std::wstring_view text, int width)
{
	const auto measured = terminal::unicode::MeasureFirstGrapheme(text);
	EXPECT_EQ(text.size(), measured.codeUnits);
	EXPECT_EQ(width, measured.width);
}

TEST(TerminalUnicode, MeasuresAsciiCjkAndCombiningText)
{
	ExpectOneGrapheme(L"A", 1);
	ExpectOneGrapheme(L"\u65E5", 2);
	ExpectOneGrapheme(L"e\u0301", 1);
}

TEST(TerminalUnicode, MeasuresEmojiVariationZwjFlagsAndSkinTone)
{
	ExpectOneGrapheme(L"\u2764\uFE0F", 2);
	ExpectOneGrapheme(L"\U0001F469\u200D\U0001F4BB", 2);
	ExpectOneGrapheme(L"\U0001F1EF\U0001F1F5", 2);
	ExpectOneGrapheme(L"\U0001F44D\U0001F3FD", 2);
}

TEST(TerminalUnicode, StopsAtTheFirstOfMultipleGraphemes)
{
	const auto measured = terminal::unicode::MeasureFirstGrapheme(L"ab");
	EXPECT_EQ(1u, measured.codeUnits);
	EXPECT_EQ(1, measured.width);
}

} // namespace
