/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "terminal/session/TerminalSession.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace terminal {

//! A longer OSC title is not a product-name announcement. Equal to the tab text
//! bound (static_assert in TerminalTabPresentation.cpp).
inline constexpr std::size_t kMaximumTerminalAgentTitleLength = 256;

//! AI agent CLIs whose presence in an integrated terminal can be recognized
//! (Issue #402). Presence is auxiliary evidence only: it never asserts that an
//! agent is working, waiting, or done (#278 trust order).
enum class TerminalAgentKind : std::uint8_t {
	ClaudeCode,
	CommandCode,
	Codex,
	Copilot,
	Devin,
	Gemini,
	OpenCode,
	Pi,
};

//! Product name for display, for example `Claude Code`. Not localized: these
//! are product names, like the branch names the Projects view also shows.
[[nodiscard]] std::wstring_view TerminalAgentDisplayName( TerminalAgentKind kind ) noexcept;

//! Exact, case-insensitive image file names observed on Windows
//! (2026-11-07): `claude.exe` (Claude Code native build), `codex.exe` (Codex,
//! including the binary the npm package launches), `devin.exe` (Devin CLI),
//! `opencode.exe` (OpenCode 1.19.5, whose npm package ships the native binary).
//! Node-hosted CLIs (Gemini, Copilot, npm Claude Code, Pi) run as `node.exe`
//! and are recognized by title only.
[[nodiscard]] std::optional<TerminalAgentKind> TerminalAgentKindFromImageFileName(
	std::wstring_view imageFileName ) noexcept;

//! The OSC title patterns current VS Code uses to recognize an Agent CLI:
//! `/claude\s*code/i`, `/command\s*code/i`, and the words `copilot` and
//! `gemini`. A path-shaped title is a shell reporting its directory, never an
//! agent. Codex and Devin emit no OSC title, so no title proves them.
//! This is also the tab label's Agent CLI allow-list (IsRecognizedAgentCliTitle
//! for terminal.integrated.tabs.allowAgentCliTitle), so it must stay exactly
//! VS Code's list. Fork-only titles belong to the tracker, never here.
[[nodiscard]] std::optional<TerminalAgentKind> TerminalAgentKindFromTitle(
	std::wstring_view sequenceTitle ) noexcept;

//! Window-local presence per terminal tab. It holds only the agent processes
//! of the last job observation and the title-derived kind, so its size is
//! bounded by the tab count times kMaximumObservedJobProcesses. Process IDs are
//! kept solely to filter exit hints, never as an identity.
//!
//! A tab's agent is the earliest-created agent process in its job (the
//! outermost one, so Claude Code running `codex` as a tool is still Claude
//! Code), otherwise the title's agent.
//!
//! Besides VS Code's title patterns, the tracker recognizes Pi's own title,
//! U+03C0 followed by ` - <folder>` (fork extension). The tab label does not
//! recognize it, because VS Code's allowAgentCliTitle list does not.
class TerminalAgentPresenceTracker final {
public:
	//! Each Apply returns true when the tab's resolved presence changed.
	bool ApplyTitle( std::uint64_t tabId, std::wstring_view sequenceTitle );
	//! nullopt (no job, failed query, or a session that is no longer live)
	//! means the tab currently has no observable agent process.
	bool ApplyJobObservation( std::uint64_t tabId,
		const std::optional<TerminalBackendJobProcesses>& observation );
	//! Drops every tab not in liveTabIds; true when any presence disappeared.
	//! This is the only way a closed tab leaves the tracker.
	bool Retain( std::span<const std::uint64_t> liveTabIds );

	[[nodiscard]] std::optional<TerminalAgentKind> Presence( std::uint64_t tabId ) const noexcept;
	//! The agent processes whose exit could change this tab's presence.
	[[nodiscard]] std::vector<std::uint32_t> AgentProcessIds( std::uint64_t tabId ) const;

private:
	struct TabState final {
		std::optional<TerminalAgentKind> titleKind;
		std::vector<TerminalBackendJobProcess> agentProcesses;
		std::optional<TerminalAgentKind> resolved;
	};

	bool Resolve( TabState& state ) noexcept;

	std::map<std::uint64_t, TabState> m_tabs;
};

} // namespace terminal
