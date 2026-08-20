/*! @file
 * @brief Provider-neutral file decorations, VS Code's `IDecorationsService` model.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::decorations {

/*!
	@brief The theme-color role one decoration paints its label with.

	VS Code's `IDecorationData.color` is a `ThemeColor`, which is a color *id*
	rather than a color: the provider names a role and the theme resolves it. This
	enumerator is that id in a form the native workbench can carry, so a consumer
	never receives a resolved COLORREF from a provider and no provider needs to
	know which theme is loaded.

	The roles are the Git extension's registered `gitDecoration.*` keys because
	Git is the only decoration provider this product has. A second provider
	contributing its own key extends this enumerator; it must not reuse a Git role
	whose name would then describe the wrong thing.
*/
enum class EFileDecorationColor : std::uint8_t {
	None,
	//! `gitDecoration.addedResourceForeground`
	GitAdded,
	//! `gitDecoration.modifiedResourceForeground`
	GitModified,
	//! `gitDecoration.deletedResourceForeground`
	GitDeleted,
	//! `gitDecoration.renamedResourceForeground`
	GitRenamed,
	//! `gitDecoration.stageModifiedResourceForeground`
	GitStageModified,
	//! `gitDecoration.stageDeletedResourceForeground`
	GitStageDeleted,
	//! `gitDecoration.untrackedResourceForeground`
	GitUntracked,
	//! `gitDecoration.ignoredResourceForeground`
	GitIgnored,
	//! `gitDecoration.conflictingResourceForeground`
	GitConflicting,
	//! `gitDecoration.submoduleResourceForeground`
	GitSubmodule,
};

//! One provider's decoration for one resource, mirroring `vscode.FileDecoration`.
struct FileDecoration final {
	//! `FileDecoration.badge`. Upstream allows one or two characters; Git uses one.
	std::wstring badge;
	//! `FileDecoration.tooltip`, unlocalized.
	std::wstring tooltip;
	EFileDecorationColor color{ EFileDecorationColor::None };
	/*!
		@brief `FileDecoration.propagate`, upstream's `IDecorationData.bubble`.

		A propagating decoration is also visible on every ancestor folder of the
		resource. The Git extension clears it for deleted resources alone, so a
		folder is never colored by a file that no longer exists in it.
	*/
	bool propagate{ true };

	[[nodiscard]] bool operator==(const FileDecoration&) const = default;
};

//! One decorated resource, keyed by its native path.
struct FileDecorationEntry final {
	//! An absolute Windows path. Separators may be either slash; case is ignored.
	std::wstring path;
	FileDecoration decoration;

	[[nodiscard]] bool operator==(const FileDecorationEntry&) const = default;
};

/*!
	@brief What one rendered row draws, after ancestors have collected their children.

	This is upstream's `IDecoration`, reduced to the parts a native row can paint.
	`containsChildren` is upstream's own flag: a row whose decoration came from its
	descendants renders the "contains emphasized items" bubble instead of a letter,
	because the letters belong to resources this row is not.
*/
struct ResolvedFileDecoration final {
	EFileDecorationColor color{ EFileDecorationColor::None };
	std::wstring badge;
	std::wstring tooltip;
	bool containsChildren{};

	[[nodiscard]] bool operator==(const ResolvedFileDecoration&) const = default;
};

//! Upstream's `localize('bubbleTitle', "Contains emphasized items")`, unlocalized.
inline constexpr std::wstring_view kContainsChildrenTooltip = L"Contains emphasized items";

namespace detail {

[[nodiscard]] inline wchar_t FoldPathChar(wchar_t value) noexcept
{
	if (value == L'/') return L'\\';
	if (value >= L'A' && value <= L'Z') return static_cast<wchar_t>(value - L'A' + L'a');
	return value;
}

//! Case-insensitive, separator-insensitive path comparison with no trailing separator.
[[nodiscard]] inline std::wstring FoldPath(std::wstring_view path)
{
	std::wstring folded;
	folded.reserve(path.size());
	for (const wchar_t value : path) folded.push_back(FoldPathChar(value));
	while (folded.size() > 1 && folded.back() == L'\\') folded.pop_back();
	return folded;
}

//! True when `candidate` names a strict descendant of the folder `directory`.
[[nodiscard]] inline bool IsDescendant(std::wstring_view directory, std::wstring_view candidate) noexcept
{
	if (directory.empty() || candidate.size() <= directory.size()) return false;
	if (candidate.compare(0, directory.size(), directory) != 0) return false;
	// A separator must actually follow, or `C:\foo` would swallow `C:\foobar`.
	// A drive root already ends in one, and popping it would make every path on
	// the drive its own sibling rather than its descendant.
	return directory.back() == L'\\' || candidate[directory.size()] == L'\\';
}

} // namespace detail

/*!
	@brief The decoration table one provider published, resolved per rendered row.

	Upstream keeps decorations in a `TernarySearchTree` keyed by URI and answers
	`getDecoration(uri, includeChildren)` from it. This holds the same table sorted
	by folded path, so the descendant walk a folder row needs is a contiguous range
	rather than a scan, and the walk order is deterministic.

	The table is pure data with no window, no provider, and no theme, so the row
	that paints it and the provider that fills it never meet.
*/
class FileDecorationTable final {
public:
	//! Replaces the whole table, which is how upstream's providers publish too:
	//! `onDidChangeFileDecorations` carries the new set, never a delta.
	void Replace(std::vector<FileDecorationEntry> entries)
	{
		m_entries.clear();
		m_entries.reserve(entries.size());
		for (auto& entry : entries) {
			if (entry.path.empty()) continue;
			m_entries.push_back({ detail::FoldPath(entry.path), std::move(entry.decoration) });
		}
		std::stable_sort(m_entries.begin(), m_entries.end(),
			[](const Entry& left, const Entry& right) { return left.key < right.key; });
		// A path published twice keeps the first entry, which is the one the
		// provider's own walk reached first.
		m_entries.erase(
			std::unique(m_entries.begin(), m_entries.end(),
				[](const Entry& left, const Entry& right) { return left.key == right.key; }),
			m_entries.end());
	}

	void Clear() noexcept { m_entries.clear(); }

	[[nodiscard]] bool Empty() const noexcept { return m_entries.empty(); }

	[[nodiscard]] std::size_t Size() const noexcept { return m_entries.size(); }

	/*!
		@brief Upstream's `getDecoration(uri, includeChildren)`.

		The resource's own decoration wins the color, exactly as upstream's
		`reduceRight` over a list whose first element is the resource's own data.
		Descendants only ever contribute when they propagate, and their presence
		alone -- not their color -- is what turns the badge into the bubble.
	*/
	[[nodiscard]] std::optional<ResolvedFileDecoration> Resolve(
		std::wstring_view path, bool includeChildren) const
	{
		const std::wstring key = detail::FoldPath(path);
		if (key.empty()) return std::nullopt;

		const FileDecoration* own = Find(key);
		const FileDecoration* child = includeChildren ? FirstPropagatingDescendant(key) : nullptr;
		if (own == nullptr && child == nullptr) return std::nullopt;

		ResolvedFileDecoration resolved;
		resolved.color = own != nullptr ? own->color : child->color;
		if (child != nullptr) {
			resolved.containsChildren = true;
			resolved.tooltip = kContainsChildrenTooltip;
		} else {
			resolved.badge = own->badge;
			resolved.tooltip = own->tooltip;
		}
		return resolved;
	}

private:
	struct Entry final {
		std::wstring key;
		FileDecoration decoration;
	};

	[[nodiscard]] const FileDecoration* Find(const std::wstring& key) const
	{
		const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), key,
			[](const Entry& entry, const std::wstring& value) { return entry.key < value; });
		if (found == m_entries.end() || found->key != key) return nullptr;
		return &found->decoration;
	}

	[[nodiscard]] const FileDecoration* FirstPropagatingDescendant(const std::wstring& key) const
	{
		auto cursor = std::upper_bound(m_entries.begin(), m_entries.end(), key,
			[](const std::wstring& value, const Entry& entry) { return value < entry.key; });
		for (; cursor != m_entries.end() && detail::IsDescendant(key, cursor->key); ++cursor) {
			if (cursor->decoration.propagate) return &cursor->decoration;
		}
		return nullptr;
	}

	std::vector<Entry> m_entries;
};

} // namespace workbench::decorations
