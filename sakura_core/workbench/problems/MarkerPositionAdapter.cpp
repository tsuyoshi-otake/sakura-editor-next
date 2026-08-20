/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/problems/MarkerPositionAdapter.h"

#include <algorithm>
#include <limits>

namespace workbench::problems {
namespace {

//! `CLogicInt` is a 32-bit signed `CStrictInteger`; a `MarkerRange` field is
//! `std::uint32_t`. No real document reaches this bound, but the conversion
//! must still be total, so anything past it is clamped rather than
//! truncated/wrapped by a narrowing cast.
constexpr std::uint32_t kMaxLogicValue = static_cast<std::uint32_t>((std::numeric_limits<int>::max)());

[[nodiscard]] bool IsHighSurrogate(wchar_t value) noexcept
{
	return value >= 0xD800 && value <= 0xDBFF;
}

[[nodiscard]] bool IsLowSurrogate(wchar_t value) noexcept
{
	return value >= 0xDC00 && value <= 0xDFFF;
}

} // namespace

ConvertedLogicPosition ConvertMarkerPositionToLogicPoint(
	std::uint32_t zeroBasedLine,
	std::uint32_t zeroBasedUtf16Column,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent)
{
	ConvertedLogicPosition result;

	// Line: clamp to the last existing line, matching validatePosition. An
	// empty document (totalLineCount == 0) has no "last line"; line 0 is the
	// only position such a document can hold.
	std::uint32_t clampedLine = 0;
	if (totalLineCount > 0) {
		clampedLine = zeroBasedLine;
		if (clampedLine >= totalLineCount) {
			clampedLine = totalLineCount - 1;
			result.clamp.line = true;
		}
	}
	else if (zeroBasedLine != 0) {
		result.clamp.line = true;
	}

	const std::wstring_view content = totalLineCount > 0 ? lineContent(clampedLine) : std::wstring_view{};
	const std::size_t lineLength = content.size();

	// Column: clamp to the line's end.
	std::size_t clampedColumn = 0;
	if (zeroBasedUtf16Column >= lineLength) {
		clampedColumn = lineLength;
		if (zeroBasedUtf16Column != lineLength) {
			result.clamp.column = true;
		}
	}
	else {
		clampedColumn = zeroBasedUtf16Column;
	}

	// Surrogate safety: never let the position land strictly inside a UTF-16
	// surrogate pair. `content[clampedColumn - 1]` / `content[clampedColumn]`
	// are both in bounds here because 0 < clampedColumn < lineLength.
	if (clampedColumn > 0 && clampedColumn < lineLength) {
		if (IsHighSurrogate(content[clampedColumn - 1]) && IsLowSurrogate(content[clampedColumn])) {
			--clampedColumn;
			result.clamp.column = true;
		}
	}

	std::uint32_t safeLine = clampedLine;
	if (safeLine > kMaxLogicValue) {
		safeLine = kMaxLogicValue;
		result.clamp.line = true;
	}
	std::uint32_t safeColumn = static_cast<std::uint32_t>(std::min<std::size_t>(clampedColumn, kMaxLogicValue));
	if (static_cast<std::size_t>(safeColumn) != clampedColumn) {
		result.clamp.column = true;
	}

	result.position = CLogicPoint(static_cast<int>(safeColumn), static_cast<int>(safeLine));
	return result;
}

ConvertedLogicPosition ConvertMarkerRangeStartToLogicPoint(
	const MarkerRange& range,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent)
{
	return ConvertMarkerPositionToLogicPoint(range.startLine, range.startColumn, totalLineCount, lineContent);
}

ConvertedLogicRange ConvertMarkerRangeToLogicRange(
	const MarkerRange& range,
	std::uint32_t totalLineCount,
	const LogicLineContentLookup& lineContent)
{
	ConvertedLogicRange result;
	result.start = ConvertMarkerPositionToLogicPoint(range.startLine, range.startColumn, totalLineCount, lineContent);
	result.end = ConvertMarkerPositionToLogicPoint(range.endLine, range.endColumn, totalLineCount, lineContent);
	return result;
}

} // namespace workbench::problems
