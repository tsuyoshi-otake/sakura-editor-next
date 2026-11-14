/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "terminal/window/TerminalGroupLayout.h"

#include <algorithm>

namespace terminal {

std::size_t TerminalGroupLayout::GroupCount() const noexcept
{
	return m_groups.size();
}

const TerminalPaneGroup* TerminalGroupLayout::GroupAt( std::size_t index ) const noexcept
{
	return index < m_groups.size() ? &m_groups[index] : nullptr;
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
