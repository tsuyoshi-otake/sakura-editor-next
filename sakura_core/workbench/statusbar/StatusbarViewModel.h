/*! @file
 * @brief Pure VS Code-compatible status bar entry visibility model.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::statusbar {

inline constexpr char kHiddenStatusbarStorageKey[] = "workbench.statusbar.hidden";
inline constexpr std::size_t kMaximumStatusbarEntries = 2048;

enum class EStatusbarEntryAlignment : std::uint8_t { Left, Right };

struct StatusbarEntry final {
	std::string id;
	std::wstring name;
	EStatusbarEntryAlignment alignment = EStatusbarEntryAlignment::Left;
	bool providerVisible = true;
	[[nodiscard]] bool operator==(const StatusbarEntry&) const noexcept = default;
};

struct StatusbarViewSnapshot final {
	std::vector<StatusbarEntry> entries;
	std::vector<std::string> hiddenIds;
};

class StatusbarViewModel final {
public:
	[[nodiscard]] bool SetEntries(std::vector<StatusbarEntry> entries);
	[[nodiscard]] bool RestoreHiddenIds(std::vector<std::string> hiddenIds);
	[[nodiscard]] bool SetHidden(std::string_view id, bool hidden);
	[[nodiscard]] bool Toggle(std::string_view id);
	[[nodiscard]] bool IsVisible(std::string_view id, bool providerVisible = true) const noexcept;
	[[nodiscard]] StatusbarViewSnapshot Snapshot() const;

	[[nodiscard]] static bool IsValidId(std::string_view id) noexcept;

private:
	mutable std::mutex m_mutex;
	std::vector<StatusbarEntry> m_entries;
	std::set<std::string, std::less<>> m_hiddenIds;
};

} // namespace workbench::statusbar
