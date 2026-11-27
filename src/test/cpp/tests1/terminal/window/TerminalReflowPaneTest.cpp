/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "terminal/window/CTerminalWnd.h"
#include "terminal/window/TerminalRenderMapping.h"

#include <Windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Print( terminal::TerminalModel& model, std::u32string_view text )
{
	for( const auto ch : text ) {
		if( ch < 32 ) model.ExecuteControl(static_cast<wchar_t>(ch));
		else model.Print(ch);
	}
}

std::wstring RowText( const terminal::TerminalRow& row )
{
	std::wstring text;
	for( const auto& cell : row.cells ) {
		if( cell.continuation ) continue;
		if( cell.length != 0 ) text.append(cell.Text());
	}
	return text;
}

class TerminalReflowPane : public testing::Test {
protected:
	void SetUp() override
	{
		parent = ::CreateWindowExW(0, L"STATIC", L"Terminal reflow test", WS_OVERLAPPED,
			0, 0, 900, 500, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
		ASSERT_NE(nullptr, parent);
		ASSERT_TRUE(window.Create(parent, ::GetModuleHandleW(nullptr)));
		window.SetResizeSink([this](terminal::TerminalSize size) { model.Resize(size.columns, size.rows); });
		window.Layout({ 0, 0, 800, 400 }, 96);
		model.Resize(window.GetTerminalSize().columns, window.GetTerminalSize().rows);
		window.SetModel(&model);
	}
	void TearDown() override
	{
		window.Close();
		if( parent ) ::DestroyWindow(parent);
	}
	void FillScreen()
	{
		for( std::size_t line = 0; line < 60; ++line ) {
			std::u32string text = U"L" + std::u32string(line < 10 ? 1 : 2, U'0' + static_cast<char32_t>(line % 10));
			text += std::u32string(line < 10 ? L'0' + static_cast<char32_t>(line) : L'0' + static_cast<char32_t>(line / 10), U'-');
			for( std::size_t column = text.size(); column < 100; ++column )
				text.push_back(U'a' + static_cast<char32_t>((line + column) % 26));
			Print(model, text + U"\r\n");
		}
	}
	std::wstring TopViewportText() const
	{
		const auto top = window.GetViewportDiagnostic().topRow;
		const auto* row = terminal::GetTerminalRow(model, top);
		return row ? RowText(*row) : std::wstring();
	}
	// A drag selection from (x1,y1) to (x2,y2) in pane client coordinates.
	void DragSelect( int x1, int y1, int x2, int y2 )
	{
		::SendMessageW(window.GetHwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x1, y1));
		::SendMessageW(window.GetHwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x2, y2));
		::SendMessageW(window.GetHwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
	}
	void ScrollUpWheels( int wheels )
	{
		for( int scroll = 0; scroll < wheels; ++scroll )
			::SendMessageW(window.GetHwnd(), WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
	}
	HWND parent{};
	terminal::TerminalModel model{ 80, 24 };
	terminal::CTerminalWnd window;
};

// A scrolled pane must keep the same text at its top row when a reflow
// renumbers the rows above the viewport (issue #445 step 2, R6).
TEST_F(TerminalReflowPane, ScrolledAnchorKeepsTheSameTextAcrossReflow)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ScrollUpWheels(6);
	const auto before = window.GetViewportDiagnostic();
	ASSERT_GT(before.scrollOffset, 0u);
	const auto topText = TopViewportText();
	ASSERT_FALSE(topText.empty());

	const auto columns = window.GetTerminalSize().columns;
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	ASSERT_NE(columns, model.Columns());
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	// The same logical line sits at the viewport top; at the new width its
	// first row holds a prefix of the old row's text.
	const auto after = TopViewportText();
	ASSERT_FALSE(after.empty());
	EXPECT_EQ(topText.substr(0, after.size()), after);
}

// A pane at the live bottom keeps the live bottom; a reflow must not
// invent a scroll offset.
TEST_F(TerminalReflowPane, LiveBottomPaneStaysAtBottomAcrossReflow)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ASSERT_EQ(0u, window.GetViewportDiagnostic().scrollOffset);
	const auto columns = window.GetTerminalSize().columns;
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	EXPECT_EQ(0u, window.GetViewportDiagnostic().scrollOffset);
	const auto diagnostic = window.GetViewportDiagnostic();
	EXPECT_EQ(diagnostic.totalRows - diagnostic.visibleRows, diagnostic.topRow);
}

// The selection is half-open over cells: after a reflow splits or merges
// rows, the same cells stay selected, so a copy returns the same text.
// SelectedText() is asserted without ever touching the OS clipboard.
TEST_F(TerminalReflowPane, SelectionTextKeepsTheSameTextAcrossReflow)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto diagnostic = window.GetViewportDiagnostic();
	const int rowHeight = 390 / static_cast<int>(diagnostic.visibleRows);
	const int cellWidth = 800 / static_cast<int>(window.GetTerminalSize().columns);
	// Drag over a wrapped line's screen rows.
	DragSelect(cellWidth, rowHeight, cellWidth * 30, rowHeight * 3);
	ASSERT_TRUE(window.HasSelection());
	const auto before = window.SelectedText();
	ASSERT_FALSE(before.empty());

	const auto columns = window.GetTerminalSize().columns;
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ASSERT_TRUE(window.HasSelection());
	EXPECT_EQ(before, window.SelectedText());
}

// D1(c): two resizes and output before one consume.  The pane must apply
// each renumbering and each append in order, not just the last layout.
TEST_F(TerminalReflowPane, OrderedResizesApplyEachRenumberingInOrder)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ScrollUpWheels(6);
	const auto before = window.GetViewportDiagnostic();
	ASSERT_GT(before.scrollOffset, 0u);
	const auto topText = TopViewportText();
	ASSERT_FALSE(topText.empty());

	const auto columns = window.GetTerminalSize().columns;
	// Two column changes plus output, then a single consume/apply.
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	window.Layout({ 0, 0, static_cast<int>(columns / 4) * 10, 400 }, 96);
	Print(model, U"post-resize output\r\n");
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto after = TopViewportText();
	ASSERT_FALSE(after.empty());
	EXPECT_EQ(topText.substr(0, after.size()), after);
}

// D7: rows and columns changing together must still anchor the scrolled
// viewport to the same top text: the old top derives from the row count
// the pane showed before this resize, not the new one.
TEST_F(TerminalReflowPane, RowsAndColumnsShrinkKeepsTheScrolledTop)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ScrollUpWheels(6);
	const auto before = window.GetViewportDiagnostic();
	ASSERT_GT(before.scrollOffset, 0u);
	const auto topText = TopViewportText();
	ASSERT_FALSE(topText.empty());

	const auto columns = window.GetTerminalSize().columns;
	const auto rows = window.GetTerminalSize().rows;
	// Narrower and shorter in one layout step: a single resize changes both.
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, static_cast<int>(rows / 2) * 15 }, 96);
	ASSERT_NE(columns, model.Columns());
	ASSERT_NE(rows, model.RowCount());
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto after = TopViewportText();
	ASSERT_FALSE(after.empty());
	EXPECT_EQ(topText.substr(0, after.size()), after);
}

// D8: a half-open selection boundary at the right edge (column == columns)
// must map to a boundary after the same cell, not clamp inside the row.
TEST_F(TerminalReflowPane, SelectionEndingAtTheRightEdgeKeepsItsLastCell)
{
	const auto columns = window.GetTerminalSize().columns;
	std::u32string line;
	for( std::size_t column = 0; column < columns; ++column )
		line.push_back(U'A' + static_cast<char32_t>(column % 26));
	Print(model, line);
	// The drag reads the pane's applied space, so consume the pending
	// resize's change first (the SetUp resize has not been consumed yet).
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto diagnostic = window.GetViewportDiagnostic();
	const int rowHeight = 390 / static_cast<int>(diagnostic.visibleRows);
	const int cellWidth = 800 / static_cast<int>(columns);
	// Drag the whole row: the endpoint lands at the right edge boundary.
	DragSelect(0, rowHeight / 2, static_cast<int>(columns) * cellWidth, rowHeight / 2);
	ASSERT_TRUE(window.HasSelection());
	const auto before = window.SelectedText();
	ASSERT_EQ(columns, before.size());

	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ASSERT_TRUE(window.HasSelection());
	const auto after = window.SelectedText();
	EXPECT_EQ(before, after);
}

// D9: a reflow under the alternate screen must not keep a stale
// main-buffer selection: it is reset, as the docs describe.
TEST_F(TerminalReflowPane, AlternateScreenReflowResetsTheSelection)
{
	FillScreen();
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	const auto diagnostic = window.GetViewportDiagnostic();
	const int rowHeight = 390 / static_cast<int>(diagnostic.visibleRows);
	const int cellWidth = 800 / static_cast<int>(window.GetTerminalSize().columns);
	DragSelect(cellWidth, rowHeight, cellWidth * 30, rowHeight * 3);
	ASSERT_TRUE(window.HasSelection());

	model.SetAlternateScreen(true);
	const auto columns = window.GetTerminalSize().columns;
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, 400 }, 96);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	EXPECT_FALSE(window.HasSelection());
	EXPECT_TRUE(window.SelectedText().empty());
	model.SetAlternateScreen(false);
}

// D12: an endpoint whose content was dropped below the cursor clamps to
// the end of the retained content; it must not jump to the history top.
TEST_F(TerminalReflowPane, DroppedEndpointClampsToTheRetainedEnd)
{
	for( std::size_t line = 0; line < 10; ++line )
		Print(model, U"L" + std::u32string(1, U'0' + static_cast<char32_t>(line)) + U"line\r\n");
	const auto diagnostic = window.GetViewportDiagnostic();
	const int rowHeight = 390 / static_cast<int>(diagnostic.visibleRows);
	const int cellWidth = 800 / static_cast<int>(window.GetTerminalSize().columns);
	// Anchor on L5 (screen row 5), drag down to a row the shrink will drop.
	DragSelect(cellWidth, rowHeight * 5 + rowHeight / 2,
		cellWidth * 4, rowHeight * 15 + rowHeight / 2);
	ASSERT_TRUE(window.HasSelection());
	const auto columns = window.GetTerminalSize().columns;
	const auto rows = window.GetTerminalSize().rows;
	// Narrower and shorter together: reflow plus a shrink that drops the
	// rows below the cursor, taking the active endpoint's row with them.
	window.Layout({ 0, 0, static_cast<int>(columns / 2) * 10, static_cast<int>(rows / 3) * 15 }, 96);
	window.ApplyScrollbackChange(model.ConsumeScrollbackChange());
	ASSERT_TRUE(window.HasSelection());
	const auto text = window.SelectedText();
	// The retained selection starts at the anchor and runs to the retained
	// end; a {0,0} endpoint would prepend the pushed history rows instead.
	EXPECT_EQ(std::wstring::npos, text.find(L"L0line"));
	EXPECT_NE(std::wstring::npos, text.find(L"L5line"));
}

} // namespace
