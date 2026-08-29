/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/runtime/TerminalCaptureIndex.h"

#include <gtest/gtest.h>

namespace terminal {
namespace {

TerminalCaptureIndex MakeIndex(TerminalCaptureIndexLimits limits = {})
{
	return TerminalCaptureIndex(
		TerminalRuntimeGeneration{ 1 }, TerminalInstanceId{ 9 }, 3, 1,
		TerminalContentRevision{ 1 }, 100, limits);
}

TEST(TerminalCaptureIndex, CoalescesCurrentDirtyRangesWithoutCopyingText)
{
	auto index = MakeIndex();
	const auto initial = index.CurrentCursor();
	TerminalChangeRecord first;
	first.revision = TerminalContentRevision{ 2 };
	first.screenEpoch = 1;
	first.dirtyScreenRanges = { { 7, 9 }, { 2, 3 } };
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(first)));
	TerminalChangeRecord second;
	second.revision = TerminalContentRevision{ 3 };
	second.screenEpoch = 1;
	second.dirtyScreenRanges = { { 4, 6 }, { 20, 20 } };
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(second)));

	const auto delta = index.ChangesSince(initial);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Delta, delta.code);
	EXPECT_FALSE(delta.gap);
	EXPECT_EQ((std::vector<TerminalRowRange>{ { 2, 9 }, { 20, 20 } }), delta.dirtyScreenRanges);
	EXPECT_EQ(3u, delta.nextCursor.revision.value);
}

TEST(TerminalCaptureIndex, AggregatesContiguousAppendedHistoryOrdinals)
{
	auto index = MakeIndex();
	const auto initial = index.CurrentCursor();
	TerminalChangeRecord first;
	first.revision = TerminalContentRevision{ 2 };
	first.screenEpoch = 1;
	first.appendedHistoryBeginOrdinal = 100;
	first.appendedHistoryEndOrdinal = 101;
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(first)));
	TerminalChangeRecord second;
	second.revision = TerminalContentRevision{ 3 };
	second.screenEpoch = 1;
	second.appendedHistoryBeginOrdinal = 102;
	second.appendedHistoryEndOrdinal = 104;
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(second)));

	const auto delta = index.ChangesSince(initial);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Delta, delta.code);
	EXPECT_EQ(100u, delta.appendedHistoryBeginOrdinal);
	EXPECT_EQ(104u, delta.appendedHistoryEndOrdinal);
}

TEST(TerminalCaptureIndex, ReportsGapWhenTheBoundedJournalEvictsHistory)
{
	auto index = MakeIndex({ 1, 16 });
	const auto initial = index.CurrentCursor();
	TerminalChangeRecord first;
	first.revision = TerminalContentRevision{ 2 };
	first.screenEpoch = 1;
	first.dirtyScreenRanges = { { 1, 1 } };
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(first)));
	TerminalChangeRecord second;
	second.revision = TerminalContentRevision{ 3 };
	second.screenEpoch = 1;
	second.dirtyScreenRanges = { { 2, 2 } };
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(second)));

	const auto delta = index.ChangesSince(initial);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Gap, delta.code);
	EXPECT_TRUE(delta.gap);
	EXPECT_TRUE(delta.resyncSnapshot);
	EXPECT_EQ(2u, delta.earliestCursor.revision.value);
}

TEST(TerminalCaptureIndex, ScrollbackEvictionInvalidatesAnOlderBaseOrdinal)
{
	auto index = MakeIndex();
	const auto initial = index.CurrentCursor();
	TerminalChangeRecord change;
	change.revision = TerminalContentRevision{ 2 };
	change.screenEpoch = 1;
	change.evictedThroughOrdinal = 105;
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(change)));

	const auto delta = index.ChangesSince(initial);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Gap, delta.code);
	EXPECT_EQ(106u, delta.nextCursor.scrollbackBaseOrdinal);
}

TEST(TerminalCaptureIndex, ACurrentRevisionWithAnOlderBaseStillReportsAGap)
{
	auto index = MakeIndex();
	TerminalChangeRecord change;
	change.revision = TerminalContentRevision{ 2 };
	change.screenEpoch = 1;
	change.evictedThroughOrdinal = 105;
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.Record(std::move(change)));
	auto forgedCurrent = index.CurrentCursor();
	forgedCurrent.scrollbackBaseOrdinal = 100;

	const auto delta = index.ChangesSince(forgedCurrent);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Gap, delta.code);
	EXPECT_TRUE(delta.resyncSnapshot);
}

TEST(TerminalCaptureIndex, ANewScreenEpochRequiresBoundedResynchronization)
{
	auto index = MakeIndex();
	const auto mainScreen = index.CurrentCursor();
	ASSERT_EQ(ETerminalCaptureIndexRecordCode::Succeeded, index.ResetScreen(2, 0));

	const auto delta = index.ChangesSince(mainScreen);
	EXPECT_EQ(ETerminalCaptureDeltaCode::Gap, delta.code);
	EXPECT_TRUE(delta.resyncSnapshot);
	EXPECT_EQ(2u, delta.nextCursor.screenEpoch);
}

TEST(TerminalCaptureIndex, RejectsSkippedRevisionWithoutChangingTheCursor)
{
	auto index = MakeIndex();
	TerminalChangeRecord invalid;
	invalid.revision = TerminalContentRevision{ 3 };
	invalid.screenEpoch = 1;
	EXPECT_EQ(ETerminalCaptureIndexRecordCode::InvalidRecord, index.Record(std::move(invalid)));
	EXPECT_EQ(1u, index.CurrentCursor().revision.value);
}

} // namespace
} // namespace terminal
