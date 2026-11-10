/*! @file
 * @brief Rename and delete of one agent CLI session in the agent's own store (#408).
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <Windows.h>

#include "platform/process/BoundedProcessRunner.h"
#include "workbench/projects/AgentSessionDiscovery.h"
#include "workbench/projects/AgentSessionHistory.h"
#include "workbench/projects/AgentSessionManagement.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

//! How Codex is run. An empty codexCommand disables the Codex operations;
//! nothing is ever resolved against the editor's CWD. Devin uses the
//! executable discovery already resolved (AgentSessionDiscoveryRoots).
struct AgentSessionCommandTools final {
	std::wstring codexCommand;	//! codex.exe, else the npm shim codex.cmd, through PATH
	std::wstring commandProcessor;	//! absolute %ComSpec%; only used to run codex.cmd
};
[[nodiscard]] AgentSessionCommandTools ResolveAgentSessionCommandTools();

//! Seams for tests. An absent member uses the production implementation.
struct AgentSessionStoreServices final {
	//! platform::process::RunBoundedProcess.
	std::function<platform::process::BoundedProcessResult(
		const platform::process::BoundedProcessRequest&, HANDLE stop)> run;
	//! Moves every path to the Windows Recycle Bin as one operation. Never
	//! falls back to a permanent delete.
	std::function<bool(const std::vector<std::wstring>& paths)> moveToRecycleBin;
	//! Whether a process with this id exists (a Claude Code live-session owner).
	std::function<bool(std::uint32_t processId)> isProcessRunning;
};

struct AgentSessionStoreContext final {
	AgentSessionDiscoveryRoots roots;
	AgentSessionCommandTools tools;
	AgentSessionStoreServices services;
	std::uint32_t commandTimeoutMilliseconds = 30000;
};

//! Both operations are synchronous; run them off the UI thread. `folder` is
//! the session's folder (the one discovery accepted it for), `stop` an
//! optional manual-reset event that cancels a running CLI.
//!
//! Rename writes the agent's own store so its CLI shows the name too:
//!   Claude Code: appends a `custom-title` record and rewrites the
//!     `custom-title.json` sidecar; refused while a CLI process owns the session.
//!   Codex: `codex app-server` `thread/name/set`.
//!   Pi: appends a `session_info` entry linked to the last entry.
//!   Devin: Unsupported (no rename command).
[[nodiscard]] EAgentSessionOperationStatus RenameAgentSession(
	const AgentSessionRecord& session, std::wstring_view folder, std::wstring_view title,
	const AgentSessionStoreContext& context, HANDLE stop);

//! Delete:
//!   Claude Code: the transcript and its per-session directories go to the
//!     Recycle Bin; refused while a CLI process owns the session.
//!   Pi: the session file goes to the Recycle Bin.
//!   Codex: `codex delete --force <id>`, permanent.
//!   Devin: `devin rm <id> --force`, permanent.
[[nodiscard]] EAgentSessionOperationStatus DeleteAgentSession(
	const AgentSessionRecord& session, std::wstring_view folder,
	const AgentSessionStoreContext& context, HANDLE stop);

} // namespace workbench::projects
