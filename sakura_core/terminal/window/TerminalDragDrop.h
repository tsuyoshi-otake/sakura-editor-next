/*! @file
 * @brief Pure drop-zone hit test and drag gesture state machine of the terminal panel.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/runtime/TerminalRuntimeTypes.h"
#include "terminal/window/TerminalGroupLayout.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <span>

namespace terminal {

//! Long-press delay that turns a held press on a pane body into a pane grab.
inline constexpr unsigned int kTerminalPaneLongPressMilliseconds = 500;
//! Dwell time over a terminal list row during a drag before that terminal is activated (upstream 500 ms).
inline constexpr unsigned int kTerminalDropHoverActivateMilliseconds = 500;

//! The edge of a pane a drop is offered on. Four zones are a fork extension: upstream offers two
//! halves on the Panel's one axis, while the fork's groups nest both axes.
enum class TerminalPaneDropEdge : std::uint8_t { Left, Right, Top, Bottom };

//! One pane's identity and hit-test rectangle in client coordinates.
struct TerminalPaneBounds final {
	std::uint64_t tabId{};
	RECT bounds{};
};

//! The pane under a drag point: its tab id, the nearest edge, and the half
//! of the pane that lights up as the drop overlay.
struct TerminalPaneDropTarget final {
	std::uint64_t targetId{};
	TerminalPaneDropEdge edge{ TerminalPaneDropEdge::Left };
	RECT overlay{};   // the half of the target pane on `edge`'s side (left/right/top/bottom half)
	[[nodiscard]] TerminalPaneOrientation Orientation() const noexcept; // Left/Right -> Horizontal, Top/Bottom -> Vertical
	[[nodiscard]] TerminalDropSide Side() const noexcept;               // Left/Top -> Before, Right/Bottom -> After
};

//! The pane under `point` (client coordinates) and its nearest edge. Absent when the point is in no
//! pane, in an empty pane rect, or in the source's own pane. Nearest edge = the smallest of the
//! normalized distances x/width, 1-x/width, y/height, 1-y/height; ties prefer Left/Right (upstream's
//! horizontal Panel axis), then Left over Right and Top over Bottom.
[[nodiscard]] std::optional<TerminalPaneDropTarget> HitTestPaneDropTarget(
	std::span<const TerminalPaneBounds> panes, std::uint64_t sourceId, POINT point ) noexcept;

//! Where a drag started: a terminal list row, or a pane body grabbed through
//! the long-press fork extension (upstream drags only the row and the header label).
enum class TerminalDragSourceKind : std::uint8_t { ListRow, PaneBody };
enum class TerminalDragPhase : std::uint8_t { Idle, Armed, Dragging };
enum class TerminalDragTransition : std::uint8_t { None, StartedDragging, Disarmed };

//! One drag gesture: Idle -> Armed (press) -> Dragging. A ListRow press starts dragging when the
//! pointer leaves the threshold rectangle (|dx| > threshold.cx or |dy| > threshold.cy, the system
//! SM_CXDRAG/SM_CYDRAG semantics). A PaneBody press starts dragging only through LongPressElapsed
//! while still inside the threshold; moving out of it first disarms (Idle, the caller keeps its
//! normal selection gesture). Every exit returns to Idle.
class TerminalDragGesture final {
public:
	void Press( TerminalDragSourceKind kind, std::uint64_t sourceId, POINT point, SIZE threshold ) noexcept; // replaces any gesture
	[[nodiscard]] TerminalDragTransition Move( POINT point ) noexcept;
	[[nodiscard]] TerminalDragTransition LongPressElapsed() noexcept; // PaneBody Armed -> Dragging, else None
	//! Ends the gesture; true when it was Dragging (the caller applies the drop), false for a click.
	[[nodiscard]] bool Release() noexcept;
	//! Ends the gesture from any phase; true when it was not Idle.
	bool Cancel() noexcept;
	[[nodiscard]] TerminalDragPhase Phase() const noexcept;
	[[nodiscard]] TerminalDragSourceKind SourceKind() const noexcept;
	[[nodiscard]] std::uint64_t SourceId() const noexcept;   // 0 when Idle
	[[nodiscard]] POINT PressPoint() const noexcept;

private:
	void Reset() noexcept;

	TerminalDragSourceKind m_kind{ TerminalDragSourceKind::ListRow };
	std::uint64_t m_sourceId{};
	POINT m_pressPoint{};
	SIZE m_threshold{};
	TerminalDragPhase m_phase{ TerminalDragPhase::Idle };
};

} // namespace terminal
