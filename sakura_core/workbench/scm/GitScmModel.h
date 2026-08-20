/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/decorations/FileDecorationModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

struct GitChange {
	wchar_t status = L'M';
	std::wstring path;
	//! Porcelain-v2 XY status. `indexStatus` describes HEAD -> index and
	//! `worktreeStatus` describes index -> worktree, matching Git's two-area
	//! source-control model.
	wchar_t indexStatus = L'.';
	wchar_t worktreeStatus = L'.';
	bool untracked = false;
	bool conflicted = false;
	std::wstring originalPath;

	[[nodiscard]] bool operator==(const GitChange&) const = default;
};

struct GitScmState {
	bool repository = false;
	//! `Branch.name`. Empty in a detached HEAD, exactly as upstream leaves
	//! `HEAD.name` undefined there; the short `commit` is what names HEAD then.
	std::wstring branch;
	//! `Branch.commit`, from `# branch.oid`. Empty before the first commit,
	//! where porcelain v2 reports the literal `(initial)`.
	std::wstring commit;
	std::wstring upstream;
	int ahead = 0;
	int behind = 0;
	std::vector<GitChange> changes;

	[[nodiscard]] bool operator==(const GitScmState&) const = default;
};

[[nodiscard]] GitScmState ParsePorcelainV2(std::string_view bytes);

//!
//! @brief Which of Git's two comparison areas a row describes.
//!
//! Upstream reads `raw.x` for the index row and `raw.y` for the working-tree row
//! in two separate switches, so one path with both an `x` and a `y` becomes two
//! rows carrying two different statuses. The area is what selects between them;
//! a conflicted or untracked path is decided before either switch runs and does
//! not depend on it.
//!
enum class EGitChangeArea : std::uint8_t {
	Index,
	WorkingTree,
};

//!
//! @brief Upstream's `Status`, in its declaration order.
//!
//! `IGNORED` is deliberately absent: it comes from the `!!` porcelain code, which
//! this product never asks git to emit, so an enumerator for it would name a
//! state no parser here can produce.
//!
enum class EGitFileStatus : std::uint8_t {
	IndexModified,
	IndexAdded,
	IndexDeleted,
	IndexRenamed,
	IndexCopied,

	Modified,
	Deleted,
	Untracked,
	IntentToAdd,
	IntentToRename,
	TypeChanged,

	AddedByUs,
	AddedByThem,
	DeletedByUs,
	DeletedByThem,
	BothAdded,
	BothDeleted,
	BothModified,
};

//!
//! @brief The status of one row, exactly as upstream's three switches assign it.
//!
//! Returns no status when that area contributes no row at all, which is upstream
//! falling out of a `switch` with no matching `case`: an index `T` and a
//! working-tree `C` are both real porcelain codes that upstream lists nowhere, so
//! they produce no resource rather than an invented one.
//!
[[nodiscard]] std::optional<EGitFileStatus> ClassifyGitFileStatus(
	const GitChange& change, EGitChangeArea area) noexcept;

//! Upstream's `Resource.getStatusText`, unlocalized.
[[nodiscard]] std::string_view GitFileStatusText(EGitFileStatus status) noexcept;

//! Upstream's `Resource.getStatusLetter`.
[[nodiscard]] wchar_t GitFileStatusLetter(EGitFileStatus status) noexcept;

//! Upstream's `Resource.strikeThrough`: every flavour of deleted.
[[nodiscard]] bool IsGitFileStatusStruckThrough(EGitFileStatus status) noexcept;

//! Upstream's `Resource.getStatusColor`, as a theme-color role rather than a color.
[[nodiscard]] decorations::EFileDecorationColor GitFileStatusDecorationColor(
	EGitFileStatus status) noexcept;

/*!
	@brief Upstream's `Resource.resourceDecoration`: `propagate` for this status.

	Upstream sets `propagate` for every status except the two deleted ones, so a
	folder is never colored by a file it no longer contains. `IGNORED` is not a
	case here because this product never asks git for it; see `EGitFileStatus`.
*/
[[nodiscard]] bool DoesGitFileStatusPropagate(EGitFileStatus status) noexcept;

} // namespace workbench::scm
