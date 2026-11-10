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
#include <string>
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

//! The session name an agent announces through its OSC title, for the Projects
//! row's hover text (#402). Claude Code writes `<status glyph> <topic>` and
//! animates the glyph while it works, so leading status glyphs (Braille
//! patterns, geometric shapes, dingbats, miscellaneous symbols, bullets, and
//! `*`) and blanks are removed; a spinner frame therefore never changes the
//! result. Control characters are removed as in the tab label. An empty,
//! path-shaped (a shell reporting its directory), or over-long title has no
//! session name and yields an empty string. Like Herdr's
//! `terminal_title_stripped`, this is the terminal title with decoration
//! removed; it does not prove which process wrote it.
[[nodiscard]] std::wstring TerminalAgentSessionTitle( std::wstring_view sequenceTitle );

//! Window-local presence per terminal tab. It holds only the agent processes
//! of the last job observation, the title-derived kind, and the bounded session
//! title, so its size is bounded by the tab count times
//! kMaximumObservedJobProcesses. Process IDs are kept solely to filter exit
//! hints, never as an identity.
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
	//! Each Apply returns true when what the Projects row shows for the tab
	//! changed: its resolved presence, or the session title of a tab that has
	//! a presence. A title change on a tab without an agent reports nothing.
	bool ApplyTitle( std::uint64_t tabId, std::wstring_view sequenceTitle );
	//! nullopt (no job, failed query, or a session that is no longer live)
	//! means the tab currently has no observable agent process.
	bool ApplyJobObservation( std::uint64_t tabId,
		const std::optional<TerminalBackendJobProcesses>& observation );
	//! Drops every tab not in liveTabIds; true when any presence disappeared.
	//! This is the only way a closed tab leaves the tracker.
	bool Retain( std::span<const std::uint64_t> liveTabIds );

	[[nodiscard]] std::optional<TerminalAgentKind> Presence( std::uint64_t tabId ) const noexcept;
	//! TerminalAgentSessionTitle of the tab's last applied title; empty for an
	//! unknown tab.
	[[nodiscard]] std::wstring SessionTitle( std::uint64_t tabId ) const;
	//! The earliest instant the agent is provably running, as unix seconds
	//! (#406 Phase 2): the creation time of the earliest observed agent
	//! process, or the moment a title-only agent's presence first resolved.
	//! Session activity at or after it could be this agent's; earlier activity
	//! is unattributable, so inference anchors here and under-claims rather
	//! than guesses. Empty for an unknown tab or no presence.
	[[nodiscard]] std::optional<std::int64_t> StartedUnixSeconds( std::uint64_t tabId ) const noexcept;
	//! The agent processes whose exit could change this tab's presence.
	[[nodiscard]] std::vector<std::uint32_t> AgentProcessIds( std::uint64_t tabId ) const;

private:
	struct TabState final {
		std::optional<TerminalAgentKind> titleKind;
		std::wstring sessionTitle;
		std::vector<TerminalBackendJobProcess> agentProcesses;
		std::optional<TerminalAgentKind> resolved;
		//! Wall-clock instant the current resolved presence first appeared.
		std::optional<std::int64_t> firstResolvedUnixSeconds;
	};

	bool Resolve( TabState& state ) noexcept;

	std::map<std::uint64_t, TabState> m_tabs;
};

} // namespace terminal
