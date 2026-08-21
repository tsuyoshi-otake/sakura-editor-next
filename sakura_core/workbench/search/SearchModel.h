/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//! HWND-free model of VS Code's Search view state.
//!
//! The types here mirror `vs/workbench/contrib/search/common/searchModel.ts`:
//! a query, a `FileMatch` per file, and a `Match` per hit inside it. Nothing in
//! this header touches Win32 or the file system, so the projection from a
//! completed search onto rendered rows stays testable on its own.
namespace workbench::search {

//! Upstream's `ITextQuery.contentPattern` plus the widget's replace text.
struct SearchQuery final {
	std::wstring text;
	std::wstring replaceText;
	//! `search.matchCase`, `search.wholeWord`, `search.useRegex` on the widget.
	bool matchCase = false;
	bool wholeWord = false;
	bool useRegex = false;
	//! `Preserve Case` on the replace box: the replacement adopts the casing
	//! pattern of the text it replaces (all upper, all lower, or Title Case).
	bool preserveCase = false;

	[[nodiscard]] bool operator==(const SearchQuery&) const noexcept = default;
};

//! One hit, in the coordinates the editor uses when the row is activated.
struct SearchMatch final {
	//! 1-based logical line, as `F_JUMP`-style navigation expects.
	std::int64_t line = 0;
	//! 1-based column in UTF-16 code units within that line.
	int column = 0;
	//! Match length in UTF-16 code units.
	int length = 0;
	//! The rendered line, already trimmed of leading whitespace exactly once so
	//! every row of a file keeps its relative indentation readable.
	std::wstring preview;
	//! Where the match sits inside `preview`.
	int previewOffset = 0;
	int previewLength = 0;

	[[nodiscard]] bool operator==(const SearchMatch&) const noexcept = default;
};

//! Upstream's `FileMatch`: one file plus every hit inside it.
struct SearchFileResult final {
	std::wstring fullPath;
	//! Path relative to the searched root, with the file name stripped off.
	//! Upstream renders this as the row's dimmed description.
	std::wstring folderLabel;
	std::wstring fileName;
	std::vector<SearchMatch> matches;

	[[nodiscard]] bool operator==(const SearchFileResult&) const noexcept = default;
};

//! Why a completed search stopped.
enum class ESearchCompletion : std::uint8_t {
	//! Ran to the end of the tree.
	Complete,
	//! Superseded by a newer query, or the view closed.
	Cancelled,
	//! `search.maxResults` was reached; the results shown are a prefix.
	LimitHit,
	//! The pattern could not be compiled. `failureText` says why.
	InvalidPattern,
	//! Regular-expression support is unavailable in this build/installation.
	RegexUnavailable,
	//! There is no folder to search.
	NoWorkspace,
};

//! One completed search.
struct SearchResults final {
	std::vector<SearchFileResult> files;
	std::size_t matchCount = 0;
	ESearchCompletion completion = ESearchCompletion::Complete;
	//! Message text for a failed pattern; empty otherwise.
	std::wstring failureText;

	[[nodiscard]] bool operator==(const SearchResults&) const noexcept = default;
};

//! `search.maxResults`' registered default.
inline constexpr std::size_t kSearchMaxResults = 20000;
//! How much of one line the preview keeps. Upstream truncates the rendered
//! label rather than the model; this bound keeps the row model itself small.
inline constexpr int kSearchPreviewMaxLength = 250;

} // namespace workbench::search
