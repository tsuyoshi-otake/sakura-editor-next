/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "terminal/window/TerminalRenderMapping.h"

#include <algorithm>

namespace terminal {
namespace {

bool PointLess( const TerminalSelectionPoint& left, const TerminalSelectionPoint& right ) noexcept
{
	return left.row < right.row || (left.row == right.row && left.column < right.column);
}

std::size_t TerminalViewportRowCount( const TerminalModel& model ) noexcept
{
	// DECSET 1049 swaps to a distinct screen.  The main screen's scrollback is
	// intentionally retained by TerminalModel while a TUI is active, but it is
	// not part of the alternate screen's coordinate space.  Keeping this rule
	// here makes painting, selection, dirty-row mapping, and the overlay
	// scrollbar all agree on the same row indices.
	return model.IsAlternateScreen() ? model.RowCount() : model.ScrollbackSize() + model.RowCount();
}

} // namespace

TerminalViewport CalculateTerminalViewport( const TerminalModel& model, std::size_t visibleRows, std::size_t scrollOffset ) noexcept
{
	TerminalViewport viewport;
	viewport.totalRows = TerminalViewportRowCount(model);
	viewport.visibleRows = std::min(visibleRows, viewport.totalRows);
	if( model.IsAlternateScreen() ) {
		// A full-screen TUI is always rendered from its top-left cell.  In
		// particular, do not apply a remembered main-screen scroll offset here.
		viewport.topRow = 0;
		return viewport;
	}
	const auto bottomTop = viewport.totalRows - viewport.visibleRows;
	viewport.topRow = bottomTop - std::min(scrollOffset, bottomTop);
	return viewport;
}

const TerminalRow* GetTerminalRow( const TerminalModel& model, std::size_t globalRow ) noexcept
{
	if( model.IsAlternateScreen() ) {
		return globalRow < model.Rows().size() ? &model.Rows()[globalRow] : nullptr;
	}
	if( globalRow < model.ScrollbackSize() ) return &model.Scrollback()[globalRow];
	const auto screenRow = globalRow - model.ScrollbackSize();
	return screenRow < model.Rows().size() ? &model.Rows()[screenRow] : nullptr;
}

TerminalSelectionPoint TerminalCellFromPoint( const TerminalViewport& viewport, int x, int y, int cellWidth, int cellHeight, std::size_t columns ) noexcept
{
	const auto safeWidth = std::max(1, cellWidth);
	const auto safeHeight = std::max(1, cellHeight);
	const auto viewportRow = static_cast<std::size_t>(std::max(0, y) / safeHeight);
	const auto column = static_cast<std::size_t>(std::max(0, x) / safeWidth);
	const auto lastRow = viewport.totalRows == 0 ? 0 : viewport.totalRows - 1;
	return {
		std::min(viewport.topRow + viewportRow, lastRow),
		std::min(column, columns),
	};
}

TerminalSelectionRange NormalizeTerminalSelection( const TerminalModel& model, TerminalSelectionPoint origin, TerminalSelectionPoint point ) noexcept
{
	if( origin == point ) return { origin, origin };
	if( PointLess(point, origin) ) std::swap(origin, point);
	const auto snapStart = [&model]( TerminalSelectionPoint value ) noexcept {
		const auto* row = GetTerminalRow(model, value.row);
		if( row == nullptr ) return value;
		value.column = std::min(value.column, row->cells.size());
		while( value.column > 0 && value.column < row->cells.size() && row->cells[value.column].continuation ) --value.column;
		return value;
	};
	const auto snapEnd = [&model]( TerminalSelectionPoint value ) noexcept {
		const auto* row = GetTerminalRow(model, value.row);
		if( row == nullptr ) return value;
		value.column = std::min(value.column, row->cells.size());
		if( value.column < row->cells.size() ) {
			const auto width = std::max<std::size_t>(1, row->cells[value.column].width);
			value.column = std::min(row->cells.size(), value.column + width);
			while( value.column < row->cells.size() && row->cells[value.column].continuation ) ++value.column;
		}
		return value;
	};
	return { snapStart(origin), snapEnd(point) };
}

std::vector<std::size_t> MapDirtyRowsToViewport( const TerminalModel& model, const TerminalViewport& viewport, const std::vector<std::size_t>& dirtyScreenRows )
{
	std::vector<std::size_t> result;
	result.reserve(dirtyScreenRows.size());
	const auto bottom = viewport.topRow + viewport.visibleRows;
	for( const auto screenRow : dirtyScreenRows ) {
		const auto globalRow = (model.IsAlternateScreen() ? 0 : model.ScrollbackSize()) + screenRow;
		if( globalRow >= viewport.topRow && globalRow < bottom ) result.push_back(globalRow - viewport.topRow);
	}
	return result;
}

std::wstring ExtractTerminalSelection( const TerminalModel& model, TerminalSelectionPoint anchor, TerminalSelectionPoint active )
{
	if( PointLess(active, anchor) ) std::swap(anchor, active);
	if( anchor == active ) return {};
	const auto totalRows = TerminalViewportRowCount(model);
	if( totalRows == 0 || anchor.row >= totalRows ) return {};
	active.row = std::min(active.row, totalRows - 1);
	std::wstring result;
	for( auto rowIndex = anchor.row; rowIndex <= active.row; ++rowIndex ) {
		const auto* row = GetTerminalRow(model, rowIndex);
		if( row == nullptr ) break;
		const auto begin = rowIndex == anchor.row ? std::min(anchor.column, row->cells.size()) : 0;
		const auto end = rowIndex == active.row ? std::min(active.column, row->cells.size()) : row->cells.size();
		std::wstring line;
		for( auto column = begin; column < end; ++column ) {
			const auto& cell = row->cells[column];
			if( cell.continuation ) continue;
			// An unwritten cell (for example a column a horizontal tab jumped over
			// without printing anything) still occupies exactly one selected
			// column. Extract it as the space it visually is instead of collapsing
			// it away, matching Windows Terminal/VS Code copy behavior for a
			// selection that spans a tab gap. Trailing spaces at a wrapped-row
			// boundary are trimmed below exactly as before.
			if( cell.length == 0 ) line.push_back(L' ');
			else line.append(cell.Text());
		}
		if( rowIndex != active.row ) {
			while( !line.empty() && line.back() == L' ' ) line.pop_back();
		}
		result += line;
		if( rowIndex != active.row && !row->wrapped ) result += L"\r\n";
		if( rowIndex == active.row ) break;
	}
	return result;
}

} // namespace terminal
