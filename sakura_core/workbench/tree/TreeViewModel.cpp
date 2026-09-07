/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "workbench/tree/TreeViewModel.h"
#include <algorithm>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace workbench::tree {
namespace {
bool Text(std::wstring_view text, std::size_t maximum) noexcept
{
	if (text.size() > maximum) return false;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const auto c = static_cast<std::uint32_t>(text[i]);
		if (!c || c > 0xffff || (c >= 0xdc00 && c <= 0xdfff)) return false;
		if (c >= 0xd800 && c <= 0xdbff && (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)) return false;
	}
	return true;
}
bool Id(std::wstring_view id) noexcept
{
	return !id.empty() && id.size() <= 256 && std::all_of(id.begin(), id.end(), [](wchar_t c) {
		return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')
			|| c == L'.' || c == L'-' || c == L'_' || c == L':' || c == L'/';
	});
}
std::size_t Bytes(const TreeItem& item) noexcept
{
	std::size_t units = item.id.size() + item.label.size() + item.description.size() + item.tooltip.size()
		+ item.icon.size() + item.commandId.size();
	for (const auto& argument : item.arguments) units += argument.size();
	return units * sizeof(wchar_t);
}
std::size_t Bytes(const TreeNodeSnapshot& node) noexcept
{
	std::size_t units = node.parentId.size() + node.nextCursor.size() + node.message.size();
	for (const auto& child : node.children) units += child.size();
	return Bytes(node.item) + units * sizeof(wchar_t);
}
}

struct TreeViewModel::Impl {
	struct Node final { TreeNodeSnapshot value; std::uint64_t pending{}; };
	struct Pending final { TreeLoadRequest request; TreeLoadKind kind{}; TreeChildrenState previous{}; };
	Node root;
	std::map<std::wstring, Node, std::less<>> nodes;
	std::unordered_map<std::uint64_t, Pending> pending;
	std::wstring selection;
	std::uint64_t generation{ 1 }, nextTicket{ 1 };
	std::size_t bytes{};
	bool closed{};
	Impl() { root.value.expanded = true; }
	Node* Find(std::wstring_view id) { if (id.empty()) return &root; const auto it = nodes.find(id); return it == nodes.end() ? nullptr : &it->second; }
	const Node* Find(std::wstring_view id) const { return const_cast<Impl*>(this)->Find(id); }
	bool Descendant(std::wstring_view id, std::wstring_view ancestor) const
	{
		for (std::size_t depth = 0; !id.empty() && depth <= kMaximumTreeDepth; ++depth) {
			if (id == ancestor) return true;
			const auto* node = Find(id); if (!node) return false;
			id = node->value.parentId;
		}
		return ancestor.empty();
	}
	bool Visible(std::wstring_view id) const
	{
		const auto* node = Find(id); if (!node) return false;
		while (!node->value.parentId.empty()) {
			node = Find(node->value.parentId);
			if (!node || !node->value.expanded) return false;
		}
		return true;
	}
	Pending* Match(const TreeLoadRequest& request)
	{
		const auto found = pending.find(request.ticket);
		if (closed || request.generation != generation || found == pending.end()) return nullptr;
		const auto& expected = found->second.request;
		return expected.generation == request.generation && expected.parentId == request.parentId && expected.cursor == request.cursor ? &found->second : nullptr;
	}
	void CancelTicket(std::uint64_t ticket, std::vector<std::uint64_t>& cancelled)
	{
		if (!ticket) return;
		const auto it = pending.find(ticket); if (it == pending.end()) return;
		if (auto* node = Find(it->second.request.parentId)) { node->pending = 0; node->value.state = it->second.previous; }
		cancelled.push_back(ticket); pending.erase(it);
	}
	void Remove(std::wstring_view id, std::vector<std::uint64_t>& cancelled)
	{
		const auto it = nodes.find(id); if (it == nodes.end()) return;
		// Depth is validated at admission, so recursive teardown is bounded to 16.
		for (const auto& child : it->second.value.children) Remove(child, cancelled);
		CancelTicket(it->second.pending, cancelled);
		if (selection == id) selection.clear();
		bytes -= Bytes(it->second.value); nodes.erase(it);
	}
};

TreeViewModel::TreeViewModel() : m_impl(std::make_unique<Impl>()) {}
TreeViewModel::~TreeViewModel() = default;
bool TreeViewModel::ValidItem(const TreeItem& item) noexcept
{
	return Id(item.id) && !item.label.empty() && Text(item.label, 1024) && Text(item.description, 1024)
		&& Text(item.tooltip, 4096) && (item.icon.empty() || Id(item.icon)) && (item.commandId.empty() || Id(item.commandId))
		&& (!item.commandId.empty() || item.arguments.empty())
		&& item.arguments.size() <= 16 && std::all_of(item.arguments.begin(), item.arguments.end(), [](const auto& v) { return Text(v, 4096); })
		&& (item.collapsibleState == TreeItemCollapsibleState::None || item.collapsibleState == TreeItemCollapsibleState::Collapsed || item.collapsibleState == TreeItemCollapsibleState::Expanded);
}

TreeLoadAdmission TreeViewModel::Begin(std::wstring_view parentId, TreeLoadKind kind)
{
	auto& state = *m_impl;
	if (state.closed) return { TreeResult::Unavailable, {} };
	auto* node = state.Find(parentId);
	if (!node || (!parentId.empty() && (node->value.item.collapsibleState == TreeItemCollapsibleState::None || !node->value.expanded || !state.Visible(parentId)))) return {};
	if (node->pending || state.pending.size() >= kMaximumTreeLoads) return { TreeResult::Busy, {} };
	if (kind == TreeLoadKind::Initial && node->value.hasSnapshot && node->value.state != TreeChildrenState::Stale) return { TreeResult::Unchanged, {} };
	if (kind == TreeLoadKind::NextPage && (node->value.nextCursor.empty() || !node->value.hasSnapshot)) return {};
	if (kind != TreeLoadKind::Initial && kind != TreeLoadKind::Refresh && kind != TreeLoadKind::NextPage) return {};
	if (state.nextTicket > static_cast<std::uint64_t>(INT64_MAX)) return { TreeResult::LimitExceeded, {} };
	TreeLoadRequest request{ state.generation, state.nextTicket++, std::wstring(parentId), kind == TreeLoadKind::NextPage ? node->value.nextCursor : L"" };
	state.pending.emplace(request.ticket, Impl::Pending{ request, kind, node->value.state });
	state.bytes -= node->value.message.size() * sizeof(wchar_t);
	node->pending = request.ticket; node->value.state = TreeChildrenState::Loading; node->value.message.clear();
	node->value.retryKind = kind;
	return { TreeResult::Accepted, std::move(request) };
}

TreeChange TreeViewModel::Apply(const TreeLoadRequest& request, TreeChildrenPage page)
{
	auto& state = *m_impl;
	const auto* pending = state.Match(request);
	if (!pending) return { TreeResult::Stale, {} };
	auto* parent = state.Find(request.parentId);
	const bool append = pending->kind == TreeLoadKind::NextPage;
	auto reject = [&](TreeResult result, const wchar_t* reason) {
		(void)Fail(request, reason);
		if (parent && result == TreeResult::Stale) parent->value.retryKind = TreeLoadKind::Refresh;
		return TreeChange{ result, {} };
	};
	if (!parent || page.parentId != request.parentId || page.items.size() > kMaximumTreePageItems
		|| page.revision > static_cast<std::uint64_t>(INT64_MAX) || !Text(page.nextCursor, 2048) || !Text(page.message, 4096)
		|| (page.state == TreePageState::Partial) != !page.nextCursor.empty()
		|| (page.state != TreePageState::Complete && page.state != TreePageState::Partial && page.state != TreePageState::Empty && page.state != TreePageState::Failed)
		|| ((page.state == TreePageState::Empty || page.state == TreePageState::Failed) && !page.items.empty()))
		return reject(TreeResult::Invalid, L"Invalid tree page.");
	if (page.state == TreePageState::Failed) { (void)Fail(request, page.message.empty() ? L"The tree could not be loaded." : std::move(page.message)); return { TreeResult::Applied, {} }; }
	if (parent->value.hasSnapshot && (page.revision < parent->value.revision || (append && page.revision != parent->value.revision)))
		return reject(TreeResult::Stale, L"The tree changed while this page was loading. Refresh to continue.");
	if (!page.nextCursor.empty() && page.nextCursor == request.cursor) return reject(TreeResult::Invalid, L"The provider repeated its page cursor.");
	std::unordered_set<std::wstring> ids;
	std::size_t newItems{}, nextBytes = state.bytes;
	for (const auto& item : page.items) {
		if (!ValidItem(item) || !ids.insert(item.id).second || state.Descendant(request.parentId, item.id))
			return reject(TreeResult::Invalid, L"The tree contains duplicate or cyclic item identities.");
		const auto* old = state.Find(item.id);
		if (old && (append || old->value.parentId != request.parentId)) return reject(TreeResult::Invalid, L"An item identity belongs to another tree position.");
		if (!old) { ++newItems; nextBytes += request.parentId.size() * sizeof(wchar_t); }
		else nextBytes -= Bytes(old->value.item);
		nextBytes += Bytes(item);
	}
	std::vector<std::wstring> removalRoots;
	if (!append) for (const auto& old : parent->value.children) if (!ids.contains(old)) removalRoots.push_back(old);
	for (const auto& item : page.items) if (item.collapsibleState == TreeItemCollapsibleState::None) {
		const auto* old = state.Find(item.id);
		if (!old) continue;
		for (const auto& child : old->value.children) removalRoots.push_back(child);
		nextBytes -= (old->value.nextCursor.size() + old->value.message.size()) * sizeof(wchar_t);
		for (const auto& child : old->value.children) nextBytes -= child.size() * sizeof(wchar_t);
	}
	std::size_t removedCount{};
	std::vector<std::wstring_view> removing;
	for (const auto& id : removalRoots) removing.push_back(id);
	while (!removing.empty()) {
		const auto id = removing.back(); removing.pop_back();
		const auto* node = state.Find(id);
		if (!node) continue;
		++removedCount; nextBytes -= Bytes(node->value);
		for (const auto& child : node->value.children) removing.push_back(child);
	}
	std::vector<std::wstring> children = append ? parent->value.children : std::vector<std::wstring>{};
	children.reserve(children.size() + page.items.size());
	for (const auto& item : page.items) children.push_back(item.id);
	for (const auto& child : parent->value.children) nextBytes -= child.size() * sizeof(wchar_t);
	for (const auto& child : children) nextBytes += child.size() * sizeof(wchar_t);
	nextBytes -= (parent->value.nextCursor.size() + parent->value.message.size()) * sizeof(wchar_t);
	nextBytes += (page.nextCursor.size() + page.message.size()) * sizeof(wchar_t);
	if ((!page.items.empty() && parent->value.depth >= kMaximumTreeDepth)
		|| state.nodes.size() - removedCount + newItems > kMaximumTreeItems || nextBytes > kMaximumTreeBytes)
		return reject(TreeResult::LimitExceeded, L"The tree limit was reached. Narrow the result set and refresh.");
	// Allocate only changed nodes and the changed sibling list. Tree-wide string
	// cloning would turn a long sequence of small pages into O(N^2) payload copies.
	// Node handles transfer preallocated map storage during the nonallocating commit.
	std::map<std::wstring, Impl::Node, std::less<>> additions;
	for (auto& item : page.items) if (!state.Find(item.id)) {
		Impl::Node node;
		node.value.parentId = request.parentId; node.value.depth = parent->value.depth + 1;
		node.value.expanded = item.collapsibleState == TreeItemCollapsibleState::Expanded;
		node.value.item = std::move(item);
		item.id.clear();
		const auto id = node.value.item.id;
		additions.emplace(id, std::move(node));
	}
	TreeChange changed{ TreeResult::Applied, {} };
	changed.cancelled.reserve(kMaximumTreeLoads);
	for (const auto& id : removalRoots) state.Remove(id, changed.cancelled);
	for (auto& item : page.items) {
		if (item.id.empty()) continue; // Moved into a prepared addition above.
		auto* existing = state.Find(item.id);
		if (existing) {
			if (item.collapsibleState == TreeItemCollapsibleState::None) {
				state.CancelTicket(existing->pending, changed.cancelled);
				existing->value.children.clear(); existing->value.expanded = false;
				existing->value.state = TreeChildrenState::Unrequested; existing->value.hasSnapshot = false;
				existing->value.nextCursor.clear(); existing->value.message.clear();
			} else if (existing->value.item.collapsibleState == TreeItemCollapsibleState::None)
				existing->value.expanded = item.collapsibleState == TreeItemCollapsibleState::Expanded;
			existing->value.item = std::move(item);
		}
	}
	while (!additions.empty()) state.nodes.insert(additions.extract(additions.begin()));
	parent->value.children = std::move(children);
	parent->value.nextCursor = std::move(page.nextCursor); parent->value.message = std::move(page.message);
	parent->value.revision = page.revision; parent->value.hasSnapshot = true;
	parent->value.state = page.state == TreePageState::Partial ? TreeChildrenState::Partial
		: parent->value.children.empty() ? TreeChildrenState::Empty : TreeChildrenState::Complete;
	parent->pending = 0; state.pending.erase(request.ticket); state.bytes = nextBytes;
	return changed;
}

TreeResult TreeViewModel::Fail(const TreeLoadRequest& request, std::wstring message)
{
	auto& state = *m_impl;
	if (!state.Match(request)) return TreeResult::Stale;
	if (!Text(message, 4096)) message = L"The tree could not be loaded.";
	auto* node = state.Find(request.parentId);
	state.bytes -= node->value.message.size() * sizeof(wchar_t);
	if (state.bytes + message.size() * sizeof(wchar_t) > kMaximumTreeBytes) message.clear();
	state.bytes += message.size() * sizeof(wchar_t);
	node->pending = 0; node->value.state = TreeChildrenState::Failed; node->value.message = std::move(message);
	state.pending.erase(request.ticket);
	return TreeResult::Applied;
}
TreeResult TreeViewModel::Cancel(const TreeLoadRequest& request)
{
	auto& state = *m_impl;
	const auto* pending = state.Match(request); if (!pending) return TreeResult::Stale;
	auto* node = state.Find(request.parentId);
	node->pending = 0; node->value.state = pending->previous;
	state.pending.erase(request.ticket);
	return TreeResult::Applied;
}
TreeChange TreeViewModel::SetExpanded(std::wstring_view id, bool expanded)
{
	auto& state = *m_impl;
	if (state.closed) return { TreeResult::Unavailable, {} };
	auto* node = state.Find(id);
	if (!node || id.empty() || node->value.item.collapsibleState == TreeItemCollapsibleState::None) return {};
	if (node->value.expanded == expanded) return { TreeResult::Unchanged, {} };
	TreeChange result{ TreeResult::Applied, {} }; result.cancelled.reserve(kMaximumTreeLoads);
	node->value.expanded = expanded;
	if (!expanded) {
		std::vector<std::uint64_t> cancelled;
		cancelled.reserve(kMaximumTreeLoads);
		for (const auto& [ticket, pending] : state.pending) if (state.Descendant(pending.request.parentId, id)) cancelled.push_back(ticket);
		for (const auto ticket : cancelled) state.CancelTicket(ticket, result.cancelled);
		if (state.Descendant(state.selection, id)) state.selection = id;
	}
	return result;
}
bool TreeViewModel::Select(std::wstring_view id)
{
	if (m_impl->closed || (!id.empty() && (!m_impl->Find(id) || !m_impl->Visible(id)))) return false;
	m_impl->selection = id; return true;
}
std::wstring TreeViewModel::Selection() const { return m_impl->selection; }
std::optional<TreeNodeSnapshot> TreeViewModel::Node(std::wstring_view id) const
{
	const auto* node = m_impl->Find(id); return node ? std::optional(node->value) : std::nullopt;
}
const TreeNodeSnapshot* TreeViewModel::Inspect(std::wstring_view id) const noexcept
{
	const auto* node = m_impl->Find(id); return node ? &node->value : nullptr;
}
std::vector<std::wstring> TreeViewModel::Demand() const
{
	std::vector<std::wstring> result;
	if (m_impl->closed) return result;
	std::vector<std::wstring> stack{ L"" };
	while (!stack.empty() && result.size() < kMaximumTreeLoads) {
		const auto id = std::move(stack.back()); stack.pop_back();
		const auto* node = m_impl->Find(id); if (!node || !node->value.expanded) continue;
		if (node->value.state == TreeChildrenState::Unrequested || node->value.state == TreeChildrenState::Stale) { result.push_back(id); continue; }
		if (node->value.state == TreeChildrenState::Loading || node->value.state == TreeChildrenState::Failed) continue;
		for (auto it = node->value.children.rbegin(); it != node->value.children.rend(); ++it) {
			const auto* child = m_impl->Find(*it);
			if (child && child->value.item.collapsibleState != TreeItemCollapsibleState::None) stack.push_back(*it);
		}
	}
	return result;
}
std::vector<std::uint64_t> TreeViewModel::CancelAll()
{
	std::vector<std::uint64_t> result; result.reserve(kMaximumTreeLoads);
	while (!m_impl->pending.empty()) m_impl->CancelTicket(m_impl->pending.begin()->first, result);
	return result;
}
std::vector<std::uint64_t> TreeViewModel::Invalidate()
{
	auto result = CancelAll();
	if (m_impl->closed) return result;
	if (m_impl->generation == static_cast<std::uint64_t>(INT64_MAX)) { (void)Close(); return result; }
	++m_impl->generation;
	m_impl->root.value.state = TreeChildrenState::Stale;
	for (auto& [id, node] : m_impl->nodes) if (node.value.item.collapsibleState != TreeItemCollapsibleState::None) node.value.state = TreeChildrenState::Stale;
	return result;
}
std::vector<std::uint64_t> TreeViewModel::Close()
{
	auto result = CancelAll();
	m_impl->closed = true; m_impl->nodes.clear(); m_impl->selection.clear(); m_impl->bytes = 0;
	m_impl->root.value = {}; m_impl->root.value.state = TreeChildrenState::Stopped;
	return result;
}
std::size_t TreeViewModel::ItemCount() const noexcept { return m_impl->nodes.size(); }
std::size_t TreeViewModel::PendingCount() const noexcept { return m_impl->pending.size(); }
std::size_t TreeViewModel::RetainedBytes() const noexcept { return m_impl->bytes; }
bool TreeViewModel::IsClosed() const noexcept { return m_impl->closed; }

} // namespace workbench::tree
