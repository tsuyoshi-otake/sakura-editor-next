/*! @file */
#include "StdAfx.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/unicode/TerminalGraphemeWidth.h"

#include <algorithm>
#include <limits>

namespace terminal {
namespace {

const TerminalAttributes g_defaultAttributes{};

std::size_t EncodeCodepoint(char32_t codepoint, std::array<wchar_t, 2>& units) noexcept
{
	if (codepoint <= 0xFFFF) {
		units[0] = static_cast<wchar_t>(codepoint);
		return 1;
	}
	if (codepoint <= 0x10FFFF) {
		codepoint -= 0x10000;
		units[0] = static_cast<wchar_t>(0xD800 + (codepoint >> 10));
		units[1] = static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF));
		return 2;
	}
	units[0] = L'\xFFFD';
	return 1;
}

std::size_t ClampScrollback( std::size_t lines ) noexcept
{
	return std::min(lines, TerminalModel::kMaxScrollbackLines);
}

} // namespace

const TerminalAttributes& TerminalRow::AttributesAt( std::size_t column ) const noexcept
{
	if( column < cellAttributes.size() ) return cellAttributes[column];
	return g_defaultAttributes;
}

TerminalModel::TerminalModel( std::size_t columns, std::size_t rows, std::size_t scrollbackLines )
	: m_columns(std::max<std::size_t>(1, columns))
	, m_rowsCount(std::max<std::size_t>(1, rows))
	, m_scrollbackLimit(ClampScrollback(scrollbackLines))
{
	Reset();
}

TerminalRow TerminalModel::MakeBlankRow() const
{
	TerminalRow row;
	row.cells.resize(m_columns);
	row.cellAttributes.resize(m_columns);
	row.attributeRuns.push_back({ 0, m_columns, {} });
	return row;
}

void TerminalModel::Reset()
{
	m_rows.clear();
	m_rows.reserve(m_rowsCount);
	for( std::size_t i = 0; i < m_rowsCount; ++i ) m_rows.push_back(MakeBlankRow());
	m_scrollback.clear();
	m_savedMainRows.clear();
	m_cursorColumn = m_cursorRow = 0;
	m_savedCursorColumn = m_savedCursorRow = 0;
	m_scrollTop = 0;
	m_scrollBottom = m_rowsCount - 1;
	m_attributes = {};
	m_modes = {};
	m_alternateScreen = false;
	m_dirtyRows.assign(m_rowsCount, true);
}

void TerminalModel::Resize( std::size_t columns, std::size_t rows )
{
	columns = std::max<std::size_t>(1, columns);
	rows = std::max<std::size_t>(1, rows);
	if( columns != m_columns ) {
		m_columns = columns;
		for( auto& row : m_rows ) {
			row.cells.resize(columns);
			row.cellAttributes.resize(columns);
			NormalizeAttributeRuns(row);
		}
	}
	if( rows != m_rowsCount ) {
		while( m_rows.size() > rows ) {
			if( !m_alternateScreen && m_scrollbackLimit != 0 ) m_scrollback.push_back(std::move(m_rows.front()));
			m_rows.erase(m_rows.begin());
		}
		while( m_rows.size() < rows ) m_rows.push_back(MakeBlankRow());
		m_rowsCount = rows;
	}
	SetScrollbackLimit(m_scrollbackLimit);
	m_cursorColumn = std::min(m_cursorColumn, m_columns - 1);
	m_cursorRow = std::min(m_cursorRow, m_rowsCount - 1);
	m_scrollTop = 0;
	m_scrollBottom = m_rowsCount - 1;
	m_dirtyRows.assign(m_rowsCount, true);
}

void TerminalModel::SetScrollbackLimit( std::size_t lines )
{
	m_scrollbackLimit = ClampScrollback(lines);
	while( m_scrollback.size() > m_scrollbackLimit ) m_scrollback.pop_front();
}

void TerminalModel::AppendCodepoint( TerminalCell& cell, char32_t codepoint ) noexcept
{
	auto append = [&]( wchar_t unit ) {
		if( cell.length < cell.text.size() ) cell.text[cell.length++] = unit;
	};
	if( codepoint <= 0xFFFF ) {
		append(static_cast<wchar_t>(codepoint));
	} else if( codepoint <= 0x10FFFF ) {
		codepoint -= 0x10000;
		append(static_cast<wchar_t>(0xD800 + (codepoint >> 10)));
		append(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
	} else {
		append(L'\xFFFD');
	}
}

bool TerminalModel::IsCombining( char32_t cp ) noexcept
{
	return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
		(cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
		(cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
		(cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) ||
		(cp >= 0xE0100 && cp <= 0xE01EF) || cp == 0x200D;
}

int TerminalModel::CodepointWidth( char32_t cp ) noexcept
{
	std::array<wchar_t, 2> units{};
	const auto length = EncodeCodepoint(cp, units);
	const auto measured = unicode::MeasureFirstGrapheme({ units.data(), length });
	return std::clamp(measured.width, 0, 2);
}

void TerminalModel::Print( char32_t codepoint )
{
	// Ask the pinned Windows Terminal UAX #29 implementation whether the new
	// codepoint extends the previous cell.  The candidate stays on the stack so
	// ordinary output retains the no-per-cell-allocation model.
	if( m_cursorColumn != 0 ) {
		auto previous = m_cursorColumn - 1;
		while( previous != 0 && m_rows[m_cursorRow].cells[previous].continuation ) --previous;
		auto& row = m_rows[m_cursorRow];
		auto& previousCell = m_rows[m_cursorRow].cells[previous];
		std::array<wchar_t, 2> encoded{};
		const auto encodedLength = EncodeCodepoint(codepoint, encoded);
		const auto combinedLength = static_cast<std::size_t>(previousCell.length) + encodedLength;
		if( previousCell.length != 0 && combinedLength <= TerminalCell::kMaxCodeUnits ) {
			std::array<wchar_t, TerminalCell::kMaxCodeUnits> combined{};
			std::copy_n(previousCell.text.begin(), previousCell.length, combined.begin());
			std::copy_n(encoded.begin(), encodedLength, combined.begin() + previousCell.length);
			const auto measured = unicode::MeasureFirstGrapheme({ combined.data(), combinedLength });
			const auto newWidth = std::clamp(measured.width, 1, 2);
			const auto oldWidth = static_cast<int>(previousCell.width);
			const auto growth = newWidth - oldWidth;
			if( measured.codeUnits == combinedLength && (growth <= 0 || m_cursorColumn + growth <= m_columns) ) {
				const auto attributes = row.AttributesAt(previous);
				AppendCodepoint(previousCell, codepoint);
				if( growth > 0 ) {
					for( int offset = 0; offset < growth; ++offset ) {
						auto& continuation = row.cells[m_cursorColumn + static_cast<std::size_t>(offset)];
						continuation = {};
						continuation.continuation = true;
						continuation.width = 0;
					}
					m_cursorColumn += static_cast<std::size_t>(growth);
				} else if( growth < 0 ) {
					ClearCellRange(row, previous + static_cast<std::size_t>(newWidth),
						previous + static_cast<std::size_t>(oldWidth));
					m_cursorColumn -= static_cast<std::size_t>(-growth);
				}
				previousCell.width = static_cast<std::uint8_t>(newWidth);
				SetCellAttributes(row, previous, static_cast<std::size_t>(newWidth), attributes);
				MarkDirty(m_cursorRow);
				return;
			}
		}
	}
	if( IsCombining(codepoint) ) return;
	const auto width = std::max(1, CodepointWidth(codepoint));
	if( m_cursorColumn >= m_columns || (width == 2 && m_cursorColumn + 1 >= m_columns) ) {
		m_rows[m_cursorRow].wrapped = true;
		CarriageReturn();
		LineFeed();
	}
	auto& row = m_rows[m_cursorRow];
	auto& cell = row.cells[m_cursorColumn];
	cell = {};
	cell.width = static_cast<std::uint8_t>(width);
	AppendCodepoint(cell, codepoint);
	SetCellAttributes(row, m_cursorColumn, width, m_attributes);
	if( width == 2 ) {
		row.cells[m_cursorColumn + 1] = {};
		row.cells[m_cursorColumn + 1].continuation = true;
		row.cells[m_cursorColumn + 1].width = 0;
	}
	m_cursorColumn += width;
	MarkDirty(m_cursorRow);
}

void TerminalModel::ExecuteControl( wchar_t control )
{
	switch( control ) {
	case L'\a': break;
	case L'\b': if( m_cursorColumn != 0 ) --m_cursorColumn; break;
	case L'\t': m_cursorColumn = std::min(m_columns - 1, ((m_cursorColumn / 8) + 1) * 8); break;
	case L'\n': case L'\v': case L'\f': LineFeed(); break;
	case L'\r': CarriageReturn(); break;
	default: break;
	}
}

void TerminalModel::LineFeed()
{
	if( m_cursorRow < m_scrollBottom ) {
		++m_cursorRow;
		return;
	}
	ScrollUp(1);
}

void TerminalModel::ReverseLineFeed()
{
	if( m_cursorRow > m_scrollTop ) --m_cursorRow;
	else ScrollDown(1);
}

void TerminalModel::ScrollUp( std::size_t lines )
{
	lines = std::min(lines, m_scrollBottom - m_scrollTop + 1);
	for( std::size_t i = 0; i < lines; ++i ) {
		if( m_scrollTop == 0 && m_scrollBottom == m_rowsCount - 1 && !m_alternateScreen && m_scrollbackLimit != 0 ) {
			m_scrollback.push_back(m_rows.front());
			if( m_scrollback.size() > m_scrollbackLimit ) m_scrollback.pop_front();
		}
		m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollTop));
		m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom), MakeBlankRow());
	}
	MarkDirtyRange(m_scrollTop, m_scrollBottom);
}

void TerminalModel::ScrollDown( std::size_t lines )
{
	lines = std::min(lines, m_scrollBottom - m_scrollTop + 1);
	for( std::size_t i = 0; i < lines; ++i ) {
		m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom));
		m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollTop), MakeBlankRow());
	}
	MarkDirtyRange(m_scrollTop, m_scrollBottom);
}

void TerminalModel::ReverseIndex() { ReverseLineFeed(); }

void TerminalModel::MoveCursorRelative( int columns, int rows )
{
	const auto column = static_cast<long long>(m_cursorColumn) + columns;
	const auto row = static_cast<long long>(m_cursorRow) + rows;
	m_cursorColumn = static_cast<std::size_t>(std::clamp<long long>(column, 0, static_cast<long long>(m_columns - 1)));
	m_cursorRow = static_cast<std::size_t>(std::clamp<long long>(row, 0, static_cast<long long>(m_rowsCount - 1)));
}

void TerminalModel::SetCursorPosition( std::size_t column, std::size_t row )
{
	m_cursorColumn = std::min(column, m_columns - 1);
	m_cursorRow = std::min(row, m_rowsCount - 1);
}

void TerminalModel::SetCursorColumn( std::size_t column ) { m_cursorColumn = std::min(column, m_columns - 1); }
void TerminalModel::SaveCursor() noexcept { m_savedCursorColumn = m_cursorColumn; m_savedCursorRow = m_cursorRow; }
void TerminalModel::RestoreCursor() noexcept { SetCursorPosition(m_savedCursorColumn, m_savedCursorRow); }

void TerminalModel::ClearCellRange( TerminalRow& row, std::size_t begin, std::size_t end )
{
	begin = std::min(begin, m_columns);
	end = std::min(end, m_columns);
	for( auto i = begin; i < end; ++i ) row.cells[i] = {};
	SetCellAttributes(row, begin, end - begin, {});
}

void TerminalModel::EraseLine( int mode )
{
	auto& row = m_rows[m_cursorRow];
	if( mode == 0 ) ClearCellRange(row, m_cursorColumn, m_columns);
	else if( mode == 1 ) ClearCellRange(row, 0, std::min(m_columns, m_cursorColumn + 1));
	else if( mode == 2 ) ClearCellRange(row, 0, m_columns);
	MarkDirty(m_cursorRow);
}

void TerminalModel::EraseDisplay( int mode )
{
	if( mode == 0 ) {
		EraseLine(0);
		for( auto row = m_cursorRow + 1; row < m_rowsCount; ++row ) ClearCellRange(m_rows[row], 0, m_columns);
		MarkDirtyRange(m_cursorRow, m_rowsCount - 1);
	} else if( mode == 1 ) {
		for( std::size_t row = 0; row < m_cursorRow; ++row ) ClearCellRange(m_rows[row], 0, m_columns);
		EraseLine(1);
		MarkDirtyRange(0, m_cursorRow);
	} else if( mode == 2 || mode == 3 ) {
		for( auto& row : m_rows ) ClearCellRange(row, 0, m_columns);
		if( mode == 3 ) m_scrollback.clear();
		MarkDirtyRange(0, m_rowsCount - 1);
	}
}

void TerminalModel::SetScrollRegion( std::size_t top, std::size_t bottom )
{
	if( top < bottom && bottom < m_rowsCount ) { m_scrollTop = top; m_scrollBottom = bottom; }
	else { m_scrollTop = 0; m_scrollBottom = m_rowsCount - 1; }
	SetCursorPosition(0, 0);
}

void TerminalModel::SetAlternateScreen( bool enabled )
{
	if( enabled == m_alternateScreen ) return;
	if( enabled ) {
		m_savedMainRows = std::move(m_rows);
		m_savedMainCursorColumn = m_cursorColumn;
		m_savedMainCursorRow = m_cursorRow;
		m_rows.clear();
		for( std::size_t i = 0; i < m_rowsCount; ++i ) m_rows.push_back(MakeBlankRow());
		m_cursorColumn = m_cursorRow = 0;
	} else {
		m_rows = std::move(m_savedMainRows);
		m_cursorColumn = std::min(m_savedMainCursorColumn, m_columns - 1);
		m_cursorRow = std::min(m_savedMainCursorRow, m_rowsCount - 1);
	}
	m_alternateScreen = enabled;
	m_scrollTop = 0;
	m_scrollBottom = m_rowsCount - 1;
	m_dirtyRows.assign(m_rowsCount, true);
}

void TerminalModel::ResetAttributes() noexcept { m_attributes = {}; }

void TerminalModel::SetMode( int mode, bool enabled ) noexcept
{
	switch( mode ) {
	case 1000: m_modes.mouseButtonTracking = enabled; break;
	case 1002: m_modes.mouseDragTracking = enabled; break;
	case 1003: m_modes.mouseAnyEventTracking = enabled; break;
	case 1006: m_modes.mouseSgrEncoding = enabled; break;
	case 2004: m_modes.bracketedPaste = enabled; break;
	case 2026: m_modes.synchronizedOutput = enabled; break;
	default: break;
	}
}

void TerminalModel::SetTitle( std::wstring title ) { m_title = std::move(title); }

void TerminalModel::SetCellAttributes( TerminalRow& row, std::size_t column, std::size_t length, const TerminalAttributes& attributes )
{
	if( length == 0 || column >= m_columns ) return;
	const auto end = std::min(m_columns, column + length);
	std::fill(row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(column),
		row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(end), attributes);

	// Runs always cover the complete row in start order. Replace only the runs
	// intersecting this edit and merge the at-most-two neighboring boundaries;
	// terminal output therefore avoids rebuilding and sorting every row on each
	// printed cell.
	const auto first = std::lower_bound(row.attributeRuns.begin(), row.attributeRuns.end(), column,
		[]( const TerminalAttributeRun& run, std::size_t position ) { return run.start + run.length <= position; });
	const auto after = std::lower_bound(first, row.attributeRuns.end(), end,
		[]( const TerminalAttributeRun& run, std::size_t position ) { return run.start < position; });
	const auto firstIndex = static_cast<std::size_t>(first - row.attributeRuns.begin());

	std::array<TerminalAttributeRun, 3> replacements{};
	std::size_t replacementCount{};
	if( first->start < column ) replacements[replacementCount++] = { first->start, column - first->start, first->attributes };
	replacements[replacementCount++] = { column, end - column, attributes };
	const auto last = std::prev(after);
	const auto lastEnd = last->start + last->length;
	if( lastEnd > end ) replacements[replacementCount++] = { end, lastEnd - end, last->attributes };

	row.attributeRuns.erase(first, after);
	row.attributeRuns.insert(row.attributeRuns.begin() + static_cast<std::ptrdiff_t>(firstIndex),
		replacements.begin(), replacements.begin() + static_cast<std::ptrdiff_t>(replacementCount));

	auto mergeBegin = firstIndex == 0 ? 0 : firstIndex - 1;
	auto mergeEnd = std::min(row.attributeRuns.size(), firstIndex + replacementCount + 1);
	for( auto index = mergeBegin; index + 1 < mergeEnd; ) {
		auto& current = row.attributeRuns[index];
		auto& next = row.attributeRuns[index + 1];
		if( current.start + current.length == next.start && current.attributes == next.attributes ) {
			current.length += next.length;
			row.attributeRuns.erase(row.attributeRuns.begin() + static_cast<std::ptrdiff_t>(index + 1));
			--mergeEnd;
		} else {
			++index;
		}
	}
}

void TerminalModel::NormalizeAttributeRuns( TerminalRow& row )
{
	std::vector<TerminalAttributeRun> normalized;
	for( auto run : row.attributeRuns ) {
		if( run.start >= m_columns ) continue;
		run.length = std::min(run.length, m_columns - run.start);
		if( run.length == 0 ) continue;
		if( !normalized.empty() && normalized.back().start + normalized.back().length == run.start && normalized.back().attributes == run.attributes ) normalized.back().length += run.length;
		else normalized.push_back(run);
	}
	if( normalized.empty() ) normalized.push_back({ 0, m_columns, {} });
	else if( normalized.back().start + normalized.back().length < m_columns ) normalized.push_back({ normalized.back().start + normalized.back().length, m_columns - normalized.back().start - normalized.back().length, {} });
	row.attributeRuns = std::move(normalized);
}

void TerminalModel::MarkDirty( std::size_t row ) noexcept { if( row < m_dirtyRows.size() ) m_dirtyRows[row] = true; }
void TerminalModel::MarkDirtyRange( std::size_t top, std::size_t bottom ) noexcept { for( auto row = top; row <= bottom && row < m_dirtyRows.size(); ++row ) m_dirtyRows[row] = true; }

std::vector<std::size_t> TerminalModel::ConsumeDirtyRows()
{
	std::vector<std::size_t> result;
	for( std::size_t i = 0; i < m_dirtyRows.size(); ++i ) if( m_dirtyRows[i] ) { result.push_back(i); m_dirtyRows[i] = false; }
	return result;
}

} // namespace terminal
