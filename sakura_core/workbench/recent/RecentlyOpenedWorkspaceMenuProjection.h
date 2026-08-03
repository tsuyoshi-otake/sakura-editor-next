/*! @file
 * @brief Stable, HWND-free projection and click mapping for Open Recent.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/recent/RecentlyOpenedWorkspaceService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::recent {

inline constexpr std::int32_t kRecentlyOpenedWorkspaceDynamicFirst = 13000;
inline constexpr std::int32_t kRecentlyOpenedWorkspaceDynamicLast =
	kRecentlyOpenedWorkspaceDynamicFirst + static_cast<std::int32_t>(kMaximumRecentlyOpenedWorkspaces) - 1;

enum class ERecentlyOpenedWorkspaceMenuRowKind : std::uint8_t { Entry, Separator };

struct RecentlyOpenedWorkspaceMenuRow final {
	ERecentlyOpenedWorkspaceMenuRowKind kind = ERecentlyOpenedWorkspaceMenuRowKind::Entry;
	std::int32_t commandId = 0;
	std::wstring label;
};

class CRecentlyOpenedWorkspaceMenuProjection final {
public:
	//! Open Recent is available for either typed workspace history or the
	//! established recent-file history.  The latter is projected by the native
	//! menu owner, so it deliberately has no dynamic command here.
	[[nodiscard]] static bool HasItems(const std::vector<RecentlyOpenedWorkspaceEntry>& entries,
		bool hasRecentFiles) noexcept
	{
		return !entries.empty() || hasRecentFiles;
	}

	[[nodiscard]] static std::vector<RecentlyOpenedWorkspaceMenuRow> Build(
		const std::vector<RecentlyOpenedWorkspaceEntry>& entries, bool hasRecentFiles)
	{
		std::vector<RecentlyOpenedWorkspaceMenuRow> result;
		result.reserve(entries.size() + (entries.empty() || !hasRecentFiles ? 0U : 1U));
		for (std::size_t index = 0; index < entries.size() && index < kMaximumRecentlyOpenedWorkspaces; ++index) {
			const auto& entry = entries[index];
			result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::Entry,
				kRecentlyOpenedWorkspaceDynamicFirst + static_cast<std::int32_t>(index),
				entry.label.value_or(entry.uri.ToString()) });
		}
		if (!entries.empty() && hasRecentFiles) result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::Separator, 0, {} });
		return result;
	}

	[[nodiscard]] static std::optional<std::size_t> Resolve(std::int32_t commandId,
		const std::vector<RecentlyOpenedWorkspaceEntry>& snapshot) noexcept
	{
		if (commandId < kRecentlyOpenedWorkspaceDynamicFirst || commandId > kRecentlyOpenedWorkspaceDynamicLast) return std::nullopt;
		const auto index = static_cast<std::size_t>(commandId - kRecentlyOpenedWorkspaceDynamicFirst);
		return index < snapshot.size() ? std::optional<std::size_t>(index) : std::nullopt;
	}
};

} // namespace workbench::recent
