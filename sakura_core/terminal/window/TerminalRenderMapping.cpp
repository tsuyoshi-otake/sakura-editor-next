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

} // namespace

TerminalViewport CalculateTerminalViewport( const TerminalModel& model, std::size_t visibleRows, std::size_t scrollOffset ) noexcept
{
	TerminalViewport viewport;
	viewport.totalRows = model.ScrollbackSize() + model.RowCount();
	viewport.visibleRows = std::min(visibleRows, viewport.totalRows);
	const auto bottomTop = viewport.totalRows - viewport.visibleRows;
	viewport.topRow = bottomTop - std::min(scrollOffset, bottomTop);
	return viewport;
}

const TerminalRow* GetTerminalRow( const TerminalModel& model, std::size_t globalRow ) noexcept
{
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

std::vector<std::size_t> MapDirtyRowsToViewport( const TerminalModel& model, const TerminalViewport& viewport, const std::vector<std::size_t>& dirtyScreenRows )
{
	std::vector<std::size_t> result;
	result.reserve(dirtyScreenRows.size());
	const auto bottom = viewport.topRow + viewport.visibleRows;
	for( const auto screenRow : dirtyScreenRows ) {
		const auto globalRow = model.ScrollbackSize() + screenRow;
		if( globalRow >= viewport.topRow && globalRow < bottom ) result.push_back(globalRow - viewport.topRow);
	}
	return result;
}

std::wstring ExtractTerminalSelection( const TerminalModel& model, TerminalSelectionPoint anchor, TerminalSelectionPoint active )
{
	if( PointLess(active, anchor) ) std::swap(anchor, active);
	if( anchor == active ) return {};
	const auto totalRows = model.ScrollbackSize() + model.RowCount();
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
			if( !cell.continuation ) line.append(cell.Text());
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
