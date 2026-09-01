/*! @file
 * @brief Typed, bounded parser for `git worktree list --porcelain -z`.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace workbench::worktree {

struct GitWorktreeRecord final {
	//! Lexically normalized absolute Windows path used for display and opening.
	std::wstring path;
	//! NFC, invariant-case-folded path used only for Windows identity comparison.
	std::wstring identity;
	std::string head;
	std::optional<std::string> branch;
	bool detached = false;
	bool bare = false;
	bool locked = false;
	bool prunable = false;
	std::string lockReason;
	std::string pruneReason;
};

enum class EGitWorktreeParseStatus : std::uint8_t {
	Succeeded,
	EmptyInput,
	InputLimitExceeded,
	FieldLimitExceeded,
	FieldCountLimitExceeded,
	RecordLimitExceeded,
	MalformedRecord,
	UnknownField,
	DuplicateField,
	InvalidUtf8,
	InvalidPath,
	InvalidHead,
	AmbiguousIdentity,
};

struct GitWorktreeParserLimits final {
	std::size_t maximumInputBytes{ 4u * 1024u * 1024u };
	std::size_t maximumRecordBytes{ 128u * 1024u };
	std::size_t maximumFieldBytes{ 32u * 1024u };
	std::size_t maximumFieldsPerRecord{ 16 };
	std::size_t maximumRecords{ 1024 };
};

struct GitWorktreeParseResult final {
	EGitWorktreeParseStatus status{ EGitWorktreeParseStatus::MalformedRecord };
	std::vector<GitWorktreeRecord> records;
	std::size_t byteOffset = 0;
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EGitWorktreeParseStatus::Succeeded;
	}
};

//! Normalize an absolute drive or UNC path without consulting the filesystem.
//! Device namespaces and Windows-ambiguous trailing-dot/space components fail closed.
[[nodiscard]] std::optional<std::pair<std::wstring, std::wstring>> NormalizeWindowsWorktreePath(
	std::string_view utf8Path);
[[nodiscard]] std::optional<std::pair<std::wstring, std::wstring>> NormalizeWindowsWorktreePath(
	std::wstring_view path);

[[nodiscard]] GitWorktreeParseResult ParseGitWorktreePorcelainZ(
	std::span<const std::uint8_t> input,
	const GitWorktreeParserLimits& limits = {});

} // namespace workbench::worktree
