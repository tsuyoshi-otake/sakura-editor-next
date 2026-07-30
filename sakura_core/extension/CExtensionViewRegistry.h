/*! @file
	@brief VS Code views/createTreeView 互換の所有権付き状態モデル
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class EExtensionTreeItemCollapsibleState : std::uint8_t {
	None,
	Collapsed,
	Expanded,
};

struct SExtensionViewDescriptor {
	std::wstring handle;
	std::wstring viewId;
	std::wstring containerId = L"sakura.extensionViews";
	std::wstring title;
	std::wstring description;
	std::wstring message;
	std::wstring extensionId;
	std::uint64_t generation = 0;
	std::uint32_t badgeValue = 0;
	std::wstring badgeTooltip;
	bool canSelectMany = false;
	bool showCollapseAll = false;
	bool visible = true;
};

struct SExtensionTreeItem {
	std::wstring handle;
	std::wstring viewHandle;
	std::wstring parentHandle;
	std::wstring stableId;
	std::wstring label;
	std::wstring description;
	std::wstring tooltip;
	std::wstring contextValue;
	std::wstring command;
	std::string commandArgumentsJson = "[]";
	EExtensionTreeItemCollapsibleState collapsibleState = EExtensionTreeItemCollapsibleState::None;
	std::optional<bool> checkboxState;
};

//! Thread-safe model. RPC owners mutate it; window-local UI reads snapshots.
class CExtensionViewRegistry final {
public:
	bool Register(SExtensionViewDescriptor view);
	bool Update(SExtensionViewDescriptor view);
	bool Unregister(std::wstring_view viewHandle, std::wstring_view extensionId, std::uint64_t generation);
	void RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation);
	void Clear();

	//! Atomically replaces direct children and removes all stale descendants.
	bool ReplaceChildren(
		std::wstring_view viewHandle,
		std::wstring_view parentHandle,
		std::wstring_view extensionId,
		std::uint64_t generation,
		std::vector<SExtensionTreeItem> children);
	//! Invalidates cached direct children so the provider is asked again.
	bool Invalidate(std::wstring_view viewHandle, std::wstring_view itemHandle = {});
	bool SetSelection(std::wstring_view viewHandle, std::vector<std::wstring> itemHandles);

	[[nodiscard]] std::vector<SExtensionViewDescriptor> Views(std::wstring_view containerId = {}) const;
	[[nodiscard]] std::vector<SExtensionTreeItem> Children(
		std::wstring_view viewHandle, std::wstring_view parentHandle = {}) const;
	//! Distinguishes a resolved empty provider result from children not requested yet.
	[[nodiscard]] bool HasChildrenSnapshot(
		std::wstring_view viewHandle, std::wstring_view parentHandle = {}) const;
	[[nodiscard]] std::vector<std::wstring> Selection(std::wstring_view viewHandle) const;
	//! Returns root-to-item handles, or an empty vector for an unknown/cyclic item.
	[[nodiscard]] std::vector<std::wstring> RevealPath(
		std::wstring_view viewHandle, std::wstring_view itemHandle) const;

private:
	struct ViewState {
		SExtensionViewDescriptor descriptor;
		std::vector<std::wstring> selection;
	};

	bool IsOwner(const ViewState& view, std::wstring_view extensionId, std::uint64_t generation) const;
	void RemoveViewLocked(std::wstring_view viewHandle);
	void RemoveDescendantsLocked(std::wstring_view viewHandle, std::wstring_view parentHandle);

	mutable std::shared_mutex m_mutex;
	std::unordered_map<std::wstring, ViewState> m_views;
	std::unordered_map<std::wstring, std::wstring> m_viewIdToHandle;
	std::unordered_map<std::wstring, SExtensionTreeItem> m_items;
	std::unordered_set<std::wstring> m_cachedChildren;
	std::uint64_t m_nextOrder = 1;
	std::unordered_map<std::wstring, std::uint64_t> m_itemOrder;
};
