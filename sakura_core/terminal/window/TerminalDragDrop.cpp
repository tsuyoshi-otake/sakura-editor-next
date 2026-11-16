/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/window/TerminalDragDrop.h"

#include <cstdlib>

namespace terminal {

TerminalPaneOrientation TerminalPaneDropTarget::Orientation() const noexcept
{
	switch( edge ) {
	case TerminalPaneDropEdge::Left:
	case TerminalPaneDropEdge::Right:
		return TerminalPaneOrientation::Horizontal;
	default:
		return TerminalPaneOrientation::Vertical;
	}
}

TerminalDropSide TerminalPaneDropTarget::Side() const noexcept
{
	switch( edge ) {
	case TerminalPaneDropEdge::Left:
	case TerminalPaneDropEdge::Top:
		return TerminalDropSide::Before;
	default:
		return TerminalDropSide::After;
	}
}

std::optional<TerminalPaneDropTarget> HitTestPaneDropTarget(
	std::span<const TerminalPaneBounds> panes, std::uint64_t sourceId, POINT point ) noexcept
{
	for( const auto& pane : panes ) {
		if( pane.tabId == sourceId ) continue;
		const auto& bounds = pane.bounds;
		const auto width = bounds.right - bounds.left;
		const auto height = bounds.bottom - bounds.top;
		if( width <= 0 || height <= 0 ) continue;
		if( !PtInRect(&bounds, point) ) continue;

		// Distances to the Left, Right, Top and Bottom edges in that order:
		// the first minimum wins, which is what prefers the horizontal axis,
		// Left over Right and Top over Bottom on a tie.
		const double distances[] = {
			static_cast<double>(point.x - bounds.left) / width,
			static_cast<double>(bounds.right - point.x) / width,
			static_cast<double>(point.y - bounds.top) / height,
			static_cast<double>(bounds.bottom - point.y) / height,
		};
		const TerminalPaneDropEdge edges[] = {
			TerminalPaneDropEdge::Left,
			TerminalPaneDropEdge::Right,
			TerminalPaneDropEdge::Top,
			TerminalPaneDropEdge::Bottom,
		};
		std::size_t nearest = 0;
		for( std::size_t index = 1; index < 4; ++index ) {
			if( distances[index] < distances[nearest] ) nearest = index;
		}

		TerminalPaneDropTarget target;
		target.targetId = pane.tabId;
		target.edge = edges[nearest];
		switch( target.edge ) {
		case TerminalPaneDropEdge::Left:
			target.overlay = { bounds.left, bounds.top, bounds.left + width / 2, bounds.bottom };
			break;
		case TerminalPaneDropEdge::Right:
			target.overlay = { bounds.left + width / 2, bounds.top, bounds.right, bounds.bottom };
			break;
		case TerminalPaneDropEdge::Top:
			target.overlay = { bounds.left, bounds.top, bounds.right, bounds.top + height / 2 };
			break;
		default:
			target.overlay = { bounds.left, bounds.top + height / 2, bounds.right, bounds.bottom };
			break;
		}
		return target;
	}
	return std::nullopt;
}

void TerminalDragGesture::Press( TerminalDragSourceKind kind, std::uint64_t sourceId,
	POINT point, SIZE threshold ) noexcept
{
	m_kind = kind;
	m_sourceId = sourceId;
	m_pressPoint = point;
	m_threshold = threshold;
	m_phase = TerminalDragPhase::Armed;
}

TerminalDragTransition TerminalDragGesture::Move( POINT point ) noexcept
{
	if( m_phase != TerminalDragPhase::Armed ) return TerminalDragTransition::None;
	const auto dx = std::abs(static_cast<long long>(point.x) - m_pressPoint.x);
	const auto dy = std::abs(static_cast<long long>(point.y) - m_pressPoint.y);
	if( dx <= m_threshold.cx && dy <= m_threshold.cy ) return TerminalDragTransition::None;
	if( m_kind == TerminalDragSourceKind::ListRow ) {
		m_phase = TerminalDragPhase::Dragging;
		return TerminalDragTransition::StartedDragging;
	}
	// A pane-body press that moves before the long press keeps being the
	// caller's selection/mouse-reporting gesture.
	Reset();
	return TerminalDragTransition::Disarmed;
}

TerminalDragTransition TerminalDragGesture::LongPressElapsed() noexcept
{
	if( m_phase != TerminalDragPhase::Armed || m_kind != TerminalDragSourceKind::PaneBody ) {
		return TerminalDragTransition::None;
	}
	m_phase = TerminalDragPhase::Dragging;
	return TerminalDragTransition::StartedDragging;
}

bool TerminalDragGesture::Release() noexcept
{
	const auto wasDragging = m_phase == TerminalDragPhase::Dragging;
	Reset();
	return wasDragging;
}

bool TerminalDragGesture::Cancel() noexcept
{
	const auto wasActive = m_phase != TerminalDragPhase::Idle;
	Reset();
	return wasActive;
}

TerminalDragPhase TerminalDragGesture::Phase() const noexcept
{
	return m_phase;
}

TerminalDragSourceKind TerminalDragGesture::SourceKind() const noexcept
{
	return m_kind;
}

std::uint64_t TerminalDragGesture::SourceId() const noexcept
{
	return m_sourceId;
}

POINT TerminalDragGesture::PressPoint() const noexcept
{
	return m_pressPoint;
}

void TerminalDragGesture::Reset() noexcept
{
	m_kind = TerminalDragSourceKind::ListRow;
	m_sourceId = 0;
	m_pressPoint = POINT{};
	m_threshold = SIZE{};
	m_phase = TerminalDragPhase::Idle;
}

} // namespace terminal
