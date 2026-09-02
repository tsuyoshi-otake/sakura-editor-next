/*! @file */
#include "StdAfx.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/unicode/TerminalGraphemeWidth.h"

#include <algorithm>
#include <utility>

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

std::size_t TerminalScrollbackChange::Appended() const noexcept
{
	return m_appended;
}

std::size_t TerminalScrollbackChange::Evicted() const noexcept
{
	return m_evicted;
}

bool TerminalScrollbackChange::Cleared() const noexcept
{
	return m_cleared;
}

bool TerminalScrollbackChange::Changed() const noexcept
{
	return m_appended != 0 || m_evicted != 0 || m_cleared;
}

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
	m_pendingScrollbackChange = {};
}

TerminalRow TerminalModel::MakeBlankRow( const TerminalAttributes& attributes ) const
{
	TerminalRow row;
	row.cells.resize(m_columns);
	row.cellAttributes.resize(m_columns, attributes);
	row.attributeRuns.push_back({ 0, m_columns, attributes });
	return row;
}

void TerminalModel::ResetRow( TerminalRow& row, const TerminalAttributes& attributes ) const
{
	// A scrolled-off row already owns column-sized buffers.  Keep those buffers
	// and merely reset their contents; steady-state scrolling at the scrollback
	// cap therefore avoids a cell/attribute allocation for every new line.
	if( row.cells.size() != m_columns ) row.cells.resize(m_columns);
	if( row.cellAttributes.size() != m_columns ) row.cellAttributes.resize(m_columns);
	std::fill(row.cells.begin(), row.cells.end(), TerminalCell{});
	std::fill(row.cellAttributes.begin(), row.cellAttributes.end(), attributes);
	row.attributeRuns.clear();
	row.attributeRuns.push_back({ 0, m_columns, attributes });
	row.wrapped = false;
}

void TerminalModel::RecycleForBlankRow( TerminalRow& outgoing, const TerminalAttributes& attributes )
{
	if( !m_alternateScreen && m_scrollbackLimit != 0 ) {
		if( m_scrollback.size() == m_scrollbackLimit ) {
			std::swap(outgoing, m_scrollback[m_scrollbackHead]);
			m_scrollbackHead = (m_scrollbackHead + 1) % m_scrollback.size();
			m_pendingScrollbackChange.RecordAppended();
			m_pendingScrollbackChange.RecordEvicted();
			ResetRow(outgoing, attributes);
			return;
		}
		m_scrollback.push_back(std::move(outgoing));
		m_pendingScrollbackChange.RecordAppended();
		outgoing = MakeBlankRow(attributes);
		return;
	}
	ResetRow(outgoing, attributes);
}

void TerminalModel::AppendScrollbackRow( TerminalRow&& row )
{
	if( m_scrollbackLimit == 0 ) return;
	if( m_scrollback.size() < m_scrollbackLimit ) {
		m_scrollback.push_back(std::move(row));
		m_pendingScrollbackChange.RecordAppended();
		return;
	}
	m_scrollback[m_scrollbackHead] = std::move(row);
	m_scrollbackHead = (m_scrollbackHead + 1) % m_scrollback.size();
	m_pendingScrollbackChange.RecordAppended();
	m_pendingScrollbackChange.RecordEvicted();
}

TerminalRow& TerminalModel::RowAt( std::size_t row ) noexcept
{
	return m_rows[(m_rowsHead + row) % m_rows.size()];
}

const TerminalRow& TerminalModel::RowAt( std::size_t row ) const noexcept
{
	return m_rows[(m_rowsHead + row) % m_rows.size()];
}

void TerminalModel::NormalizeRowsOrder()
{
	if( m_rowsHead == 0 || m_rows.empty() ) return;
	std::rotate(m_rows.begin(),
		m_rows.begin() + static_cast<std::ptrdiff_t>(m_rowsHead),
		m_rows.end());
	m_rowsHead = 0;
}

void TerminalModel::NormalizeScrollbackOrder()
{
	if( m_scrollbackHead == 0 || m_scrollback.empty() ) return;
	std::rotate(m_scrollback.begin(),
		m_scrollback.begin() + static_cast<std::ptrdiff_t>(m_scrollbackHead),
		m_scrollback.end());
	m_scrollbackHead = 0;
}

void TerminalModel::Reset()
{
	const auto discardedHistory = m_scrollback.size();
	m_rows.clear();
	m_rowsHead = 0;
	for( std::size_t i = 0; i < m_rowsCount; ++i ) m_rows.push_back(MakeBlankRow());
	m_scrollback.clear();
	m_scrollbackHead = 0;
	m_savedMainRows.clear();
	m_cursorColumn = m_cursorRow = 0;
	m_savedCursorColumn = m_savedCursorRow = 0;
	m_savedMainCursorColumn = m_savedMainCursorRow = 0;
	m_savedMainSavedCursorColumn = m_savedMainSavedCursorRow = 0;
	m_savedMainScrollTop = 0;
	m_savedMainScrollBottom = m_rowsCount - 1;
	m_scrollTop = 0;
	m_scrollBottom = m_rowsCount - 1;
	m_attributes = {};
	m_savedMainAttributes = {};
	m_modes = {};
	m_synchronizedOutputCommitGeneration = 0;
	m_alternateScreen = false;
	m_dirtyRows.assign(m_rowsCount, true);
	m_pendingScrollbackChange.RecordEvicted(discardedHistory);
	m_pendingScrollbackChange.MarkCleared();
}

void TerminalModel::Resize( std::size_t columns, std::size_t rows )
{
	NormalizeRowsOrder();
	columns = std::max<std::size_t>(1, columns);
	rows = std::max<std::size_t>(1, rows);
	if( columns != m_columns ) {
		m_columns = columns;
		const auto resizeColumns = [this, columns]( auto& rowsToResize ) {
			for( auto& row : rowsToResize ) {
				row.cells.resize(columns);
				row.cellAttributes.resize(columns);
				RepairWideCells(row);
				RebuildAttributeRuns(row);
			}
		};
		resizeColumns(m_rows);
		resizeColumns(m_scrollback);
		resizeColumns(m_savedMainRows);
	}
	if( rows != m_rowsCount ) {
		while( m_rows.size() > rows ) {
			TerminalRow outgoing = std::move(m_rows.front());
			m_rows.pop_front();
			if( !m_alternateScreen && m_scrollbackLimit != 0 ) {
				AppendScrollbackRow(std::move(outgoing));
			}
		}
		while( m_rows.size() < rows ) m_rows.push_back(MakeBlankRow(m_attributes));
		if( m_alternateScreen ) {
			while( m_savedMainRows.size() > rows ) m_savedMainRows.erase(m_savedMainRows.begin());
			while( m_savedMainRows.size() < rows ) m_savedMainRows.push_back(MakeBlankRow(m_savedMainAttributes));
		}
		m_rowsCount = rows;
	}
	SetScrollbackLimit(m_scrollbackLimit);
	m_cursorColumn = std::min(m_cursorColumn, m_columns - 1);
	m_cursorRow = std::min(m_cursorRow, m_rowsCount - 1);
	m_savedMainCursorColumn = std::min(m_savedMainCursorColumn, m_columns - 1);
	m_savedMainCursorRow = std::min(m_savedMainCursorRow, m_rowsCount - 1);
	m_savedMainSavedCursorColumn = std::min(m_savedMainSavedCursorColumn, m_columns - 1);
	m_savedMainSavedCursorRow = std::min(m_savedMainSavedCursorRow, m_rowsCount - 1);
	m_scrollTop = 0;
	m_scrollBottom = m_rowsCount - 1;
	m_dirtyRows.assign(m_rowsCount, true);
}

void TerminalModel::SetScrollbackLimit( std::size_t lines )
{
	lines = ClampScrollback(lines);
	if( lines == m_scrollbackLimit ) return;
	NormalizeScrollbackOrder();
	m_scrollbackLimit = lines;
	const auto previousSize = m_scrollback.size();
	while( m_scrollback.size() > m_scrollbackLimit ) m_scrollback.pop_front();
	m_scrollbackHead = 0;
	m_pendingScrollbackChange.RecordEvicted(previousSize - m_scrollback.size());
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
	// Printable ASCII always occupies one terminal cell.  Keep the common shell
	// output path out of the general Unicode grapheme state machine.
	if( cp >= U' ' && cp <= U'~' ) return 1;
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
		while( previous != 0 && RowAt(m_cursorRow).cells[previous].continuation ) --previous;
		auto& row = RowAt(m_cursorRow);
		auto& previousCell = row.cells[previous];
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
	// The vendored Windows Terminal width detector is the source of truth for
	// non-extending codepoints.  In particular, format and combining codepoints
	// such as ZWSP must not become an empty one-column glyph merely because they
	// were not appended to the preceding grapheme above.  The explicit combining
	// set remains a conservative fallback for the supported extension forms when
	// they arrive without a preceding base cell.
	const auto codepointWidth = CodepointWidth(codepoint);
	if( codepointWidth == 0 || IsCombining(codepoint) ) return;
	const auto width = std::min<int>(static_cast<int>(m_columns), codepointWidth);
	if( m_cursorColumn >= m_columns || (width == 2 && m_cursorColumn + 1 >= m_columns) ) {
		if( m_modes.autowrap ) {
			RowAt(m_cursorRow).wrapped = true;
			CarriageReturn();
			LineFeed();
		} else {
			m_cursorColumn = m_columns - static_cast<std::size_t>(width);
		}
	}
	auto& row = RowAt(m_cursorRow);
	// A printable character always replaces the entire grapheme it intersects.
	// Preserve the rendition on a displaced half; only the newly printed cells
	// receive the current rendition below.
	if( row.cells[m_cursorColumn].continuation ) {
		if( m_cursorColumn != 0 ) {
			auto& lead = row.cells[m_cursorColumn - 1];
			if( !lead.continuation && lead.width == 2 ) lead = {};
		}
	} else if( row.cells[m_cursorColumn].width == 2 && m_cursorColumn + 1 < m_columns ) {
		auto& continuation = row.cells[m_cursorColumn + 1];
		if( continuation.continuation ) continuation = {};
	}
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
	if( lines == 0 ) return;
	const bool wholeMainScreen = m_scrollTop == 0 && m_scrollBottom == m_rowsCount - 1;
	if( wholeMainScreen ) {
		for( std::size_t i = 0; i < lines; ++i ) {
			const auto outgoingIndex = m_rowsHead;
			RecycleForBlankRow(m_rows[outgoingIndex], m_attributes);
			m_rowsHead = (m_rowsHead + 1) % m_rows.size();
		}
		MarkDirtyRange(m_scrollTop, m_scrollBottom);
		return;
	}

	// Partial DECSTBM regions are not scrollback. Rotate the row objects in
	// place, then reset only the rows introduced at the bottom. This preserves
	// all rows outside the region and avoids vector erase/insert churn.
	NormalizeRowsOrder();
	auto first = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollTop);
	auto last = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom + 1);
	std::rotate(first, first + static_cast<std::ptrdiff_t>(lines), last);
	for( auto row = last - static_cast<std::ptrdiff_t>(lines); row != last; ++row ) ResetRow(*row, m_attributes);
	MarkDirtyRange(m_scrollTop, m_scrollBottom);
}

void TerminalModel::ScrollDown( std::size_t lines )
{
	lines = std::min(lines, m_scrollBottom - m_scrollTop + 1);
	if( lines == 0 ) return;
	NormalizeRowsOrder();
	const auto first = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollTop);
	const auto last = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom + 1);
	std::rotate(first, last - static_cast<std::ptrdiff_t>(lines), last);
	for( auto row = first; row != first + static_cast<std::ptrdiff_t>(lines); ++row ) ResetRow(*row, m_attributes);
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
void TerminalModel::SetCursorRow( std::size_t row ) { m_cursorRow = std::min(row, m_rowsCount - 1); }
void TerminalModel::SaveCursor() noexcept { m_savedCursorColumn = m_cursorColumn; m_savedCursorRow = m_cursorRow; }
void TerminalModel::RestoreCursor() noexcept { SetCursorPosition(m_savedCursorColumn, m_savedCursorRow); }

void TerminalModel::ClearCellRange( TerminalRow& row, std::size_t begin, std::size_t end )
{
	begin = std::min(begin, m_columns);
	end = std::min(end, m_columns);
	for( auto i = begin; i < end; ++i ) row.cells[i] = {};
	SetCellAttributes(row, begin, end - begin, m_attributes);
}

void TerminalModel::EraseLine( int mode )
{
	auto& row = RowAt(m_cursorRow);
	if( mode == 0 ) ClearCellRange(row, m_cursorColumn, m_columns);
	else if( mode == 1 ) ClearCellRange(row, 0, std::min(m_columns, m_cursorColumn + 1));
	else if( mode == 2 ) ClearCellRange(row, 0, m_columns);
	MarkDirty(m_cursorRow);
}

void TerminalModel::EraseCharacters( std::size_t count )
{
	auto& row = RowAt(m_cursorRow);
	ClearCellRange(row, m_cursorColumn,
		std::min(m_columns, m_cursorColumn + std::max<std::size_t>(1, count)));
	RepairWideCells(row);
	MarkDirty(m_cursorRow);
}

void TerminalModel::InsertCharacters( std::size_t count )
{
	auto& row = RowAt(m_cursorRow);
	count = std::min(std::max<std::size_t>(1, count), m_columns - m_cursorColumn);
	const auto sourceEnd = m_columns - count;
	std::move_backward(row.cells.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn),
		row.cells.begin() + static_cast<std::ptrdiff_t>(sourceEnd), row.cells.end());
	std::move_backward(row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn),
		row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(sourceEnd), row.cellAttributes.end());
	for( auto column = m_cursorColumn; column < m_cursorColumn + count; ++column ) {
		row.cells[column] = {};
		row.cellAttributes[column] = m_attributes;
	}
	RepairWideCells(row);
	RebuildAttributeRuns(row);
	MarkDirty(m_cursorRow);
}

void TerminalModel::DeleteCharacters( std::size_t count )
{
	auto& row = RowAt(m_cursorRow);
	count = std::min(std::max<std::size_t>(1, count), m_columns - m_cursorColumn);
	std::move(row.cells.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn + count), row.cells.end(),
		row.cells.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn));
	std::move(row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn + count), row.cellAttributes.end(),
		row.cellAttributes.begin() + static_cast<std::ptrdiff_t>(m_cursorColumn));
	for( auto column = m_columns - count; column < m_columns; ++column ) {
		row.cells[column] = {};
		row.cellAttributes[column] = m_attributes;
	}
	RepairWideCells(row);
	RebuildAttributeRuns(row);
	MarkDirty(m_cursorRow);
}

void TerminalModel::InsertLines( std::size_t count )
{
	if( m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom ) return;
	count = std::min(std::max<std::size_t>(1, count), m_scrollBottom - m_cursorRow + 1);
	NormalizeRowsOrder();
	const auto first = m_rows.begin() + static_cast<std::ptrdiff_t>(m_cursorRow);
	const auto last = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom + 1);
	std::rotate(first, last - static_cast<std::ptrdiff_t>(count), last);
	for( auto row = first; row != first + static_cast<std::ptrdiff_t>(count); ++row ) ResetRow(*row, m_attributes);
	MarkDirtyRange(m_cursorRow, m_scrollBottom);
}

void TerminalModel::DeleteLines( std::size_t count )
{
	if( m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom ) return;
	count = std::min(std::max<std::size_t>(1, count), m_scrollBottom - m_cursorRow + 1);
	NormalizeRowsOrder();
	const auto first = m_rows.begin() + static_cast<std::ptrdiff_t>(m_cursorRow);
	const auto last = m_rows.begin() + static_cast<std::ptrdiff_t>(m_scrollBottom + 1);
	std::rotate(first, first + static_cast<std::ptrdiff_t>(count), last);
	for( auto row = last - static_cast<std::ptrdiff_t>(count); row != last; ++row ) ResetRow(*row, m_attributes);
	MarkDirtyRange(m_cursorRow, m_scrollBottom);
}

void TerminalModel::EraseDisplay( int mode )
{
	if( mode == 0 ) {
		EraseLine(0);
		for( auto row = m_cursorRow + 1; row < m_rowsCount; ++row ) ClearCellRange(RowAt(row), 0, m_columns);
		MarkDirtyRange(m_cursorRow, m_rowsCount - 1);
	} else if( mode == 1 ) {
		for( std::size_t row = 0; row < m_cursorRow; ++row ) ClearCellRange(RowAt(row), 0, m_columns);
		EraseLine(1);
		MarkDirtyRange(0, m_cursorRow);
	} else if( mode == 2 || mode == 3 ) {
		for( auto& row : m_rows ) ClearCellRange(row, 0, m_columns);
		if( mode == 3 ) {
			m_pendingScrollbackChange.RecordEvicted(m_scrollback.size());
			m_scrollback.clear();
			m_scrollbackHead = 0;
			m_pendingScrollbackChange.MarkCleared();
		}
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
		NormalizeRowsOrder();
		m_savedMainRows = std::move(m_rows);
		m_savedMainCursorColumn = m_cursorColumn;
		m_savedMainCursorRow = m_cursorRow;
		m_savedMainSavedCursorColumn = m_savedCursorColumn;
		m_savedMainSavedCursorRow = m_savedCursorRow;
		m_savedMainScrollTop = m_scrollTop;
		m_savedMainScrollBottom = m_scrollBottom;
		m_savedMainAttributes = m_attributes;
		m_attributes = {};
		m_rows.clear();
		m_rowsHead = 0;
		for( std::size_t i = 0; i < m_rowsCount; ++i ) m_rows.push_back(MakeBlankRow());
		m_cursorColumn = m_cursorRow = 0;
		m_savedCursorColumn = m_savedCursorRow = 0;
		m_scrollTop = 0;
		m_scrollBottom = m_rowsCount - 1;
	} else {
		m_rows = std::move(m_savedMainRows);
		m_rowsHead = 0;
		m_cursorColumn = std::min(m_savedMainCursorColumn, m_columns - 1);
		m_cursorRow = std::min(m_savedMainCursorRow, m_rowsCount - 1);
		m_savedCursorColumn = std::min(m_savedMainSavedCursorColumn, m_columns - 1);
		m_savedCursorRow = std::min(m_savedMainSavedCursorRow, m_rowsCount - 1);
		m_scrollTop = std::min(m_savedMainScrollTop, m_rowsCount - 1);
		m_scrollBottom = std::clamp(m_savedMainScrollBottom, m_scrollTop, m_rowsCount - 1);
		m_attributes = m_savedMainAttributes;
	}
	m_alternateScreen = enabled;
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
	case 7: m_modes.autowrap = enabled; break;
	case 25: m_modes.cursorVisible = enabled; MarkDirty(m_cursorRow); break;
	case 2026:
		if( m_modes.synchronizedOutput && !enabled ) ++m_synchronizedOutputCommitGeneration;
		m_modes.synchronizedOutput = enabled;
		break;
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

void TerminalModel::RebuildAttributeRuns( TerminalRow& row )
{
	row.attributeRuns.clear();
	if( row.cellAttributes.size() < m_columns ) row.cellAttributes.resize(m_columns);
	for( std::size_t column = 0; column < m_columns; ) {
		const auto attributes = row.cellAttributes[column];
		auto end = column + 1;
		while( end < m_columns && row.cellAttributes[end] == attributes ) ++end;
		row.attributeRuns.push_back({ column, end - column, attributes });
		column = end;
	}
}

void TerminalModel::RepairWideCells( TerminalRow& row )
{
	for( std::size_t column = 0; column < m_columns; ++column ) {
		auto& cell = row.cells[column];
		if( cell.continuation ) {
			if( column == 0 || row.cells[column - 1].continuation || row.cells[column - 1].width != 2 ) cell = {};
			continue;
		}
		if( cell.width != 2 ) continue;
		if( column + 1 >= m_columns ) {
			// A resize may have removed the continuation half of a wide
			// grapheme. Do not render the remaining half as a clipped glyph.
			cell = {};
			continue;
		}
		auto& continuation = row.cells[column + 1];
		continuation = {};
		continuation.continuation = true;
		continuation.width = 0;
		row.cellAttributes[column + 1] = row.cellAttributes[column];
		++column;
	}
}

void TerminalModel::MarkDirty( std::size_t row ) noexcept { if( row < m_dirtyRows.size() ) m_dirtyRows[row] = true; }
void TerminalModel::MarkDirtyRange( std::size_t top, std::size_t bottom ) noexcept { for( auto row = top; row <= bottom && row < m_dirtyRows.size(); ++row ) m_dirtyRows[row] = true; }

std::vector<std::size_t> TerminalModel::ConsumeDirtyRows()
{
	std::vector<std::size_t> result;
	for( std::size_t i = 0; i < m_dirtyRows.size(); ++i ) if( m_dirtyRows[i] ) { result.push_back(i); m_dirtyRows[i] = false; }
	return result;
}

TerminalScrollbackChange TerminalModel::ConsumeScrollbackChange() noexcept
{
	return std::exchange(m_pendingScrollbackChange, TerminalScrollbackChange{});
}

} // namespace terminal
