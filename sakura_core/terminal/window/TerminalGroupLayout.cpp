/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/window/TerminalGroupLayout.h"

#include <algorithm>
#include <unordered_set>

namespace terminal {

std::size_t TerminalGroupLayout::GroupCount() const noexcept
{
	return m_groups.size();
}

const TerminalPaneGroup* TerminalGroupLayout::GroupAt( std::size_t index ) const noexcept
{
	return index < m_groups.size() ? &m_groups[index] : nullptr;
}

const std::vector<TerminalPaneGroup>& TerminalGroupLayout::Groups() const noexcept
{
	return m_groups;
}

TerminalPaneGroup* TerminalGroupLayout::MutableGroupAt( std::size_t index ) noexcept
{
	return index < m_groups.size() ? &m_groups[index] : nullptr;
}

std::optional<std::size_t> TerminalGroupLayout::ActiveGroupIndex() const noexcept
{
	return m_activeGroup;
}

const TerminalPaneGroup* TerminalGroupLayout::ActiveGroup() const noexcept
{
	if( !m_activeGroup || *m_activeGroup >= m_groups.size() ) return nullptr;
	return &m_groups[*m_activeGroup];
}

std::optional<std::size_t> TerminalGroupLayout::FindGroup( std::uint64_t tabId ) const noexcept
{
	for( std::size_t index = 0; index < m_groups.size(); ++index ) {
		const auto& group = m_groups[index];
		if( std::find(group.tabIds.begin(), group.tabIds.end(), tabId) != group.tabIds.end() ) return index;
	}
	return std::nullopt;
}

std::optional<std::size_t> TerminalGroupLayout::RelativeGroupIndex( int direction ) const noexcept
{
	if( m_groups.empty() ) return std::nullopt;
	const auto current = m_activeGroup.value_or(0);
	const auto count = m_groups.size();
	return direction < 0 ? (current + count - 1) % count : (current + 1) % count;
}

std::vector<std::uint64_t> TerminalGroupLayout::InstancesInOrder() const
{
	std::vector<std::uint64_t> ids;
	std::size_t total = 0;
	for( const auto& group : m_groups ) total += group.tabIds.size();
	ids.reserve(total);
	// `tabIds` is the maintained depth-first leaf order of the tree.
	for( const auto& group : m_groups ) {
		ids.insert(ids.end(), group.tabIds.begin(), group.tabIds.end());
	}
	return ids;
}

TerminalSplitPosition TerminalGroupLayout::SplitPositionOf( std::uint64_t tabId ) const noexcept
{
	for( const auto& group : m_groups ) {
		const auto found = std::find(group.tabIds.begin(), group.tabIds.end(), tabId);
		if( found == group.tabIds.end() ) continue;
		if( group.tabIds.size() < 2 ) return TerminalSplitPosition::None;
		const auto position = static_cast<std::size_t>(found - group.tabIds.begin());
		if( position == 0 ) return TerminalSplitPosition::First;
		return position + 1 == group.tabIds.size() ? TerminalSplitPosition::Last : TerminalSplitPosition::Middle;
	}
	return TerminalSplitPosition::None;
}

std::optional<TerminalPaneOrientation> TerminalGroupLayout::NodeOrientation(
	std::size_t groupIndex, std::size_t nodeIndex ) const noexcept
{
	const auto* group = GroupAt(groupIndex);
	if( !group || nodeIndex >= group->nodes.size() ) return std::nullopt;
	return group->nodes[nodeIndex].orientation;
}

TerminalPaneLayoutTreeNode TerminalGroupLayout::MakePaneLeaf( std::uint64_t tabId )
{
	TerminalPaneLayoutTreeNode node;
	node.leafId = tabId;
	return node;
}

void TerminalGroupLayout::CollectTabIds( const TerminalPaneGroup& group, std::size_t nodeIndex,
	std::vector<std::uint64_t>& tabIds )
{
	if( nodeIndex >= group.nodes.size() ) return;
	const auto& node = group.nodes[nodeIndex];
	if( node.children.empty() ) {
		if( node.leafId != 0 ) tabIds.push_back(node.leafId);
		return;
	}
	for( const auto child : node.children ) CollectTabIds(group, child, tabIds);
}

void TerminalGroupLayout::NormalizeNode( TerminalPaneGroup& group, std::size_t nodeIndex ) noexcept
{
	if( nodeIndex >= group.nodes.size() ) return;
	auto& node = group.nodes[nodeIndex];
	if( node.children.empty() ) {
		node.weights.clear();
		return;
	}
	const bool valid = node.children.size() >= 2
		&& node.weights.size() == node.children.size()
		&& std::all_of(node.weights.begin(), node.weights.end(), [](int weight) { return weight > 0; })
		&& std::all_of(node.children.begin(), node.children.end(), [&group, nodeIndex](std::size_t child) {
			return child < group.nodes.size() && child != nodeIndex;
		});
	if( !valid ) node.weights.assign(node.children.size(), 1000);
	for( const auto child : node.children ) NormalizeNode(group, child);
}

void TerminalGroupLayout::NormalizeGroupTree( TerminalPaneGroup& group )
{
	if( group.nodes.empty() ) {
		group.tabIds.clear();
		group.root = 0;
		return;
	}
	NormalizeNode(group, group.root);
	group.tabIds.clear();
	CollectTabIds(group, group.root, group.tabIds);
}

void TerminalGroupLayout::NormalizeGroup( std::size_t groupIndex )
{
	if( auto* group = MutableGroupAt(groupIndex) ) NormalizeGroupTree(*group);
}

std::size_t TerminalGroupLayout::AppendGroup( std::uint64_t tabId )
{
	TerminalPaneGroup group;
	group.tabIds.push_back(tabId);
	group.nodes.push_back(MakePaneLeaf(tabId));
	group.root = 0;
	m_groups.push_back(std::move(group));
	return m_groups.size() - 1;
}

std::size_t TerminalGroupLayout::EnsureGroupForTab( std::uint64_t tabId )
{
	if( const auto existing = FindGroup(tabId) ) return *existing;
	const auto created = AppendGroup(tabId);
	m_activeGroup = created;
	return created;
}

void TerminalGroupLayout::SetActiveGroup( std::optional<std::size_t> index ) noexcept
{
	m_activeGroup = index;
}

void TerminalGroupLayout::NormalizeActiveGroup() noexcept
{
	if( m_activeGroup && *m_activeGroup >= m_groups.size() ) m_activeGroup.reset();
	if( !m_activeGroup && !m_groups.empty() ) m_activeGroup = 0;
}

std::optional<TerminalGroupLayout::PaneLocation> TerminalGroupLayout::FindPaneLocation(
	const TerminalPaneGroup& group, std::size_t nodeIndex, std::uint64_t tabId ) noexcept
{
	if( nodeIndex >= group.nodes.size() ) return std::nullopt;
	const auto& node = group.nodes[nodeIndex];
	if( node.children.empty() ) {
		return node.leafId == tabId ? std::optional<PaneLocation>(PaneLocation{ nodeIndex, std::nullopt, 0 })
			: std::nullopt;
	}
	for( std::size_t position = 0; position < node.children.size(); ++position ) {
		auto location = FindPaneLocation(group, node.children[position], tabId);
		if( !location ) continue;
		if( !location->parentIndex ) {
			location->parentIndex = nodeIndex;
			location->childPosition = position;
		}
		return location;
	}
	return std::nullopt;
}

TerminalPaneOrientation TerminalGroupLayout::PaneOrientationForTab( std::size_t groupIndex,
	std::uint64_t tabId ) const noexcept
{
	const auto* group = GroupAt(groupIndex);
	if( !group ) return TerminalPaneOrientation::Horizontal;
	if( const auto location = FindPaneLocation(*group, group->root, tabId); location && location->parentIndex ) {
		return group->nodes[*location->parentIndex].orientation;
	}
	return TerminalPaneOrientation::Horizontal;
}

TerminalGroupLayout::RemovePaneResult TerminalGroupLayout::RemovePaneFromNode(
	TerminalPaneGroup& group, std::size_t nodeIndex, std::uint64_t tabId ) noexcept
{
	if( nodeIndex >= group.nodes.size() ) return {};
	auto& node = group.nodes[nodeIndex];
	if( node.children.empty() ) return { node.leafId == tabId, std::nullopt };
	for( std::size_t position = 0; position < node.children.size(); ++position ) {
		const auto childIndex = node.children[position];
		const auto childResult = RemovePaneFromNode(group, childIndex, tabId);
		if( !childResult.removed ) continue;
		if( childResult.replacement ) {
			node.children[position] = *childResult.replacement;
		} else {
			node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(position));
			if( position < node.weights.size() ) {
				node.weights.erase(node.weights.begin() + static_cast<std::ptrdiff_t>(position));
			}
		}
		if( node.children.size() <= 1 ) {
			return { true, node.children.empty() ? std::nullopt
				: std::optional<std::size_t>(node.children.front()) };
		}
		if( node.weights.size() != node.children.size() ) node.weights.assign(node.children.size(), 1000);
		return { true, nodeIndex };
	}
	return {};
}

bool TerminalGroupLayout::RemoveTab( std::uint64_t tabId )
{
	for( std::size_t index = 0; index < m_groups.size(); ++index ) {
		auto& group = m_groups[index];
		if( std::find(group.tabIds.begin(), group.tabIds.end(), tabId) == group.tabIds.end() ) continue;
		const auto result = RemovePaneFromNode(group, group.root, tabId);
		if( !result.removed ) return false;
		if( result.replacement ) group.root = *result.replacement;
		else group.nodes.clear();
		NormalizeGroupTree(group);
		if( !group.tabIds.empty() ) return true;

		if( m_activeGroup ) {
			if( *m_activeGroup == index ) {
				if( index + 1 < m_groups.size() ) *m_activeGroup = index;
				else if( index > 0 ) *m_activeGroup = index - 1;
				else m_activeGroup.reset();
			} else if( *m_activeGroup > index ) {
				--*m_activeGroup;
			}
		}
		m_groups.erase(m_groups.begin() + static_cast<std::ptrdiff_t>(index));
		return true;
	}
	return false;
}

TerminalLayoutEdit TerminalGroupLayout::Unsplit( std::uint64_t tabId )
{
	const auto group = FindGroup(tabId);
	if( !group || m_groups[*group].tabIds.size() < 2 ) return TerminalLayoutEdit::Unchanged;
	auto updated = *this;
	if( !updated.RemoveTab(tabId) ) return TerminalLayoutEdit::Unchanged;
	updated.AppendGroup(tabId);
	*this = std::move(updated);
	return TerminalLayoutEdit::Changed;
}

TerminalLayoutEdit TerminalGroupLayout::Join( const std::vector<std::uint64_t>& tabIds )
{
	std::vector<std::uint64_t> known;
	for( const auto id : tabIds ) {
		if( FindGroup(id) && std::find(known.begin(), known.end(), id) == known.end() ) known.push_back(id);
	}
	if( known.size() < 2 ) return TerminalLayoutEdit::Unchanged;
	const auto firstGroup = *FindGroup(known.front());
	if( m_groups[firstGroup].tabIds.size() == known.size()
		&& std::all_of(known.begin(), known.end(), [this, firstGroup](std::uint64_t id) {
			return FindGroup(id) == firstGroup;
		}) ) return TerminalLayoutEdit::Unchanged;

	auto updated = *this;
	std::optional<std::uint64_t> anchor;
	for( const auto id : known ) {
		const auto group = updated.FindGroup(id);
		if( group && updated.m_groups[*group].tabIds.size() == 1 ) {
			anchor = id;
			break;
		}
	}
	for( const auto id : known ) {
		if( anchor && updated.FindGroup(id) == updated.FindGroup(*anchor) ) continue;
		if( !updated.RemoveTab(id) ) return TerminalLayoutEdit::Unchanged;
		if( !anchor ) {
			updated.AppendGroup(id);
			anchor = id;
		} else {
			const auto group = updated.FindGroup(*anchor);
			if( !group || !updated.InsertPaneSplit(*group, *anchor, id, TerminalPaneOrientation::Horizontal) ) {
				return TerminalLayoutEdit::Unchanged;
			}
		}
	}
	updated.SetActiveGroup(updated.FindGroup(known.front()));
	*this = std::move(updated);
	return TerminalLayoutEdit::Changed;
}

TerminalLayoutEdit TerminalGroupLayout::Swap( std::uint64_t first, std::uint64_t second )
{
	const auto firstGroup = FindGroup(first);
	const auto secondGroup = FindGroup(second);
	if( first == second || !firstGroup || !secondGroup ) return TerminalLayoutEdit::Unchanged;
	auto& left = m_groups[*firstGroup];
	auto& right = m_groups[*secondGroup];
	const auto leftLeaf = FindPaneLocation(left, left.root, first);
	const auto rightLeaf = FindPaneLocation(right, right.root, second);
	if( !leftLeaf || !rightLeaf ) return TerminalLayoutEdit::Unchanged;
	std::swap(left.nodes[leftLeaf->nodeIndex].leafId, right.nodes[rightLeaf->nodeIndex].leafId);
	NormalizeGroupTree(left);
	if( firstGroup != secondGroup ) NormalizeGroupTree(right);
	return TerminalLayoutEdit::Changed;
}

std::optional<std::uint64_t> TerminalGroupLayout::RelativeLeaf( std::uint64_t tabId, int direction ) const
{
	const auto group = FindGroup(tabId);
	if( !group ) return std::nullopt;
	const auto& ids = m_groups[*group].tabIds;
	if( ids.size() < 2 ) return std::nullopt;
	const auto index = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), tabId) - ids.begin());
	return ids[direction < 0 ? (index + ids.size() - 1) % ids.size() : (index + 1) % ids.size()];
}

bool TerminalGroupLayout::InsertPaneSplit( std::size_t groupIndex, std::uint64_t primaryId,
	std::uint64_t newId, TerminalPaneOrientation orientation )
{
	auto* group = MutableGroupAt(groupIndex);
	if( !group ) return false;
	NormalizeGroupTree(*group);
	const auto location = FindPaneLocation(*group, group->root, primaryId);
	if( !location ) return false;

	const auto newLeafIndex = group->nodes.size();
	group->nodes.push_back(MakePaneLeaf(newId));
	if( location->parentIndex
		&& group->nodes[*location->parentIndex].orientation == orientation ) {
		auto& parent = group->nodes[*location->parentIndex];
		parent.children.insert(parent.children.begin()
			+ static_cast<std::ptrdiff_t>(location->childPosition + 1), newLeafIndex);
		// Repeated splits on one axis are an equal distribution of that
		// sibling row/column. An orthogonal split remains local to the leaf.
		parent.weights.assign(parent.children.size(), 1000);
	} else {
		TerminalPaneLayoutTreeNode split;
		split.children = { location->nodeIndex, newLeafIndex };
		split.weights = { 1000, 1000 };
		split.orientation = orientation;
		const auto splitIndex = group->nodes.size();
		group->nodes.push_back(std::move(split));
		if( location->parentIndex ) {
			group->nodes[*location->parentIndex].children[location->childPosition] = splitIndex;
		} else {
			group->root = splitIndex;
		}
	}
	NormalizeGroupTree(*group);
	return true;
}

bool TerminalGroupLayout::ApplyDividerResize( std::size_t groupIndex, std::size_t nodeIndex,
	std::size_t childPosition, int firstExtent, int combinedExtent )
{
	auto* group = MutableGroupAt(groupIndex);
	if( !group || nodeIndex >= group->nodes.size() ) return false;
	auto& node = group->nodes[nodeIndex];
	if( childPosition + 1 >= node.children.size() ) return false;
	if( node.weights.size() != node.children.size() ) node.weights.assign(node.children.size(), 1000);
	const int combinedWeight = std::max(2, node.weights[childPosition] + node.weights[childPosition + 1]);
	const int firstWeight = std::clamp(static_cast<int>(
		(static_cast<long long>(combinedWeight) * firstExtent) / combinedExtent), 1, combinedWeight - 1);
	node.weights[childPosition] = firstWeight;
	node.weights[childPosition + 1] = combinedWeight - firstWeight;
	return true;
}

bool TerminalGroupLayout::ValidateGroupTree( const TerminalPaneGroup& group )
{
	if( group.nodes.empty() || group.root >= group.nodes.size() ) return false;
	std::unordered_set<std::size_t> visited;
	std::unordered_set<std::uint64_t> leaves;
	std::vector<std::uint64_t> collected;
	const std::function<bool(std::size_t)> walk = [&]( std::size_t nodeIndex ) -> bool {
		// A second visit means a cycle or a node used as a child twice.
		if( nodeIndex >= group.nodes.size() || !visited.insert(nodeIndex).second ) return false;
		const auto& node = group.nodes[nodeIndex];
		if( node.children.empty() ) {
			if( node.leafId == 0 || !leaves.insert(node.leafId).second ) return false;
			collected.push_back(node.leafId);
			return true;
		}
		if( node.children.size() < 2 || node.weights.size() != node.children.size() ) return false;
		for( const auto weight : node.weights ) {
			if( weight <= 0 ) return false;
		}
		for( const auto child : node.children ) {
			if( child == nodeIndex || !walk(child) ) return false;
		}
		return true;
	};
	if( !walk(group.root) ) return false;
	return collected == group.tabIds;
}

std::optional<std::size_t> TerminalGroupLayout::InstallGroup( TerminalPaneGroup group )
{
	if( !ValidateGroupTree(group) ) return std::nullopt;
	for( const auto tabId : group.tabIds ) {
		if( FindGroup(tabId) ) return std::nullopt;
	}
	m_groups.push_back(std::move(group));
	return m_groups.size() - 1;
}

void TerminalGroupLayout::ReplaceGroup( std::size_t index, TerminalPaneGroup group )
{
	if( index < m_groups.size() ) m_groups[index] = std::move(group);
}

void TerminalGroupLayout::Clear() noexcept
{
	m_groups.clear();
	m_activeGroup.reset();
}

} // namespace terminal
