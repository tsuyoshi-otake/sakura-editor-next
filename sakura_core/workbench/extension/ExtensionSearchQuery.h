/*! @file
	@brief Marketplace-style `@filter` search-box query parsing (pure, unwired)
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace workbench::extension {

//! Filters this extension model can actually evaluate. Real VS Code's
//! Extensions view additionally recognizes `@builtin`, `@category:`, `@tag:`,
//! `@ext:`, `@id:`, and `@workspaceUnsupported`; those are deliberately left
//! unrecognized here (see the .cpp) rather than silently matching everything
//! or nothing, because `SOpenVsxExtension` carries no backing data for them.
enum class ExtensionSearchFilter {
	Installed,   //!< @installed
	Disabled,    //!< @disabled
	Enabled,     //!< @enabled
	Outdated,    //!< @outdated (VS Code alias: @updates)
	Deprecated,  //!< @deprecated
	Recommended, //!< @recommended
};

//! `@sort:<key>` directives this model can evaluate against `SOpenVsxExtension`
//! fields. Matches VS Code's sort key spellings where a corresponding field
//! exists (`installs`, `rating`, `name`); `updateDate`/`publishedDate` are not
//! recognized because the DTO carries no timestamp.
enum class ExtensionSearchSortKey {
	Relevance,    //!< No @sort: token was present (VS Code's default).
	InstallCount, //!< @sort:installs
	Rating,       //!< @sort:rating
	Name,         //!< @sort:name
};

struct ParsedExtensionSearchQuery {
	std::vector<ExtensionSearchFilter> filters;
	ExtensionSearchSortKey sortKey = ExtensionSearchSortKey::Relevance;
	//! Free-text search terms with every recognized `@token` removed, collapsed
	//! to single spaces, and trimmed.
	std::wstring searchText;
	//! `@`-prefixed tokens that matched neither a known filter nor `@sort:`.
	//! Kept (not silently dropped) so a caller can decide how to treat them --
	//! for example folding an unrecognized token back into free text, the way
	//! VS Code treats a query with no matching filter as a literal search.
	std::vector<std::wstring> unknownTokens;

	[[nodiscard]] bool HasFilter(ExtensionSearchFilter filter) const noexcept;
};

//! Parses one Marketplace-search-box-style query string into recognized
//! `@filter` tokens, an optional `@sort:<key>` directive, and the remaining
//! free-text search terms. Token recognition is case-insensitive, matching VS
//! Code's own Extensions view search box. This function does no filtering or
//! sorting itself and touches no extension data; it only classifies the query
//! string, so it has nothing to fetch and nothing to fail on.
[[nodiscard]] ParsedExtensionSearchQuery ParseExtensionSearchQuery(std::wstring_view query);

} // namespace workbench::extension
