/*! @file
 * @brief `SCMHistoryProvider`: the commits behind the Source Control Graph view.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

//! Which kind of ref decorates a commit. Upstream's Graph renders a local
//! branch, a remote branch, and a tag as three visually distinct badges, so the
//! kind has to survive parsing rather than being recovered from the name later.
enum class EGitHistoryRefKind : std::uint8_t {
	Head,
	LocalBranch,
	RemoteBranch,
	Tag,
};

struct GitHistoryRef final {
	EGitHistoryRefKind kind{ EGitHistoryRefKind::LocalBranch };
	//! The ref's short name (`main`, `origin/main`, `v1.2`), never its full
	//! `refs/...` path: upstream's badge shows the short name.
	std::wstring name;

	[[nodiscard]] bool operator==(const GitHistoryRef&) const = default;
};

//! One `ISCMHistoryItem`.
struct GitHistoryItem final {
	std::wstring id;
	std::vector<std::wstring> parentIds;
	std::wstring subject;
	//! The commit's full message (`%B`), which is what upstream's `Copy Commit
	//! Message` puts on the clipboard. Kept separate from `subject` because a
	//! one-line row and a clipboard payload are not the same text.
	std::wstring message;
	std::wstring authorName;
	std::wstring authorEmail;
	//! Author date, seconds since the epoch, as `%at` reports it.
	std::int64_t authorTimestamp{};
	std::vector<GitHistoryRef> refs;

	[[nodiscard]] bool operator==(const GitHistoryItem&) const = default;
};

//!
//! @brief The field and record separators `MakeHistoryFormat` asks git for.
//!
//! `%x1f` and `%x1e` are the ASCII unit and record separators. They are used
//! instead of a newline because a commit subject cannot be assumed free of any
//! printable character, and instead of `-z` because `git log -z` separates
//! entries but leaves the fields inside one entry ambiguous.
//!
constexpr wchar_t kGitHistoryFieldSeparator = L'\x1f';
constexpr wchar_t kGitHistoryRecordSeparator = L'\x1e';

//! `--format=` for the query above: id, parents, refs, author, email, date,
//! subject, full message. The order is the order `ParseGitHistory` reads, and
//! the multi-line `%B` is last so its newlines cannot be mistaken for a field
//! boundary.
[[nodiscard]] std::wstring MakeGitHistoryFormat();

//! `log` arguments for the newest `maximumCount` commits reachable from HEAD.
//! `--topo-order` is what keeps a branch's commits contiguous, which is what
//! makes the swimlanes below continuous rather than interleaved by date.
[[nodiscard]] std::vector<std::wstring> MakeGitHistoryArguments(std::size_t maximumCount);

//! Parse the UTF-8 output of that query. A malformed record is skipped rather
//! than truncating the history at it.
[[nodiscard]] std::vector<GitHistoryItem> ParseGitHistory(std::string_view bytes);

//! Parse one `%D` decoration list (`HEAD -> main, origin/main, tag: v1`).
[[nodiscard]] std::vector<GitHistoryRef> ParseGitHistoryRefs(std::wstring_view decorations);

//!
//! @brief One vertical line passing through a row, upstream's "swimlane".
//!
//! `id` is the commit the lane is currently waiting to draw; `colorIndex` is an
//! index into the renderer's own palette, kept as an index so the model stays
//! free of colours.
//!
struct ScmGraphSwimlane final {
	std::wstring id;
	std::size_t colorIndex{};

	[[nodiscard]] bool operator==(const ScmGraphSwimlane&) const = default;
};

//! What one rendered row needs: the lanes entering it from above, the lanes
//! leaving it below, and where its own commit circle sits.
struct ScmGraphRow final {
	std::vector<ScmGraphSwimlane> inputSwimlanes;
	std::vector<ScmGraphSwimlane> outputSwimlanes;
	std::size_t circleLane{};
	std::size_t circleColorIndex{};

	[[nodiscard]] bool operator==(const ScmGraphRow&) const = default;
};

//! How many distinct lane colours the model cycles through. It is upstream's
//! own count, so a renderer's palette must have exactly this many entries.
constexpr std::size_t kScmGraphColorCount = 5;

//!
//! @brief Upstream's `toISCMHistoryItemViewModelArray`.
//!
//! Walks the commits in the order given, carrying the set of open swimlanes
//! from row to row: a lane waiting for this commit becomes the commit's circle
//! and is handed to its first parent, further lanes waiting for the same commit
//! are merged away, and every additional parent opens a new lane.
//!
[[nodiscard]] std::vector<ScmGraphRow> BuildScmHistoryGraph(const std::vector<GitHistoryItem>& items);

//!
//! @brief The command payload naming one history item.
//!
//! Upstream's `scm/historyItem/context` commands receive the item itself. A
//! command here carries a serialized payload instead, and a commit is fully
//! identified by its id, so the payload is that id as a JSON string and nothing
//! else. The receiver looks the commit up in the history it already holds
//! rather than trusting a caller's copy of its message.
//!
[[nodiscard]] std::string BuildGitHistoryItemArguments(std::wstring_view historyItemId);

//! Nothing when the payload is not one JSON string holding a plausible commit
//! id: a caller that cannot name a commit must not have one chosen for it.
[[nodiscard]] std::optional<std::wstring> ParseGitHistoryItemArguments(std::string_view argumentsJson);

} // namespace workbench::scm
