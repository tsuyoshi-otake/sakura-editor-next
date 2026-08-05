/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/statusbar/StatusbarViewModel.h"

#include <sakura/storage/StorageTypes.h>

#include <algorithm>

namespace workbench::statusbar {

bool StatusbarViewModel::IsValidId(std::string_view id) noexcept
{
	return !id.empty() && id.size() <= 512 && platform::storage::IsValidStorageUtf8(id, false)
		&& std::none_of(id.begin(), id.end(), [](char value) {
			const auto byte = static_cast<unsigned char>(value);
			return byte < 0x20 || byte == 0x7f;
		});
}

bool StatusbarViewModel::SetEntries(std::vector<StatusbarEntry> entries)
{
	if (entries.size() > kMaximumStatusbarEntries) return false;
	std::set<std::string, std::less<>> unique;
	for (const auto& entry : entries) {
		if (!IsValidId(entry.id) || entry.name.empty() || entry.name.size() > 4096
			|| entry.name.find(L'\0') != std::wstring::npos || !unique.emplace(entry.id).second) return false;
	}
	std::lock_guard lock(m_mutex);
	m_entries = std::move(entries);
	return true;
}

bool StatusbarViewModel::RestoreHiddenIds(std::vector<std::string> hiddenIds)
{
	if (hiddenIds.size() > kMaximumStatusbarEntries) return false;
	std::set<std::string, std::less<>> next;
	for (auto& id : hiddenIds) {
		if (!IsValidId(id) || !next.emplace(std::move(id)).second) return false;
	}
	std::lock_guard lock(m_mutex);
	m_hiddenIds = std::move(next);
	return true;
}

bool StatusbarViewModel::SetHidden(std::string_view id, bool hidden)
{
	if (!IsValidId(id)) return false;
	std::lock_guard lock(m_mutex);
	if (hidden) return m_hiddenIds.emplace(id).second;
	return m_hiddenIds.erase(std::string(id)) != 0;
}

bool StatusbarViewModel::Toggle(std::string_view id)
{
	if (!IsValidId(id)) return false;
	std::lock_guard lock(m_mutex);
	const auto found = m_hiddenIds.find(id);
	if (found == m_hiddenIds.end()) m_hiddenIds.emplace(id);
	else m_hiddenIds.erase(found);
	return true;
}

bool StatusbarViewModel::IsVisible(std::string_view id, bool providerVisible) const noexcept
{
	if (!providerVisible || !IsValidId(id)) return false;
	std::lock_guard lock(m_mutex);
	return !m_hiddenIds.contains(id);
}

StatusbarViewSnapshot StatusbarViewModel::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	StatusbarViewSnapshot snapshot;
	snapshot.entries = m_entries;
	snapshot.hiddenIds.assign(m_hiddenIds.begin(), m_hiddenIds.end());
	return snapshot;
}

} // namespace workbench::statusbar
