/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/GitDiffModel.h"

#include "sakura/uri/UriIdentity.h"
#include "workbench/commands/CommandArgumentsJson.h"
#include "workbench/scm/GitStageCommands.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace workbench::scm {
namespace {

//!
//! @brief Upstream's `sanitizeRelativePath`: backslashes become forward slashes.
//!
//! `git show <ref>:<path>` reads the path as a repository path, where `\` is a
//! literal character in a file name rather than a separator.
//!
std::wstring ToGitPath(std::wstring_view path)
{
	std::wstring result(path);
	std::replace(result.begin(), result.end(), L'\\', L'/');
	return result;
}

//!
//! @brief One separator form and one case, for comparing two paths.
//!
//! Windows paths are case-insensitive, so `C:\Repo\a.cpp` and `c:\repo\A.CPP`
//! name one file; a byte comparison would report the second as living outside
//! the first's repository and refuse to compare a file the user is looking at.
//!
std::wstring NormalizeForComparison(std::wstring_view path)
{
	std::wstring result(path);
	std::replace(result.begin(), result.end(), L'/', L'\\');
	std::transform(result.begin(), result.end(), result.begin(),
		[](wchar_t character) { return static_cast<wchar_t>(std::towupper(static_cast<std::wint_t>(character))); });
	while (!result.empty() && result.back() == L'\\') {
		result.pop_back();
	}
	return result;
}

//!
//! @brief The inverse of `JoinRepositoryPath`.
//!
//! Nothing when the path does not lie **inside** the root. The separator check
//! is what makes that a containment test rather than a spelling test: a sibling
//! `C:\RepoOther` starts with `C:\Repo` as a string and belongs to a different
//! repository, and comparing this one's file against it would be exactly the
//! wrong-repository mistake the root argument exists to prevent.
//!
std::optional<std::wstring> RelativeRepositoryPath(
	std::wstring_view repositoryRoot, std::wstring_view absolutePath)
{
	const auto root = NormalizeForComparison(repositoryRoot);
	const auto full = NormalizeForComparison(absolutePath);
	if (root.empty() || full.size() <= root.size()) return std::nullopt;
	if (full.compare(0, root.size(), root) != 0) return std::nullopt;
	if (full[root.size()] != L'\\') return std::nullopt;
	// Sliced out of the original, so the answer keeps the file's own spelling
	// rather than the upper-cased form the comparison needed.
	return ToGitPath(absolutePath.substr(root.size() + 1));
}

//! `path.basename`. The row's own path, so both separators must be honoured.
std::wstring_view BaseName(std::wstring_view path)
{
	const auto position = path.find_last_of(L"/\\");
	return position == std::wstring_view::npos ? path : path.substr(position + 1);
}

GitDiffEndpoint FromRepository(std::wstring_view ref, std::wstring_view path)
{
	return { EGitDiffSource::Repository, std::wstring(ref), ToGitPath(path) };
}

GitDiffEndpoint FromWorkingTree(std::wstring_view path)
{
	return { EGitDiffSource::WorkingTree, {}, ToGitPath(path) };
}

//!
//! @brief Upstream's `getLeftResource`.
//!
//! Every arm it does not list returns nothing, which is what turns the row's
//! click into `vscode.open` instead of `vscode.diff`.
//!
std::optional<GitDiffEndpoint> ResolveOriginal(const GitDiffRow& row)
{
	switch (row.status) {
	case EGitFileStatus::IndexModified:
	case EGitFileStatus::IndexRenamed:
	case EGitFileStatus::IntentToRename:
	case EGitFileStatus::TypeChanged:
		// `toGitUri(resource.original, 'HEAD')` — the pre-rename name, because
		// that is the only name HEAD knows the content under.
		return FromRepository(L"HEAD", row.originalPath);
	case EGitFileStatus::Modified:
		// `toGitUri(resource.resourceUri, '~')`, then `sanitizeRef`.
		return FromRepository(row.stagedInIndex ? std::wstring_view{} : std::wstring_view{ L"HEAD" }, row.path);
	case EGitFileStatus::DeletedByUs:
	case EGitFileStatus::DeletedByThem:
		// `~1` is the merge base.
		return FromRepository(L":1", row.path);
	default:
		return std::nullopt;
	}
}

//! Upstream's `getRightResource`.
std::optional<GitDiffEndpoint> ResolveModified(const GitDiffRow& row)
{
	switch (row.status) {
	case EGitFileStatus::IndexModified:
	case EGitFileStatus::IndexAdded:
	case EGitFileStatus::IndexCopied:
	case EGitFileStatus::IndexRenamed:
		// `toGitUri(resource.resourceUri, '')` — the index, under the new name.
		return FromRepository({}, row.path);
	case EGitFileStatus::IndexDeleted:
	case EGitFileStatus::Deleted:
		// A deleted file has no right-hand text at all, so upstream puts HEAD
		// there and leaves the left side empty: the click opens the content that
		// was removed rather than comparing against a file that is gone.
		return FromRepository(L"HEAD", row.path);
	case EGitFileStatus::DeletedByUs:
		return FromRepository(L":3", row.path);
	case EGitFileStatus::DeletedByThem:
		return FromRepository(L":2", row.path);
	case EGitFileStatus::Modified:
	case EGitFileStatus::Untracked:
	case EGitFileStatus::IntentToAdd:
	case EGitFileStatus::IntentToRename:
	case EGitFileStatus::TypeChanged:
	case EGitFileStatus::BothAdded:
	case EGitFileStatus::BothModified:
		// Upstream additionally looks the path up in the index group to pick up a
		// staged rename's new name. Porcelain v2 already reports the new name in
		// the change's own path, so that lookup would only ever find the same
		// string; the divergence is recorded in this directory's CLAUDE.md.
		return FromWorkingTree(row.path);
	default:
		return std::nullopt;
	}
}

//!
//! @brief Bound on Myers' edit distance, standing in for upstream's timeout.
//!
//! The trace this algorithm keeps for backtracking costs `(D + 1)^2` integers,
//! so the bound is a memory bound as much as a work bound. Two thousand edits
//! is roughly sixteen megabytes at the very worst and far beyond any diff a
//! human reads line by line.
//!
constexpr int kMaxEditDistance = 2000;

} // namespace

GitDiffRow MakeGitDiffRow(const GitChange& change, EGitFileStatus status, bool stagedInIndex)
{
	GitDiffRow row;
	row.status = status;
	row.path = change.path;
	// Upstream's `Resource.original` is the raw resource URI, which equals the
	// rendered one whenever there was no rename. Normalizing here means no caller
	// has to remember which statuses carry a second path.
	row.originalPath = change.originalPath.empty() ? change.path : change.originalPath;
	row.stagedInIndex = stagedInIndex;
	return row;
}

std::wstring BuildGitDiffTitle(EGitFileStatus status, std::wstring_view path, const GitDiffTextResolver& text)
{
	const std::wstring basename(BaseName(path));
	const auto localized = [&text, &basename](std::string_view key, std::wstring_view fallback) {
		std::wstring result = text ? text(key, basename) : std::wstring{};
		if (result.empty()) result.assign(fallback);
		std::size_t position = 0;
		while ((position = result.find(L"{0}", position)) != std::wstring::npos) {
			result.replace(position, 3, basename);
			position += basename.size();
		}
		return result;
	};
	switch (status) {
	case EGitFileStatus::IndexModified:
	case EGitFileStatus::IndexRenamed:
	case EGitFileStatus::IndexAdded:
		return localized("GitDiffIndex", L"{0} (Index)");
	case EGitFileStatus::Modified:
	case EGitFileStatus::BothAdded:
	case EGitFileStatus::BothModified:
		return localized("GitDiffWorkingTree", L"{0} (Working Tree)");
	case EGitFileStatus::IndexDeleted:
	case EGitFileStatus::Deleted:
		return localized("GitDiffDeleted", L"{0} (Deleted)");
	case EGitFileStatus::DeletedByUs:
		return localized("GitDiffTheirs", L"{0} (Theirs)");
	case EGitFileStatus::DeletedByThem:
		return localized("GitDiffOurs", L"{0} (Ours)");
	case EGitFileStatus::Untracked:
		return localized("GitDiffUntracked", L"{0} (Untracked)");
	case EGitFileStatus::IntentToAdd:
	case EGitFileStatus::IntentToRename:
		return localized("GitDiffIntentToAdd", L"{0} (Intent to add)");
	case EGitFileStatus::TypeChanged:
		return localized("GitDiffTypeChanged", L"{0} (Type changed)");
	default:
		// `INDEX_COPIED` and the remaining conflicts land here, exactly as they
		// land in upstream's `default` arm. An empty title is upstream's own
		// answer, not a missing one.
		return {};
	}
}

GitDiffInput ResolveGitDiffInput(const GitDiffRow& row, const GitDiffTextResolver& text)
{
	GitDiffInput input;
	input.original = ResolveOriginal(row);
	input.modified = ResolveModified(row);
	input.title = BuildGitDiffTitle(row.status, row.path, text);
	if (input.original) {
		input.kind = EGitDiffCommandKind::Diff;
	} else if (input.modified) {
		input.kind = EGitDiffCommandKind::Open;
	} else {
		input.kind = EGitDiffCommandKind::None;
	}
	return input;
}

std::vector<std::wstring> BuildGitShowArguments(const GitDiffEndpoint& endpoint)
{
	if (endpoint.source != EGitDiffSource::Repository) return {};
	// `--textconv` is upstream's; it applies a configured textconv filter so a
	// diff of a filtered file shows the same text the user configured git to
	// show. An empty ref is the index and must stay empty, producing `:path`.
	return { L"show", L"--textconv", endpoint.ref + L":" + endpoint.path };
}

std::wstring BuildGitUriQuery(const GitUriParams& params)
{
	// Built in UTF-8 because that is the one JSON vocabulary this product has,
	// then widened. The result is our own output, so the decode cannot fail.
	std::string json;
	json += "{\"path\":";
	commands::json::AppendQuoted(json, params.path);
	json += ",\"ref\":";
	commands::json::AppendQuoted(json, params.ref);
	json += '}';
	auto wide = commands::json::ToWideStrict(json);
	return wide.has_value() ? *std::move(wide) : std::wstring{};
}

std::optional<GitUriParams> ParseGitUriQuery(std::wstring_view query)
{
	const auto utf8 = commands::json::ToUtf8(query);
	if (utf8.empty()) return std::nullopt;

	std::size_t index = 0;
	if (!commands::json::Expect(utf8, index, '{')) return std::nullopt;

	GitUriParams params;
	bool sawPath = false;
	while (true) {
		std::string key;
		commands::json::SkipWhitespace(utf8, index);
		if (!commands::json::ReadString(utf8, index, key)) return std::nullopt;
		if (!commands::json::Expect(utf8, index, ':')) return std::nullopt;
		commands::json::SkipWhitespace(utf8, index);
		if (key == "path") {
			if (!commands::json::ReadWideString(utf8, index, params.path)) return std::nullopt;
			sawPath = true;
		}
		else if (key == "ref") {
			if (!commands::json::ReadWideString(utf8, index, params.ref)) return std::nullopt;
		}
		else {
			// `submoduleOf` is upstream's third member. Nothing here can act on
			// it, and reading the rest while ignoring it would silently compare
			// the wrong repository's file.
			return std::nullopt;
		}
		commands::json::SkipWhitespace(utf8, index);
		if (index < utf8.size() && utf8[index] == ',') {
			++index;
			continue;
		}
		break;
	}
	if (!commands::json::Expect(utf8, index, '}')) return std::nullopt;
	if (!commands::json::AtEnd(utf8, index)) return std::nullopt;
	// An empty ref is the index; an empty path names no file at all.
	return sawPath && !params.path.empty() ? std::optional{ std::move(params) } : std::nullopt;
}

std::wstring BuildGitDiffEndpointUri(const GitDiffEndpoint& endpoint, std::wstring_view repositoryRoot)
{
	const auto absolute = JoinRepositoryPath(repositoryRoot, endpoint.path);
	if (absolute.empty()) return {};
	const auto file = platform::uri::Uri::FromWindowsPath(absolute);
	if (!file) return {};
	if (endpoint.source == EGitDiffSource::WorkingTree) {
		return file.value->ToString();
	}
	const auto query = BuildGitUriQuery({ absolute, endpoint.ref });
	if (query.empty()) return {};
	// `uri.with({ scheme, query })`: the path component stays the file's own, so
	// both sides of a comparison still name the same file to anything that reads
	// the path, and the query carries what the provider actually reads.
	const auto gitUri = platform::uri::Uri::FromComponents(std::wstring(kGitUriScheme),
		file.value->Authority(), file.value->Path(), query, file.value->Fragment(),
		file.value->HasAuthority());
	return gitUri ? gitUri.value->ToString() : std::wstring{};
}

std::optional<GitDiffEndpoint> ResolveGitDiffEndpointUri(std::wstring_view uri, std::wstring_view repositoryRoot)
{
	const auto parsed = platform::uri::Uri::Parse(uri);
	if (!parsed) return std::nullopt;

	std::wstring absolute;
	std::wstring ref;
	EGitDiffSource source = EGitDiffSource::WorkingTree;
	if (parsed.value->Scheme() == kGitUriScheme) {
		// A `git:` URI with no query at all carries no ref and no path. It is not
		// an index reference with an empty ref; it names nothing.
		const auto& query = parsed.value->Query();
		if (!query.has_value()) return std::nullopt;
		auto params = ParseGitUriQuery(*query);
		if (!params.has_value()) return std::nullopt;
		source = EGitDiffSource::Repository;
		absolute = std::move(params->path);
		ref = std::move(params->ref);
	}
	else {
		const auto windowsPath = parsed.value->ToWindowsPath();
		if (!windowsPath) return std::nullopt;
		absolute = *windowsPath.value;
	}

	auto relative = RelativeRepositoryPath(repositoryRoot, absolute);
	if (!relative.has_value()) return std::nullopt;
	return GitDiffEndpoint{ source, std::move(ref), *std::move(relative) };
}

std::vector<std::wstring> SplitGitDiffLines(std::wstring_view text)
{
	std::vector<std::wstring> lines;
	std::size_t start = 0;
	for (std::size_t index = 0; index < text.size(); ++index) {
		const wchar_t character = text[index];
		if (character != L'\n' && character != L'\r') continue;
		lines.emplace_back(text.substr(start, index - start));
		if (character == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') ++index;
		start = index + 1;
	}
	// The remainder after the last terminator. It is pushed even when empty,
	// which is what gives a file ending in a newline its final empty line.
	lines.emplace_back(text.substr(start));
	return lines;
}

GitLineDiff ComputeGitLineDiff(const std::vector<std::wstring>& original, const std::vector<std::wstring>& modified)
{
	GitLineDiff diff;
	const int originalCount = static_cast<int>(original.size());
	const int modifiedCount = static_cast<int>(modified.size());

	// Common prefix and suffix are not an optimization here: they are what keeps
	// a one-line edit in a large file from costing a full Myers search.
	int prefix = 0;
	while (prefix < originalCount && prefix < modifiedCount && original[prefix] == modified[prefix]) ++prefix;
	int suffix = 0;
	while (suffix < originalCount - prefix && suffix < modifiedCount - prefix
		&& original[originalCount - 1 - suffix] == modified[modifiedCount - 1 - suffix]) {
		++suffix;
	}

	const int count = originalCount - suffix - prefix;
	const int other = modifiedCount - suffix - prefix;
	if (count == 0 && other == 0) return diff;

	const auto atOriginal = [&](int index) -> const std::wstring& { return original[prefix + index]; };
	const auto atModified = [&](int index) -> const std::wstring& { return modified[prefix + index]; };

	// A pure insertion or a pure deletion needs no search, and answering it
	// directly keeps the common "added a block of lines" case exact even when the
	// block is larger than the edit-distance bound below.
	const auto emit = [&](int originalStart, int originalEnd, int modifiedStart, int modifiedEnd) {
		diff.changes.push_back({
			GitLineRange{ prefix + originalStart + 1, prefix + originalEnd + 1 },
			GitLineRange{ prefix + modifiedStart + 1, prefix + modifiedEnd + 1 },
		});
	};
	if (count == 0 || other == 0) {
		emit(0, count, 0, other);
		return diff;
	}

	const int maximum = std::min(count + other, kMaxEditDistance);
	const int offset = maximum + 1;
	std::vector<int> reach(static_cast<std::size_t>(2 * offset + 1), 0);
	// The classic seed: at `d == 0` the rule reads `reach[k + 1]`, so diagonal 0
	// must start at x = 0.
	reach[static_cast<std::size_t>(offset + 1)] = 0;

	// One snapshot per step, holding only the diagonals that step can reach.
	std::vector<std::vector<int>> trace;
	trace.reserve(static_cast<std::size_t>(maximum) + 1);

	int distance = -1;
	for (int d = 0; d <= maximum; ++d) {
		for (int k = -d; k <= d; k += 2) {
			int x = 0;
			if (k == -d || (k != d && reach[static_cast<std::size_t>(offset + k - 1)]
				< reach[static_cast<std::size_t>(offset + k + 1)])) {
				x = reach[static_cast<std::size_t>(offset + k + 1)];
			} else {
				x = reach[static_cast<std::size_t>(offset + k - 1)] + 1;
			}
			int y = x - k;
			while (x < count && y < other && atOriginal(x) == atModified(y)) {
				++x;
				++y;
			}
			reach[static_cast<std::size_t>(offset + k)] = x;
			if (x >= count && y >= other) {
				distance = d;
				break;
			}
		}
		std::vector<int> snapshot(static_cast<std::size_t>(2 * d + 1), 0);
		for (int k = -d; k <= d; k += 2) {
			snapshot[static_cast<std::size_t>(k + d)] = reach[static_cast<std::size_t>(offset + k)];
		}
		trace.push_back(std::move(snapshot));
		if (distance >= 0) break;
	}

	if (distance < 0) {
		// The bound was reached before the two sides met. Reporting one region
		// that covers everything still unmatched is true — those lines did change
		// — while `hitTimeout` says the subdivision is missing.
		diff.hitTimeout = true;
		emit(0, count, 0, other);
		return diff;
	}

	// Backtrack the trace into single edits, newest first.
	struct Edit final {
		bool deletion{};
		int originalIndex{};
		int modifiedIndex{};
	};
	std::vector<Edit> edits;
	int x = count;
	int y = other;
	for (int d = distance; d > 0; --d) {
		const auto& previous = trace[static_cast<std::size_t>(d) - 1];
		const int k = x - y;
		int previousK = 0;
		if (k == -d) {
			previousK = k + 1;
		} else if (k == d) {
			previousK = k - 1;
		} else {
			previousK = previous[static_cast<std::size_t>(k - 1 + d - 1)] < previous[static_cast<std::size_t>(k + 1 + d - 1)]
				? k + 1
				: k - 1;
		}
		const int previousX = previous[static_cast<std::size_t>(previousK + d - 1)];
		const int previousY = previousX - previousK;
		while (x > previousX && y > previousY) {
			--x;
			--y;
		}
		// Exactly one of the two coordinates advanced past its predecessor, and
		// which one says whether this step deleted an original line or inserted a
		// modified one.
		edits.push_back({ y == previousY, previousX, previousY });
		x = previousX;
		y = previousY;
	}
	std::reverse(edits.begin(), edits.end());

	// Merge adjacent edits into one region per changed block, which is what a
	// `LineRangeMapping` is. A gap on either side means an unchanged line between
	// them, and therefore a new region.
	int originalStart = 0;
	int originalEnd = 0;
	int modifiedStart = 0;
	int modifiedEnd = 0;
	bool open = false;
	for (const auto& edit : edits) {
		if (open && (edit.originalIndex != originalEnd || edit.modifiedIndex != modifiedEnd)) {
			emit(originalStart, originalEnd, modifiedStart, modifiedEnd);
			open = false;
		}
		if (!open) {
			originalStart = originalEnd = edit.originalIndex;
			modifiedStart = modifiedEnd = edit.modifiedIndex;
			open = true;
		}
		if (edit.deletion) {
			++originalEnd;
		} else {
			++modifiedEnd;
		}
	}
	if (open) emit(originalStart, originalEnd, modifiedStart, modifiedEnd);
	return diff;
}

std::vector<GitDiffViewRow> BuildGitDiffViewRows(
	int originalLineCount, int modifiedLineCount, const GitLineDiff& diff)
{
	std::vector<GitDiffViewRow> rows;
	const int originals = std::max(originalLineCount, 0);
	const int modifieds = std::max(modifiedLineCount, 0);
	rows.reserve(static_cast<std::size_t>(std::max(originals, modifieds)));

	int original = 1;
	int modified = 1;
	for (const auto& change : diff.changes) {
		// The unchanged run before this region. Both sides advance together
		// because an unchanged line exists on both, which is exactly what makes
		// the two columns line up without any view zone.
		while (original < change.original.startLineNumber && original <= originals && modified <= modifieds) {
			rows.push_back({ false, original, modified });
			++original;
			++modified;
		}
		const int originalLength = std::max(change.original.Length(), 0);
		const int modifiedLength = std::max(change.modified.Length(), 0);
		for (int index = 0; index < std::max(originalLength, modifiedLength); ++index) {
			// A side that has run out of lines in this region contributes `0`,
			// which is the padding the other side needs to stay level.
			const int left = index < originalLength ? change.original.startLineNumber + index : 0;
			const int right = index < modifiedLength ? change.modified.startLineNumber + index : 0;
			rows.push_back({ true, left <= originals ? left : 0, right <= modifieds ? right : 0 });
		}
		original = change.original.endLineNumberExclusive;
		modified = change.modified.endLineNumberExclusive;
	}
	// The unchanged tail. Bounded by both counts so a diff that disagrees with
	// the documents it was computed from truncates rather than naming a line
	// neither side has.
	while (original <= originals && modified <= modifieds) {
		rows.push_back({ false, original, modified });
		++original;
		++modified;
	}
	return rows;
}

} // namespace workbench::scm
