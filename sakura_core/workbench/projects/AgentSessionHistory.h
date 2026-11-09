/*! @file
 * @brief Pure agent CLI session-history records, parsers, and resume commands.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

//! Agent CLIs whose on-disk session history the Projects view can list and
//! resume (#406). Each one has a verified storage format and an exact resume
//! command; CLIs without both are deliberately absent (see the owning
//! workbench/CLAUDE.md section for the reasons).
enum class EAgentSessionProvider : std::uint8_t {
	ClaudeCode,
	Codex,
	Devin,
	Pi,
};

//! Product name, not localized (like the Projects agent rows).
[[nodiscard]] std::wstring_view AgentSessionProviderDisplayName(EAgentSessionProvider provider) noexcept;

//! One resumable session. `id` always satisfies IsValidAgentSessionId, so it
//! can be placed in a shell command line without quoting.
struct AgentSessionRecord final {
	EAgentSessionProvider provider{ EAgentSessionProvider::ClaudeCode };
	std::wstring id;
	std::wstring title;	//! normalized display title; empty when the agent recorded none
	std::int64_t lastActivityUnixSeconds{};	//! 0 = unknown

	[[nodiscard]] bool operator==(const AgentSessionRecord&) const noexcept = default;
};

inline constexpr std::size_t kMaximumAgentSessionIdLength = 128;
inline constexpr std::size_t kMaximumAgentSessionTitleLength = 160;

//! `[A-Za-z0-9][A-Za-z0-9_-]{0,127}`. Every verified provider's identifier
//! (UUIDs, Devin's `ninth-tamarind` names) fits, and nothing that a shell
//! could interpret does.
[[nodiscard]] bool IsValidAgentSessionId(std::wstring_view id) noexcept;

//! The exact command that resumes `id` in a terminal started in the session's
//! folder, without a trailing newline. nullopt for an invalid id.
//!   Claude Code: `claude --resume <id>`   Codex: `codex resume <id>`
//!   Devin: `devin -r <id>`                Pi: `pi --session <id>`
[[nodiscard]] std::optional<std::wstring> BuildAgentSessionResumeCommand(
	EAgentSessionProvider provider, std::wstring_view id);

//! Stable row identity, for example `codex:01a1...`.
[[nodiscard]] std::wstring AgentSessionIdentity(const AgentSessionRecord& session);

//! Folder identity used to accept a session: separators unified, trailing
//! separators removed, compared case-insensitively (Windows paths).
[[nodiscard]] bool IsSameAgentSessionFolder(std::wstring_view left, std::wstring_view right) noexcept;

//! Claude Code's per-project directory name: every character that is not an
//! ASCII letter or digit becomes `-` (`C:\a\b` -> `C--a-b`).
[[nodiscard]] std::wstring ClaudeCodeProjectDirectoryName(std::wstring_view folder);
//! Pi 1.3.0's per-cwd directory name: one leading separator removed, then
//! `/`, `\`, and `:` become `-`, wrapped in `--` (`C:\a` -> `--C--a--`).
[[nodiscard]] std::wstring PiSessionDirectoryName(std::wstring_view folder);

//! Collapses whitespace and control characters into single spaces and bounds
//! the result to kMaximumAgentSessionTitleLength (with a trailing ellipsis).
[[nodiscard]] std::wstring NormalizeAgentSessionTitle(std::wstring_view raw);

//! Parsers take bounded UTF-8 slices of a JSONL file. A line cut by a slice
//! edge simply fails to parse and is ignored. `tail` may be empty when `head`
//! already holds the whole file; head lines are applied before tail lines.

//! Claude Code transcript `<id>.jsonl`. The id is the file stem; the session is
//! accepted only when a record's `cwd` names `folder`. Title precedence: latest
//! `custom-title`, latest `ai-title`, latest `summary`, then the first real user
//! prompt (meta records and `<...>` command/tool wrappers are skipped). A
//! sub-agent (all-sidechain) transcript is rejected.
[[nodiscard]] std::optional<AgentSessionRecord> ParseClaudeCodeTranscript(
	std::string_view head, std::string_view tail, std::wstring_view fileStem,
	std::wstring_view folder, std::int64_t lastWriteUnixSeconds);

//! Codex `session_index.jsonl`: id -> thread name, later lines override
//! earlier ones and an empty name clears the entry.
[[nodiscard]] std::map<std::wstring, std::wstring> ParseCodexSessionIndex(std::string_view text);

//! Codex rollout file. The first line must be `session_meta` for `folder`;
//! sub-agent threads (`thread_source` other than `user`, or a `subagent`
//! source) are not user sessions and are rejected. Title: the index name, then
//! the first `user_message` event in `head`.
[[nodiscard]] std::optional<AgentSessionRecord> ParseCodexRollout(
	std::string_view head, std::wstring_view folder, std::int64_t lastWriteUnixSeconds,
	const std::map<std::wstring, std::wstring>& threadNames);

//! Pi session file. The first line must be the `session` header for `folder`.
//! Title: the latest `session_info.name` (an empty one clears it), then the
//! first user message text.
[[nodiscard]] std::optional<AgentSessionRecord> ParsePiSession(
	std::string_view head, std::string_view tail, std::wstring_view folder,
	std::int64_t lastWriteUnixSeconds);

//! `devin list --format json`: an array of sessions. Only entries whose
//! `working_directory` names `folder` and whose id is valid are kept.
[[nodiscard]] std::vector<AgentSessionRecord> ParseDevinSessionList(
	std::string_view json, std::wstring_view folder);

//! Newest first (unknown times last), ties by provider then id, duplicates by
//! identity removed, at most `maximum` records.
[[nodiscard]] std::vector<AgentSessionRecord> MergeAgentSessions(
	std::vector<AgentSessionRecord> sessions, std::size_t maximum);

struct AgentSessionAgeTexts final {
	std::wstring now{ L"now" };
	std::wstring minutesFormat{ L"{0}m" };
	std::wstring hoursFormat{ L"{0}h" };
	std::wstring daysFormat{ L"{0}d" };
};

//! Relative age for the row's trailing column. Empty when the time is unknown;
//! a time in the future (clock skew) reads as `now`.
[[nodiscard]] std::wstring FormatAgentSessionAge(std::int64_t lastActivityUnixSeconds,
	std::int64_t nowUnixSeconds, const AgentSessionAgeTexts& texts = {});

} // namespace workbench::projects
