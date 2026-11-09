/*! @file
 * @brief Bounded, read-only discovery of agent CLI session history on disk.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include "workbench/projects/AgentSessionHistory.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

//! Where each provider keeps its history. An empty member disables that
//! provider; discovery never guesses another location.
struct AgentSessionDiscoveryRoots final {
	std::wstring claudeConfigDirectory;	//! %CLAUDE_CONFIG_DIR%, else %USERPROFILE%\.claude
	std::wstring codexHome;	//! %CODEX_HOME%, else %USERPROFILE%\.codex
	std::wstring piAgentDirectory;	//! %PI_CODING_AGENT_DIR% / %TAU_CODING_AGENT_DIR%, else %USERPROFILE%\.pi\agent
	std::wstring devinExecutable;	//! devin.exe resolved through PATH, never the CWD
};

//! Reads the environment once. A set but relative override disables its
//! provider instead of being resolved against the editor's CWD.
[[nodiscard]] AgentSessionDiscoveryRoots ResolveAgentSessionDiscoveryRoots();

struct AgentSessionDiscoveryLimits final {
	std::size_t maximumSessionsPerProvider = 20;
	std::size_t maximumSessions = 20;
	std::size_t headBytes = 256u * 1024u;
	std::size_t tailBytes = 256u * 1024u;
	//! Codex rollouts begin with a ~20 KB `session_meta` line and can grow to
	//! gigabytes, so only a head is read.
	std::size_t codexHeadBytes = 64u * 1024u;
	std::size_t maximumFileReadsPerProvider = 256;
	std::size_t maximumDirectoryEntries = 20000;
	std::size_t maximumCodexIndexBytes = 4u * 1024u * 1024u;
	std::uint32_t devinTimeoutMilliseconds = 5000;
	std::size_t devinMaximumOutputBytes = 1024u * 1024u;
};

//! Synchronous and read-only; run it off the UI thread. Every provider is
//! independent: one that is missing or fails contributes no sessions and
//! never fails the others. Returns nullopt only when `stop` (an optional
//! manual-reset event) was signalled, in which case partial results are
//! discarded.
[[nodiscard]] std::optional<std::vector<AgentSessionRecord>> DiscoverAgentSessions(
	std::wstring_view folder, const AgentSessionDiscoveryRoots& roots,
	const AgentSessionDiscoveryLimits& limits, HANDLE stop);

} // namespace workbench::projects
