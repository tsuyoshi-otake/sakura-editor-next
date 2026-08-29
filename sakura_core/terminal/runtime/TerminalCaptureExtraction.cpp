/*! @file @brief Bounded text extraction from the parsed terminal model. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/runtime/TerminalCaptureExtraction.h"

#include <algorithm>
#include <limits>
#include <string_view>

namespace terminal {
namespace {

constexpr char16_t kReplacementCharacter = u'\ufffd';

struct NormalizedText final {
	std::u16string value;
	std::size_t utf8Bytes{};
};

bool IsHighSurrogate( char16_t value ) noexcept
{
	return value >= 0xd800 && value <= 0xdbff;
}

bool IsLowSurrogate( char16_t value ) noexcept
{
	return value >= 0xdc00 && value <= 0xdfff;
}

std::size_t Utf8Length( char32_t value ) noexcept
{
	if( value <= 0x7f ) return 1;
	if( value <= 0x7ff ) return 2;
	if( value <= 0xffff ) return 3;
	return 4;
}

void AppendNormalized( NormalizedText& output, std::wstring_view text )
{
	for( std::size_t index = 0; index < text.size(); ++index ) {
		const auto first = static_cast<char16_t>(text[index]);
		if( IsHighSurrogate(first) ) {
			if( index + 1 < text.size() ) {
				const auto second = static_cast<char16_t>(text[index + 1]);
				if( IsLowSurrogate(second) ) {
					const auto scalar = static_cast<char32_t>(0x10000)
						+ ((static_cast<char32_t>(first) - 0xd800) << 10)
						+ (static_cast<char32_t>(second) - 0xdc00);
					output.value.push_back(first);
					output.value.push_back(second);
					output.utf8Bytes += Utf8Length(scalar);
					++index;
					continue;
				}
			}
			output.value.push_back(kReplacementCharacter);
			output.utf8Bytes += 3;
			continue;
		}
		if( IsLowSurrogate(first) ) {
			output.value.push_back(kReplacementCharacter);
			output.utf8Bytes += 3;
			continue;
		}
		output.value.push_back(first);
		output.utf8Bytes += Utf8Length(first);
	}
}

NormalizedText ExtractRow( const TerminalRow& row, bool preserveTrailingSpaces )
{
	NormalizedText result;
	result.value.reserve(row.cells.size());
	for( const auto& cell : row.cells ) {
		if( cell.continuation ) continue;
		if( cell.length == 0 ) {
			result.value.push_back(u' ');
			++result.utf8Bytes;
		} else {
			AppendNormalized(result, cell.Text());
		}
	}
	if( preserveTrailingSpaces ) return result;
	while( !result.value.empty() && result.value.back() == u' ' ) {
		result.value.pop_back();
		--result.utf8Bytes;
	}
	return result;
}

std::int64_t SaturatingSizeToInt64( std::size_t value ) noexcept
{
	return value > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())
		? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(value);
}

const TerminalRow* ResolveRow( const TerminalModel& model, std::int64_t coordinate ) noexcept
{
	if( coordinate >= 0 ) {
		const auto row = static_cast<std::uint64_t>(coordinate);
		return row < model.Rows().size() ? &model.Rows()[static_cast<std::size_t>(row)] : nullptr;
	}
	if( model.IsAlternateScreen() ) return nullptr;
	const auto historySize = SaturatingSizeToInt64(model.ScrollbackSize());
	if( coordinate < -historySize ) return nullptr;
	const auto index = historySize + coordinate;
	return index >= 0 ? &model.Scrollback()[static_cast<std::size_t>(index)] : nullptr;
}

void MarkTruncated(
	TerminalCaptureExtractionResult& result,
	TerminalCaptureTruncationReason reason ) noexcept
{
	result.truncated = true;
	result.truncationReason = reason;
}

} // namespace

TerminalCaptureExtractionResult ExtractTerminalCapture(
	const TerminalModel& model,
	const TerminalCaptureExtractionRequest& request )
{
	TerminalCaptureExtractionResult result;
	result.alternateScreen = model.IsAlternateScreen();
	if( request.limits.maximumPhysicalRows == 0 || request.limits.maximumCodeUnits == 0
		|| request.limits.maximumUtf8Bytes == 0 || request.limits.uiBudget <= std::chrono::milliseconds::zero() ) {
		return result;
	}

	const auto historySize = result.alternateScreen ? 0 : SaturatingSizeToInt64(model.ScrollbackSize());
	const auto firstRetained = -historySize;
	const auto lastScreen = SaturatingSizeToInt64(model.RowCount()) - 1;
	if( lastScreen < 0 ) return result;
	std::int64_t start = request.startLine.value_or(0);
	std::int64_t end = request.endLine.value_or(lastScreen);
	start = std::clamp(start, firstRetained, lastScreen);
	end = std::clamp(end, firstRetained, lastScreen);
	if( start > end ) return result;
	std::vector<TerminalRowRange> ranges;
	if( request.selectedRanges ) {
		if( request.selectedRanges->size() > request.limits.maximumPhysicalRows ) return result;
		ranges.reserve(request.selectedRanges->size());
		for( const auto& selected : *request.selectedRanges ) {
			if( selected.first > selected.last ) return result;
			const auto first = (std::max)({ selected.first, firstRetained, start });
			const auto last = (std::min)({ selected.last, lastScreen, end });
			if( first <= last ) ranges.push_back({ first, last });
		}
		std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
			return left.first < right.first || (left.first == right.first && left.last < right.last);
		});
		std::vector<TerminalRowRange> normalized;
		normalized.reserve(ranges.size());
		for( const auto& range : ranges ) {
			if( normalized.empty() || (normalized.back().last != (std::numeric_limits<std::int64_t>::max)()
				&& range.first > normalized.back().last + 1) ) {
				normalized.push_back(range);
			} else {
				normalized.back().last = (std::max)(normalized.back().last, range.last);
			}
		}
		ranges = std::move(normalized);
	} else {
		ranges.push_back({ start, end });
	}

	const auto started = std::chrono::steady_clock::now();
	result.code = TerminalCaptureResultCode::Succeeded;
	for( const auto& range : ranges ) {
	for( auto coordinate = range.first; coordinate <= range.last; ++coordinate ) {
		if( result.physicalRowsVisited >= request.limits.maximumPhysicalRows ) {
			MarkTruncated(result, TerminalCaptureTruncationReason::Rows);
			break;
		}
		if( result.physicalRowsVisited % 32 == 0 ) {
			const auto now = std::chrono::steady_clock::now();
			if( request.deadline != std::chrono::steady_clock::time_point{} && now >= request.deadline ) {
				MarkTruncated(result, TerminalCaptureTruncationReason::Deadline);
				break;
			}
			if( now - started >= request.limits.uiBudget ) {
				MarkTruncated(result, TerminalCaptureTruncationReason::UiBudget);
				break;
			}
		}

		const auto* row = ResolveRow(model, coordinate);
		if( row == nullptr ) {
			result.code = TerminalCaptureResultCode::InvalidRequest;
			result.lines.clear();
			return result;
		}
		auto text = ExtractRow(*row, request.joinWrappedLines);
		if( text.value.size() > request.limits.maximumCodeUnits - result.codeUnits ) {
			MarkTruncated(result, TerminalCaptureTruncationReason::CodeUnits);
			break;
		}
		const bool joinsPrevious = request.joinWrappedLines && !result.lines.empty()
			&& result.lines.back().wrapped && result.lines.back().lastRow + 1 == coordinate;
		const auto serializedBytes = text.utf8Bytes + (joinsPrevious ? 0u : 1u);
		if( serializedBytes > request.limits.maximumUtf8Bytes - result.utf8Bytes ) {
			MarkTruncated(result, TerminalCaptureTruncationReason::Utf8Bytes);
			break;
		}

		++result.physicalRowsVisited;
		result.codeUnits += text.value.size();
		result.utf8Bytes += serializedBytes;
		if( joinsPrevious ) {
			result.lines.back().text.append(text.value);
			result.lines.back().lastRow = coordinate;
			result.lines.back().wrapped = row->wrapped;
			result.lines.back().joined = true;
		} else {
			result.lines.push_back({ coordinate, coordinate, row->wrapped, false, std::move(text.value) });
		}
		if( coordinate == std::numeric_limits<std::int64_t>::max() ) break;
	}
	}
	return result;
}

} // namespace terminal
