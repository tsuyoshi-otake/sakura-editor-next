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

//! A node in the recursive terminal split tree. A leaf has no children and
//! identifies one terminal session; an internal node lays its children out on
//! one axis. Child indices refer to the same tree vector and are intentionally
//! independent of native HWNDs or terminal-session ownership.
struct TerminalPaneLayoutTreeNode {
	std::uint64_t leafId{};
	std::vector<std::size_t> children;
	std::vector<int> weights;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
};

struct TerminalPaneTreeLayoutInput {
	RECT content{};
	unsigned int dpi{ 96 };
	std::span<const TerminalPaneLayoutTreeNode> nodes{};
	std::size_t root{};
	bool showTabs{};
	TerminalTabsLocation tabsLocation{ TerminalTabsLocation::Right };
};

struct TerminalPaneLayoutResult {
	RECT panesBounds{};
	RECT tabsBounds{};
	RECT tabsDivider{};
	std::vector<RECT> panes;
	std::vector<RECT> paneDividers;
	//! Metadata parallel to paneDividers for recursive-tree divider dragging.
	std::vector<std::size_t> paneDividerNodes;
	std::vector<std::size_t> paneDividerChildren;
	std::vector<RECT> paneDividerFirstPanes;
	std::vector<RECT> paneDividerSecondPanes;
	TerminalPaneOrientation orientation{ TerminalPaneOrientation::Horizontal };
};

[[nodiscard]] TerminalPaneLayoutResult CalculateTerminalPaneLayout( const TerminalPaneLayoutInput& input ) noexcept;
[[nodiscard]] TerminalPaneLayoutResult CalculateTerminalPaneTreeLayout(
	const TerminalPaneTreeLayoutInput& input ) noexcept;

} // namespace terminal
