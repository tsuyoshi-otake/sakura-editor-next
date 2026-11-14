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

//! A hand-built two-leaf split group (leaf 1 beside leaf 2) used by the
//! InstallGroup cases below.
terminal::TerminalPaneGroup SplitPairGroup( std::uint64_t first, std::uint64_t second,
	terminal::TerminalPaneOrientation orientation = terminal::TerminalPaneOrientation::Horizontal,
	std::vector<int> weights = { 600, 400 } )
{
	terminal::TerminalPaneGroup group;
	group.nodes.resize(3);
	group.nodes[0].leafId = first;
	group.nodes[1].leafId = second;
	group.nodes[2].children = { 0, 1 };
	group.nodes[2].weights = std::move(weights);
	group.nodes[2].orientation = orientation;
	group.root = 2;
	group.tabIds = { first, second };
	return group;
}

TEST(TerminalGroupLayout, InstallGroupAppendsAValidTreeWithoutActivatingIt)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	const auto installed = layout.InstallGroup(SplitPairGroup(2, 3,
		terminal::TerminalPaneOrientation::Vertical));
	ASSERT_TRUE(installed.has_value());
	EXPECT_EQ(1u, *installed);
	ExpectModelInvariants(layout);
	// Install never steals the active selection.
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	const auto* group = layout.GroupAt(1);
	ASSERT_TRUE(group != nullptr);
	EXPECT_EQ((std::vector<std::uint64_t>{ 2, 3 }), group->tabIds);
	EXPECT_EQ(terminal::TerminalPaneOrientation::Vertical,
		group->nodes[group->root].orientation);
	EXPECT_EQ((std::vector<int>{ 600, 400 }), group->nodes[group->root].weights);
	EXPECT_EQ(1u, layout.FindGroup(2));
	EXPECT_EQ(1u, layout.FindGroup(3));
	// Groups() exposes the ordered projection for the layout snapshot.
	EXPECT_EQ(2u, layout.Groups().size());
}

TEST(TerminalGroupLayout, InstallGroupRejectsMalformedTreesWithoutMutating)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	const auto baseline = *layout.GroupAt(0);

	// An empty group, a root out of range, a child index out of range.
	{
		auto attempt = layout;
		EXPECT_FALSE(attempt.InstallGroup(terminal::TerminalPaneGroup{}));
		auto outOfRange = SplitPairGroup(2, 3);
		outOfRange.root = 7;
		EXPECT_FALSE(attempt.InstallGroup(std::move(outOfRange)));
		auto badChild = SplitPairGroup(2, 3);
		badChild.nodes[2].children = { 0, 9 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(badChild)));
		EXPECT_EQ(1u, attempt.GroupCount());
		EXPECT_EQ(baseline.tabIds, attempt.GroupAt(0)->tabIds);
	}

	// A cycle (the root splits into itself), a split with fewer than two
	// children, a non-positive weight, and a weight list that does not match
	// the children.
	{
		auto attempt = layout;
		auto cyclic = SplitPairGroup(2, 3);
		cyclic.nodes[2].children = { 2, 1 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(cyclic)));
		auto tooSmall = SplitPairGroup(2, 3);
		tooSmall.nodes[2].children = { 0 };
		tooSmall.nodes[2].weights = { 600 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(tooSmall)));
		auto deadWeight = SplitPairGroup(2, 3);
		deadWeight.nodes[2].weights = { 600, 0 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(deadWeight)));
		auto shortWeights = SplitPairGroup(2, 3);
		shortWeights.nodes[2].weights = { 600 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(shortWeights)));
		EXPECT_EQ(1u, attempt.GroupCount());
		EXPECT_EQ(baseline.tabIds, attempt.GroupAt(0)->tabIds);
	}

	// A leaf used twice, a tab already grouped elsewhere, and a tabIds list
	// that disagrees with the depth-first leaf order.
	{
		auto attempt = layout;
		auto reusedLeaf = SplitPairGroup(2, 3);
		reusedLeaf.nodes[2].children = { 0, 0 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(reusedLeaf)));
		EXPECT_FALSE(attempt.InstallGroup(SplitPairGroup(1, 4)));
		auto wrongOrder = SplitPairGroup(2, 3);
		wrongOrder.tabIds = { 3, 2 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(wrongOrder)));
		auto extraTab = SplitPairGroup(2, 3);
		extraTab.tabIds = { 2, 3, 4 };
		EXPECT_FALSE(attempt.InstallGroup(std::move(extraTab)));
		// Nothing installed, nothing changed.
		EXPECT_EQ(1u, attempt.GroupCount());
		EXPECT_EQ(baseline.tabIds, attempt.GroupAt(0)->tabIds);
		EXPECT_EQ(0u, attempt.ActiveGroupIndex());
	}
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, InstallGroupIgnoresUnreachableDeadNodes)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	auto group = SplitPairGroup(2, 3);
	// Dead data left over from a collapse is carried along, never validated.
	terminal::TerminalPaneLayoutTreeNode dead;
	dead.leafId = 42;
	group.nodes.insert(group.nodes.begin(), dead);
	// Shift every index by one for the inserted node.
	group.nodes[3].children = { 1, 2 };
	group.root = 3;
	EXPECT_TRUE(layout.InstallGroup(std::move(group)).has_value());
	ExpectModelInvariants(layout);
	EXPECT_EQ((std::vector<std::uint64_t>{ 2, 3 }),
		layout.GroupAt(1)->tabIds);
}

TEST(TerminalGroupLayout, InstancesInOrderIsEmptyWithoutGroups)
{
	const terminal::TerminalGroupLayout layout;
	EXPECT_TRUE(layout.InstancesInOrder().empty());
}

TEST(TerminalGroupLayout, InstancesInOrderListsEachGroupInOrder)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 2 }), layout.InstancesInOrder());
	ExpectModelInvariants(layout);
}

TEST(TerminalGroupLayout, InstancesInOrderPutsSplitLeavesAfterTheirSibling)
{
	// VS Code's tabs list shows a split group's terminals beside it: T1 in its
	// own group and T2 in another, then splitting T1 for T3 reads T1, T3, T2,
	// not the creation order T1, T2, T3.
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 3, terminal::TerminalPaneOrientation::Horizontal));
	ExpectModelInvariants(layout);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 3, 2 }), layout.InstancesInOrder());
}

TEST(TerminalGroupLayout, InstancesInOrderFollowsNestedLeafOrder)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	// Splitting leaf 1 orthogonally wraps it, so the tree reads 1, 4, 2.
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 4, terminal::TerminalPaneOrientation::Vertical));
	layout.AppendGroup(3);
	ExpectModelInvariants(layout);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 4, 2, 3 }), layout.InstancesInOrder());
}

TEST(TerminalGroupLayout, SplitPositionOfNamesTheLeafPositions)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	// A single-terminal group has no split position, and an unknown or
	// ungrouped tab has none either.
	EXPECT_EQ(terminal::TerminalSplitPosition::None, layout.SplitPositionOf(1));
	EXPECT_EQ(terminal::TerminalSplitPosition::None, layout.SplitPositionOf(42));
	layout.AppendGroup(9);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	ASSERT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Horizontal));
	ExpectModelInvariants(layout);
	EXPECT_EQ(terminal::TerminalSplitPosition::First, layout.SplitPositionOf(1));
	EXPECT_EQ(terminal::TerminalSplitPosition::Middle, layout.SplitPositionOf(2));
	EXPECT_EQ(terminal::TerminalSplitPosition::Last, layout.SplitPositionOf(3));
	EXPECT_EQ(terminal::TerminalSplitPosition::None, layout.SplitPositionOf(9));
}

TEST(TerminalGroupLayout, SplitPositionPrefixesMatchTheTreeGlyphs)
{
	EXPECT_STREQ(L"", terminal::TerminalSplitPrefix(terminal::TerminalSplitPosition::None));
	EXPECT_STREQ(L"\u250C ", terminal::TerminalSplitPrefix(terminal::TerminalSplitPosition::First));
	EXPECT_STREQ(L"\u251C ", terminal::TerminalSplitPrefix(terminal::TerminalSplitPosition::Middle));
	EXPECT_STREQ(L"\u2514 ", terminal::TerminalSplitPrefix(terminal::TerminalSplitPosition::Last));
}

//! VS Code unsplit appends the removed leaf and leaves its original group active.
TEST(TerminalGroupLayout, UnsplitAppendsAndPreservesTheOldGroup)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	ASSERT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Unsplit(2));
	ExpectModelInvariants(layout);
	ASSERT_EQ(2u, layout.GroupCount());
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 3 }), layout.GroupAt(0)->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 2 }), layout.GroupAt(1)->tabIds);
	EXPECT_EQ(0u, layout.ActiveGroupIndex());
	const auto before = layout;
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Unsplit(2));
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Unsplit(99));
	ExpectModelInvariants(layout);
	EXPECT_EQ(before.InstancesInOrder(), layout.InstancesInOrder());
	EXPECT_EQ(before.ActiveGroupIndex(), layout.ActiveGroupIndex());
	ASSERT_EQ(before.GroupCount(), layout.GroupCount());
	for( std::size_t i = 0; i < before.GroupCount(); ++i ) {
		const auto& left = *before.GroupAt(i);
		const auto& right = *layout.GroupAt(i);
		EXPECT_EQ(left.root, right.root);
		EXPECT_EQ(left.tabIds, right.tabIds);
		ASSERT_EQ(left.nodes.size(), right.nodes.size());
		for( std::size_t j = 0; j < left.nodes.size(); ++j ) {
			EXPECT_EQ(left.nodes[j].leafId, right.nodes[j].leafId);
			EXPECT_EQ(left.nodes[j].children, right.nodes[j].children);
			EXPECT_EQ(left.nodes[j].weights, right.nodes[j].weights);
			EXPECT_EQ(left.nodes[j].orientation, right.nodes[j].orientation);
		}
	}
}

//! VS Code join chooses the first single group and normalizes the source group.
TEST(TerminalGroupLayout, JoinUsesTheFirstSingletonCandidate)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	layout.AppendGroup(2);
	layout.AppendGroup(3);
	ASSERT_TRUE(layout.InsertPaneSplit(2, 3, 4, terminal::TerminalPaneOrientation::Horizontal));
	const auto unchanged = layout;
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Join({ 3, 4 }));
	EXPECT_EQ(unchanged.InstancesInOrder(), layout.InstancesInOrder());
	EXPECT_EQ(unchanged.ActiveGroupIndex(), layout.ActiveGroupIndex());
	ExpectModelInvariants(layout);
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Join({ 2, 3 }));
	ExpectModelInvariants(layout);
	ASSERT_EQ(3u, layout.GroupCount());
	EXPECT_EQ((std::vector<std::uint64_t>{ 2, 3 }), layout.GroupAt(1)->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 4 }), layout.GroupAt(2)->tabIds);
	EXPECT_EQ(1u, layout.ActiveGroupIndex());
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Join({ 99, 2, 2 }));
	ExpectModelInvariants(layout);
}

//! VS Code join creates a last group when no selected instance is alone.
TEST(TerminalGroupLayout, JoinWithoutSingletonAppendsANewGroup)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	layout.AppendGroup(3);
	ASSERT_TRUE(layout.InsertPaneSplit(1, 3, 4, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Join({ 1, 3 }));
	ExpectModelInvariants(layout);
	ASSERT_EQ(3u, layout.GroupCount());
	EXPECT_EQ((std::vector<std::uint64_t>{ 2 }), layout.GroupAt(0)->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 4 }), layout.GroupAt(1)->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1, 3 }), layout.GroupAt(2)->tabIds);
	EXPECT_EQ(2u, layout.ActiveGroupIndex());
}

//! VS Code addInstance repeatedly inserts immediately after the candidate's active leaf.
TEST(TerminalGroupLayout, JoinInsertsEachAddedLeafAfterTheAnchor)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(5);
	layout.AppendGroup(6);
	layout.AppendGroup(7);
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Join({ 5, 6, 7 }));
	ExpectModelInvariants(layout);
	ASSERT_EQ(1u, layout.GroupCount());
	EXPECT_EQ((std::vector<std::uint64_t>{ 5, 7, 6 }), layout.GroupAt(0)->tabIds);
}

//! tmux swap-pane exchanges identities while sizes remain attached to positions.
TEST(TerminalGroupLayout, SwapKeepsWeightsAndSupportsCrossGroupLeaves)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	const auto root = layout.GroupAt(0)->root;
	ASSERT_TRUE(layout.ApplyDividerResize(0, root, 0, 250, 1000));
	const auto weights = layout.GroupAt(0)->nodes[root].weights;
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Swap(1, 2));
	ExpectModelInvariants(layout);
	EXPECT_EQ(weights, layout.GroupAt(0)->nodes[root].weights);
	EXPECT_EQ((std::vector<std::uint64_t>{ 2, 1 }), layout.GroupAt(0)->tabIds);
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Swap(1, 1));
	EXPECT_EQ(terminal::TerminalLayoutEdit::Unchanged, layout.Swap(1, 99));
	ExpectModelInvariants(layout);
	layout.AppendGroup(3);
	EXPECT_EQ(terminal::TerminalLayoutEdit::Changed, layout.Swap(1, 3));
	ExpectModelInvariants(layout);
	EXPECT_EQ((std::vector<std::uint64_t>{ 2, 3 }), layout.GroupAt(0)->tabIds);
	EXPECT_EQ((std::vector<std::uint64_t>{ 1 }), layout.GroupAt(1)->tabIds);
	EXPECT_EQ(weights, layout.GroupAt(0)->nodes[root].weights);
}

//! tmux swap-pane relative targets wrap and a lone pane has no target.
TEST(TerminalGroupLayout, RelativeLeafWrapsAndRejectsLoneOrUnknownLeaves)
{
	terminal::TerminalGroupLayout layout;
	layout.EnsureGroupForTab(1);
	EXPECT_FALSE(layout.RelativeLeaf(1, 1));
	EXPECT_FALSE(layout.RelativeLeaf(99, -1));
	ExpectModelInvariants(layout);
	ASSERT_TRUE(layout.InsertPaneSplit(0, 1, 2, terminal::TerminalPaneOrientation::Horizontal));
	ASSERT_TRUE(layout.InsertPaneSplit(0, 2, 3, terminal::TerminalPaneOrientation::Horizontal));
	EXPECT_EQ(3u, layout.RelativeLeaf(1, -1));
	EXPECT_EQ(1u, layout.RelativeLeaf(3, 1));
	EXPECT_EQ(2u, layout.RelativeLeaf(1, 1));
	ExpectModelInvariants(layout);
}

} // namespace
