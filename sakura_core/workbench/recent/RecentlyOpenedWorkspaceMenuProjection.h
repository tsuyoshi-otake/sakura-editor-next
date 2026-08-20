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
#include <string_view>
#include <vector>

namespace workbench::recent {

inline constexpr std::int32_t kRecentlyOpenedWorkspaceDynamicFirst = 13000;
inline constexpr std::int32_t kRecentlyOpenedWorkspaceDynamicLast =
	kRecentlyOpenedWorkspaceDynamicFirst + static_cast<std::int32_t>(kMaximumRecentlyOpenedWorkspaces) - 1;

enum class ERecentlyOpenedWorkspaceMenuRowKind : std::uint8_t { Entry, Separator, ClearRecentlyOpened };

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

	//! `workspaceLabelFormat` is the localized VS Code "{0} (Workspace)"
	//! verbose workspace-label format; the projection substitutes "{0}" with
	//! the entry's display path.
	[[nodiscard]] static std::vector<RecentlyOpenedWorkspaceMenuRow> Build(
		const std::vector<RecentlyOpenedWorkspaceEntry>& entries, bool hasRecentFiles,
		std::wstring_view workspaceLabelFormat)
	{
		std::vector<RecentlyOpenedWorkspaceMenuRow> result;
		result.reserve(entries.size() + (entries.empty() || !hasRecentFiles ? 0U : 1U));
		for (std::size_t index = 0; index < entries.size() && index < kMaximumRecentlyOpenedWorkspaces; ++index) {
			const auto& entry = entries[index];
			result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::Entry,
				kRecentlyOpenedWorkspaceDynamicFirst + static_cast<std::int32_t>(index),
				FormatEntryLabel(entry, workspaceLabelFormat) });
		}
		if (!entries.empty() && hasRecentFiles) result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::Separator, 0, {} });
		return result;
	}

	//! VS Code closes Open Recent with a static `Clear Recently Opened...`
	//! contribution below every dynamic history group, so the row exists even
	//! when the history is empty and the command stays reachable. The owning
	//! menu supplies its command identifier; this projection owns only the
	//! dynamic 13000.. range and never names a legacy function code.
	[[nodiscard]] static std::vector<RecentlyOpenedWorkspaceMenuRow> BuildTrailing(
		bool hasPrecedingRows, std::int32_t clearCommandId, std::wstring_view clearLabel)
	{
		std::vector<RecentlyOpenedWorkspaceMenuRow> result;
		result.reserve(hasPrecedingRows ? 2U : 1U);
		if (hasPrecedingRows) result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::Separator, 0, {} });
		result.push_back({ ERecentlyOpenedWorkspaceMenuRowKind::ClearRecentlyOpened, clearCommandId,
			std::wstring(clearLabel) });
		return result;
	}

	//! VS Code renders Open Recent rows through
	//! labelService.getWorkspaceLabel(uri, { verbose: Verbosity.LONG }): an
	//! explicit label always wins, file URIs become the native OS path, and a
	//! saved workspace drops its ".code-workspace" extension before the
	//! localized workspace-label format wraps it.
	[[nodiscard]] static std::wstring FormatEntryLabel(const RecentlyOpenedWorkspaceEntry& entry,
		std::wstring_view workspaceLabelFormat)
	{
		if (entry.label.has_value()) return *entry.label;
		std::wstring display = UriDisplayLabel(entry.uri);
		if (entry.kind != ERecentlyOpenedWorkspaceKind::Workspace) return display;
		return ApplyWorkspaceLabelFormat(workspaceLabelFormat, StripWorkspaceExtension(std::move(display)));
	}

	[[nodiscard]] static std::optional<std::size_t> Resolve(std::int32_t commandId,
		const std::vector<RecentlyOpenedWorkspaceEntry>& snapshot) noexcept
	{
		if (commandId < kRecentlyOpenedWorkspaceDynamicFirst || commandId > kRecentlyOpenedWorkspaceDynamicLast) return std::nullopt;
		const auto index = static_cast<std::size_t>(commandId - kRecentlyOpenedWorkspaceDynamicFirst);
		return index < snapshot.size() ? std::optional<std::size_t>(index) : std::nullopt;
	}

private:
	//! labelService.getUriLabel: a file URI renders as the platform path with
	//! an uppercase drive letter (UNC as \\server\share); a URI with no
	//! Windows path form keeps its canonical URI string.
	[[nodiscard]] static std::wstring UriDisplayLabel(const platform::uri::Uri& uri)
	{
		auto windowsPath = uri.ToWindowsPath();
		if (!windowsPath.value.has_value()) return uri.ToString();
		std::wstring display = std::move(*windowsPath.value);
		if (display.size() >= 2 && display[1] == L':' && display[0] >= L'a' && display[0] <= L'z') {
			display[0] = static_cast<wchar_t>(display[0] - (L'a' - L'A'));
		}
		return display;
	}

	//! VS Code strips exactly one trailing ".code-workspace" and its endsWith
	//! check is case-sensitive; an empty remainder is kept intact.
	[[nodiscard]] static std::wstring StripWorkspaceExtension(std::wstring display)
	{
		constexpr std::wstring_view kExtension = L".code-workspace";
		if (display.size() > kExtension.size()
			&& std::wstring_view(display).substr(display.size() - kExtension.size()) == kExtension) {
			display.resize(display.size() - kExtension.size());
		}
		return display;
	}

	[[nodiscard]] static std::wstring ApplyWorkspaceLabelFormat(std::wstring_view format,
		std::wstring_view path)
	{
		constexpr std::wstring_view kPlaceholder = L"{0}";
		const auto position = format.find(kPlaceholder);
		if (position == std::wstring_view::npos) return std::wstring(path);
		std::wstring result;
		result.reserve(format.size() - kPlaceholder.size() + path.size());
		result.append(format.substr(0, position));
		result.append(path);
		result.append(format.substr(position + kPlaceholder.size()));
		return result;
	}
};

} // namespace workbench::recent
