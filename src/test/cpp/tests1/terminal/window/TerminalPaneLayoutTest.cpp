#include "pch.h"
#include "terminal/window/TerminalPaneLayout.h"

#include <array>

namespace {

TEST(TerminalPaneLayout, SupportsThreeOrMorePanesWithWeightedAllocation)
{
	const std::array weights{ 1, 2, 1, 2 };
	const auto result = terminal::CalculateTerminalPaneLayout({ { 0, 0, 1000, 400 }, 96, 4, weights, false });
	ASSERT_EQ(4u, result.panes.size());
	ASSERT_EQ(3u, result.paneDividers.size());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal, result.orientation);
	EXPECT_EQ(1000, result.panesBounds.right);
	EXPECT_EQ(0, result.panes.front().left);
	EXPECT_EQ(1000, result.panes.back().right);
	for (std::size_t i = 1; i < result.panes.size(); ++i) EXPECT_LE(result.panes[i - 1].right, result.panes[i].left);
	for (const auto& pane : result.panes) EXPECT_GE(pane.right - pane.left, 80);
	EXPECT_GT(result.panes[1].right - result.panes[1].left, result.panes[0].right - result.panes[0].left);
	EXPECT_GT(result.panes[3].right - result.panes[3].left, result.panes[2].right - result.panes[2].left);
}

TEST(TerminalPaneLayout, StacksPanesVerticallyWithoutPaneCountCap)
{
	const std::array weights{ 1, 1, 1, 1, 1 };
	const auto result = terminal::CalculateTerminalPaneLayout({
		{ 0, 0, 500, 500 }, 96, 5, weights, false, terminal::TerminalPaneOrientation::Vertical });
	ASSERT_EQ(5u, result.panes.size());
	ASSERT_EQ(4u, result.paneDividers.size());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, result.orientation);
	EXPECT_EQ(0, result.panes.front().top);
	EXPECT_EQ(500, result.panes.back().bottom);
	for (std::size_t i = 1; i < result.panes.size(); ++i) {
		EXPECT_LE(result.panes[i - 1].bottom, result.panes[i].top);
		EXPECT_EQ(0, result.panes[i].left);
		EXPECT_EQ(500, result.panes[i].right);
	}
	for (const auto& pane : result.panes) EXPECT_GE(pane.bottom - pane.top, 80);
	for (const auto& divider : result.paneDividers) {
		EXPECT_EQ(0, divider.left);
		EXPECT_EQ(500, divider.right);
		EXPECT_EQ(4, divider.bottom - divider.top);
	}
}

TEST(TerminalPaneLayout, PlacesTerminalListOnTheRightForVerticalPanes)
{
	const auto result = terminal::CalculateTerminalPaneLayout({
		{ 10, 20, 1010, 420 }, 96, 2, {}, true, terminal::TerminalPaneOrientation::Vertical });
	EXPECT_EQ(10, result.panesBounds.left);
	EXPECT_EQ(889, result.panesBounds.right);
	EXPECT_EQ(890, result.tabsBounds.left);
	EXPECT_EQ(1010, result.tabsBounds.right);
	ASSERT_EQ(2u, result.panes.size());
	EXPECT_EQ(20, result.panes[0].top);
	EXPECT_EQ(result.panesBounds.right, result.panes[0].right);
	EXPECT_LE(result.panes[0].bottom, result.panes[1].top);
}

TEST(TerminalPaneLayout, PlacesTerminalListOnTheRight)
{
	const auto result = terminal::CalculateTerminalPaneLayout({ { 10, 20, 1010, 420 }, 96, 2, {}, true });
	EXPECT_EQ(10, result.panesBounds.left);
	EXPECT_EQ(889, result.panesBounds.right);
	EXPECT_EQ(889, result.tabsDivider.left);
	EXPECT_EQ(890, result.tabsDivider.right);
	EXPECT_EQ(890, result.tabsBounds.left);
	EXPECT_EQ(1010, result.tabsBounds.right);
}

TEST(TerminalPaneLayout, PlacesTerminalListOnTheLeftWithoutOverlap)
{
	const auto result = terminal::CalculateTerminalPaneLayout({
		{ 10, 20, 1010, 420 }, 96, 2, {}, true,
		terminal::TerminalPaneOrientation::Horizontal, terminal::TerminalTabsLocation::Left });
	EXPECT_EQ(10, result.tabsBounds.left);
	EXPECT_EQ(130, result.tabsBounds.right);
	EXPECT_EQ(130, result.tabsDivider.left);
	EXPECT_EQ(131, result.tabsDivider.right);
	EXPECT_EQ(131, result.panesBounds.left);
	EXPECT_EQ(1010, result.panesBounds.right);
	ASSERT_EQ(2u, result.panes.size());
	for (const auto& pane : result.panes) {
		EXPECT_GE(pane.left, result.panesBounds.left);
		EXPECT_LE(pane.right, result.panesBounds.right);
	}
}

TEST(TerminalPaneLayout, ScalesPolicyDimensionsByDpi)
{
	const auto result = terminal::CalculateTerminalPaneLayout({ { 0, 0, 1200, 300 }, 192, 1, {}, true });
	EXPECT_EQ(958, result.panesBounds.right);
	EXPECT_EQ(958, result.tabsDivider.left);
	EXPECT_EQ(960, result.tabsDivider.right);
}

TEST(TerminalPaneLayout, OmitsListAndKeepsRectsValidWhenNarrow)
{
	const auto result = terminal::CalculateTerminalPaneLayout({ { 0, 0, 100, 50 }, 96, 8, {}, true });
	EXPECT_EQ(0, result.tabsBounds.left);
	EXPECT_EQ(0, result.tabsBounds.right);
	ASSERT_EQ(8u, result.panes.size());
	for (const auto& pane : result.panes) EXPECT_LE(pane.left, pane.right);
	for (const auto& divider : result.paneDividers) EXPECT_LE(divider.left, divider.right);
}

TEST(TerminalPaneLayout, FallsBackToUniformForMissingOrMalformedWeights)
{
	const std::array weights{ 10, -1 };
	const auto result = terminal::CalculateTerminalPaneLayout({ { 0, 0, 400, 100 }, 96, 2, weights, false });
	ASSERT_EQ(2u, result.panes.size());
	EXPECT_EQ(198, result.panes[0].right);
	EXPECT_EQ(400, result.panes[1].right);
}

} // namespace
