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
#include "env/CReadManager.h"

namespace {

using namespace encoded_eol_corpus;

class GuardedRegion {
public:
	GuardedRegion()
	{
		SYSTEM_INFO systemInfo{};
		::GetSystemInfo(&systemInfo);
		m_pageSize = systemInfo.dwPageSize;
		if (m_pageSize <= 3) {
			throw std::runtime_error("Invalid system page size");
		}
		m_page = static_cast<char*>(
			::VirtualAlloc(nullptr, m_pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (m_page == nullptr) {
			throw std::runtime_error("Cannot allocate the corpus guard-page region");
		}
		DWORD oldProtection = 0;
		if (!::VirtualProtect(m_page + m_pageSize, m_pageSize, PAGE_NOACCESS, &oldProtection)) {
			::VirtualFree(m_page, 0, MEM_RELEASE);
			throw std::runtime_error("Cannot protect the corpus guard page");
		}
	}

	~GuardedRegion()
	{
		::VirtualFree(m_page, 0, MEM_RELEASE);
	}

	GuardedRegion(const GuardedRegion&) = delete;
	GuardedRegion& operator=(const GuardedRegion&) = delete;

	const char* PlaceAtEnd(std::string_view bytes)
	{
		return Place(bytes, m_pageSize - bytes.size());
	}

	const char* PlaceAtOddAddress(std::string_view bytes)
	{
		return Place(bytes, 1);
	}

private:
	const char* Place(std::string_view bytes, size_t offset)
	{
		assert(bytes.size() < m_pageSize - 1);
		if (bytes.size() >= m_pageSize - 1) {
			throw std::runtime_error("Corpus case does not fit in the guard-page region");
		}
		std::memset(m_page, 0, m_pageSize);
		char* const data = m_page + offset;
		if (!bytes.empty()) {
			std::memcpy(data, bytes.data(), bytes.size());
		}
		return data;
	}

	size_t m_pageSize;
	char* m_page;
};

struct Range {
	const size_t begin;
	const size_t end;
};

// Records found by repeated production scans of [start, end) of data; offsets are bytes from data[0].
// The last scan must report exactly `end` as its finish offset (trailing content included).
std::vector<Record> ScanRange(
	CFileLoadScannerForTest& scanner, Format format, bool extendedEol, const char* data, size_t start, size_t end)
{
	std::vector<Record> records;
	size_t next = start;
	for (;;) {
		const auto result = scanner.Scan(TraitOf(format), data, end, extendedEol, next);
		if (result.eolLength == 0) {
			EXPECT_EQ(end, result.nextOffset);
			return records;
		}
		if (result.nextOffset <= next || result.nextOffset > end) {
			ADD_FAILURE() << "Scanner returned an invalid next offset: " << result.nextOffset;
			return records;
		}
		records.push_back({next + result.lineLength, result.eolLength, result.eolType});
		next = result.nextOffset;
	}
}

// The partition loop of CReadManager::ReadFile (sakura_core/env/CReadManager.cpp) cannot be called on its own, so its
// arithmetic is repeated here over the production CFileLoad::GetNextLineOffset. Empty ranges are dropped as in the loop;
// the result is ascending. If that loop changes, this copy and the literal expectations below change with it.
std::vector<Range> PlanPartitions(CFileLoadScannerForTest& planner, size_t fileSize, int threadCount)
{
	std::vector<Range> ranges;
	size_t offsetBegin = planner.GetNextLineOffset(fileSize);
	for (int i = threadCount - 1; 0 <= i; i--) {
		const size_t offsetEnd = offsetBegin;
		offsetBegin = planner.GetNextLineOffset(
			static_cast<size_t>(static_cast<double>(fileSize) / threadCount * i));
		if (offsetBegin == offsetEnd) {
			continue;
		}
		ranges.push_back({offsetBegin, offsetEnd});
	}
	// Collected from the end of the file backwards; Range is immutable, so the ascending result is copied out in reverse.
	return std::vector<Range>(ranges.rbegin(), ranges.rend());
}

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

// Ranges must be ascending, contiguous and non-empty, start at 0 and end at the planned end of the file.
void ExpectWellFormedPlan(const std::vector<Range>& ranges, size_t plannedEnd)
{
	if (ranges.empty()) {
		EXPECT_EQ(0u, plannedEnd);
		return;
	}
	EXPECT_EQ(0u, ranges.front().begin);
	EXPECT_EQ(plannedEnd, ranges.back().end);
	for (size_t i = 0; i < ranges.size(); ++i) {
		EXPECT_LT(ranges[i].begin, ranges[i].end);
		if (i + 1 < ranges.size()) {
			EXPECT_EQ(ranges[i].end, ranges[i + 1].begin);
		}
	}
}

// Compact comparison for long lists: reports the first difference instead of printing every record.
void ExpectSameRecords(const std::vector<Record>& expected, const std::vector<Record>& actual)
{
	EXPECT_EQ(expected.size(), actual.size());
	const size_t common = (std::min)(expected.size(), actual.size());
	for (size_t i = 0; i < common; ++i) {
		if (expected[i].offset != actual[i].offset || expected[i].width != actual[i].width || expected[i].kind != actual[i].kind) {
			ADD_FAILURE() << "first difference at record " << i << ": expected " << ToString({expected[i]})
				<< " but got " << ToString({actual[i]});
			return;
		}
	}
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

// Deterministic file image for the partition-threshold test: complete lines whose EOLs cycle through LF, CRLF, CR, NEL, LS,
// PS, LF, CRLF; text units that are never delimiters (lone surrogate, units whose bytes look like CR or LF in a byte-wise
// scan, and for UTF-32 units with non-zero upper bits); LF as the last complete unit; a CR LF pair planted at units
// crlfSecondUnit - 1 and crlfSecondUnit; `strayBytes` extra bytes (a partial final unit) after the last complete unit.
//
// The lines are long on purpose: 4096 to 5120 bytes of text each, so a 1 MiB image holds about 230 EOL records. The
// production scanner converts the EOL definitions to the file's encoding every time it detects an EOL, which costs on the
// order of 100 microseconds per EOL under the Full Page Heap that tests1.exe may run with, and this test scans each image
// dozens of times; lines of about ten units took nine minutes there. The partition planner only depends on where a seed
// falls relative to the line starts, and the small-fixture tests sweep every split point of dense short lines.
std::string BuildImage(Format format, size_t unitCount, size_t strayBytes, size_t crlfSecondUnit)
{
	constexpr size_t kBaseTextBytes = 4096;
	constexpr size_t kTextBytesStep = 64;
	constexpr size_t kTextBytesSteps = 17;
	const size_t width = UnitWidth(format);
	// The longest line: its text plus a two-unit EOL.
	const size_t maxLineUnits = (kBaseTextBytes + (kTextBytesSteps - 1) * kTextBytesStep) / width + 2;

	std::vector<std::uint32_t> palette{0x41, 0x42, 0x43, 0x44, 0xD800, 0x0A0D, 0x0D0A, 0x2027};
	if (width == 4) {
		palette.push_back(0x0001000A);
		palette.push_back(0x00010085);
	}
	// Second element 0 means "no second unit"; NUL is never a delimiter.
	constexpr std::array<std::array<std::uint32_t, 2>, 8> kEols{{
		{0x0A, 0}, {0x0D, 0x0A}, {0x0D, 0}, {0x85, 0}, {0x2028, 0}, {0x2029, 0}, {0x0A, 0}, {0x0D, 0x0A},
	}};

	std::vector<std::uint32_t> units;
	units.reserve(unitCount);
	for (size_t line = 0; units.size() + maxLineUnits < unitCount; ++line) {
		const size_t textLength = (kBaseTextBytes + ((line * 5 + 3) % kTextBytesSteps) * kTextBytesStep) / width;
		for (size_t j = 0; j < textLength; ++j) {
			units.push_back(palette[(line + j) % palette.size()]);
		}
		for (const auto unit : kEols[line % kEols.size()]) {
			if (unit != 0) {
				units.push_back(unit);
			}
		}
	}
	while (units.size() + 1 < unitCount) {
		units.push_back(0x41);
	}
	units.push_back(0x0A);
	units[crlfSecondUnit - 1] = 0x0D;
	units[crlfSecondUnit] = 0x0A;

	std::string bytes;
	bytes.reserve(units.size() * width + strayBytes);
	for (const auto unit : units) {
		AppendUnit(bytes, format, unit, width);
	}
	bytes.append(strayBytes, 'A');
	return bytes;
}

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
