/*! @file */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

enum class TerminalColorKind : std::uint8_t {
	Default,
	Indexed,
	Rgb,
};

struct TerminalColor {
	TerminalColorKind kind{ TerminalColorKind::Default };
	std::uint32_t value{};

	static constexpr TerminalColor Indexed( std::uint8_t index ) noexcept { return { TerminalColorKind::Indexed, index }; }
	static constexpr TerminalColor Rgb( std::uint8_t red, std::uint8_t green, std::uint8_t blue ) noexcept {
		return { TerminalColorKind::Rgb, (static_cast<std::uint32_t>(red) << 16) | (static_cast<std::uint32_t>(green) << 8) | blue };
	}
	friend constexpr bool operator==( const TerminalColor&, const TerminalColor& ) noexcept = default;
};

struct TerminalAttributes {
	TerminalColor foreground;
	TerminalColor background;
	bool bold{};
	bool underline{};
	bool inverse{};
	friend constexpr bool operator==( const TerminalAttributes&, const TerminalAttributes& ) noexcept = default;
};

// A grapheme is stored inline. Ordinary terminal output therefore performs no
// heap allocation per cell. Excessively long combining sequences are safely
// truncated at a UTF-16 boundary instead of growing without bound.
struct TerminalCell {
	static constexpr std::size_t kMaxCodeUnits = 12;
	std::array<wchar_t, kMaxCodeUnits> text{};
	std::uint8_t length{};
	std::uint8_t width{ 1 };
	bool continuation{};

	std::wstring_view Text() const noexcept { return { text.data(), length }; }
};

struct TerminalAttributeRun {
	std::size_t start{};
	std::size_t length{};
	TerminalAttributes attributes;
};

struct TerminalRow {
	std::vector<TerminalCell> cells;
	// This parallel, contiguous cell store makes renderer-side attribute lookup
	// constant time. attributeRuns remains the compact representation exposed to
	// consumers that paint or inspect ranges.
	std::vector<TerminalAttributes> cellAttributes;
	std::vector<TerminalAttributeRun> attributeRuns;
	bool wrapped{};

	const TerminalAttributes& AttributesAt( std::size_t column ) const noexcept;
};

//! One UI-thread-owned mutation of the main-screen scrollback coordinate space.
//! `appended` advances an anchored viewport away from the live bottom, while
//! `evicted` shifts every surviving global history coordinate toward zero.
struct TerminalScrollbackChange final {
	std::size_t appended{};
	std::size_t evicted{};
	bool cleared{};

	[[nodiscard]] bool Changed() const noexcept
	{
		return appended != 0 || evicted != 0 || cleared;
	}
};

class TerminalScrollbackView final {
public:
	class const_iterator final {
	public:
		using value_type = TerminalRow;
		using difference_type = std::ptrdiff_t;
		using pointer = const TerminalRow*;
		using reference = const TerminalRow&;
		using iterator_category = std::forward_iterator_tag;

		const_iterator() = default;
		const_iterator( const TerminalScrollbackView* view, std::size_t index ) noexcept
			: m_view(view), m_index(index) {}
		reference operator*() const noexcept { return (*m_view)[m_index]; }
		pointer operator->() const noexcept { return &(*m_view)[m_index]; }
		const_iterator& operator++() noexcept { ++m_index; return *this; }
		const_iterator operator++(int) noexcept { auto copy = *this; ++*this; return copy; }
		friend bool operator==( const const_iterator&, const const_iterator& ) noexcept = default;

	private:
		const TerminalScrollbackView* m_view{};
		std::size_t m_index{};
	};

	TerminalScrollbackView( const std::deque<TerminalRow>& rows, std::size_t head ) noexcept
		: m_rows(&rows), m_head(head) {}
	std::size_t size() const noexcept { return m_rows->size(); }
	bool empty() const noexcept { return m_rows->empty(); }
	const TerminalRow& operator[]( std::size_t index ) const noexcept {
		return (*m_rows)[m_rows->empty() ? 0 : (m_head + index) % m_rows->size()];
	}
	const_iterator begin() const noexcept { return { this, 0 }; }
	const_iterator end() const noexcept { return { this, size() }; }

private:
	const std::deque<TerminalRow>* m_rows;
	std::size_t m_head;
};

struct TerminalModes {
	bool cursorVisible{ true };
	bool autowrap{ true };
	bool bracketedPaste{};
	bool mouseButtonTracking{};
	bool mouseDragTracking{};
	bool mouseAnyEventTracking{};
	bool mouseSgrEncoding{};
	bool synchronizedOutput{};
};

class TerminalModel final {
public:
	static constexpr std::size_t kDefaultScrollbackLines = 1000;
	static constexpr std::size_t kMaxScrollbackLines = 100000;

	TerminalModel( std::size_t columns, std::size_t rows, std::size_t scrollbackLines = kDefaultScrollbackLines );

	void Reset();
	void Resize( std::size_t columns, std::size_t rows );
	void SetScrollbackLimit( std::size_t lines );
	void Print( char32_t codepoint );
	void ExecuteControl( wchar_t control );

	void MoveCursorRelative( int columns, int rows );
	void SetCursorPosition( std::size_t column, std::size_t row );
	void SetCursorColumn( std::size_t column );
	void SetCursorRow( std::size_t row );
	void SaveCursor() noexcept;
	void RestoreCursor() noexcept;
	void EraseDisplay( int mode );
	void EraseLine( int mode );
	void EraseCharacters( std::size_t count );
	void InsertCharacters( std::size_t count );
	void DeleteCharacters( std::size_t count );
	void InsertLines( std::size_t count );
	void DeleteLines( std::size_t count );
	void SetScrollRegion( std::size_t top, std::size_t bottom );
	void ScrollUp( std::size_t lines );
	void ScrollDown( std::size_t lines );
	void ReverseIndex();
	void SetAlternateScreen( bool enabled );

	void ResetAttributes() noexcept;
	void SetBold( bool enabled ) noexcept { m_attributes.bold = enabled; }
	void SetUnderline( bool enabled ) noexcept { m_attributes.underline = enabled; }
	void SetInverse( bool enabled ) noexcept { m_attributes.inverse = enabled; }
	void SetForeground( TerminalColor color ) noexcept { m_attributes.foreground = color; }
	void SetBackground( TerminalColor color ) noexcept { m_attributes.background = color; }
	void SetMode( int mode, bool enabled ) noexcept;
	void SetTitle( std::wstring title );

	std::size_t Columns() const noexcept { return m_columns; }
	std::size_t RowCount() const noexcept { return m_rowsCount; }
	std::size_t CursorColumn() const noexcept { return m_cursorColumn; }
	std::size_t CursorRow() const noexcept { return m_cursorRow; }
	std::size_t ScrollbackSize() const noexcept { return m_scrollback.size(); }
	std::size_t ScrollbackLimit() const noexcept { return m_scrollbackLimit; }
	bool IsAlternateScreen() const noexcept { return m_alternateScreen; }
	const TerminalAttributes& CurrentAttributes() const noexcept { return m_attributes; }
	const TerminalModes& Modes() const noexcept { return m_modes; }
	const std::wstring& Title() const noexcept { return m_title; }
	//! Monotonically advances whenever DEC synchronized output commits a frame.
	//! Consumers compare generations around parser drains so a completed frame is
	//! not lost when the same drain immediately begins the next frame.
	std::uint64_t SynchronizedOutputCommitGeneration() const noexcept { return m_synchronizedOutputCommitGeneration; }
	const std::deque<TerminalRow>& Rows() const noexcept { return m_rows; }
	TerminalScrollbackView Scrollback() const noexcept { return { m_scrollback, m_scrollbackHead }; }
	std::vector<std::size_t> ConsumeDirtyRows();
	TerminalScrollbackChange ConsumeScrollbackChange() noexcept;

private:
	TerminalRow MakeBlankRow( const TerminalAttributes& attributes = {} ) const;
	void ResetRow( TerminalRow& row, const TerminalAttributes& attributes ) const;
	TerminalRow RecycleForBlankRow( TerminalRow&& outgoing, const TerminalAttributes& attributes );
	void AppendScrollbackRow( TerminalRow&& row );
	void NormalizeScrollbackOrder();
	void ClearCellRange( TerminalRow& row, std::size_t begin, std::size_t end );
	void SetCellAttributes( TerminalRow& row, std::size_t column, std::size_t length, const TerminalAttributes& attributes );
	void NormalizeAttributeRuns( TerminalRow& row );
	void RebuildAttributeRuns( TerminalRow& row );
	void RepairWideCells( TerminalRow& row );
	void MarkDirty( std::size_t row ) noexcept;
	void MarkDirtyRange( std::size_t top, std::size_t bottom ) noexcept;
	void LineFeed();
	void ReverseLineFeed();
	void CarriageReturn() noexcept { m_cursorColumn = 0; }
	void AppendCodepoint( TerminalCell& cell, char32_t codepoint ) noexcept;
	static int CodepointWidth( char32_t codepoint ) noexcept;
	static bool IsCombining( char32_t codepoint ) noexcept;

	std::size_t m_columns;
	std::size_t m_rowsCount;
	std::size_t m_scrollbackLimit;
	// Screen rows are a deque so full-screen scrolling can transfer a row to
	// scrollback and recycle the evicted row in O(1), without vector erase/insert
	// shifts or per-line cell-buffer allocations.  Indexed access remains O(1)
	// for the renderer and parser.
	std::deque<TerminalRow> m_rows;
	// A bounded deque keeps steady-state eviction at the scrollback cap O(1).
	std::deque<TerminalRow> m_scrollback;
	std::size_t m_scrollbackHead{};
	TerminalScrollbackChange m_pendingScrollbackChange;
	std::deque<TerminalRow> m_savedMainRows;
	std::vector<bool> m_dirtyRows;
	std::size_t m_cursorColumn{};
	std::size_t m_cursorRow{};
	std::size_t m_savedCursorColumn{};
	std::size_t m_savedCursorRow{};
	std::size_t m_savedMainCursorColumn{};
	std::size_t m_savedMainCursorRow{};
	std::size_t m_savedMainSavedCursorColumn{};
	std::size_t m_savedMainSavedCursorRow{};
	std::size_t m_savedMainScrollTop{};
	std::size_t m_savedMainScrollBottom{};
	std::size_t m_scrollTop{};
	std::size_t m_scrollBottom{};
	TerminalAttributes m_attributes;
	TerminalAttributes m_savedMainAttributes;
	TerminalModes m_modes;
	std::uint64_t m_synchronizedOutputCommitGeneration{};
	std::wstring m_title;
	bool m_alternateScreen{};
};

} // namespace terminal
