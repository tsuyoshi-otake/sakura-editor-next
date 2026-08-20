/*! @file */
#pragma once

#include "TerminalTabPresentation.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace terminal {

//! Flat terminal-group orientation. VS Code's bottom Panel defaults to
//! horizontal (side-by-side) panes; Sakura also allows vertical (stacked) panes
//! as an intentional divergence from upstream's single-axis Panel restriction.
enum class TerminalPaneOrientation : std::uint8_t {
	Horizontal,
	Vertical,
};

struct TerminalPaneLayoutInput {
	RECT content{};
	unsigned int dpi{ 96 };
	std::size_t paneCount{};
	std::span<const int> paneWeights{};
	bool showTabs{};
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
	//! The terminal list location is a presentation policy; this layout remains
	//! the sole authority for the resulting pane/list rectangles.
	TerminalTabsLocation tabsLocation{ TerminalTabsLocation::Right };
};

struct TerminalPaneLayoutResult {
	RECT panesBounds{};
	RECT tabsBounds{};
	RECT tabsDivider{};
	std::vector<RECT> panes;
	std::vector<RECT> paneDividers;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
};

[[nodiscard]] TerminalPaneLayoutResult CalculateTerminalPaneLayout( const TerminalPaneLayoutInput& input ) noexcept;

} // namespace terminal
