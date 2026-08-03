/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "extension/CExtensionStatusBar.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

bool Valid(std::wstring_view value, std::size_t maximum, bool allowEmpty = false)
{
	return (allowEmpty || !value.empty()) && value.size() <= maximum &&
		value.find(L'\0') == std::wstring_view::npos;
}
} // namespace

bool CExtensionStatusBar::Upsert(SExtensionStatusBarItem item)
{
	if (!Valid(item.handle, 512) || !Valid(item.itemId, 512, true) || !Valid(item.name, 4096, true)
		|| !Valid(item.extensionId, 255) ||
		!Valid(item.text, 4096, true) || !Valid(item.tooltip, 16384, true) ||
		!Valid(item.command, 512, true) || !Valid(item.accessibilityLabel, 4096, true) ||
		item.generation == 0 || !std::isfinite(item.priority)) return false;
	std::unique_lock lock(m_mutex);
	const auto found = m_items.find(item.handle);
	if (found != m_items.end() && (found->second.extensionId != item.extensionId ||
		found->second.generation != item.generation)) return false;
	m_items.insert_or_assign(item.handle, std::move(item));
	return true;
}

bool CExtensionStatusBar::Remove(
	std::wstring_view handle,
	std::wstring_view ownerExtensionId,
	std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	const auto found = m_items.find(std::wstring(handle));
	if (found == m_items.end()) return false;
	if (found->second.extensionId != ownerExtensionId || found->second.generation != generation) return false;
	m_items.erase(found);
	return true;
}

void CExtensionStatusBar::RemoveOwnedBy(std::wstring_view extensionId, std::uint64_t generation)
{
	std::unique_lock lock(m_mutex);
	std::erase_if(m_items, [extensionId, generation](const auto& pair) {
		return pair.second.extensionId == extensionId &&
			(generation == 0 || pair.second.generation == generation);
	});
}

void CExtensionStatusBar::Clear()
{
	std::unique_lock lock(m_mutex);
	m_items.clear();
}

std::vector<SExtensionStatusBarItem> CExtensionStatusBar::Snapshot() const
{
	std::vector<SExtensionStatusBarItem> result;
	{
		std::shared_lock lock(m_mutex);
		result.reserve(m_items.size());
		for (const auto& [handle, item] : m_items) {
			if (item.visible) result.push_back(item);
		}
	}
	std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
		if (left.alignment != right.alignment) return left.alignment < right.alignment;
		if (left.priority != right.priority) return left.priority > right.priority;
		return left.handle < right.handle;
	});
	return result;
}

std::optional<std::wstring> CExtensionStatusBar::CommandFor(std::wstring_view handle) const
{
	std::shared_lock lock(m_mutex);
	const auto found = m_items.find(std::wstring(handle));
	if (found == m_items.end() || !found->second.visible || found->second.command.empty()) return std::nullopt;
	return found->second.command;
}
