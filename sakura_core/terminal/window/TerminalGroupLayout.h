/*! @file
 * @brief HWND-free model of the terminal panel's ordered groups and split trees.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/window/TerminalPaneLayout.h"
#include "terminal/runtime/TerminalRuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace terminal {

//! One terminal group of the panel layout: the leaf tab ids in depth-first
//! order plus the recursive split tree that produces that order. Node indices
//! inside `nodes` are local to the group; `root` is the tree entry point.
//! Unreachable nodes may remain in `nodes` after a collapse; only the tree
//! reachable from `root` defines the layout. Kept free of native handles so
//! the model can be exercised without a window.
struct TerminalPaneGroup final {
	std::vector<std::uint64_t> tabIds;
	std::vector<TerminalPaneLayoutTreeNode> nodes;
	std::size_t root{};
};

//! Pure model of the terminal panel's group list. A terminal session belongs
//! to exactly one group, and only the selected group owns native viewport
//! HWNDs at a time; this type owns neither the renderers nor the sessions,
//! only the ordered groups, each group's recursive split tree (orientation,
//! weights, leaves as tab ids), and the optional active group index.
//!
//! The group keeps a recursive split tree so an orthogonal split affects only
//! the focused pane, e.g. a full height left pane beside two stacked panes on
//! the right. Invariants maintained by every mutation: a tab id appears in at
//! most one group, no group outlives its last tab (the emptied group is
//! erased and the active index adjusted), a reachable split node has at least
//! two children whose weights are positive, and `tabIds` always equals the
//! depth-first leaf order of `root`.
class TerminalGroupLayout final {
public:
	TerminalGroupLayout() = default;

	[[nodiscard]] std::size_t GroupCount() const noexcept;
	[[nodiscard]] const TerminalPaneGroup* GroupAt( std::size_t index ) const noexcept;
	//! The ordered groups, for projections that export the layout as plain
	//! data (#415 Phase 3). The reference is invalidated by any mutation.
	[[nodiscard]] const std::vector<TerminalPaneGroup>& Groups() const noexcept;
	[[nodiscard]] std::optional<std::size_t> ActiveGroupIndex() const noexcept;
	[[nodiscard]] const TerminalPaneGroup* ActiveGroup() const noexcept;
	[[nodiscard]] std::optional<std::size_t> FindGroup( std::uint64_t tabId ) const noexcept;
	//! The group index `direction` steps away from the active one, wrapping
	//! around; the active index itself is never changed. Absent when empty.
	[[nodiscard]] std::optional<std::size_t> RelativeGroupIndex( int direction ) const noexcept;
	[[nodiscard]] std::optional<TerminalPaneOrientation> NodeOrientation( std::size_t groupIndex,
		std::size_t nodeIndex ) const noexcept;
	//! The orientation of the split node that directly contains `tabId`'s
	//! leaf; Horizontal when the leaf has no parent or the tab is unknown.
	[[nodiscard]] TerminalPaneOrientation PaneOrientationForTab( std::size_t groupIndex,
		std::uint64_t tabId ) const noexcept;

	//! Appends a new single-leaf group holding `tabId` at the end. The active
	//! group is not changed. Returns the new group's index.
	std::size_t AppendGroup( std::uint64_t tabId );
	//! Returns the group already containing `tabId` when there is one.
	//! Otherwise appends a new single-leaf group and makes it active.
	std::size_t EnsureGroupForTab( std::uint64_t tabId );
	//! Stores the active index as given; validity is enforced lazily by
	//! ActiveGroup()/NormalizeActiveGroup().
	void SetActiveGroup( std::optional<std::size_t> index ) noexcept;
	//! Repairs the active index after external changes: drops it when it is
	//! out of range, and selects the first group when none is active.
	void NormalizeActiveGroup() noexcept;
	//! Removes `tabId`'s leaf from its group and collapses any split left
	//! with a single child. An emptied group is erased and the active index
	//! adjusted. Returns false when no group contained the tab.
	[[nodiscard]] bool RemoveTab( std::uint64_t tabId );
	//! Splits `primaryId`'s leaf in `groupIndex`'s tree on `orientation`.
	//! When the leaf's parent already splits on that axis the new leaf is
	//! inserted beside it and the row/column is equalized; otherwise the leaf
	//! is wrapped in a new two-child split. Returns false when `primaryId`
	//! is not in the group.
	[[nodiscard]] bool InsertPaneSplit( std::size_t groupIndex, std::uint64_t primaryId,
		std::uint64_t newId, TerminalPaneOrientation orientation );
	//! Recomputes the two weights beside a dragged divider so `firstExtent`
	//! out of `combinedExtent` pixels becomes their share of the combined
	//! weight. Returns false when the node or child position is invalid.
	[[nodiscard]] bool ApplyDividerResize( std::size_t groupIndex, std::size_t nodeIndex,
		std::size_t childPosition, int firstExtent, int combinedExtent );
	//! Validates `group` and appends it (#415 Phase 3 restore): the tree must
	//! be non-empty, `root` in range, every reachable leaf id non-zero, used
	//! once within the tree and not already in another group, every reachable
	//! split must own at least two children with a matching positive weight
	//! each, child indices must stay in range and acyclic, and `tabIds` must
	//! equal the depth-first leaf order. Unreachable nodes are dead data and
	//! ignored. Returns the new group's index; a rejected group changes
	//! nothing and returns nullopt.
	[[nodiscard]] std::optional<std::size_t> InstallGroup( TerminalPaneGroup group );
	//! Repairs `groupIndex`'s tree in place: malformed weight lists are reset
	//! to equal 1000 weights and `tabIds` is rebuilt in depth-first order.
	void NormalizeGroup( std::size_t groupIndex );
	//! Replaces a whole group record, used to roll a failed split back.
	void ReplaceGroup( std::size_t index, TerminalPaneGroup group );
	void Clear() noexcept;

private:
	struct PaneLocation final {
		std::size_t nodeIndex{};
		std::optional<std::size_t> parentIndex;
		std::size_t childPosition{};
	};

	struct RemovePaneResult final {
		bool removed{};
		std::optional<std::size_t> replacement;
	};

	static TerminalPaneLayoutTreeNode MakePaneLeaf( std::uint64_t tabId );
	static void CollectTabIds( const TerminalPaneGroup& group, std::size_t nodeIndex,
		std::vector<std::uint64_t>& tabIds );
	[[nodiscard]] static bool ValidateGroupTree( const TerminalPaneGroup& group );
	static void NormalizeNode( TerminalPaneGroup& group, std::size_t nodeIndex ) noexcept;
	static void NormalizeGroupTree( TerminalPaneGroup& group );
	static std::optional<PaneLocation> FindPaneLocation( const TerminalPaneGroup& group,
		std::size_t nodeIndex, std::uint64_t tabId ) noexcept;
	static RemovePaneResult RemovePaneFromNode( TerminalPaneGroup& group, std::size_t nodeIndex,
		std::uint64_t tabId ) noexcept;
	[[nodiscard]] TerminalPaneGroup* MutableGroupAt( std::size_t index ) noexcept;

	std::vector<TerminalPaneGroup> m_groups;
	std::optional<std::size_t> m_activeGroup;
};

} // namespace terminal
