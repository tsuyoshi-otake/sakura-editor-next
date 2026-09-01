/*! @file
 * @brief Typed, bounded parser for `git worktree list --porcelain -z`.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"
#include "workbench/worktree/GitWorktreePorcelainParser.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace workbench::worktree {
namespace {

GitWorktreeParseResult Failure(EGitWorktreeParseStatus status, std::size_t offset,
	std::string diagnostic)
{
	GitWorktreeParseResult result;
	result.status = status;
	result.byteOffset = offset;
	result.diagnostic = std::move(diagnostic);
	return result;
}

std::optional<std::wstring> StrictUtf8ToWide(std::string_view value)
{
	if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return std::nullopt;
	}
	const int required = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0) return std::nullopt;
	std::wstring converted(static_cast<std::size_t>(required), L'\0');
	if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), converted.data(), required) != required) {
		return std::nullopt;
	}
	return converted;
}

std::optional<std::wstring> NormalizeNfc(std::wstring_view value)
{
	if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return std::nullopt;
	}
	const int required = ::NormalizeString(NormalizationC, value.data(),
		static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0) return std::nullopt;
	std::wstring normalized(static_cast<std::size_t>(required), L'\0');
	const int written = ::NormalizeString(NormalizationC, value.data(),
		static_cast<int>(value.size()), normalized.data(), required);
	if (written <= 0) return std::nullopt;
	normalized.resize(static_cast<std::size_t>(written));
	return normalized;
}

std::optional<std::wstring> FoldInvariant(std::wstring_view value)
{
	if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
		return std::nullopt;
	}
	const int required = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
		value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
	if (required <= 0) return std::nullopt;
	std::wstring folded(static_cast<std::size_t>(required), L'\0');
	const int written = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
		value.data(), static_cast<int>(value.size()), folded.data(), required,
		nullptr, nullptr, 0);
	if (written <= 0) return std::nullopt;
	folded.resize(static_cast<std::size_t>(written));
	return folded;
}

bool IsDriveAbsolute(std::wstring_view path) noexcept
{
	return path.size() >= 3
		&& ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
		&& path[1] == L':' && path[2] == L'\\';
}

bool IsUncAbsolute(std::wstring_view path) noexcept
{
	if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\') return false;
	const auto serverEnd = path.find(L'\\', 2);
	if (serverEnd == std::wstring_view::npos || serverEnd == 2) return false;
	const auto shareEnd = path.find(L'\\', serverEnd + 1);
	return shareEnd == std::wstring_view::npos
		? serverEnd + 1 < path.size()
		: shareEnd > serverEnd + 1;
}

bool HasAmbiguousComponent(std::wstring_view path) noexcept
{
	std::size_t start = IsDriveAbsolute(path) ? 3 : 2;
	while (start < path.size()) {
		const auto end = path.find(L'\\', start);
		const auto length = (end == std::wstring_view::npos ? path.size() : end) - start;
		if (length != 0) {
			const auto component = path.substr(start, length);
			if (component == L"." || component == L"..") {
				if (end == std::wstring_view::npos) break;
				start = end + 1;
				continue;
			}
			const wchar_t last = path[start + length - 1];
			if (last == L'.' || last == L' ') return true;
			if (component.find(L':') != std::wstring_view::npos) return true;
		}
		if (end == std::wstring_view::npos) break;
		start = end + 1;
	}
	return false;
}

bool IsValidHead(std::string_view head) noexcept
{
	if (head.size() != 40 && head.size() != 64) return false;
	return std::all_of(head.begin(), head.end(), [](unsigned char value) {
		return std::isxdigit(value) != 0;
	});
}

bool IsValidUtf8(std::string_view value)
{
	if (value.empty()) return true;
	return StrictUtf8ToWide(value).has_value();
}

bool IsValidLocalBranchRef(std::string_view branch)
{
	constexpr std::string_view prefix = "refs/heads/";
	if (!branch.starts_with(prefix) || branch.size() == prefix.size() || !IsValidUtf8(branch)) {
		return false;
	}
	const auto tail = branch.substr(prefix.size());
	if (tail == "@" || tail.ends_with('/') || tail.ends_with('.') || tail.ends_with(".lock")
		|| tail.find("..") != std::string_view::npos || tail.find("@{") != std::string_view::npos) {
		return false;
	}
	std::size_t componentStart = 0;
	for (std::size_t index = 0; index < tail.size(); ++index) {
		const unsigned char value = static_cast<unsigned char>(tail[index]);
		if (value < 0x20 || value == 0x7f || value == ' ' || value == '~' || value == '^'
			|| value == ':' || value == '?' || value == '*' || value == '[' || value == '\\') {
			return false;
		}
		if (value == '/') {
			if (index == componentStart || tail[componentStart] == '.'
				|| tail.substr(componentStart, index - componentStart).ends_with('.')
				|| tail.substr(componentStart, index - componentStart).ends_with(".lock")) {
				return false;
			}
			componentStart = index + 1;
		}
	}
	return componentStart < tail.size() && tail[componentStart] != '.';
}

bool SameRecord(const GitWorktreeRecord& left, const GitWorktreeRecord& right) noexcept
{
	return left.head == right.head
		&& left.branch == right.branch
		&& left.detached == right.detached
		&& left.bare == right.bare
		&& left.locked == right.locked
		&& left.prunable == right.prunable
		&& left.lockReason == right.lockReason
		&& left.pruneReason == right.pruneReason;
}

struct PendingRecord final {
	std::optional<std::string> path;
	std::optional<std::string> head;
	std::optional<std::string> branch;
	bool detached = false;
	bool bare = false;
	bool locked = false;
	bool prunable = false;
	std::string lockReason;
	std::string pruneReason;
	std::size_t fields = 0;
	std::size_t bytes = 0;
};

} // namespace

std::optional<std::pair<std::wstring, std::wstring>> NormalizeWindowsWorktreePath(
	std::string_view utf8Path)
{
	auto wide = StrictUtf8ToWide(utf8Path);
	if (!wide) return std::nullopt;
	return NormalizeWindowsWorktreePath(*wide);
}

std::optional<std::pair<std::wstring, std::wstring>> NormalizeWindowsWorktreePath(
	std::wstring_view path)
{
	if (path.empty() || path.find(L'\0') != std::wstring_view::npos) return std::nullopt;
	std::wstring wide(path);
	std::replace(wide.begin(), wide.end(), L'/', L'\\');
	if (wide.starts_with(L"\\\\?\\") || wide.starts_with(L"\\\\.\\")) return std::nullopt;
	if (!IsDriveAbsolute(wide) && !IsUncAbsolute(wide)) return std::nullopt;
	if (HasAmbiguousComponent(wide)) return std::nullopt;

	const DWORD required = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
	if (required == 0 || required > 32767) return std::nullopt;
	std::wstring full(static_cast<std::size_t>(required), L'\0');
	const DWORD written = ::GetFullPathNameW(wide.c_str(), required, full.data(), nullptr);
	if (written == 0 || written >= required) return std::nullopt;
	full.resize(written);
	std::replace(full.begin(), full.end(), L'/', L'\\');
	if (!IsDriveAbsolute(full) && !IsUncAbsolute(full)) return std::nullopt;
	if (HasAmbiguousComponent(full)) return std::nullopt;
	if (IsDriveAbsolute(full)) full[0] = static_cast<wchar_t>(std::towupper(full[0]));
	while (full.size() > 3 && full.back() == L'\\') full.pop_back();

	auto normalized = NormalizeNfc(full);
	if (!normalized) return std::nullopt;
	auto identity = FoldInvariant(*normalized);
	if (!identity) return std::nullopt;
	return std::pair<std::wstring, std::wstring>{ std::move(*normalized), std::move(*identity) };
}

GitWorktreeParseResult ParseGitWorktreePorcelainZ(std::span<const std::uint8_t> input,
	const GitWorktreeParserLimits& limits)
{
	if (input.empty()) return Failure(EGitWorktreeParseStatus::EmptyInput, 0, "porcelain output is empty");
	if (input.size() > limits.maximumInputBytes) {
		return Failure(EGitWorktreeParseStatus::InputLimitExceeded, limits.maximumInputBytes,
			"porcelain output exceeds the input budget");
	}
	if (limits.maximumRecordBytes == 0 || limits.maximumFieldBytes == 0
		|| limits.maximumFieldsPerRecord == 0 || limits.maximumRecords == 0) {
		return Failure(EGitWorktreeParseStatus::MalformedRecord, 0, "parser limits are invalid");
	}

	std::vector<GitWorktreeRecord> records;
	records.reserve(std::min<std::size_t>(limits.maximumRecords, 32));
	std::unordered_map<std::wstring, std::size_t> identities;
	identities.reserve(std::min<std::size_t>(limits.maximumRecords, 32));
	PendingRecord pending;
	std::size_t offset = 0;
	std::size_t recordCount = 0;
	bool sawSeparator = false;

	auto finishRecord = [&](std::size_t recordOffset) -> std::optional<GitWorktreeParseResult> {
		if (pending.fields == 0) {
			return Failure(EGitWorktreeParseStatus::MalformedRecord, recordOffset,
				"unexpected empty record");
		}
		if (!pending.path) {
			return Failure(EGitWorktreeParseStatus::MalformedRecord, recordOffset,
				"record has no worktree path");
		}
		if (++recordCount > limits.maximumRecords) {
			return Failure(EGitWorktreeParseStatus::RecordLimitExceeded, recordOffset,
				"porcelain record count exceeds the budget");
		}
		if (pending.bare) {
			if (pending.head || pending.branch || pending.detached) {
				return Failure(EGitWorktreeParseStatus::MalformedRecord, recordOffset,
					"bare record has checkout-only fields");
			}
		} else {
			if (!pending.head) {
				return Failure(EGitWorktreeParseStatus::MalformedRecord, recordOffset,
					"record has no HEAD");
			}
			if (!IsValidHead(*pending.head)) {
				return Failure(EGitWorktreeParseStatus::InvalidHead, recordOffset,
					"HEAD is not a supported object identity");
			}
			if (pending.detached == pending.branch.has_value()) {
				return Failure(EGitWorktreeParseStatus::MalformedRecord, recordOffset,
					"record must name exactly one branch or detached state");
			}
		}

		auto path = NormalizeWindowsWorktreePath(*pending.path);
		if (!path) {
			return Failure(EGitWorktreeParseStatus::InvalidPath, recordOffset,
				"worktree path is not an unambiguous absolute Windows path");
		}
		GitWorktreeRecord record;
		record.path = std::move(path->first);
		record.identity = std::move(path->second);
		record.head = pending.head.value_or(std::string{});
		record.branch = std::move(pending.branch);
		record.detached = pending.detached;
		record.bare = pending.bare;
		record.locked = pending.locked;
		record.prunable = pending.prunable;
		record.lockReason = std::move(pending.lockReason);
		record.pruneReason = std::move(pending.pruneReason);

		const auto existing = identities.find(record.identity);
		if (existing != identities.end()) {
			if (!SameRecord(records[existing->second], record)) {
				return Failure(EGitWorktreeParseStatus::AmbiguousIdentity, recordOffset,
					"case-aliased worktree identity has conflicting metadata");
			}
		} else {
			identities.emplace(record.identity, records.size());
			records.push_back(std::move(record));
		}
		pending = PendingRecord{};
		return std::nullopt;
	};

	while (offset < input.size()) {
		const auto* begin = reinterpret_cast<const char*>(input.data() + offset);
		const auto remaining = input.size() - offset;
		const auto* terminator = static_cast<const char*>(std::memchr(begin, '\0', remaining));
		if (terminator == nullptr) {
			return Failure(EGitWorktreeParseStatus::MalformedRecord, offset,
				"final porcelain field is truncated");
		}
		const std::size_t fieldBytes = static_cast<std::size_t>(terminator - begin);
		const std::size_t fieldOffset = offset;
		offset += fieldBytes + 1;
		if (fieldBytes == 0) {
			auto failure = finishRecord(fieldOffset);
			if (failure) return std::move(*failure);
			sawSeparator = true;
			continue;
		}
		if (sawSeparator && pending.fields == 0) sawSeparator = false;
		if (fieldBytes > limits.maximumFieldBytes) {
			return Failure(EGitWorktreeParseStatus::FieldLimitExceeded, fieldOffset,
				"porcelain field exceeds the budget");
		}
		if (++pending.fields > limits.maximumFieldsPerRecord) {
			return Failure(EGitWorktreeParseStatus::FieldCountLimitExceeded, fieldOffset,
				"porcelain record has too many fields");
		}
		if (fieldBytes + 1 > limits.maximumRecordBytes - pending.bytes) {
			return Failure(EGitWorktreeParseStatus::RecordLimitExceeded, fieldOffset,
				"porcelain record exceeds the byte budget");
		}
		pending.bytes += fieldBytes + 1;
		const std::string_view field(begin, fieldBytes);
		if (pending.fields == 1 && !field.starts_with("worktree ")) {
			return Failure(EGitWorktreeParseStatus::MalformedRecord, fieldOffset,
				"worktree must be the first field in a record");
		}

		auto duplicate = [&](bool present, const char* name) -> std::optional<GitWorktreeParseResult> {
			if (!present) return std::nullopt;
			return Failure(EGitWorktreeParseStatus::DuplicateField, fieldOffset,
				std::string("duplicate ") + name + " field");
		};
		if (field.starts_with("worktree ")) {
			if (auto failure = duplicate(pending.path.has_value(), "worktree")) return std::move(*failure);
			pending.path = std::string(field.substr(9));
			if (pending.path->empty() || !IsValidUtf8(*pending.path)) {
				return Failure(EGitWorktreeParseStatus::InvalidUtf8, fieldOffset,
					"worktree path is empty or invalid UTF-8");
			}
		} else if (field.starts_with("HEAD ")) {
			if (auto failure = duplicate(pending.head.has_value(), "HEAD")) return std::move(*failure);
			pending.head = std::string(field.substr(5));
			std::transform(pending.head->begin(), pending.head->end(), pending.head->begin(),
				[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
		} else if (field.starts_with("branch ")) {
			if (auto failure = duplicate(pending.branch.has_value(), "branch")) return std::move(*failure);
			pending.branch = std::string(field.substr(7));
			if (!IsValidLocalBranchRef(*pending.branch)) {
				return Failure(EGitWorktreeParseStatus::MalformedRecord, fieldOffset,
					"branch is not a valid local branch ref");
			}
		} else if (field == "detached") {
			if (auto failure = duplicate(pending.detached, "detached")) return std::move(*failure);
			pending.detached = true;
		} else if (field == "bare") {
			if (auto failure = duplicate(pending.bare, "bare")) return std::move(*failure);
			pending.bare = true;
		} else if (field == "locked" || field.starts_with("locked ")) {
			if (auto failure = duplicate(pending.locked, "locked")) return std::move(*failure);
			pending.locked = true;
			pending.lockReason = field.size() == 6 ? std::string{} : std::string(field.substr(7));
			if (!IsValidUtf8(pending.lockReason)) {
				return Failure(EGitWorktreeParseStatus::InvalidUtf8, fieldOffset, "lock reason is invalid UTF-8");
			}
		} else if (field == "prunable" || field.starts_with("prunable ")) {
			if (auto failure = duplicate(pending.prunable, "prunable")) return std::move(*failure);
			pending.prunable = true;
			pending.pruneReason = field.size() == 8 ? std::string{} : std::string(field.substr(9));
			if (!IsValidUtf8(pending.pruneReason)) {
				return Failure(EGitWorktreeParseStatus::InvalidUtf8, fieldOffset, "prune reason is invalid UTF-8");
			}
		} else {
			return Failure(EGitWorktreeParseStatus::UnknownField, fieldOffset,
				"porcelain record contains an unsupported field");
		}
	}

	if (pending.fields != 0 || !sawSeparator) {
		return Failure(EGitWorktreeParseStatus::MalformedRecord, input.size(),
			"porcelain record is missing its empty NUL terminator");
	}
	GitWorktreeParseResult result;
	result.status = EGitWorktreeParseStatus::Succeeded;
	result.records = std::move(records);
	return result;
}

} // namespace workbench::worktree
