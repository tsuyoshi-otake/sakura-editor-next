/*! @file
	@brief Pure filtering/sorting of already-available Extensions view rows against a parsed search query
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/openvsx/OpenVsxProtocol.h"
#include "workbench/extension/ExtensionSearchQuery.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace workbench::extension {

//! One extension the Extensions view can filter/sort. Presentation-neutral: it
//! carries exactly the fields `ApplyExtensionSearchFilters` needs and nothing
//! HWND/network/filesystem-shaped, so both the installed-extension enumeration
//! and an already-fetched marketplace search result can be projected into it.
struct ExtensionSearchCandidate {
	SOpenVsxExtension extension;
	//! Empty when the extension is not installed.
	std::wstring installedVersion;
	//! Only meaningful when installedVersion is non-empty.
	bool enabled = false;
};

//! Applies a parsed query's per-item filters (Installed/Enabled/Disabled/
//! Outdated/Deprecated), free-text substring matching, and @sort: ordering to
//! an already-available candidate set. Pure: no network, no filesystem, no
//! HWND, and it never mutates `candidates`.
//!
//! What this function does **not** evaluate, by design:
//! - `ExtensionSearchFilter::Recommended` has no backing data anywhere in this
//!   application (there is no workspace/user recommendation source), so a
//!   caller must reject a query containing it *before* calling this function
//!   rather than let it silently pass every candidate or silently match none
//!   -- either would misrepresent "recommended" as something this build knows.
//! - `query.unknownTokens` is not consulted; a caller must reject a query with
//!   unknown tokens before calling this function for the same reason.
//! - `ExtensionSearchFilter::Outdated` is answered strictly from
//!   `candidate.installedVersion` vs `candidate.extension.sVersion`. When both
//!   come from the same source (for example, an installed-only enumeration
//!   where no marketplace fetch has happened) they are equal by construction,
//!   so Outdated correctly reports "no known outdated extensions" rather than
//!   fabricating an answer this build cannot know without a network check.
//!
//! Free-text matching requires every whitespace-separated term in
//! `query.searchText` to be a substring (case-insensitive) of the candidate's
//! display name, name, namespace, or description. Pass a query with
//! `searchText` cleared to skip free-text filtering entirely -- the correct
//! choice when the text was already used as a marketplace search term and the
//! server's relevance match must not be narrowed again by a naive local
//! substring check.
//!
//! Sorting: `Relevance` performs no resort and preserves the input order (the
//! caller's enumeration order or the registry's server-relevance order, both
//! of which are more meaningful than an invented tiebreak). `Name` sorts
//! ascending, case-insensitive, by display name (falling back to name).
//! `InstallCount` and `Rating` sort descending; a candidate with no known
//! rating (`!HasRating()`) sorts after every rated candidate under `Rating`.
[[nodiscard]] std::vector<std::size_t> ApplyExtensionSearchFilters(
	const ParsedExtensionSearchQuery& query,
	std::span<const ExtensionSearchCandidate> candidates);

} // namespace workbench::extension
