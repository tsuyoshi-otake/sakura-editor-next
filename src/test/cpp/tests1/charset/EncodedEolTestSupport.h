#pragma once

#include <Windows.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "CFileLoadScannerForTest.hpp"
#include "EncodedEolCorpus.h"

// Shared helpers for EncodedEolContract and EncodedEolRustCandidate.
// They provide guard-page placement, the production range scan and partition
// planner, comparison helpers, and the 1 MiB image builder.
// They live in encoded_eol_corpus next to the corpus loader.

namespace encoded_eol_corpus {

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
inline std::vector<Record> ScanRange(
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
inline std::vector<Range> PlanPartitions(CFileLoadScannerForTest& planner, size_t fileSize, int threadCount)
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

// Ranges must be ascending, contiguous and non-empty, start at 0 and end at the planned end of the file.
inline void ExpectWellFormedPlan(const std::vector<Range>& ranges, size_t plannedEnd)
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
inline void ExpectSameRecords(const std::vector<Record>& expected, const std::vector<Record>& actual)
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
inline std::string BuildImage(Format format, size_t unitCount, size_t strayBytes, size_t crlfSecondUnit)
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

} // namespace encoded_eol_corpus
