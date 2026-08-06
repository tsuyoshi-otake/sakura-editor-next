/*! @file
 * @brief What a Source Control row compares, and the line diff between the two sides.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitScmModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief Where one side of a comparison reads its text from.
//!
//! Upstream expresses this as a URI scheme: `git:` goes through its
//! `GitFileSystemProvider`, `file:` is the document on disk. The distinction is
//! not cosmetic — the working-tree side is the only one a user can edit, and it
//! is the only one that must never be read through `git show`.
//!
enum class EGitDiffSource : std::uint8_t {
	//! `git show --textconv <ref>:<path>`.
	Repository,
	//! The file on disk.
	WorkingTree,
};

//!
//! @brief One resolved side of a comparison.
//!
struct GitDiffEndpoint final {
	EGitDiffSource source{ EGitDiffSource::WorkingTree };
	//!
	//! @brief The git treeish, already through upstream's `sanitizeRef`.
	//!
	//! An **empty** ref is not an absent one: `git show :path` reads the index,
	//! and that is exactly what upstream's `toGitUri(uri, '')` means. `source` is
	//! what says whether this field is used at all.
	//!
	std::wstring ref;
	//! Repository-relative, with forward slashes, matching upstream's
	//! `sanitizeRelativePath`. Git accepts nothing else after a `:`.
	std::wstring path;

	[[nodiscard]] bool operator==(const GitDiffEndpoint&) const = default;
};

//!
//! @brief What upstream's resolver decided a row's click should do.
//!
enum class EGitDiffCommandKind : std::uint8_t {
	//! Both sides exist: upstream's `vscode.diff`.
	Diff,
	//! Only the right side exists: upstream's `vscode.open`.
	Open,
	//! Neither side exists. Upstream reaches its `vscode.open` branch with an
	//! undefined URI here; failing closed is the honest form of that, because
	//! there is no text to show on either side.
	None,
};

//!
//! @brief One row's comparison, as upstream's `getResources` collapses it.
//!
struct GitDiffInput final {
	EGitDiffCommandKind kind{ EGitDiffCommandKind::None };
	//! Upstream's `leftUri`. Present exactly when `kind` is `Diff`.
	std::optional<GitDiffEndpoint> original;
	//! Upstream's `rightUri`. Absent only when `kind` is `None`.
	std::optional<GitDiffEndpoint> modified;
	//! Upstream's `getTitle()`, already carrying the basename. Empty exactly
	//! where upstream's `default` arm returns `''`.
	std::wstring title;

	[[nodiscard]] bool operator==(const GitDiffInput&) const = default;
};

//!
//! @brief One published row, as the diff resolver needs to see it.
//!
struct GitDiffRow final {
	EGitFileStatus status{ EGitFileStatus::Modified };
	//! Upstream's `resource.resourceUri`: the name the row renders. For a rename
	//! or a copy that is the **new** path, which is also what porcelain v2 puts
	//! in `GitChange::path`.
	std::wstring path;
	//! Upstream's `resource.original`: the pre-rename path. Equal to `path` when
	//! git reported no rename, so a caller never has to special-case it.
	std::wstring originalPath;
	//!
	//! @brief The same path also has a row in Staged Changes.
	//!
	//! Read by exactly one rule, upstream's `sanitizeRef('~')`: an unstaged edit
	//! is compared against the index when something is staged for that path, and
	//! against HEAD when nothing is. Getting this wrong shows the user a diff
	//! that silently includes their staged work.
	//!
	bool stagedInIndex{};
};

//! Fill a row from one parsed change, given the status its area produced.
[[nodiscard]] GitDiffRow MakeGitDiffRow(const GitChange& change, EGitFileStatus status, bool stagedInIndex);

//! Upstream's `Resource.getTitle()`, unlocalized.
[[nodiscard]] std::wstring BuildGitDiffTitle(EGitFileStatus status, std::wstring_view path);

//! Upstream's `getLeftResource` / `getRightResource` / `getResources`, collapsed.
[[nodiscard]] GitDiffInput ResolveGitDiffInput(const GitDiffRow& row);

//! Upstream's `Repository.buffer`: `show --textconv <ref>:<path>`. Empty for a
//! working-tree endpoint, which is read from disk and never from git.
[[nodiscard]] std::vector<std::wstring> BuildGitShowArguments(const GitDiffEndpoint& endpoint);

//! Upstream's `toGitUri` scheme, and the one this subsystem answers to.
inline constexpr std::wstring_view kGitUriScheme = L"git";

//!
//! @brief The `git:` URI query, as upstream's `toGitUri` serializes it.
//!
//! Upstream writes `JSON.stringify({ path: uri.fsPath, ref })`, so `path` is an
//! **absolute** platform path even though the URI's own path component is the
//! same file's URI form. Both halves are kept because both are load-bearing:
//! the URI path is what makes two sides of a comparison look like the same
//! file, and the query path is what the provider actually reads.
//!
struct GitUriParams final {
	//! Upstream's `uri.fsPath`: an absolute Windows path.
	std::wstring path;
	//! Empty is the index, exactly as in `GitDiffEndpoint::ref`.
	std::wstring ref;

	[[nodiscard]] bool operator==(const GitUriParams&) const = default;
};

//! Serialize a `git:` URI query. Upstream's member order, which a consumer
//! comparing two URIs as strings depends on.
[[nodiscard]] std::wstring BuildGitUriQuery(const GitUriParams& params);

//! Fails closed on a malformed query, an unknown member, or a missing `path`.
//! An absent `ref` is not missing: it is upstream's index reference.
[[nodiscard]] std::optional<GitUriParams> ParseGitUriQuery(std::wstring_view query);

//!
//! @brief The URI a published resource command names this side by.
//!
//! A working-tree side is the plain `file:` URI of the document on disk — the
//! same URI the row itself renders — and a repository side is `toGitUri` of
//! that file plus the ref. Empty when the endpoint's path cannot be joined onto
//! the repository root, which is the same fail-closed answer as refusing to
//! read it.
//!
[[nodiscard]] std::wstring BuildGitDiffEndpointUri(
	const GitDiffEndpoint& endpoint, std::wstring_view repositoryRoot);

//!
//! @brief Turn a published URI back into the side it names.
//!
//! The inverse of `BuildGitDiffEndpointUri`. A URI whose file does not lie
//! inside `repositoryRoot` yields nothing: this subsystem may only compare
//! files of the repository the command was issued against, and a path that
//! escapes the root is exactly what that rule exists to refuse.
//!
[[nodiscard]] std::optional<GitDiffEndpoint> ResolveGitDiffEndpointUri(
	std::wstring_view uri, std::wstring_view repositoryRoot);

//!
//! @brief VS Code's `LineRange`: `[startLineNumber, endLineNumberExclusive)`, 1-based.
//!
//! Empty is a real, meaningful value: an insertion has an empty original range
//! and a deletion an empty modified range, and the position of that empty range
//! is what says where the lines went.
//!
struct GitLineRange final {
	int startLineNumber{ 1 };
	int endLineNumberExclusive{ 1 };

	[[nodiscard]] bool IsEmpty() const noexcept { return startLineNumber == endLineNumberExclusive; }
	[[nodiscard]] int Length() const noexcept { return endLineNumberExclusive - startLineNumber; }
	[[nodiscard]] bool operator==(const GitLineRange&) const = default;
};

//! VS Code's `LineRangeMapping`: one changed region, on both sides.
struct GitLineRangeMapping final {
	GitLineRange original;
	GitLineRange modified;

	[[nodiscard]] bool operator==(const GitLineRangeMapping&) const = default;
};

//!
//! @brief VS Code's `IDocumentDiff`, minus what this product does not compute.
//!
//! `hitTimeout` is upstream's own field and carries the same meaning: the diff
//! is **not** authoritative. Upstream bounds the work by a clock; a pure model
//! has none, so the bound here is the edit distance, which is the quantity the
//! clock was standing in for. Reporting it is what keeps a bounded result from
//! being mistaken for a complete one.
//!
struct GitLineDiff final {
	std::vector<GitLineRangeMapping> changes;
	bool hitTimeout{};

	[[nodiscard]] bool operator==(const GitLineDiff&) const = default;
};

//!
//! @brief Split decoded text into lines the way a VS Code text model does.
//!
//! `\r\n`, `\n`, and a lone `\r` all end a line, and a trailing terminator
//! produces a final empty line — `"a\n"` is two lines, not one. That is not a
//! detail: line-level staging rebuilds the file from these lines, so a model
//! that swallowed the last empty line would drop the file's final newline.
//!
[[nodiscard]] std::vector<std::wstring> SplitGitDiffLines(std::wstring_view text);

//! Line-level diff of two texts already split into lines.
[[nodiscard]] GitLineDiff ComputeGitLineDiff(
	const std::vector<std::wstring>& original, const std::vector<std::wstring>& modified);

//!
//! @brief One vertical position of a side-by-side diff view.
//!
//! VS Code's side-by-side diff editor keeps the two sides vertically aligned by
//! inserting view zones, so a row is a *pair* of optional lines rather than a
//! line of one document. A deletion is a row whose modified side is absent and
//! an insertion is a row whose original side is absent; both are real rows and
//! occupy height on the other side too, which is what keeps the surrounding
//! unchanged text level.
//!
struct GitDiffViewRow final {
	//! Inside one of the diff's `changes`. Upstream paints a present original
	//! line in a changed region as a deletion and a present modified line as an
	//! insertion, so this one flag plus the two line numbers is the whole
	//! rendering decision.
	bool changed{};
	//! 1-based line on the original side, or `0` when this row has none.
	int originalLineNumber{};
	//! 1-based line on the modified side, or `0` when this row has none.
	int modifiedLineNumber{};

	[[nodiscard]] bool operator==(const GitDiffViewRow&) const = default;
};

//!
//! @brief Align both sides into the rows a side-by-side view renders.
//!
//! The counts are the two documents' line counts, i.e. `SplitGitDiffLines`'
//! sizes; the ranges in `diff` index those same lines. A changed region starts
//! level on both sides and the shorter side is padded at the region's **end**.
//! Upstream can additionally align inside a region when its character-level
//! inner diff produces alignment points; that inner diff is not computed here,
//! and the divergence is recorded in this directory's CLAUDE.md.
//!
[[nodiscard]] std::vector<GitDiffViewRow> BuildGitDiffViewRows(
	int originalLineCount, int modifiedLineCount, const GitLineDiff& diff);

} // namespace workbench::scm
