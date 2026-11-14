#include "pch.h"
#include "terminal/window/TerminalGroupLayout.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

namespace {

//! Depth-first leaf order of a group's reachable tree, computed independently
//! of the model's own collection loop.
std::vector<std::uint64_t> DepthFirstLeaves( const terminal::TerminalPaneGroup& group )
{
	std::vector<std::uint64_t> leaves;
	const std::function<void(std::size_t)> walk = [&]( std::size_t nodeIndex ) {
		if( nodeIndex >= group.nodes.size() ) return;
		const auto& node = group.nodes[nodeIndex];
		if( node.children.empty() ) {
			if( node.leafId != 0 ) leaves.push_back(node.leafId);
			return;
		}
		for( const auto child : node.children ) walk(child);
	};
	walk(group.root);
	return leaves;
}

void ExpectModelInvariants( const terminal::TerminalGroupLayout& layout )
{
	std::unordered_set<std::uint64_t> seen;
	for( std::size_t index = 0; index < layout.GroupCount(); ++index ) {
		const auto* group = layout.GroupAt(index);
		ASSERT_TRUE(group != nullptr);
		// No empty group survives a mutation.
		ASSERT_FALSE(group->tabIds.empty());
		ASSERT_LT(group->root, group->nodes.size());
		const std::function<void(std::size_t)> walk = [&]( std::size_t nodeIndex ) {
			ASSERT_LT(nodeIndex, group->nodes.size());
			const auto& node = group->nodes[nodeIndex];
			if( node.children.empty() ) return;
			// A reachable split keeps at least two children, each with a
			// positive weight.
			EXPECT_GE(node.children.size(), 2u);
			EXPECT_EQ(node.children.size(), node.weights.size());
			for( const auto weight : node.weights ) EXPECT_GT(weight, 0);
			for( const auto child : node.children ) {
				EXPECT_NE(child, nodeIndex);
				walk(child);
			}
		};
		walk(group->root);
		// tabIds is the depth-first leaf order of the tree.
		EXPECT_EQ(group->tabIds, DepthFirstLeaves(*group));
		// Every tab id lives in exactly one group.
		for( const auto tabId : group->tabIds ) EXPECT_TRUE(seen.insert(tabId).second);
	}
	const auto active = layout.ActiveGroupIndex();
	if( layout.GroupCount() == 0 ) {
		EXPECT_FALSE(active.has_value());
		EXPECT_TRUE(layout.ActiveGroup() == nullptr);
	} else if( active ) {
		EXPECT_LT(*active, layout.GroupCount());
	}
	EXPECT_EQ(layout.ActiveGroup() != nullptr,
		active.has_value() && *active < layout.GroupCount());
}

TEST(TerminalGroupLayout, StartsEmpty)
{
	const terminal::TerminalGroupLayout layout;
	EXPECT_EQ(0u, layout.GroupCount());
	EXPECT_FALSE(layout.ActiveGroupIndex().has_value());
	EXPECT_TRUE(layout.ActiveGroup() == nullptr);
	EXPECT_FALSE(layout.FindGroup(1).has_value());
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, EnsureGroupForTabCreatesSingleLeafGroupAndActivates)
{
	terminal::TerminalGroupLayout layout;
	EXPECT_EQ(0u, layout.EnsureGroupForTab(1));
	ExpectModelInvariants(layout);
	ASSERT_EQ(1u, layout.GroupCount());
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	ASSERT_TRUE(layout.ActiveGroup() != nullptr);
	EXPECT_EQ(std::vector<std::uint64_t>{ 1 }, layout.ActiveGroup()->tabIds);
	EXPECT_EQ(0u, layout.FindGroup(1));
}

TEST(TerminalGroupLayout, EnsureGroupForTabReturnsExistingGroupWithoutActivating)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	ASSERT_EQ(0u, layout.ActiveGroupIndex());
	EXPECT_EQ(1u, layout.EnsureGroupForTab(2));
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, AppendGroupDoesNotChangeTheActiveGroup)
{
	terminal::TerminalGroupLayout layout;
	layout.AppendGroup(1);
	// Matches the reconcile path: the new group is appended and the active
	// selection is decided later.
	EXPECT_FALSE(layout.ActiveGroupIndex().has_value());
	EXPECT_TRUE(layout.ActiveGroup() == nullptr);
	layout.AppendGroup(2);
	layout.EnsureGroupForTab(3);
	EXPECT_EQ(2u, layout.ActiveGroupIndex());
	layout.AppendGroup(4);
	EXPECT_EQ(2u, layout.ActiveGroupIndex());
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, InsertPaneSplitInsertsBesideTargetOnSameAxis)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Horizontal));
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2, 3 }), group->tabIds);
	ASSERT_LT(group->root, group->nodes.size());
	const auto& root = group->nodes[group->root];
	// The second same-axis split joined the existing row, not a nested split.
	ASSERT_EQ(3u, root.children.size());
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal, root.orientation);
	for( const auto child : root.children ) {
		ASSERT_LT(child, group->nodes.size());
		EXPECT_TRUE(group->nodes[child].children.empty());
	}
}

TEST(TerminalGroupLayout, InsertPaneSplitWrapsTargetLeafOnDifferentAxis)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Vertical));
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2, 3 }), group->tabIds);
	const auto& root = group->nodes[group->root];
	ASSERT_EQ(2u, root.children.size());
	const auto& wrapped = group->nodes[root.children[1]];
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical, wrapped.orientation);
	ASSERT_EQ(2u, wrapped.children.size());
	EXPECT_EQ(2u, group->nodes[wrapped.children[0]].leafId);
	EXPECT_EQ(3u, group->nodes[wrapped.children[1]].leafId);
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical,
		layout.PaneOrientationForTab(0, 3));
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal,
		layout.PaneOrientationForTab(0, 1));
}

TEST(TerminalGroupLayout, InsertPaneSplitRejectsUnknownPrimary)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_FALSE(layout.InsertPaneSplit(0, 42, 2, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_FALSE(layout.InsertPaneSplit(7, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_EQ(1u, layout.GroupCount());
	EXPECT_EQ(std::vector<std::uint64_t>{ 1 }, layout.GroupAt(0)->tabIds);
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, RemoveTabCollapsesParentWithOneChild)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_TRUE(layout.RemoveTab(1));
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ(std::vector<std::uint64_t>{ 2 }, group->tabIds);
	EXPECT_TRUE(group->nodes[group->root].children.empty());
	EXPECT_EQ(2u, group->nodes[group->root].leafId);
}

TEST(TerminalGroupLayout, RemoveTabKeepsTwoOrMoreChildren)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	ASSERT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_TRUE(layout.RemoveTab(2));
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 3 }), group->tabIds);
	const auto& root = group->nodes[group->root];
	EXPECT_EQ(2u, root.children.size());
	EXPECT_EQ(2u, root.weights.size());
}

TEST(TerminalGroupLayout, RemoveTabErasesEmptyGroupAndActivatesTheGroupNowAtItsIndex)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	layout.AppendGroup(3);
	layout.SetActiveGroup(1);
	EXPECT_TRUE(layout.RemoveTab(2));
	ExpectModelInvariants(layout);
	ASSERT_EQ(2u, layout.GroupCount());
	// The group that followed the erased one keeps the active index.
	EXPECT_EQ(1u, layout.ActiveGroupIndex());
	ASSERT_TRUE(layout.ActiveGroup() != nullptr);
	EXPECT_EQ(std::vector<std::uint64_t>{ 3 }, layout.ActiveGroup()->tabIds);
	EXPECT_EQ(0u, layout.FindGroup(1));
	EXPECT_EQ(1u, layout.FindGroup(3));
	EXPECT_FALSE(layout.FindGroup(2).has_value());
}

TEST(TerminalGroupLayout, RemoveTabActivatesPreviousGroupWhenTheLastGroupIsErased)
{
	terminal::TerminalGroupLayout layout;
	layout.AppendGroup(1);
	layout.AppendGroup(2);
	layout.AppendGroup(3);
	layout.SetActiveGroup(2);
	EXPECT_TRUE(layout.RemoveTab(3));
	ExpectModelInvariants(layout);
	ASSERT_EQ(2u, layout.GroupCount());
	EXPECT_EQ(1u, layout.ActiveGroupIndex());
	ASSERT_TRUE(layout.ActiveGroup() != nullptr);
	EXPECT_EQ(std::vector<std::uint64_t>{ 2 }, layout.ActiveGroup()->tabIds);
}

TEST(TerminalGroupLayout, RemoveTabShiftsActiveGroupIndexDownAfterAnEarlierErase)
{
	terminal::TerminalGroupLayout layout;
	layout.AppendGroup(1);
	layout.AppendGroup(2);
	layout.AppendGroup(3);
	layout.SetActiveGroup(2);
	EXPECT_TRUE(layout.RemoveTab(1));
	ExpectModelInvariants(layout);
	ASSERT_EQ(2u, layout.GroupCount());
	EXPECT_EQ(1u, layout.ActiveGroupIndex());
	ASSERT_TRUE(layout.ActiveGroup() != nullptr);
	EXPECT_EQ(std::vector<std::uint64_t>{ 3 }, layout.ActiveGroup()->tabIds);
}

TEST(TerminalGroupLayout, RemoveTabResetsActiveGroupWhenTheLastGroupDisappears)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_TRUE(layout.RemoveTab(1));
	ExpectModelInvariants(layout);
	EXPECT_EQ(0u, layout.GroupCount());
	EXPECT_FALSE(layout.ActiveGroupIndex().has_value());
	EXPECT_TRUE(layout.ActiveGroup() == nullptr);
}

TEST(TerminalGroupLayout, RemoveTabReportsUnchangedForUnknownTab)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_FALSE(layout.RemoveTab(42));
	EXPECT_EQ(1u, layout.GroupCount());
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, ApplyDividerResizeSplitsTheCombinedWeightByExtent)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	const auto root = group->root;
	EXPECT_TRUE(layout.ApplyDividerResize(0, root, 0, 250, 1000));
	ExpectModelInvariants(layout);
	group = layout.GroupAt(0);
	const auto& node = group->nodes[root];
	ASSERT_EQ(2u, node.weights.size());
	// A quarter of the combined extent becomes a quarter of the combined weight.
	EXPECT_EQ(500, node.weights[0]);
	EXPECT_EQ(1500, node.weights[1]);
}

TEST(TerminalGroupLayout, ApplyDividerResizeClampsToPositiveWeights)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	const auto root = layout.GroupAt(0)->root;
	EXPECT_TRUE(layout.ApplyDividerResize(0, root, 0, 0, 1000));
	const auto& node = layout.GroupAt(0)->nodes[root];
	EXPECT_EQ(1, node.weights[0]);
	EXPECT_EQ(1999, node.weights[1]);
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, ApplyDividerResizeRejectsInvalidPositions)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	const auto root = layout.GroupAt(0)->root;
	EXPECT_FALSE(layout.ApplyDividerResize(7, root, 0, 100, 1000));
	EXPECT_FALSE(layout.ApplyDividerResize(0, root + 99, 0, 100, 1000));
	EXPECT_FALSE(layout.ApplyDividerResize(0, root, 1, 100, 1000));
	const auto& node = layout.GroupAt(0)->nodes[root];
	EXPECT_EQ((std::vector<int>{ 1000, 1000 }), node.weights);
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, NormalizeGroupRepairsMalformedWeights)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(9);
	terminal::TerminalPaneGroup malformed;
	malformed.nodes.resize(3);
	malformed.nodes[0].children = { 1, 2 };
	malformed.nodes[0].weights = { 5, -3 };
	malformed.nodes[0].orientation = terminal::TerminalPaneOrientation::Horizontal;
	malformed.nodes[1].leafId = 1;
	malformed.nodes[2].leafId = 2;
	malformed.tabIds = { 42 };
	malformed.root = 0;
	layout.ReplaceGroup(0, malformed);
	layout.NormalizeGroup(0);
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	EXPECT_EQ((std::vector<int>{ 1000, 1000 }), group->nodes[group->root].weights);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2 }), group->tabIds);
}

TEST(TerminalGroupLayout, RelativeGroupIndexWrapsBothDirections)
{
	terminal::TerminalGroupLayout layout;
	layout.AppendGroup(1);
	layout.AppendGroup(2);
	layout.AppendGroup(3);
	layout.SetActiveGroup(1);
	EXPECT_EQ(2u, layout.RelativeGroupIndex(1));
	EXPECT_EQ(0u, layout.RelativeGroupIndex(-1));
	layout.SetActiveGroup(2);
	EXPECT_EQ(0u, layout.RelativeGroupIndex(1));
	layout.SetActiveGroup(0);
	EXPECT_EQ(2u, layout.RelativeGroupIndex(-1));
}

TEST(TerminalGroupLayout, RelativeGroupIndexIsAbsentWhenEmpty)
{
	const terminal::TerminalGroupLayout layout;
	EXPECT_FALSE(layout.RelativeGroupIndex(1).has_value());
	EXPECT_FALSE(layout.RelativeGroupIndex(-1).has_value());
}

TEST(TerminalGroupLayout, NormalizeActiveGroupRepairsStaleAndMissingIndex)
{
	terminal::TerminalGroupLayout layout;
	layout.AppendGroup(1);
	layout.SetActiveGroup(std::size_t{ 5 });
	layout.NormalizeActiveGroup();
	// A stale index is dropped, then the first group becomes active.
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	layout.SetActiveGroup(std::nullopt);
	layout.NormalizeActiveGroup();
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	layout.Clear();
	layout.NormalizeActiveGroup();
	EXPECT_FALSE(layout.ActiveGroupIndex().has_value());
}

TEST(TerminalGroupLayout, PaneOrientationForTabDefaultsToHorizontal)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal,
		layout.PaneOrientationForTab(0, 1));
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal,
		layout.PaneOrientationForTab(0, 42));
	EXPECT_EQ(terminal::TerminalPaneOrientation::Horizontal,
		layout.PaneOrientationForTab(9, 1));
}

TEST(TerminalGroupLayout, DepthFirstLeafOrderFollowsTheTreeNotCreationOrder)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	// Splitting the first leaf wraps it in a new pair, so the tree reads 4
	// between the surviving 1 and 2 while creation order was 1, 2, 4.
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 4, terminal::TerminalPaneOrientation::Vertical));
	ExpectModelInvariants(layout);
	const auto* group = layout.GroupAt(0);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 4, 2 }), group->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 4, 2 }), DepthFirstLeaves(*group));
}

TEST(TerminalGroupLayout, ClearDropsEveryGroupAndTheActiveIndex)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	layout.Clear();
	ExpectModelInvariants(layout);
	EXPECT_EQ(0u, layout.GroupCount());
	EXPECT_FALSE(layout.ActiveGroupIndex().has_value());
}

} // namespace
