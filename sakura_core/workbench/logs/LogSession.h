/*! @file
 * @brief Naming and retention of the per-process session logs folder.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::logs {

class ILogFileSystem;

//! Local wall-clock time, as VS Code stamps its log lines and session folders.
struct LogTimestamp final {
	int year{};
	int month{};
	int day{};
	int hour{};
	int minute{};
	int second{};
	int millisecond{};

	[[nodiscard]] bool operator==(const LogTimestamp&) const noexcept = default;
};

[[nodiscard]] LogTimestamp CurrentLocalLogTimestamp() noexcept;

//! VS Code names a session folder `YYYYMMDDTHHMMSS`. Each Sakura editor
//! process is its own session holding one window, so the name adds the
//! process id: `YYYYMMDDTHHMMSS-<pid>`, and the window folder is `window1`.
[[nodiscard]] std::wstring FormatLogSessionFolderName(const LogTimestamp& started, std::uint32_t processId);
//! True only for names FormatLogSessionFolderName produces; retention never
//! touches any other directory under the logs root.
[[nodiscard]] bool IsLogSessionFolderName(std::wstring_view name) noexcept;

inline constexpr std::wstring_view kLogWindowFolderName = L"window1";
//! VS Code keeps its most recent session folders and deletes older ones at
//! startup. This keeps the newest 10, counting the current session.
inline constexpr std::size_t kRetainedLogSessions = 10;

//! Which of `names` (immediate children of the logs root) retention deletes:
//! session folders older than the newest `retain`, never `current`. Session
//! names sort by start time, so the order is ordinal.
[[nodiscard]] std::vector<std::wstring> SelectExpiredLogSessions(
	std::vector<std::wstring> names, std::wstring_view current, std::size_t retain = kRetainedLogSessions);

//! One editor process's session: `<root>/<session>/window1`.
struct LogSessionLayout final {
	std::filesystem::path root;
	std::wstring sessionName;

	[[nodiscard]] std::filesystem::path SessionFolder() const { return root / sessionName; }
	[[nodiscard]] std::filesystem::path WindowFolder() const { return SessionFolder() / std::wstring(kLogWindowFolderName); }
};

//! Whether the editor process that owns a session folder is still running.
//! Each editor process is its own session, so an expired-by-age folder can
//! still belong to a live window; deleting it would strand that window's
//! writers.
using LogSessionLiveness = std::function<bool(std::wstring_view sessionName)>;

//! Production liveness: the folder's PID names a running process that was
//! created no later than the folder's start time (plus a small slack), so a
//! PID reused by a later process does not count.
[[nodiscard]] bool IsLogSessionProcessRunning(std::wstring_view sessionName) noexcept;

//! Creates the window folder, then deletes expired sessions that are not live.
//! Returns false when the window folder cannot be created; a failed deletion
//! is ignored, as another process may still hold a file in that session open.
//! `stopRequested` is checked between deletions so a slow disk cannot hold
//! up shutdown.
[[nodiscard]] bool PrepareLogSession(ILogFileSystem& files, const LogSessionLayout& layout,
	const LogSessionLiveness& isLive = IsLogSessionProcessRunning,
	const std::function<bool()>& stopRequested = {}) noexcept;

} // namespace workbench::logs
