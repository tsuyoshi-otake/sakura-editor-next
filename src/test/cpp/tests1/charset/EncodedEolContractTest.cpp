/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CFileLoadScannerForTest.hpp"
#include "EncodedEolCorpus.h"
#include "EncodedEolTestSupport.h"
#include "TestMemoryPlacement.h"
#include "env/CReadManager.h"

namespace {

using namespace encoded_eol_corpus;

std::vector<Record> ReplayRanges(
	CFileLoadScannerForTest& scanner, Format format, bool extendedEol, const char* data, const std::vector<Range>& ranges)
{
	std::vector<Record> records;
	for (const auto& range : ranges) {
		const auto part = ScanRange(scanner, format, extendedEol, data, range.begin, range.end);
		for (const auto& record : part) {
			records.push_back(record);
		}
	}
	return records;
}

// EOF terminals of the partition planner, frozen as they behave today (specification, not refactoring):
//  - None: a file shorter than one unit has no planned range at all (GetNextLineOffset returns 0), so no line is read.
//  - CompleteUnits: the last complete unit is an EOL and a partial unit follows; the planned range stops after that EOL,
//    the partial bytes lie outside every range, and a direct scan of them keeps them as trailing content.
//  - WholeSize: the last complete unit is not an EOL; the planned range ends at the file size, partial bytes included.
// Changing any of these is a specification change for the document loader, not a refactoring.
enum class PlannedEnd { None, CompleteUnits, WholeSize };

struct EofTerminal {
	const char* const id;
	const PlannedEnd extendedOff;
	const PlannedEnd extendedOn;
};

constexpr std::array kEofTerminals{
	EofTerminal{"sub-unit-a1", PlannedEnd::None, PlannedEnd::None},
	EofTerminal{"sub-unit-a2", PlannedEnd::None, PlannedEnd::None},
	EofTerminal{"sub-unit-a3", PlannedEnd::None, PlannedEnd::None},
	EofTerminal{"sub-unit-lf1", PlannedEnd::None, PlannedEnd::None},
	EofTerminal{"sub-unit-cr1", PlannedEnd::None, PlannedEnd::None},
	EofTerminal{"tail-after-lf", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"tail-after-crlf", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"tail-after-lf-2", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"tail-after-lf-3", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"tail-after-text", PlannedEnd::WholeSize, PlannedEnd::WholeSize},
	EofTerminal{"tail-after-nel", PlannedEnd::WholeSize, PlannedEnd::CompleteUnits},
	EofTerminal{"cr-truncated-lf-1", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"cr-truncated-lf-2", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"cr-truncated-lf-3", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
	EofTerminal{"truncated-nel", PlannedEnd::WholeSize, PlannedEnd::WholeSize},
	EofTerminal{"start-at-tail", PlannedEnd::CompleteUnits, PlannedEnd::CompleteUnits},
};

} // namespace

TEST(EncodedEolContract, CorpusIsWellFormedAndCoversTheRequiredClasses)
{
	std::vector<Case> cases;
	try {
		cases = LoadCases();
	} catch (const std::exception& error) {
		FAIL() << error.what();
	}
	ASSERT_FALSE(cases.empty());

	std::set<std::string> classes;
	std::set<Format> formats;
	std::set<bool> extendedEol;
	for (const auto& testCase : cases) {
		classes.insert(testCase.cls);
		formats.insert(testCase.format);
		extendedEol.insert(testCase.extendedEol);
	}
	const std::set<std::string> expectedClasses{
		"basic", "empty", "extended", "lookalike", "no-eol", "positions", "runs", "short-tail",
		"start", "sub-unit", "surrogate", "truncated-pair", "utf32-high-bits",
	};
	const std::set<Format> expectedFormats{Format::Utf16Le, Format::Utf16Be, Format::Utf32Le, Format::Utf32Be};
	const std::set<bool> expectedFlags{false, true};
	EXPECT_EQ(expectedClasses, classes);
	EXPECT_EQ(expectedFormats, formats);
	EXPECT_EQ(expectedFlags, extendedEol);
}

TEST(EncodedEolContract, DirectScanMatchesTheCorpus)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		for (bool oddAddress : {false, true}) {
			SCOPED_TRACE(oddAddress ? "odd-address" : "page-end");
			const char* data = oddAddress ? region.PlaceAtOddAddress(c.bytes) : region.PlaceAtEnd(c.bytes);
			EXPECT_EQ(ToString(c.records), ToString(ScanRange(scanner, c.format, c.extendedEol, data, c.start, c.bytes.size())));
		}
	}
}

TEST(EncodedEolContract, EverySplitSeedLandsOnALineStartAndReplaysTheSingleScan)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		const size_t size = c.bytes.size();
		const char* data = region.PlaceAtEnd(c.bytes);
		scanner.Attach(TraitOf(c.format), data, size, c.extendedEol);
		const auto single = ScanRange(scanner, c.format, c.extendedEol, data, 0, size);
		const size_t plannedEnd = scanner.GetNextLineOffset(size);
		std::set<size_t> lineStarts{0, plannedEnd};
		for (const auto& record : single) {
			lineStarts.insert(record.offset + record.width);
		}
		for (size_t seed = 0; seed <= size; ++seed) {
			SCOPED_TRACE(testing::Message() << "seed=" << seed);
			const size_t split = scanner.GetNextLineOffset(seed);
			EXPECT_LE(split, plannedEnd);
			EXPECT_TRUE(lineStarts.contains(split)) << "split=" << split;
			std::vector<Range> ranges;
			if (0 < split) {
				ranges.push_back({0, split});
			}
			if (split < plannedEnd) {
				ranges.push_back({split, plannedEnd});
			}
			EXPECT_EQ(ToString(single), ToString(ReplayRanges(scanner, c.format, c.extendedEol, data, ranges)));
		}
	}
}

TEST(EncodedEolContract, ASplitInsideCrLfMovesPastTheLf)
{
	// Corpus row "crlf" is: A CR LF B (units 0..3). GetNextLineOffset steps back one unit and scans to the next line start,
	// so seeds on the CR, on the LF and just after the LF never cut the pair, and a seed at the end of the file stays there.
	const std::array<size_t, 5> expectedUnits{0, 3, 3, 3, 4};
	CFileLoadScannerForTest scanner;
	size_t checked = 0;
	for (const auto& c : LoadCases()) {
		if (c.id != "crlf") {
			continue;
		}
		SCOPED_TRACE(c.Describe());
		const size_t width = UnitWidth(c.format);
		scanner.Attach(TraitOf(c.format), c.bytes.data(), c.bytes.size(), c.extendedEol);
		for (size_t unit = 0; unit < expectedUnits.size(); ++unit) {
			EXPECT_EQ(expectedUnits[unit] * width, scanner.GetNextLineOffset(unit * width)) << "seed unit " << unit;
		}
		++checked;
	}
	EXPECT_EQ(8u, checked);
}

TEST(EncodedEolContract, ReadManagerPartitionArithmeticReplaysTheSingleScan)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		const size_t size = c.bytes.size();
		const char* data = region.PlaceAtEnd(c.bytes);
		scanner.Attach(TraitOf(c.format), data, size, c.extendedEol);
		const auto single = ScanRange(scanner, c.format, c.extendedEol, data, 0, size);
		const size_t plannedEnd = scanner.GetNextLineOffset(size);
		for (const int threadCount : {1, 2, 3, 4, 8}) {
			SCOPED_TRACE(testing::Message() << "threads=" << threadCount);
			const auto ranges = PlanPartitions(scanner, size, threadCount);
			ExpectWellFormedPlan(ranges, plannedEnd);
			EXPECT_EQ(ToString(single), ToString(ReplayRanges(scanner, c.format, c.extendedEol, data, ranges)));
		}
	}
}

TEST(EncodedEolContract, EofTerminalsOfThePartitionPlannerAreFrozen)
{
	const auto cases = LoadCases();
	ASSERT_FALSE(cases.empty());
	std::set<std::string> corpusIds;
	for (const auto& c : cases) {
		corpusIds.insert(c.id);
	}
	for (const auto& terminal : kEofTerminals) {
		EXPECT_TRUE(corpusIds.contains(terminal.id)) << terminal.id;
	}

	CFileLoadScannerForTest scanner;
	GuardedRegion region;
	size_t frozen = 0;
	for (const auto& c : cases) {
		SCOPED_TRACE(c.Describe());
		const auto terminal = std::ranges::find_if(kEofTerminals, [&c](const EofTerminal& t) { return c.id == t.id; });
		const bool listed = terminal != kEofTerminals.end();
		// Every corpus case with a partial final unit has a frozen terminal, and only those do.
		EXPECT_EQ(c.hasTail, listed);
		if (!listed) {
			continue;
		}
		++frozen;
		const size_t size = c.bytes.size();
		const size_t completeBytes = size - size % UnitWidth(c.format);
		const char* data = region.PlaceAtEnd(c.bytes);
		scanner.Attach(TraitOf(c.format), data, size, c.extendedEol);
		const size_t plannedEnd = scanner.GetNextLineOffset(size);
		switch (c.extendedEol ? terminal->extendedOn : terminal->extendedOff) {
		case PlannedEnd::None:
			EXPECT_EQ(0u, plannedEnd);
			for (int threadCount = 1; threadCount <= 8; ++threadCount) {
				EXPECT_TRUE(PlanPartitions(scanner, size, threadCount).empty()) << "threads=" << threadCount;
			}
			break;
		case PlannedEnd::CompleteUnits: {
			EXPECT_EQ(completeBytes, plannedEnd);
			EXPECT_LT(plannedEnd, size);
			const auto tail = scanner.Scan(TraitOf(c.format), data, size, c.extendedEol, plannedEnd);
			EXPECT_EQ(0u, tail.eolLength);
			EXPECT_EQ(size - plannedEnd, tail.lineLength);
			EXPECT_EQ(size, tail.nextOffset);
			break;
		}
		case PlannedEnd::WholeSize:
			EXPECT_EQ(size, plannedEnd);
			break;
		}
	}
	// Ten rows cover eight format/flag combinations each, and six UTF-32-only rows cover four each.
	EXPECT_EQ(104u, frozen);
}

TEST(EncodedEolContract, GeneratedFilesAtThePartitionThresholdReplayTheSingleScan)
{
	// kMinimumBytesPerReadPartition is 1 MiB: SelectReadThreadCount gives 1 partition for 1 MiB and 2 for 1 MiB + 1 byte.
	// Both 2-way files have the aligned seed of the second partition at byte 524288 (size / 2 aligned down), which is the LF
	// of the planted CR LF, so the planner must step back and move the split to 524288 + width, past the LF.
	constexpr size_t kMiB = 1024 * 1024;
	constexpr size_t kSeed = kMiB / 2;
	struct Fixture {
		const char* const name;
		const size_t extraUnits;
		const size_t strayBytes;
		const int expectedThreads;
	};
	constexpr std::array fixtures{
		Fixture{"1MiB", 0, 0, 1},
		Fixture{"1MiB+1byte", 0, 1, 2},
		Fixture{"1MiB+1unit", 1, 0, 2},
	};
	CFileLoadScannerForTest scanner;
	for (const Format format : {Format::Utf16Le, Format::Utf16Be, Format::Utf32Le, Format::Utf32Be}) {
		const size_t width = UnitWidth(format);
		for (const auto& fixture : fixtures) {
			const std::string image = BuildImage(format, kMiB / width + fixture.extraUnits, fixture.strayBytes, kSeed / width);
			const size_t size = image.size();
			ASSERT_EQ(kMiB + fixture.extraUnits * width + fixture.strayBytes, size);
			EXPECT_EQ(fixture.expectedThreads, CReadManager::SelectReadThreadCount(size, 8));
			for (const bool extendedEol : {false, true}) {
				SCOPED_TRACE(testing::Message() << Name(format) << " " << fixture.name << " extended=" << extendedEol);
				scanner.Attach(TraitOf(format), image.data(), size, extendedEol);
				const auto single = ScanRange(scanner, format, extendedEol, image.data(), 0, size);

				// The image exercises every delimiter kind (extended on) or only CR/LF/CRLF (extended off).
				std::set<EEolType> kinds;
				for (const auto& record : single) {
					kinds.insert(record.kind);
				}
				EXPECT_EQ(extendedEol ? 6u : 3u, kinds.size());
				// About 230 lines of 4-5 KiB (about 140 delimited records without the extended EOLs); see BuildImage.
				EXPECT_GT(single.size(), 100u);

				// The stray byte of the 1 MiB + 1 byte file lies outside the planned range (EOF terminal, see Test 6).
				const size_t plannedEnd = scanner.GetNextLineOffset(size);
				EXPECT_EQ(kMiB + fixture.extraUnits * width, plannedEnd);

				for (const int threadCount : {1, 2, 3, 4, 8}) {
					SCOPED_TRACE(testing::Message() << "threads=" << threadCount);
					const auto ranges = PlanPartitions(scanner, size, threadCount);
					ExpectWellFormedPlan(ranges, plannedEnd);
					ExpectSameRecords(single, ReplayRanges(scanner, format, extendedEol, image.data(), ranges));
				}

				// The plan the production loader would make for this file, as literal ranges.
				const auto planned = PlanPartitions(scanner, size, fixture.expectedThreads);
				std::vector<Range> expectedRanges;
				if (fixture.expectedThreads == 1) {
					expectedRanges.push_back({0, plannedEnd});
				} else {
					expectedRanges.push_back({0, kSeed + width});
					expectedRanges.push_back({kSeed + width, plannedEnd});
				}
				ASSERT_EQ(expectedRanges.size(), planned.size());
				for (size_t i = 0; i < planned.size(); ++i) {
					EXPECT_EQ(expectedRanges[i].begin, planned[i].begin) << "range " << i;
					EXPECT_EQ(expectedRanges[i].end, planned[i].end) << "range " << i;
				}
			}
		}
	}
}
