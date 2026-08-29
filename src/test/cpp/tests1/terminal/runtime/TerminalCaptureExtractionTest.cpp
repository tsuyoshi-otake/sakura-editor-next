/*! @file @brief Tests for bounded parsed terminal capture extraction. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "StdAfx.h"

#include "terminal/runtime/TerminalCaptureExtraction.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

namespace terminal {
namespace {

void PrintLine( TerminalModel& model, std::u32string_view text )
{
	for( const auto codepoint : text ) model.Print(codepoint);
	model.ExecuteControl(L'\r');
	model.ExecuteControl(L'\n');
}

TerminalCaptureExtractionRequest FullCapture( bool join = false )
{
	TerminalCaptureExtractionRequest request;
	request.startLine = -1000;
	request.endLine = 1000;
	request.joinWrappedLines = join;
	request.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	return request;
}

TEST(TerminalCaptureExtraction, SelectsRetainedHistoryAndVisibleRowsBeforeSerializing)
{
	TerminalModel model(6, 2, 4);
	PrintLine(model, U"one");
	PrintLine(model, U"two");
	for( const auto codepoint : std::u32string_view{ U"three" } ) model.Print(codepoint);

	auto request = FullCapture();
	request.startLine = -1;
	request.endLine = 1;
	const auto result = ExtractTerminalCapture(model, request);
	ASSERT_EQ(TerminalCaptureResultCode::Succeeded, result.code);
	ASSERT_EQ(3U, result.lines.size());
	EXPECT_EQ(-1, result.lines[0].firstRow);
	EXPECT_EQ(u"one", result.lines[0].text);
	EXPECT_EQ(u"two", result.lines[1].text);
	EXPECT_EQ(u"three", result.lines[2].text);
	EXPECT_FALSE(result.truncated);
}

TEST(TerminalCaptureExtraction, SelectsDisjointDeltaRangesBeforeSerializing)
{
	TerminalModel model(8, 4);
	PrintLine(model, U"zero");
	PrintLine(model, U"one");
	PrintLine(model, U"two");
	for( const auto codepoint : std::u32string_view{ U"three" } ) model.Print(codepoint);

	auto request = FullCapture();
	request.selectedRanges = std::vector<TerminalRowRange>{ { 0, 0 }, { 2, 2 } };
	const auto result = ExtractTerminalCapture(model, request);
	ASSERT_EQ(TerminalCaptureResultCode::Succeeded, result.code);
	ASSERT_EQ(2U, result.lines.size());
	EXPECT_EQ(0, result.lines[0].firstRow);
	EXPECT_EQ(u"zero", result.lines[0].text);
	EXPECT_EQ(2, result.lines[1].firstRow);
	EXPECT_EQ(u"two", result.lines[1].text);
}

TEST(TerminalCaptureExtraction, JoinsWrappedRowsAndPreservesCellPositions)
{
	TerminalModel model(4, 3);
	for( const auto codepoint : std::u32string_view{ U"ABCDEF" } ) model.Print(codepoint);
	const auto separate = ExtractTerminalCapture(model, FullCapture(false));
	ASSERT_GE(separate.lines.size(), 2U);
	EXPECT_EQ(u"ABCD", separate.lines[0].text);
	EXPECT_EQ(u"EF", separate.lines[1].text);

	const auto joined = ExtractTerminalCapture(model, FullCapture(true));
	ASSERT_GE(joined.lines.size(), 1U);
	EXPECT_EQ(u"ABCDEF  ", joined.lines[0].text);
	EXPECT_TRUE(joined.lines[0].joined);
	EXPECT_EQ(0, joined.lines[0].firstRow);
	EXPECT_EQ(1, joined.lines[0].lastRow);
}

TEST(TerminalCaptureExtraction, ExcludesMainHistoryOnAlternateScreen)
{
	TerminalModel model(4, 2, 4);
	PrintLine(model, U"old");
	PrintLine(model, U"main");
	model.SetAlternateScreen(true);
	for( const auto codepoint : std::u32string_view{ U"alt" } ) model.Print(codepoint);

	auto request = FullCapture();
	request.startLine = -1;
	const auto result = ExtractTerminalCapture(model, request);
	ASSERT_EQ(TerminalCaptureResultCode::Succeeded, result.code);
	ASSERT_FALSE(result.lines.empty());
	EXPECT_EQ(0, result.lines.front().firstRow);
	EXPECT_EQ(u"alt", result.lines.front().text);
	EXPECT_TRUE(result.alternateScreen);
}

TEST(TerminalCaptureExtraction, CountsWideAndAstralGraphemesWithoutContinuationDuplicates)
{
	TerminalModel model(8, 1);
	model.Print(U'\u65e5');
	model.Print(U'\U0001f600');
	const auto result = ExtractTerminalCapture(model, FullCapture(false));
	ASSERT_EQ(1U, result.lines.size());
	EXPECT_EQ(u"\u65e5\U0001f600", result.lines[0].text);
	EXPECT_EQ(3U, result.codeUnits);
	EXPECT_EQ(8U, result.utf8Bytes); // 3 + 4 UTF-8 bytes plus LF.
}

TEST(TerminalCaptureExtraction, ReportsEachHardLimitWithoutReturningAnUnmarkedPartial)
{
	TerminalModel model(4, 3);
	PrintLine(model, U"aaaa");
	PrintLine(model, U"bbbb");
	for( const auto codepoint : std::u32string_view{ U"cccc" } ) model.Print(codepoint);

	auto rows = FullCapture();
	rows.limits.maximumPhysicalRows = 1;
	auto result = ExtractTerminalCapture(model, rows);
	EXPECT_TRUE(result.truncated);
	EXPECT_EQ(TerminalCaptureTruncationReason::Rows, result.truncationReason);

	auto codeUnits = FullCapture();
	codeUnits.limits.maximumCodeUnits = 3;
	result = ExtractTerminalCapture(model, codeUnits);
	EXPECT_TRUE(result.truncated);
	EXPECT_EQ(TerminalCaptureTruncationReason::CodeUnits, result.truncationReason);

	auto utf8 = FullCapture();
	utf8.limits.maximumUtf8Bytes = 4;
	result = ExtractTerminalCapture(model, utf8);
	EXPECT_TRUE(result.truncated);
	EXPECT_EQ(TerminalCaptureTruncationReason::Utf8Bytes, result.truncationReason);
}

TEST(TerminalCaptureExtraction, RejectsReversedRange)
{
	TerminalModel model(4, 2);
	auto request = FullCapture();
	request.startLine = 1;
	request.endLine = 0;
	const auto result = ExtractTerminalCapture(model, request);
	EXPECT_EQ(TerminalCaptureResultCode::InvalidRequest, result.code);
	EXPECT_TRUE(result.lines.empty());
}

} // namespace
} // namespace terminal
