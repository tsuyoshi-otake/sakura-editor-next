/*! @file
 * @brief Which session a running agent CLI is in now, read from the record the
 * CLI itself keeps while it runs (#448). A CLI can move to another session
 * without restarting, so neither the command that started it nor the session
 * history can name that session.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/projects/AgentSessionDiscovery.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace workbench::projects {

//! One running process of an agent CLI, as the terminal observed it.
struct AgentProcessCoordinates final {
	const std::uint32_t processId{};
	const std::uint64_t creationTime{};	//! FILETIME ticks; 0 = unknown
};

//! Where the running CLIs keep their live records. An empty directory turns its
//! provider off.
struct AgentLiveSessionRoots final {
	const std::wstring claudeConfigDirectory;
	const std::wstring devinStateDirectory;	//! %APPDATA%\devin\cli
};

//! Claude Code shares the discovery root and so its CLAUDE_CONFIG_DIR override.
//! Reads the environment once.
[[nodiscard]] AgentLiveSessionRoots ResolveAgentLiveSessionRoots(
	const AgentSessionDiscoveryRoots& discovery);

//! The AgentSessionIdentity of the session that `processes` -- every process of
//! one agent in one terminal, oldest first -- is in now. The oldest process
//! with a record of its own answers: a launcher without one leaves the answer
//! to its child, and a nested agent never overrides the one that started it.
//! nullopt when no record answers.
//!
//! Formats verified 2026-11-28 against the installed CLIs:
//!  - Claude Code: `<claudeConfigDirectory>\sessions\<pid>.json` names `pid`,
//!    `sessionId`, and `procStart` (the process creation FILETIME, as a
//!    string). The CLI rewrites it when it switches session (`/clear`) and
//!    removes it on exit.
//!  - Devin 3000.12.57: `<devinStateDirectory>\session_locks\<session-id>.lock`
//!    holds the holder's pid in decimal. The holder keeps the byte range
//!    locked, so only a mapped view can read it. Locks are never removed and a
//!    released one keeps its old pid, so a lock counts only when it was
//!    written since the process started; the newest write wins.
//!  - Codex and Pi keep no verified live record, so they always get nullopt.
[[nodiscard]] std::optional<std::wstring> ReadLiveAgentSessionIdentity(
	EAgentSessionProvider provider, std::span<const AgentProcessCoordinates> processes,
	const AgentLiveSessionRoots& roots);

} // namespace workbench::projects
