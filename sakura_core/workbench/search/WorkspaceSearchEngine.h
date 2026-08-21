/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/search/SearchModel.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

//! The file-system half of the Search view: it walks one workspace folder and
//! produces the model in SearchModel.h. It owns no window and no thread, so the
//! view can run it on whatever worker it likes.
namespace workbench::search {

//! Polled between files and between lines. Returning true abandons the search.
using SearchCancelPredicate = std::function<bool()>;

/*!
	@brief Runs one text search over `root`.

	Directory traversal skips the entries VS Code's own registered defaults for
	`files.exclude` and `search.exclude` skip, so a search in a repository does
	not walk `.git` or `node_modules`. Those globs are not user-editable yet;
	the `files to include` / `files to exclude` boxes are a separate surface.
*/
[[nodiscard]] SearchResults RunWorkspaceSearch(std::wstring_view root, const SearchQuery& query,
	const SearchCancelPredicate& cancelled);

//! What one replace pass actually did.
struct ReplaceOutcome final {
	std::size_t replacedMatches = 0;
	std::size_t replacedFiles = 0;
	//! Files whose contents no longer matched the recorded positions, or that
	//! could not be rewritten. Upstream reports the same case as a failed edit
	//! rather than replacing at a guessed position.
	std::vector<std::wstring> failedFiles;
};

/*!
	@brief Rewrites every listed match with the query's replacement text.

	The caller passes the exact `SearchFileResult`s to act on, so "Replace All",
	"replace this file", and "replace this one match" are the same operation over
	different slices of the model. A file is rewritten only when every recorded
	match still matches at its recorded position; otherwise the file is left
	untouched and named in `failedFiles`.
*/
[[nodiscard]] ReplaceOutcome ReplaceMatches(const std::vector<SearchFileResult>& files,
	const SearchQuery& query, const SearchCancelPredicate& cancelled);

//! Applies the query's `preserveCase` rule to one replacement.
//! Exposed for tests; `matched` is the text about to be overwritten.
[[nodiscard]] std::wstring ApplyPreserveCase(std::wstring_view replacement, std::wstring_view matched);

} // namespace workbench::search
