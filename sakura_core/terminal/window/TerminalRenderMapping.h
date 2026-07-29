/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/model/TerminalModel.h"

#include <cstddef>
#include <string>
#include <vector>

namespace terminal {

struct TerminalViewport {
	std::size_t totalRows{};
	std::size_t visibleRows{};
	std::size_t topRow{};
};

struct TerminalSelectionPoint {
	std::size_t row{};
	std::size_t column{};
	friend constexpr bool operator==( const TerminalSelectionPoint&, const TerminalSelectionPoint& ) noexcept = default;
};

[[nodiscard]] TerminalViewport CalculateTerminalViewport( const TerminalModel& model, std::size_t visibleRows, std::size_t scrollOffset ) noexcept;
[[nodiscard]] const TerminalRow* GetTerminalRow( const TerminalModel& model, std::size_t globalRow ) noexcept;
[[nodiscard]] TerminalSelectionPoint TerminalCellFromPoint( const TerminalViewport& viewport, int x, int y, int cellWidth, int cellHeight, std::size_t columns ) noexcept;
[[nodiscard]] std::vector<std::size_t> MapDirtyRowsToViewport( const TerminalModel& model, const TerminalViewport& viewport, const std::vector<std::size_t>& dirtyScreenRows );
[[nodiscard]] std::wstring ExtractTerminalSelection( const TerminalModel& model, TerminalSelectionPoint anchor, TerminalSelectionPoint active );

} // namespace terminal
