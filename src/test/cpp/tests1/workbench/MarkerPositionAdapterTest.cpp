/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/problems/MarkerPositionAdapter.h"

#include <string_view>
#include <vector>

namespace workbench::problems {
namespace {

//! A tiny fake document: each entry is one line's content, EOL and BOM
//! already stripped, exactly like `CDocLine` holds it once loaded.
LogicLineContentLookup FakeDocument(std::vector<std::wstring_view> lines)
{
	return [lines = std::move(lines)](std::uint32_t zeroBasedLine) -> std::wstring_view {
		EXPECT_LT(zeroBasedLine, lines.size());
		return lines.at(zeroBasedLine);
	};
}

MarkerRange Range(std::uint32_t startLine, std::uint32_t startColumn, std::uint32_t endLine, std::uint32_t endColumn)
{
	return { startLine, startColumn, endLine, endColumn };
}

TEST(MarkerPositionAdapter, PassesThroughAnOrdinaryAsciiPosition)
{
	const auto doc = FakeDocument({ L"int main() {", L"    return 0;", L"}" });

	const auto converted = ConvertMarkerPositionToLogicPoint(1, 4, 3, doc);

	EXPECT_EQ(4, converted.position.GetX2().GetValue());
	EXPECT_EQ(1, converted.position.GetY2().GetValue());
	EXPECT_FALSE(converted.clamp.Any());
}

TEST(MarkerPositionAdapter, ColumnZeroOnAnEmptyLineIsExact)
{
	const auto doc = FakeDocument({ L"", L"x" });

	const auto converted = ConvertMarkerPositionToLogicPoint(0, 0, 2, doc);

	EXPECT_EQ(0, converted.position.GetX2().GetValue());
	EXPECT_EQ(0, converted.position.GetY2().GetValue());
	EXPECT_FALSE(converted.clamp.Any());
}

TEST(MarkerPositionAdapter, ColumnPastLineEndClampsToLineLength)
{
	const auto doc = FakeDocument({ L"abc" });

	const auto converted = ConvertMarkerPositionToLogicPoint(0, 99, 1, doc);

	EXPECT_EQ(3, converted.position.GetX2().GetValue());
	EXPECT_EQ(0, converted.position.GetY2().GetValue());
	EXPECT_FALSE(converted.clamp.line);
	EXPECT_TRUE(converted.clamp.column);
}

TEST(MarkerPositionAdapter, ColumnExactlyAtLineEndIsNotClamped)
{
	// A position at the line's length -- one past the last character -- is a
	// valid VS Code Position (it is where the caret sits after the last
	// character), not an out-of-range one.
	const auto doc = FakeDocument({ L"abc" });

	const auto converted = ConvertMarkerPositionToLogicPoint(0, 3, 1, doc);

	EXPECT_EQ(3, converted.position.GetX2().GetValue());
	EXPECT_FALSE(converted.clamp.Any());
}

TEST(MarkerPositionAdapter, LinePastDocumentEndClampsToLastLine)
{
	const auto doc = FakeDocument({ L"first", L"second" });

	const auto converted = ConvertMarkerPositionToLogicPoint(50, 0, 2, doc);

	EXPECT_EQ(1, converted.position.GetY2().GetValue());
	EXPECT_TRUE(converted.clamp.line);
}

TEST(MarkerPositionAdapter, EmptyDocumentClampsToOriginRegardlessOfRequestedPosition)
{
	const auto converted = ConvertMarkerPositionToLogicPoint(7, 12, 0, LogicLineContentLookup{});

	EXPECT_EQ(0, converted.position.GetX2().GetValue());
	EXPECT_EQ(0, converted.position.GetY2().GetValue());
	EXPECT_TRUE(converted.clamp.line);
	// Column 12 was requested and column 0 was produced, so the column axis
	// did move. `MarkerPositionClamp` reports the move on both axes here;
	// see ZeroPositionOnAnEmptyDocumentIsNotClamped for the exact case.
	EXPECT_TRUE(converted.clamp.column);
}

TEST(MarkerPositionAdapter, ZeroPositionOnAnEmptyDocumentIsNotClamped)
{
	const auto converted = ConvertMarkerPositionToLogicPoint(0, 0, 0, LogicLineContentLookup{});

	EXPECT_FALSE(converted.clamp.Any());
}

TEST(MarkerPositionAdapter, SurrogatePairSplitColumnMovesBeforeThePair)
{
	// U+1F600 GRINNING FACE, encoded as the surrogate pair D83D DE00, sitting
	// between an 'a' and a 'b'. Column 2 (between the two surrogate halves)
	// must never be produced.
	const std::wstring line = std::wstring(L"a") + wchar_t(0xD83D) + wchar_t(0xDE00) + L"b";
	const auto doc = FakeDocument({ line });

	const auto converted = ConvertMarkerPositionToLogicPoint(0, 2, 1, doc);

	EXPECT_EQ(1, converted.position.GetX2().GetValue());
	EXPECT_TRUE(converted.clamp.column);
}

TEST(MarkerPositionAdapter, ColumnBeforeOrAfterASurrogatePairIsUnaffected)
{
	const std::wstring line = std::wstring(L"a") + wchar_t(0xD83D) + wchar_t(0xDE00) + L"b";
	const auto doc = FakeDocument({ line });

	const auto before = ConvertMarkerPositionToLogicPoint(0, 1, 1, doc);
	EXPECT_EQ(1, before.position.GetX2().GetValue());
	EXPECT_FALSE(before.clamp.column);

	const auto after = ConvertMarkerPositionToLogicPoint(0, 3, 1, doc);
	EXPECT_EQ(3, after.position.GetX2().GetValue());
	EXPECT_FALSE(after.clamp.column);
}

TEST(MarkerPositionAdapter, TabsAreOrdinaryLogicColumnsNotExpanded)
{
	// Logic coordinates are pre-tab-expansion by construction (that is the
	// whole Logic/Layout distinction). A tab is one code unit, one logic
	// column, same as any other character -- this adapter must not try to
	// widen it.
	const auto doc = FakeDocument({ L"\tfoo" });

	const auto converted = ConvertMarkerPositionToLogicPoint(0, 1, 1, doc);

	EXPECT_EQ(1, converted.position.GetX2().GetValue());
	EXPECT_FALSE(converted.clamp.Any());
}

TEST(MarkerPositionAdapter, RangeStartHelperUsesOnlyTheStartPosition)
{
	const auto doc = FakeDocument({ L"line zero", L"line one" });
	const auto range = Range(1, 3, 1, 7);

	const auto converted = ConvertMarkerRangeStartToLogicPoint(range, 2, doc);

	EXPECT_EQ(3, converted.position.GetX2().GetValue());
	EXPECT_EQ(1, converted.position.GetY2().GetValue());
}

TEST(MarkerPositionAdapter, FullRangeConvertsBothEndpointsIndependently)
{
	const auto doc = FakeDocument({ L"first line", L"second line" });
	const auto range = Range(0, 2, 1, 999);

	const auto converted = ConvertMarkerRangeToLogicRange(range, 2, doc);

	EXPECT_EQ(2, converted.start.position.GetX2().GetValue());
	EXPECT_EQ(0, converted.start.position.GetY2().GetValue());
	EXPECT_FALSE(converted.start.clamp.Any());

	EXPECT_EQ(11, converted.end.position.GetX2().GetValue());
	EXPECT_EQ(1, converted.end.position.GetY2().GetValue());
	EXPECT_TRUE(converted.end.clamp.column);
}

} // namespace
} // namespace workbench::problems
