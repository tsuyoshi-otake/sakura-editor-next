/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionViewRegistry.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace {

bool Valid(std::wstring_view value, std::size_t maximum, bool allowEmpty = false)
{
	return (allowEmpty || !value.empty()) && value.size() <= maximum &&
		value.find(L'\0') == std::wstring_view::npos;
}

bool ValidView(const SExtensionViewDescriptor& view)
{
	return Valid(view.handle, 512) && Valid(view.viewId, 512) && Valid(view.containerId, 512) &&
		Valid(view.title, 4096) && Valid(view.description, 4096, true) && Valid(view.message, 16384, true) &&
		Valid(view.extensionId, 255) && Valid(view.badgeTooltip, 4096, true) && view.generation != 0;
}

bool ValidItem(const SExtensionTreeItem& item)
{
	return Valid(item.handle, 1024) && Valid(item.viewHandle, 512) && Valid(item.parentHandle, 1024, true) &&
		Valid(item.stableId, 1024, true) && Valid(item.label, 4096) && Valid(item.description, 4096, true) &&
		Valid(item.tooltip, 16384, true) && Valid(item.contextValue, 512, true) && Valid(item.command, 512, true) &&
		item.commandArgumentsJson.size() <= 1024u * 1024u;
}

std::wstring ChildrenCacheKey(std::wstring_view viewHandle, std::wstring_view parentHandle)
{
	std::wstring key(viewHandle);
	key.push_back(L'\0');
	key.append(parentHandle);
	return key;
}

} // namespace

bool CExtensionViewRegistry::Register(SExtensionViewDescriptor view)
{
	if (!ValidView(view)) return false;
	std::unique_lock lock(m_mutex);
	if (m_views.contains(view.handle) || m_viewIdToHandle.contains(view.viewId)) return false;
	const std::wstring handle = view.handle;
	const std::wstring viewId = view.viewId;
	m_views.emplace(handle, ViewState{ std::move(view), {} });
	m_viewIdToHandle.emplace(viewId, handle);
	return true;
}

bool CExtensionViewRegistry::Update(SExtensionViewDescriptor view)
{
	if (!ValidView(view)) return false;
	std::unique_lock lock(m_mutex);
	const auto found = m_views.find(view.handle);
	if (found == m_views.end() || !IsOwner(found->second, view.extensionId, view.generation) ||
		found->second.descriptor.viewId != view.viewId) return false;
	found->second.descriptor = std::move(view);
	return true;
}

bool CExtensionViewRegistry::Unregister(
	std::wstring_view viewHandle,
	std::wstring_view extensionId,
	std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	const auto found = m_views.find(std::wstring(viewHandle));
	if (found == m_views.end() || !IsOwner(found->second, extensionId, generation)) return false;
	RemoveViewLocked(viewHandle);
	return true;
}

void CExtensionViewRegistry::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	std::vector<std::wstring> handles;
	for (const auto& [handle, view] : m_views) {
		if (view.descriptor.extensionId == extensionId &&
			(generation == 0 || view.descriptor.generation == generation)) handles.push_back(handle);
	}
	for (const auto& handle : handles) RemoveViewLocked(handle);
}

void CExtensionViewRegistry::Clear()
{
	std::unique_lock lock(m_mutex);
	m_items.clear();
	m_cachedChildren.clear();
	m_itemOrder.clear();
	m_viewIdToHandle.clear();
	m_views.clear();
	m_nextOrder = 1;
}

bool CExtensionViewRegistry::ReplaceChildren(
	std::wstring_view viewHandle,
	std::wstring_view parentHandle,
	std::wstring_view extensionId,
	std::uint64_t generation,
	std::vector<SExtensionTreeItem> children)
{
	std::unique_lock lock(m_mutex);
	const auto view = m_views.find(std::wstring(viewHandle));
	if (view == m_views.end() || !IsOwner(view->second, extensionId, generation)) return false;
	if (!parentHandle.empty()) {
		const auto parent = m_items.find(std::wstring(parentHandle));
		if (parent == m_items.end() || parent->second.viewHandle != viewHandle) return false;
	}
	std::unordered_set<std::wstring> handles;
	for (const auto& item : children) {
		if (!ValidItem(item) || item.viewHandle != viewHandle || item.parentHandle != parentHandle ||
			item.handle == parentHandle || !handles.emplace(item.handle).second) return false;
		const auto existing = m_items.find(item.handle);
		if (existing != m_items.end() &&
			(existing->second.viewHandle != viewHandle || existing->second.parentHandle != parentHandle)) return false;
	}

	RemoveDescendantsLocked(viewHandle, parentHandle);
	for (auto& item : children) {
		m_itemOrder.emplace(item.handle, m_nextOrder++);
		m_items.emplace(item.handle, std::move(item));
	}
	m_cachedChildren.emplace(ChildrenCacheKey(viewHandle, parentHandle));
	std::erase_if(view->second.selection, [this](const auto& handle) { return !m_items.contains(handle); });
	return true;
}

bool CExtensionViewRegistry::Invalidate(std::wstring_view viewHandle, std::wstring_view itemHandle)
{
	std::unique_lock lock(m_mutex);
	const auto view = m_views.find(std::wstring(viewHandle));
	if (view == m_views.end()) return false;
	if (!itemHandle.empty()) {
		const auto item = m_items.find(std::wstring(itemHandle));
		if (item == m_items.end() || item->second.viewHandle != viewHandle) return false;
	}
	RemoveDescendantsLocked(viewHandle, itemHandle);
	std::erase_if(view->second.selection, [this](const auto& handle) { return !m_items.contains(handle); });
	return true;
}

bool CExtensionViewRegistry::SetSelection(std::wstring_view viewHandle, std::vector<std::wstring> itemHandles)
{
	std::unique_lock lock(m_mutex);
	const auto view = m_views.find(std::wstring(viewHandle));
	if (view == m_views.end() || (!view->second.descriptor.canSelectMany && itemHandles.size() > 1)) return false;
	std::unordered_set<std::wstring> unique;
	for (const auto& handle : itemHandles) {
		const auto item = m_items.find(handle);
		if (item == m_items.end() || item->second.viewHandle != viewHandle || !unique.emplace(handle).second) return false;
	}
	view->second.selection = std::move(itemHandles);
	return true;
}

std::vector<SExtensionViewDescriptor> CExtensionViewRegistry::Views(std::wstring_view containerId) const
{
	std::shared_lock lock(m_mutex);
	std::vector<SExtensionViewDescriptor> result;
	for (const auto& [handle, view] : m_views) {
		if (view.descriptor.visible && (containerId.empty() || view.descriptor.containerId == containerId)) {
			result.push_back(view.descriptor);
		}
	}
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		if (left.containerId != right.containerId) return left.containerId < right.containerId;
		if (left.title != right.title) return left.title < right.title;
		return left.viewId < right.viewId;
	});
	return result;
}

std::vector<SExtensionTreeItem> CExtensionViewRegistry::Children(
	std::wstring_view viewHandle,
	std::wstring_view parentHandle) const
{
	std::shared_lock lock(m_mutex);
	std::vector<SExtensionTreeItem> result;
	for (const auto& [handle, item] : m_items) {
		if (item.viewHandle == viewHandle && item.parentHandle == parentHandle) result.push_back(item);
	}
	std::sort(result.begin(), result.end(), [this](const auto& left, const auto& right) {
		return m_itemOrder.at(left.handle) < m_itemOrder.at(right.handle);
	});
	return result;
}

bool CExtensionViewRegistry::HasChildrenSnapshot(
	std::wstring_view viewHandle,
	std::wstring_view parentHandle) const
{
	std::shared_lock lock(m_mutex);
	return m_cachedChildren.contains(ChildrenCacheKey(viewHandle, parentHandle));
}

std::vector<std::wstring> CExtensionViewRegistry::Selection(std::wstring_view viewHandle) const
{
	std::shared_lock lock(m_mutex);
	const auto view = m_views.find(std::wstring(viewHandle));
	return view == m_views.end() ? std::vector<std::wstring>{} : view->second.selection;
}

std::vector<std::wstring> CExtensionViewRegistry::RevealPath(
	std::wstring_view viewHandle,
	std::wstring_view itemHandle) const
{
	std::shared_lock lock(m_mutex);
	std::vector<std::wstring> reversed;
	std::unordered_set<std::wstring> visited;
	std::wstring current(itemHandle);
	while (!current.empty()) {
		if (!visited.emplace(current).second) return {};
		const auto item = m_items.find(current);
		if (item == m_items.end() || item->second.viewHandle != viewHandle) return {};
		reversed.push_back(current);
		current = item->second.parentHandle;
	}
	std::reverse(reversed.begin(), reversed.end());
	return reversed;
}

bool CExtensionViewRegistry::IsOwner(
	const ViewState& view,
	std::wstring_view extensionId,
	std::uint64_t generation) const
{
	return view.descriptor.extensionId == extensionId && view.descriptor.generation == generation;
}

void CExtensionViewRegistry::RemoveViewLocked(std::wstring_view viewHandle)
{
	const auto view = m_views.find(std::wstring(viewHandle));
	if (view == m_views.end()) return;
	m_viewIdToHandle.erase(view->second.descriptor.viewId);
	std::erase_if(m_items, [viewHandle](const auto& pair) { return pair.second.viewHandle == viewHandle; });
	std::erase_if(m_itemOrder, [this](const auto& pair) { return !m_items.contains(pair.first); });
	const std::wstring prefix = ChildrenCacheKey(viewHandle, {});
	std::erase_if(m_cachedChildren, [&prefix](const auto& key) {
		return key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
	});
	m_views.erase(view);
}

void CExtensionViewRegistry::RemoveDescendantsLocked(
	std::wstring_view viewHandle,
	std::wstring_view parentHandle)
{
	std::vector<std::wstring> pending{ std::wstring(parentHandle) };
	std::unordered_set<std::wstring> remove;
	while (!pending.empty()) {
		const std::wstring parent = std::move(pending.back());
		pending.pop_back();
		for (const auto& [handle, item] : m_items) {
			if (item.viewHandle == viewHandle && item.parentHandle == parent && remove.emplace(handle).second) {
				pending.push_back(handle);
			}
		}
	}
	for (const auto& handle : remove) {
		m_items.erase(handle);
		m_itemOrder.erase(handle);
	}
	const std::wstring prefix = ChildrenCacheKey(viewHandle, {});
	std::erase_if(m_cachedChildren, [&](const auto& key) {
		if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) return false;
		const std::wstring_view cachedParent(key.data() + prefix.size(), key.size() - prefix.size());
		return cachedParent == parentHandle || remove.contains(std::wstring(cachedParent));
	});
}
