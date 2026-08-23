/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitScmModel.h"
#include "workbench/scm/GitCommandRunner.h"

#include <charconv>
#include <utility>

namespace workbench::scm {
namespace {

std::wstring FromUtf8(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return std::wstring(value.begin(), value.end());
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
	return result;
}

std::string_view FieldAfterSpaces(std::string_view record, int spaces)
{
	std::size_t position = 0;
	for (int count = 0; count < spaces; ++count) {
		position = record.find(' ', position);
		if (position == std::string_view::npos) return {};
		++position;
	}
	return record.substr(position);
}

int ParseSigned(std::string_view value)
{
	if (!value.empty() && value.front() == '+') value.remove_prefix(1);
	int result{};
	std::from_chars(value.data(), value.data() + value.size(), result);
	return result;
}

GitChange MakeChange(
	const wchar_t indexStatus,
	const wchar_t worktreeStatus,
	std::wstring path,
	const bool conflicted = false,
	std::wstring originalPath = {})
{
	const bool untracked = indexStatus == L'?' && worktreeStatus == L'?';
	const wchar_t status = conflicted ? L'!' : untracked ? L'U'
		: worktreeStatus != L'.' ? worktreeStatus : indexStatus != L'.' ? indexStatus : L'M';
	return {
		.status = status,
		.path = std::move(path),
		.indexStatus = indexStatus,
		.worktreeStatus = worktreeStatus,
		.untracked = untracked,
		.conflicted = conflicted,
		.originalPath = std::move(originalPath),
	};
}

} // namespace

EGitStatusRefreshDisposition ClassifyGitStatusRefresh(EGitExecutionStatus status) noexcept
{
	return status == EGitExecutionStatus::Succeeded
		? EGitStatusRefreshDisposition::ApplySnapshot
		: EGitStatusRefreshDisposition::RetainSnapshot;
}

GitScmState ParsePorcelainV2(std::string_view bytes)
{
	GitScmState state;
	std::size_t position = 0;
	while (position < bytes.size()) {
		const auto end = bytes.find('\0', position);
		const auto record = bytes.substr(position, end == std::string_view::npos ? bytes.size() - position : end - position);
		position = end == std::string_view::npos ? bytes.size() : end + 1;
		if (record.starts_with("# branch.oid ")) {
			state.repository = true;
			// `(initial)` means there is no commit yet, which is upstream's
			// undefined `HEAD.commit`, not an object name to display.
			const auto oid = record.substr(13);
			if (oid != "(initial)") state.commit = FromUtf8(oid);
		} else if (record.starts_with("# branch.head ")) {
			state.repository = true;
			state.branch = FromUtf8(record.substr(14));
			// A detached HEAD has no branch name upstream either; naming it
			// "detached HEAD" would invent a branch that does not exist, and the
			// short object name is what VS Code shows in that state.
			if (state.branch == L"(detached)") state.branch.clear();
		} else if (record.starts_with("# branch.upstream ")) {
			state.upstream = FromUtf8(record.substr(18));
		} else if (record.starts_with("# branch.ab ")) {
			const auto values = record.substr(12);
			const auto separator = values.find(' ');
			state.ahead = ParseSigned(values.substr(0, separator));
			if (separator != std::string_view::npos) state.behind = -ParseSigned(values.substr(separator + 1));
		} else if (record.starts_with("? ")) {
			state.changes.push_back(MakeChange(L'?', L'?', FromUtf8(record.substr(2))));
		} else if (record.starts_with("! ")) {
			continue;
		} else if (!record.empty() && (record[0] == '1' || record[0] == '2' || record[0] == 'u')) {
			const wchar_t x = record.size() > 2 ? static_cast<unsigned char>(record[2]) : L'.';
			const wchar_t y = record.size() > 3 ? static_cast<unsigned char>(record[3]) : L'.';
			const int pathField = record[0] == '2' ? 9 : record[0] == 'u' ? 10 : 8;
			const auto path = FieldAfterSpaces(record, pathField);
			if (!path.empty()) {
				std::wstring originalPath;
				if (record[0] == '2' && position < bytes.size()) {
					// -z rename records append the original path as an extra NUL record.
					const auto originalEnd = bytes.find('\0', position);
					const auto original = bytes.substr(position,
						originalEnd == std::string_view::npos ? bytes.size() - position : originalEnd - position);
					originalPath = FromUtf8(original);
					position = originalEnd == std::string_view::npos ? bytes.size() : originalEnd + 1;
				}
				state.changes.push_back(MakeChange(x, y, FromUtf8(path), record[0] == 'u', std::move(originalPath)));
			}
		}
	}
	return state;
}

std::optional<EGitFileStatus> ClassifyGitFileStatus(const GitChange& change, EGitChangeArea area) noexcept
{
	// Upstream's first switch is on `raw.x + raw.y`, and every one of its cases
	// returns before the per-area switches run. A conflicted or untracked path
	// therefore has exactly one status no matter which area asks for it.
	if (change.conflicted) {
		const wchar_t ours = change.indexStatus;
		const wchar_t theirs = change.worktreeStatus;
		if (ours == L'D' && theirs == L'D') return EGitFileStatus::BothDeleted;
		if (ours == L'A' && theirs == L'U') return EGitFileStatus::AddedByUs;
		if (ours == L'U' && theirs == L'D') return EGitFileStatus::DeletedByThem;
		if (ours == L'U' && theirs == L'A') return EGitFileStatus::AddedByThem;
		if (ours == L'D' && theirs == L'U') return EGitFileStatus::DeletedByUs;
		if (ours == L'A' && theirs == L'A') return EGitFileStatus::BothAdded;
		if (ours == L'U' && theirs == L'U') return EGitFileStatus::BothModified;
		// An unmerged entry whose XY matches none of upstream's seven cases falls
		// through upstream's switch into the per-area ones, where `U` matches no
		// case either. No row is what upstream produces, so no status is what this
		// returns; inventing `BothModified` here would offer to diff two sides
		// git never said existed.
		return std::nullopt;
	}
	if (change.untracked) return EGitFileStatus::Untracked;

	if (area == EGitChangeArea::Index) {
		switch (change.indexStatus) {
		case L'M': return EGitFileStatus::IndexModified;
		case L'A': return EGitFileStatus::IndexAdded;
		case L'D': return EGitFileStatus::IndexDeleted;
		case L'R': return EGitFileStatus::IndexRenamed;
		case L'C': return EGitFileStatus::IndexCopied;
		default: break;
		}
		// Upstream lists no index `T`, so a type change staged in the index
		// contributes only its working-tree row.
		return std::nullopt;
	}

	switch (change.worktreeStatus) {
	case L'M': return EGitFileStatus::Modified;
	case L'D': return EGitFileStatus::Deleted;
	case L'A': return EGitFileStatus::IntentToAdd;
	case L'R': return EGitFileStatus::IntentToRename;
	case L'T': return EGitFileStatus::TypeChanged;
	default: break;
	}
	return std::nullopt;
}

std::string_view GitFileStatusText(EGitFileStatus status) noexcept
{
	switch (status) {
	case EGitFileStatus::IndexModified: return "Index Modified";
	case EGitFileStatus::Modified: return "Modified";
	case EGitFileStatus::IndexAdded: return "Index Added";
	case EGitFileStatus::IndexDeleted: return "Index Deleted";
	case EGitFileStatus::Deleted: return "Deleted";
	case EGitFileStatus::IndexRenamed: return "Index Renamed";
	case EGitFileStatus::IndexCopied: return "Index Copied";
	case EGitFileStatus::Untracked: return "Untracked";
	case EGitFileStatus::IntentToAdd: return "Intent to Add";
	case EGitFileStatus::IntentToRename: return "Intent to Rename";
	case EGitFileStatus::TypeChanged: return "Type Changed";
	case EGitFileStatus::BothDeleted: return "Conflict: Both Deleted";
	case EGitFileStatus::AddedByUs: return "Conflict: Added By Us";
	case EGitFileStatus::DeletedByThem: return "Conflict: Deleted By Them";
	case EGitFileStatus::AddedByThem: return "Conflict: Added By Them";
	case EGitFileStatus::DeletedByUs: return "Conflict: Deleted By Us";
	case EGitFileStatus::BothAdded: return "Conflict: Both Added";
	case EGitFileStatus::BothModified: return "Conflict: Both Modified";
	}
	return {};
}

wchar_t GitFileStatusLetter(EGitFileStatus status) noexcept
{
	switch (status) {
	case EGitFileStatus::IndexModified:
	case EGitFileStatus::Modified:
		return L'M';
	case EGitFileStatus::IndexAdded:
	case EGitFileStatus::IntentToAdd:
		return L'A';
	case EGitFileStatus::IndexDeleted:
	case EGitFileStatus::Deleted:
		return L'D';
	case EGitFileStatus::IndexRenamed:
	case EGitFileStatus::IntentToRename:
		return L'R';
	case EGitFileStatus::TypeChanged:
		return L'T';
	case EGitFileStatus::Untracked:
		return L'U';
	case EGitFileStatus::IndexCopied:
		return L'C';
	default:
		// Upstream returns `!` for every conflict, with the comment that the
		// warning sign renders badly on Windows.
		return L'!';
	}
}

decorations::EFileDecorationColor GitFileStatusDecorationColor(EGitFileStatus status) noexcept
{
	switch (status) {
	case EGitFileStatus::IndexModified:
		return decorations::EFileDecorationColor::GitStageModified;
	case EGitFileStatus::Modified:
	case EGitFileStatus::TypeChanged:
		return decorations::EFileDecorationColor::GitModified;
	case EGitFileStatus::IndexDeleted:
		return decorations::EFileDecorationColor::GitStageDeleted;
	case EGitFileStatus::Deleted:
		return decorations::EFileDecorationColor::GitDeleted;
	case EGitFileStatus::IndexAdded:
	case EGitFileStatus::IntentToAdd:
		return decorations::EFileDecorationColor::GitAdded;
	case EGitFileStatus::IndexCopied:
	case EGitFileStatus::IndexRenamed:
	case EGitFileStatus::IntentToRename:
		return decorations::EFileDecorationColor::GitRenamed;
	case EGitFileStatus::Untracked:
		return decorations::EFileDecorationColor::GitUntracked;
	default:
		// Every conflict shares one color, the way every conflict shares `!`.
		return decorations::EFileDecorationColor::GitConflicting;
	}
}

bool DoesGitFileStatusPropagate(EGitFileStatus status) noexcept
{
	return status != EGitFileStatus::Deleted && status != EGitFileStatus::IndexDeleted;
}

bool IsGitFileStatusStruckThrough(EGitFileStatus status) noexcept
{
	switch (status) {
	case EGitFileStatus::Deleted:
	case EGitFileStatus::BothDeleted:
	case EGitFileStatus::DeletedByThem:
	case EGitFileStatus::DeletedByUs:
	case EGitFileStatus::IndexDeleted:
		return true;
	default:
		return false;
	}
}

} // namespace workbench::scm
