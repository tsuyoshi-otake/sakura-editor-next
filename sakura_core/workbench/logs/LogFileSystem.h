/*! @file
 * @brief The file operations the session logs folder needs, behind one seam.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::logs {

/*!
	The session logs folder is diagnostic output, not durable state: an editor
	process writes it directly, as a VS Code window writes its own log files.
	Every operation reports failure by value and never throws, so a writer can
	keep running (and keep the in-memory channel) when the disk refuses a write.
	Tests inject a fake to drive rotation and write failures.
*/
class ILogFileSystem {
public:
	virtual ~ILogFileSystem() = default;

	[[nodiscard]] virtual bool CreateDirectories(const std::filesystem::path& directory) noexcept = 0;
	//! Appends `bytes` to `file`, creating it when absent. A reader may keep the
	//! file open while it grows.
	[[nodiscard]] virtual bool Append(const std::filesystem::path& file, std::string_view bytes) noexcept = 0;
	[[nodiscard]] virtual std::optional<std::uint64_t> FileSize(const std::filesystem::path& file) noexcept = 0;
	//! Renames `from` to `to`, replacing `to`.
	[[nodiscard]] virtual bool MoveReplace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept = 0;
	//! Replaces `file`'s whole content atomically: a reader sees the old or the
	//! new content, never a partial write.
	[[nodiscard]] virtual bool ReplaceContent(const std::filesystem::path& file, std::string_view bytes) noexcept = 0;
	//! Names of the immediate subdirectories of `directory`.
	[[nodiscard]] virtual std::optional<std::vector<std::wstring>> ListDirectories(
		const std::filesystem::path& directory) noexcept = 0;
	//! Removes `directory` and its contents. A file another process holds open
	//! may survive; that is not an error a caller can act on.
	[[nodiscard]] virtual bool RemoveTree(const std::filesystem::path& directory) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<ILogFileSystem> CreateWin32LogFileSystem();

} // namespace workbench::logs
