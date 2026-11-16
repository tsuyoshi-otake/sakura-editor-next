#include "pch.h"
#include "terminal/window/TerminalDragDrop.h"

#include <cstdint>
#include <span>

namespace {

void ExpectRect( const RECT& rect, LONG left, LONG top, LONG right, LONG bottom )
{
	EXPECT_EQ(left, rect.left);
	EXPECT_EQ(top, rect.top);
	EXPECT_EQ(right, rect.right);
	EXPECT_EQ(bottom, rect.bottom);
}

TEST(TerminalDragDrop, NearestLeftEdgeOffersTheLeftHalf)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 10, 20, 111, 81 } },
	};
	const auto target = terminal::HitTestPaneDropTarget(panes, 0, POINT{ 10, 50 });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(7u, target->targetId);
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Left, target->edge);
	// The width is 101, so the left half ends at 10 + 50.
	ExpectRect(target->overlay, 10, 20, 60, 81);
}

TEST(TerminalDragDrop, NearestRightEdgeOffersTheRightHalf)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 10, 20, 111, 81 } },
	};
	const auto target = terminal::HitTestPaneDropTarget(panes, 0, POINT{ 110, 50 });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Right, target->edge);
	// The right half starts where the left half ends: the halves partition.
	ExpectRect(target->overlay, 60, 20, 111, 81);
}

TEST(TerminalDragDrop, NearestTopEdgeOffersTheTopHalf)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 10, 20, 111, 81 } },
	};
	const auto target = terminal::HitTestPaneDropTarget(panes, 0, POINT{ 60, 20 });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Top, target->edge);
	// The height is 61, so the top half ends at 20 + 30.
	ExpectRect(target->overlay, 10, 20, 111, 50);
}

TEST(TerminalDragDrop, NearestBottomEdgeOffersTheBottomHalf)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 10, 20, 111, 81 } },
	};
	const auto target = terminal::HitTestPaneDropTarget(panes, 0, POINT{ 60, 80 });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Bottom, target->edge);
	ExpectRect(target->overlay, 10, 50, 111, 81);
}

TEST(TerminalDragDrop, NormalizedTiesPreferTheHorizontalEdge)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 0, 0, 200, 100 } },
	};
	// 20/200 == 10/100 on the left, 180/200 == 10/100 on the right: the
	// vertical candidates tie but lose to the horizontal axis.
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Left,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 20, 10 })->edge);
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Right,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 180, 10 })->edge);
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Right,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 180, 90 })->edge);
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Left,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 20, 90 })->edge);
}

TEST(TerminalDragDrop, CenterTiePrefersLeftThenTopOrder)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 7, RECT{ 0, 0, 100, 100 } },
	};
	// Every distance is 0.5 at the center; Left wins the four-way tie.
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Left,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 50, 50 })->edge);
}

TEST(TerminalDragDrop, SourcePaneIsNotATarget)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 1, RECT{ 0, 0, 50, 100 } },
		{ 2, RECT{ 50, 0, 100, 100 } },
	};
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 1, POINT{ 25, 50 }).has_value());
	// Another pane under the same source still is a target.
	const auto target = terminal::HitTestPaneDropTarget(panes, 1, POINT{ 75, 50 });
	ASSERT_TRUE(target.has_value());
	EXPECT_EQ(2u, target->targetId);
}

TEST(TerminalDragDrop, PointOutsideEveryPaneHasNoTarget)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 1, RECT{ 0, 0, 50, 100 } },
	};
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 60, 50 }).has_value());
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(std::span<const terminal::TerminalPaneBounds>{},
		0, POINT{ 0, 0 }).has_value());
}

TEST(TerminalDragDrop, EmptyPaneRectIsNotATarget)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 1, RECT{ 10, 10, 10, 60 } },  // zero width
		{ 2, RECT{ 10, 10, 50, 10 } },  // zero height
		{ 3, RECT{ 60, 60, 10, 10 } },  // inverted
	};
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 10, 30 }).has_value());
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 30, 10 }).has_value());
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 30, 30 }).has_value());
}

TEST(TerminalDragDrop, RectEdgesFollowPtInRect)
{
	const terminal::TerminalPaneBounds panes[] = {
		{ 1, RECT{ 0, 0, 100, 100 } },
	};
	// Left and top are inclusive; the (0, 0) corner ties Left over Top.
	EXPECT_EQ(terminal::TerminalPaneDropEdge::Left,
		terminal::HitTestPaneDropTarget(panes, 0, POINT{ 0, 0 })->edge);
	// Right and bottom are exclusive.
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 100, 50 }).has_value());
	EXPECT_FALSE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 50, 100 }).has_value());
	EXPECT_TRUE(terminal::HitTestPaneDropTarget(panes, 0, POINT{ 99, 99 }).has_value());
}

TEST(TerminalDragDrop, OrientationMapsEdgesToSplitAxes)
{
	terminal::TerminalPaneDropTarget target;
	target.edge = terminal::TerminalPaneDropEdge::Left;
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal, target.Orientation());
	target.edge = terminal::TerminalPaneDropEdge::Right;
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal, target.Orientation());
	target.edge = terminal::TerminalPaneDropEdge::Top;
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, target.Orientation());
	target.edge = terminal::TerminalPaneDropEdge::Bottom;
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, target.Orientation());
}

TEST(TerminalDragDrop, SideMapsEdgesToBeforeOrAfter)
{
	terminal::TerminalPaneDropTarget target;
	target.edge = terminal::TerminalPaneDropEdge::Left;
	EXPECT_EQ(terminal::TerminalDropSide::Before, target.Side());
	target.edge = terminal::TerminalPaneDropEdge::Top;
	EXPECT_EQ(terminal::TerminalDropSide::Before, target.Side());
	target.edge = terminal::TerminalPaneDropEdge::Right;
	EXPECT_EQ(terminal::TerminalDropSide::After, target.Side());
	target.edge = terminal::TerminalPaneDropEdge::Bottom;
	EXPECT_EQ(terminal::TerminalDropSide::After, target.Side());
}

TEST(TerminalDragGesture, StartsIdle)
{
	const terminal::TerminalDragGesture gesture;
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
}

TEST(TerminalDragGesture, ListRowClickReleasesAsAClick)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	ASSERT_EQ(terminal::TerminalDragPhase::Armed, gesture.Phase());
	EXPECT_EQ(1u, gesture.SourceId());
	EXPECT_EQ(terminal::TerminalDragSourceKind::ListRow, gesture.SourceKind());
	// A move inside the threshold is not a drag.
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.Move(POINT{ 102, 101 }));
	EXPECT_FALSE(gesture.Release());
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
}

TEST(TerminalDragGesture, ListRowPastTheHorizontalThresholdStartsDragging)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	EXPECT_EQ(terminal::TerminalDragTransition::StartedDragging,
		gesture.Move(POINT{ 105, 100 }));
	EXPECT_EQ(terminal::TerminalDragPhase::Dragging, gesture.Phase());
	EXPECT_EQ(1u, gesture.SourceId());
	// Further moves keep dragging without another transition.
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.Move(POINT{ 130, 140 }));
	EXPECT_EQ(terminal::TerminalDragPhase::Dragging, gesture.Phase());
}

TEST(TerminalDragGesture, ListRowPastTheVerticalThresholdStartsDragging)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	EXPECT_EQ(terminal::TerminalDragTransition::StartedDragging,
		gesture.Move(POINT{ 100, 105 }));
	EXPECT_EQ(terminal::TerminalDragPhase::Dragging, gesture.Phase());
}

TEST(TerminalDragGesture, ListRowExactlyAtTheThresholdStaysArmed)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.Move(POINT{ 104, 104 }));
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.Move(POINT{ 96, 96 }));
	EXPECT_EQ(terminal::TerminalDragPhase::Armed, gesture.Phase());
}

TEST(TerminalDragGesture, PaneBodyLongPressStartsDragging)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::PaneBody, 2, POINT{ 50, 50 }, SIZE{ 4, 4 });
	// Small moves inside the threshold do not disarm the pending grab.
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.Move(POINT{ 52, 51 }));
	EXPECT_EQ(terminal::TerminalDragTransition::StartedDragging, gesture.LongPressElapsed());
	EXPECT_EQ(terminal::TerminalDragPhase::Dragging, gesture.Phase());
	EXPECT_EQ(2u, gesture.SourceId());
	EXPECT_TRUE(gesture.Release());
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
}

TEST(TerminalDragGesture, PaneBodyMovingOutFirstDisarms)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::PaneBody, 2, POINT{ 50, 50 }, SIZE{ 4, 4 });
	EXPECT_EQ(terminal::TerminalDragTransition::Disarmed, gesture.Move(POINT{ 60, 50 }));
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
	// A late long press has nothing left to grab.
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.LongPressElapsed());
}

TEST(TerminalDragGesture, LongPressElapsedOnlyAppliesToAnArmedPaneBody)
{
	terminal::TerminalDragGesture gesture;
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.LongPressElapsed());
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.LongPressElapsed());
	EXPECT_EQ(terminal::TerminalDragPhase::Armed, gesture.Phase());
	ASSERT_EQ(terminal::TerminalDragTransition::StartedDragging,
		gesture.Move(POINT{ 110, 100 }));
	EXPECT_EQ(terminal::TerminalDragTransition::None, gesture.LongPressElapsed());
	EXPECT_EQ(terminal::TerminalDragPhase::Dragging, gesture.Phase());
}

TEST(TerminalDragGesture, ReleaseWhileDraggingAppliesTheDrop)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	ASSERT_EQ(terminal::TerminalDragTransition::StartedDragging,
		gesture.Move(POINT{ 110, 100 }));
	EXPECT_TRUE(gesture.Release());
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
	EXPECT_FALSE(gesture.Release());
}

TEST(TerminalDragGesture, CancelEndsTheGestureFromAnyPhase)
{
	terminal::TerminalDragGesture gesture;
	EXPECT_FALSE(gesture.Cancel());
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 100, 100 }, SIZE{ 4, 4 });
	EXPECT_TRUE(gesture.Cancel());
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
	gesture.Press(terminal::TerminalDragSourceKind::PaneBody, 2, POINT{ 50, 50 }, SIZE{ 4, 4 });
	ASSERT_EQ(terminal::TerminalDragTransition::StartedDragging, gesture.LongPressElapsed());
	EXPECT_TRUE(gesture.Cancel());
	EXPECT_EQ(terminal::TerminalDragPhase::Idle, gesture.Phase());
	EXPECT_EQ(0u, gesture.SourceId());
}

TEST(TerminalDragGesture, PressWhileDraggingReplacesTheGesture)
{
	terminal::TerminalDragGesture gesture;
	gesture.Press(terminal::TerminalDragSourceKind::ListRow, 1, POINT{ 0, 0 }, SIZE{ 4, 4 });
	ASSERT_EQ(terminal::TerminalDragTransition::StartedDragging,
		gesture.Move(POINT{ 9, 0 }));
	gesture.Press(terminal::TerminalDragSourceKind::PaneBody, 9, POINT{ 7, 8 }, SIZE{ 8, 8 });
	EXPECT_EQ(terminal::TerminalDragPhase::Armed, gesture.Phase());
	EXPECT_EQ(9u, gesture.SourceId());
	EXPECT_EQ(terminal::TerminalDragSourceKind::PaneBody, gesture.SourceKind());
	EXPECT_EQ(7, gesture.PressPoint().x);
	EXPECT_EQ(8, gesture.PressPoint().y);
}

} // namespace
