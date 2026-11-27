/*! @file */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
};

constexpr bool operator==( const TerminalColor& left, const TerminalColor& right ) noexcept {
	return left.kind == right.kind && left.value == right.value;
}

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

struct TerminalReflowLayout;

//! One UI-thread-owned mutation of the main-screen scrollback coordinate space.
//! Appended rows advance an anchored viewport away from the live bottom, while
//! evicted rows shift every surviving global history coordinate toward zero.
//! A column reflow renumbers retained rows entirely and carries the layout map.
//! The change is an ORDERED list of steps: scrollback-count deltas accumulate
//! into one step until a reflow step is pushed, then a fresh counts step
//! starts.  Several resizes between two consumes therefore apply each
//! renumbering exactly once and in order instead of merging into one.
struct TerminalScrollbackChange final {
public:
	//! Either scrollback-count deltas (reflow == nullptr) or one column
	//! reflow's layout map covering the pre-resize global coordinate space
	//! (that step's old scrollback + old screen).
	struct Step final {
		std::size_t appended{};
		std::size_t evicted{};
		bool cleared{};
		std::shared_ptr<const TerminalReflowLayout> reflow;

		[[nodiscard]] bool IsReflow() const noexcept { return reflow != nullptr; }
	};

	//! The ordered steps; a consumer applies every one in order.
	[[nodiscard]] const std::vector<Step>& Steps() const noexcept { return m_steps; }
	[[nodiscard]] bool Changed() const noexcept { return !m_steps.empty(); }
	//! Aggregates across every counts step (a reflow step's renumbering
	//! lives in its layout, not in these totals).
	[[nodiscard]] std::size_t Appended() const noexcept;
	[[nodiscard]] std::size_t Evicted() const noexcept;
	[[nodiscard]] bool Cleared() const noexcept;
	//! The newest reflow step's layout, when the change holds one.
	[[nodiscard]] std::shared_ptr<const TerminalReflowLayout> Reflow() const noexcept;

private:
	friend class TerminalModel;

	void RecordAppended() noexcept { ++Counts().appended; }
	void RecordEvicted( std::size_t count = 1 ) noexcept
	{
		if( count == 0 ) return;
		Counts().evicted += count;
	}
	void MarkCleared() noexcept { Counts().cleared = true; }
	void SetReflow( std::shared_ptr<const TerminalReflowLayout> layout ) noexcept
	{
		m_steps.push_back({ .reflow = std::move(layout) });
	}
	Step& Counts() noexcept
	{
		if( m_steps.empty() || m_steps.back().IsReflow() ) m_steps.push_back({});
		return m_steps.back();
	}

	std::vector<Step> m_steps;
};

class TerminalRowView final {
public:
	class const_iterator final {
	public:
		using value_type = TerminalRow;
		using difference_type = std::ptrdiff_t;
		using pointer = const TerminalRow*;
		using reference = const TerminalRow&;
		using iterator_category = std::forward_iterator_tag;

		const_iterator() = default;
		const_iterator( const TerminalRowView* view, std::size_t index ) noexcept
			: m_view(view), m_index(index) {}
		reference operator*() const noexcept { return (*m_view)[m_index]; }
		pointer operator->() const noexcept { return &(*m_view)[m_index]; }
		const_iterator& operator++() noexcept { ++m_index; return *this; }
		const_iterator operator++(int) noexcept { auto copy = *this; ++*this; return copy; }
		friend bool operator==( const const_iterator&, const const_iterator& ) noexcept = default;

	private:
		const TerminalRowView* m_view{};
		std::size_t m_index{};
	};

	TerminalRowView( const std::deque<TerminalRow>& rows, std::size_t head ) noexcept
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

using TerminalScrollbackView = TerminalRowView;

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
	//! An explicit scroll (CSI S) clears the rotated seams; the autowrap
	//! path passes keepsAutowrapLeader so the flag it just set survives.
	void ScrollUp( std::size_t lines, bool keepsAutowrapLeader = false );
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
	//! OSC 52 write from the application. Only the latest request is retained
	//! until the UI owner consumes it; reading the clipboard is never offered.
	void RequestClipboardWrite( std::wstring text ) { m_pendingClipboardWrite = std::move(text); }
	[[nodiscard]] std::optional<std::wstring> ConsumeClipboardWrite() noexcept { return std::exchange(m_pendingClipboardWrite, std::nullopt); }

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
	//! Advances on every column reflow: retained history/screen rows were
	//! renumbered, so every older capture coordinate is stale.
	std::uint64_t LayoutGeneration() const noexcept { return m_layoutGeneration; }
	//! Monotonic history ordinals owned by the model, independent of who
	//! consumed TerminalScrollbackChange.  Retained scrollback row i is
	//! ordinal HistoryEvictedTotal() + i + 1.  A column reflow retires
	//! every old ordinal (evicted becomes the old appended total, appended
	//! becomes evicted + retained rows), so both totals stay monotonic
	//! across renumbering.
	std::uint64_t HistoryAppendedTotal() const noexcept { return m_historyAppendedTotal; }
	std::uint64_t HistoryEvictedTotal() const noexcept { return m_historyEvictedTotal; }
	TerminalRowView Rows() const noexcept { return { m_rows, m_rowsHead }; }
	TerminalScrollbackView Scrollback() const noexcept { return { m_scrollback, m_scrollbackHead }; }
	std::vector<std::size_t> ConsumeDirtyRows();
	//! Whether an unconsumed scrollback change is pending (peek without
	//! consuming: a resize must know if it produced one while leaving it
	//! for the pane and the drain).
	[[nodiscard]] bool HasPendingScrollbackChange() const noexcept { return m_pendingScrollbackChange.Changed(); }
	TerminalScrollbackChange ConsumeScrollbackChange() noexcept;

private:
	TerminalRow MakeBlankRow( const TerminalAttributes& attributes = {} ) const;
	void ResetRow( TerminalRow& row, const TerminalAttributes& attributes ) const;
	void RecycleForBlankRow( TerminalRow& outgoing, const TerminalAttributes& attributes );
	void AppendScrollbackRow( TerminalRow&& row );
	TerminalRow& RowAt( std::size_t row ) noexcept;
	const TerminalRow& RowAt( std::size_t row ) const noexcept;
	void NormalizeRowsOrder();
	void NormalizeScrollbackOrder();
	void ClearCellRange( TerminalRow& row, std::size_t begin, std::size_t end );
	void SetCellAttributes( TerminalRow& row, std::size_t column, std::size_t length, const TerminalAttributes& attributes );
	void RebuildAttributeRuns( TerminalRow& row );
	void RepairWideCells( TerminalRow& row );
	void MarkDirty( std::size_t row ) noexcept;
	void MarkDirtyRange( std::size_t top, std::size_t bottom ) noexcept;
	//! The "previous row" of screen row `row`: row - 1, or the newest
	//! history row when `row` is 0 on the main screen (none exists on the
	//! alternate screen).  Clears its `wrapped` flag.
	void ClearPreviousRowFlag( std::size_t row ) noexcept;
	//! keepsAutowrapLeader: the autowrap path set the leader flag itself;
	//! an explicit LF clears it before calling and keeps the seam clears.
	void LineFeed( bool keepsAutowrapLeader = false );
	void ReverseLineFeed();
	void CarriageReturn() noexcept { m_cursorColumn = 0; }
	void AppendCodepoint( TerminalCell& cell, char32_t codepoint ) noexcept;
	static int CodepointWidth( char32_t codepoint ) noexcept;
	static bool IsCombining( char32_t codepoint ) noexcept;

	std::size_t m_columns;
	std::size_t m_rowsCount;
	std::size_t m_scrollbackLimit;
	// Full-screen scrolling advances this logical head and recycles the outgoing
	// row in place.  The deque never shifts or allocates for a steady-state line;
	// TerminalRowView preserves O(1) logical access for renderer and parser.
	std::deque<TerminalRow> m_rows;
	std::size_t m_rowsHead{};
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
	std::uint64_t m_layoutGeneration{};
	std::uint64_t m_historyAppendedTotal{};
	std::uint64_t m_historyEvictedTotal{};
	std::wstring m_title;
	std::optional<std::wstring> m_pendingClipboardWrite;
	bool m_alternateScreen{};
};

} // namespace terminal
