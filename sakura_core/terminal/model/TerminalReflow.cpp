/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/model/TerminalReflow.h"

#include <algorithm>

namespace terminal {
namespace {

const TerminalAttributes g_reflowDefaultAttributes{};

const TerminalAttributes& ReflowAttributesAt( const TerminalRow& row, std::size_t column ) noexcept
{
	return column < row.cellAttributes.size() ? row.cellAttributes[column] : g_reflowDefaultAttributes;
}

void RebuildReflowRuns( TerminalRow& row, std::size_t columns )
{
	row.attributeRuns.clear();
	for( std::size_t column = 0; column < columns; ) {
		const auto attributes = row.cellAttributes[column];
		auto end = column + 1;
		while( end < columns && row.cellAttributes[end] == attributes ) ++end;
		row.attributeRuns.push_back({ column, end - column, attributes });
		column = end;
	}
}

TerminalRow MakeReflowRow( std::size_t columns, const TerminalAttributes& attributes )
{
	TerminalRow row;
	row.cells.assign(columns, TerminalCell{});
	row.cellAttributes.assign(columns, attributes);
	row.attributeRuns.push_back({ 0, columns, attributes });
	return row;
}

} // namespace

std::size_t TerminalRowWrittenEnd( const TerminalRow& row ) noexcept
{
	for( std::size_t index = row.cells.size(); index > 0; --index ) {
		const auto& cell = row.cells[index - 1];
		if( cell.length != 0 ) {
			return index - 1 + std::max<std::size_t>(1, cell.width);
		}
	}
	return 0;
}

std::size_t TerminalRowContentEnd( const TerminalRow& row ) noexcept
{
	for( std::size_t index = row.cells.size(); index > 0; --index ) {
		const auto column = index - 1;
		const auto& cell = row.cells[column];
		if( cell.length != 0 ) {
			return column + std::max<std::size_t>(1, cell.width);
		}
		if( !cell.continuation && !(ReflowAttributesAt(row, column) == g_reflowDefaultAttributes) ) {
			return index;
		}
	}
	return 0;
}

TerminalReflowRegionMap ReflowTerminalRows(
	std::deque<TerminalRow> rows,
	std::size_t newColumns,
	const TerminalAttributes& padAttributes,
	bool openEnd )
{
	newColumns = std::max<std::size_t>(1, newColumns);
	TerminalReflowRegionMap map;
	map.oldRowCount = rows.size();
	map.oldRowLine.assign(rows.size(), 0);
	map.oldRowStreamBegin.assign(rows.size(), 0);
	map.oldRowStreamLen.assign(rows.size(), 0);

	std::vector<TerminalCell> streamCells;
	std::vector<TerminalAttributes> streamAttributes;

	std::size_t first = 0;
	while( first < rows.size() ) {
		auto last = first;
		while( last + 1 < rows.size() && rows[last].wrapped ) ++last;
		const bool boundary = openEnd && last + 1 == rows.size() && rows[last].wrapped;

		streamCells.clear();
		streamAttributes.clear();
		for( auto row = first; row <= last; ++row ) {
			map.oldRowLine[row] = map.lines.size();
			map.oldRowStreamBegin[row] = streamCells.size();
			// R5: only a line's last row contributes its colored tail
			// (colored blank fill is content there) - and only when the
			// line does not continue past this region.  Every earlier row
			// is wrapped, and the boundary line's last history row is
			// wrapped onto screen row 0: its unwritten tail cells are a
			// gap that must not re-enter the stream as blanks.
			const auto end = row == last && !boundary
				? TerminalRowContentEnd(rows[row])
				: TerminalRowWrittenEnd(rows[row]);
			for( std::size_t column = 0; column < end && column < rows[row].cells.size(); ++column ) {
				streamCells.push_back(rows[row].cells[column]);
				streamAttributes.push_back(ReflowAttributesAt(rows[row], column));
			}
			map.oldRowStreamLen[row] = streamCells.size() - map.oldRowStreamBegin[row];
		}

		TerminalReflowLineMap line;
		line.streamLen = streamCells.size();
		line.firstNewRow = map.newRows.size();

		if( streamCells.empty() ) {
			auto row = MakeReflowRow(newColumns, padAttributes);
			row.wrapped = boundary;
			line.newRowStreamBegin.push_back(0);
			line.newRowCount = 1;
			map.newRows.push_back(std::move(row));
		} else {
			auto out = MakeReflowRow(newColumns, padAttributes);
			line.newRowStreamBegin.push_back(0);
			std::size_t cursor = 0;
			std::size_t slot = 0;
			while( slot < streamCells.size() ) {
				const auto& cell = streamCells[slot];
				if( cell.continuation ) {
					// A continuation without its lead cannot start a row;
					// intact streams never produce one, but keep the walk
					// total on malformed input.
					++slot;
					continue;
				}
				const bool wide = cell.width == 2
					&& slot + 1 < streamCells.size() && streamCells[slot + 1].continuation;
				const std::size_t group = wide ? 2 : 1;
				const std::size_t groupWidth = std::min<std::size_t>(wide ? 2 : 1, newColumns);
				if( cursor + groupWidth > newColumns ) {
					out.wrapped = true;
					RebuildReflowRuns(out, newColumns);
					map.newRows.push_back(std::move(out));
					out = MakeReflowRow(newColumns, padAttributes);
					line.newRowStreamBegin.push_back(slot);
					cursor = 0;
				}
				auto placed = cell;
				placed.width = static_cast<std::uint8_t>(groupWidth);
				out.cells[cursor] = placed;
				out.cellAttributes[cursor] = streamAttributes[slot];
				if( groupWidth == 2 ) {
					out.cells[cursor + 1] = {};
					out.cells[cursor + 1].continuation = true;
					out.cells[cursor + 1].width = 0;
					out.cellAttributes[cursor + 1] = streamAttributes[slot];
				}
				cursor += groupWidth;
				slot += group;
			}
			out.wrapped = boundary;
			RebuildReflowRuns(out, newColumns);
			map.newRows.push_back(std::move(out));
			line.newRowCount = map.newRows.size() - line.firstNewRow;
		}

		map.lines.push_back(std::move(line));
		first = last + 1;
	}
	return map;
}

bool MapReflowedPosition(
	const TerminalReflowRegionMap& map,
	std::size_t oldRow,
	std::size_t oldColumn,
	std::size_t newColumns,
	bool boundary,
	std::size_t& newRow,
	std::size_t& newColumn ) noexcept
{
	if( oldRow >= map.oldRowCount ) return false;
	const auto& line = map.lines[map.oldRowLine[oldRow]];
	newColumns = std::max<std::size_t>(1, newColumns);
	const auto lastColumn = newColumns - 1;
	if( line.streamLen == 0 ) {
		newRow = line.firstNewRow;
		newColumn = std::min(oldColumn, lastColumn);
		return true;
	}
	// R3: a column inside a row's unwritten tail counts no stream cells:
	// it maps to the boundary at the end of that row's stream piece, never
	// into the stream cells of a later row.
	const auto rowBegin = map.oldRowStreamBegin[oldRow];
	const auto rowLen = map.oldRowStreamLen[oldRow];
	const auto column = std::min(oldColumn, rowLen);
	const auto offset = rowBegin + column;
	if( offset >= line.streamLen ) {
		// A position past the content creates no rows: it stays on the last
		// output row.  A boundary lands after the row's last cell (the R8/D8
		// right-edge boundary); a cell position keeps its distance past the
		// row's stream end, clamped to the last column.
		newRow = line.firstNewRow + line.newRowCount - 1;
		const auto lastRowCells = line.streamLen - line.newRowStreamBegin.back();
		newColumn = boundary ? lastRowCells
			: std::min(lastRowCells + (oldColumn - column), lastColumn);
		return true;
	}
	std::size_t chunk = 0;
	while( chunk + 1 < line.newRowStreamBegin.size()
		&& line.newRowStreamBegin[chunk + 1] <= offset ) ++chunk;
	newRow = line.firstNewRow + chunk;
	newColumn = std::min(offset - line.newRowStreamBegin[chunk], lastColumn);
	return true;
}

bool TerminalReflowLayout::MapPoint(
	std::size_t oldGlobalRow,
	std::size_t column,
	std::size_t newColumns,
	std::size_t& newGlobalRow,
	std::size_t& newColumn,
	bool boundary ) const noexcept
{
	std::size_t global = npos;
	if( oldGlobalRow < oldHistoryRowCount ) {
		std::size_t row = 0, mappedColumn = 0;
		if( !MapReflowedPosition(history, oldGlobalRow, column, newColumns, boundary, row, mappedColumn) ) return false;
		global = row;
		newColumn = mappedColumn;
	} else {
		const auto local = oldGlobalRow - oldHistoryRowCount;
		std::size_t row = 0, mappedColumn = 0;
		if( !MapReflowedPosition(screen, local, column, newColumns, boundary, row, mappedColumn) ) return false;
		if( row >= screenNewRowGlobal.size() ) return false;
		global = screenNewRowGlobal[row];
		if( global == npos ) return false;
		newColumn = mappedColumn;
	}
	if( global == npos || global < globalShift ) return false;
	newGlobalRow = global - globalShift;
	return true;
}

} // namespace terminal
