/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace terminal {

// A wrapped row contributes its columns up to and including its last
// written cell (R5).  A written space counts; a wide cell includes its
// continuation.  Unwritten and erased cells after it do not.
[[nodiscard]] std::size_t TerminalRowWrittenEnd( const TerminalRow& row ) noexcept;

// A row's reflow stream is its cells before the content end: the last cell
// that is written or that carries non-default attributes (colored blank
// fill is content, the unwritten gap left by a wide cell is not).
[[nodiscard]] std::size_t TerminalRowContentEnd( const TerminalRow& row ) noexcept;

// How one logical line (a run of rows joined by `wrapped`) was re-chunked.
struct TerminalReflowLineMap final {
	std::size_t streamLen{};
	std::size_t firstNewRow{};
	std::size_t newRowCount{};
	// Stream offset at which each new row of this line begins; the last row
	// covers through streamLen.  Offsets count stream cells, which is also
	// the column the cell landed on (a wide cell's continuation occupies a
	// stream cell and a column alike).
	std::vector<std::size_t> newRowStreamBegin;
};

// The product of reflowing one row region (history or screen) to a new
// column count, plus the row/position maps the callers need.
struct TerminalReflowRegionMap final {
	std::size_t oldRowCount{};
	std::vector<std::size_t> oldRowLine;         // per old row: its line index
	std::vector<std::size_t> oldRowStreamBegin;  // per old row: first stream cell
	std::vector<std::size_t> oldRowStreamLen;    // per old row: contributed cells
	std::vector<TerminalReflowLineMap> lines;
	std::deque<TerminalRow> newRows;
};

// Re-chunks every logical line of `rows` at `newColumns`.  When `openEnd`
// is set the region's last row continues into the next region (the newest
// history line continues on screen row 0), so the last output row of the
// last line keeps `wrapped`.  `padAttributes` fills the trailing cells the
// resize's blank padding used to carry.
[[nodiscard]] TerminalReflowRegionMap ReflowTerminalRows(
	std::deque<TerminalRow> rows,
	std::size_t newColumns,
	const TerminalAttributes& padAttributes,
	bool openEnd );

// Maps (oldRow, oldColumn) to (newRowInRegion, newColumn) by the cell
// offset inside its logical line (R3).  Returns false when the old row was
// dropped with its whole line.  `boundary` marks a half-open position
// between cells (a selection end may equal the column count); a boundary
// after a cell maps to the boundary after the same cell, while a cell
// position clamps `newColumn` to `newColumns - 1`.
[[nodiscard]] bool MapReflowedPosition(
	const TerminalReflowRegionMap& map,
	std::size_t oldRow,
	std::size_t oldColumn,
	std::size_t newColumns,
	bool boundary,
	std::size_t& newRow,
	std::size_t& newColumn ) noexcept;

// One column-reflow's whole-model coordinate change: the history region
// map, the screen region map, and the bookkeeping the height rule added
// (rows pushed to history, rows dropped below the cursor, rows the
// scrollback limit evicted).  Published on the scrollback change so the
// pane and other coordinate-space owners can remap exactly.
struct TerminalReflowLayout final {
	TerminalReflowRegionMap history;
	TerminalReflowRegionMap screen;
	std::size_t oldHistoryRowCount{};
	// Per reflowed screen row: the global row (scrollback + screen index
	// space) it landed on, or npos when the row was dropped.
	std::vector<std::size_t> screenNewRowGlobal;
	// Rows evicted from the scrollback front after the region globals were
	// assigned (limit enforcement); every global shifts down by this many.
	std::size_t globalShift{};
	static constexpr std::size_t npos = static_cast<std::size_t>(-1);

	// Maps an old (global row, column) to the new (global row, column) by
	// cell offset.  `boundary` selects half-open boundary semantics for
	// selection ends; false is a cell position (cursor).  Returns false
	// when the position's content is gone.
	[[nodiscard]] bool MapPoint(
		std::size_t oldGlobalRow,
		std::size_t column,
		std::size_t newColumns,
		std::size_t& newGlobalRow,
		std::size_t& newColumn,
		bool boundary = false ) const noexcept;
};

} // namespace terminal
