/*! @file
 * @brief Staging a selected part of a change, as upstream's `staging.ts` does it.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/scm/GitDiffModel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//!
//! @brief VS Code's `LineChange` (`extensions/git/src/staging.ts`).
//!
//! This is **not** `GitLineRangeMapping` renamed. That type is the modern
//! half-open `LineRange` pair the diff produces; this one is the older
//! inclusive-end form the staging algorithm is written against, and its two
//! degenerate encodings carry meaning that a half-open range cannot express:
//! an insertion sets `originalEndLineNumber` to `0` and puts the line it
//! follows in `originalStartLineNumber`, and a deletion does the same on the
//! modified side. Upstream keeps both types for that reason, and `staging.ts`
//! converts between them in `toLineChanges`; converting once here rather than
//! rewriting the algorithm is what keeps it checkable against upstream.
//!
struct GitLineChange final {
	int originalStartLineNumber{};
	int originalEndLineNumber{};
	int modifiedStartLineNumber{};
	int modifiedEndLineNumber{};

	//! Nothing on the original side: these lines only exist in the modified text.
	[[nodiscard]] bool IsInsertion() const noexcept { return originalEndLineNumber == 0; }
	//! Nothing on the modified side: these lines only exist in the original text.
	[[nodiscard]] bool IsDeletion() const noexcept { return modifiedEndLineNumber == 0; }

	[[nodiscard]] bool operator==(const GitLineChange&) const = default;
};

//! Upstream's `toLineChanges`: the diff's half-open ranges in the inclusive form
//! the staging algorithm reads.
[[nodiscard]] std::vector<GitLineChange> ToGitLineChanges(const GitLineDiff& diff);

//! Upstream's `invertLineChange`: the same change seen from the other side. This
//! is the whole of how unstage and revert reuse the stage algorithm.
[[nodiscard]] GitLineChange InvertGitLineChange(const GitLineChange& change);

//! Upstream's `compareLineChanges`, as a strict ordering.
[[nodiscard]] bool IsGitLineChangeBefore(const GitLineChange& left, const GitLineChange& right) noexcept;

//!
//! @brief One side of a staging computation, as a text model would hold it.
//!
//! A VS Code `TextDocument` has exactly one end-of-line sequence no matter what
//! the bytes contained, because the model normalizes on load. `applyLineChanges`
//! depends on that: it reads text out of two documents and concatenates it, and
//! each side contributes its own document's terminator. Keeping `eol` beside the
//! lines is what makes that reproducible instead of guessed at the join.
//!
struct GitStagingText final {
	std::vector<std::wstring> lines;
	std::wstring eol{ L"\n" };

	[[nodiscard]] int LineCount() const noexcept { return static_cast<int>(lines.size()); }

	[[nodiscard]] bool operator==(const GitStagingText&) const = default;
};

//!
//! @brief The terminator a VS Code text model would give this text.
//!
//! Upstream's `PieceTreeTextBufferBuilder._getEOL`: `\r\n` when more than half
//! of the line terminators carry a carriage return, `\n` otherwise. A text with
//! no terminator at all has no evidence, so upstream falls back to the `files.eol`
//! setting, whose `auto` default is the platform's — `\r\n` here.
//!
[[nodiscard]] std::wstring DetectGitTextEol(std::wstring_view text);

//! Split and detect together, which is what opening a document does.
[[nodiscard]] GitStagingText MakeGitStagingText(std::wstring_view text);

//!
//! @brief A selected span of whole lines, 0-based and inclusive on both ends.
//!
//! Upstream selects with `Range`s carrying characters and then widens each one
//! to whole lines in `toLineRanges`, because staging cannot act on part of a
//! line. This type is that widened form, so the widening cannot be forgotten.
//!
struct GitSelectedLines final {
	int startLine{};
	int endLine{};

	[[nodiscard]] bool operator==(const GitSelectedLines&) const = default;
};

//!
//! @brief Upstream's `toLineRanges`: sort the selections and merge what touches.
//!
//! Two spans merge when they overlap or when one begins on the line after the
//! other ends, which is upstream's rule. The divergence is what happens when
//! they overlap; it is recorded in this directory's CLAUDE.md.
//!
[[nodiscard]] std::vector<GitSelectedLines> NormalizeGitSelectedLines(
	std::vector<GitSelectedLines> selections, const GitStagingText& modified);

//!
//! @brief Upstream's `intersectDiffWithRange`: the part of one change a
//!        selection covers.
//!
//! Nothing when the selection does not reach the change. A deletion is returned
//! whole, because there is no modified text to take a part of. Everything else
//! is narrowed to the selected lines, and the original side follows only when
//! both sides changed by the same number of lines — upstream's own heuristic,
//! and the reason a lopsided change is staged whole.
//!
[[nodiscard]] std::optional<GitLineChange> IntersectGitLineChange(
	const GitStagingText& modified, const GitLineChange& change, const GitSelectedLines& selection);

//!
//! @brief The changes a selection stages, in upstream's `stageSelectedChanges` order.
//!
//! A change is kept at most once, narrowed by the **first** selection that
//! reaches it. Upstream's `reduce` short-circuits on the first non-null result,
//! so a change spanned by two selections is not staged twice and not widened to
//! their union.
//!
[[nodiscard]] std::vector<GitLineChange> SelectGitLineChanges(
	const GitStagingText& modified,
	const std::vector<GitLineChange>& changes,
	const std::vector<GitSelectedLines>& selections);

//!
//! @brief Upstream's `applyLineChanges`: the original text with those changes applied.
//!
//! This is the whole staging model: upstream builds no patch and asks git to
//! apply nothing. It assembles the complete new content of the index entry and
//! writes that, so a hunk that would not apply cleanly is not a failure mode
//! that exists here.
//!
[[nodiscard]] std::wstring ApplyGitLineChanges(
	const GitStagingText& original, const GitStagingText& modified, const std::vector<GitLineChange>& changes);

//!
//! @brief Which decoding `DecodeGitOutput` used for these bytes.
//!
//! Staging is the first thing in this subsystem that has to write bytes back,
//! and a decoder with a fallback is only half of a round trip. Recording which
//! branch ran is what lets the inverse be exact instead of hopeful.
//!
enum class EGitTextEncoding : std::uint8_t {
	//! `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` succeeded.
	Utf8,
	//! It did not, and the decoder widened each byte. Only `U+0000`–`U+00FF` survive.
	Latin1Fallback,
};

//! Which branch `DecodeGitOutput` would take for these bytes.
[[nodiscard]] EGitTextEncoding ClassifyGitTextEncoding(std::string_view bytes) noexcept;

//!
//! @brief The exact inverse of `DecodeGitOutput` for the encoding it used.
//!
//! Nothing when the text cannot be represented: an unpaired surrogate for UTF-8,
//! or a character above `U+00FF` for the byte-wise fallback. A staged blob is
//! durable content, so substituting a replacement character would write
//! corruption into the index under the name of a successful stage.
//!
[[nodiscard]] std::optional<std::string> EncodeGitText(std::wstring_view text, EGitTextEncoding encoding);

//!
//! @brief Upstream's `Repository.stage` step 1: `hash-object --stdin -w --path <path>`.
//!
//! `--path` is not cosmetic. It is what makes git apply the `.gitattributes`
//! filters that belong to that path — `text=auto` line-ending conversion above
//! all — so the blob written here matches the blob a plain `git add` would write.
//!
[[nodiscard]] std::vector<std::wstring> BuildGitHashObjectArguments(std::wstring_view path);

//!
//! @brief Upstream's `getObjectDetails` for a repository that has a HEAD commit.
//!
//! `ls-tree -l HEAD -- <path>`. Upstream reads the mode from HEAD rather than
//! from the index so that staging part of a change cannot silently change the
//! file's mode.
//!
[[nodiscard]] std::vector<std::wstring> BuildGitHeadObjectDetailsArguments(std::wstring_view path);

//! Upstream's `lsfiles`, used when the repository has no HEAD commit yet:
//! `ls-files --stage -- <path>`.
[[nodiscard]] std::vector<std::wstring> BuildGitStagedObjectDetailsArguments(std::wstring_view path);

//!
//! @brief The mode from an `ls-tree -l` or `ls-files --stage` line.
//!
//! Both formats begin with the mode, so one parser serves both. Nothing when the
//! path is not known there, which is upstream's `UnknownPath` and its signal to
//! add the entry at `100644` instead.
//!
[[nodiscard]] std::optional<std::wstring> ParseGitObjectMode(std::wstring_view text);

//! Upstream's `Repository.stage` step 2:
//! `update-index [--add] --cacheinfo <mode> <object> <path>`.
[[nodiscard]] std::vector<std::wstring> BuildGitUpdateIndexArguments(
	std::wstring_view mode, std::wstring_view object, std::wstring_view path, bool add);

//! The object name `hash-object` wrote, or nothing when git did not print one.
[[nodiscard]] std::optional<std::wstring> ParseGitHashObjectName(std::wstring_view text);

} // namespace workbench::scm
