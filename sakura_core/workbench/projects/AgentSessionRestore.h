/*! @file
 * @brief Pure Phase-2 agent session restore model (#406): what a closing window
 * records per Project folder, how that state round-trips through the profile
 * store, and what the next Project open resumes.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/projects/AgentSessionHistory.h"
#include "workbench/projects/AgentSessionManagement.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

//! At most this many sessions are recorded for and restored into one Project
//! folder. Recording keeps the newest claims; planning drops what is left over.
inline constexpr std::size_t kMaximumAgentSessionRestoreSessions = 8;
//! One store record covers at most this many folders; the least recently
//! written entry is dropped to make room (the entries are write-ordered).
inline constexpr std::size_t kMaximumAgentSessionRestoreProjects = 64;
inline constexpr std::uint32_t kAgentSessionRestoreSchemaVersion = 1;

//! One live agent terminal observed when its window closes or switches away.
struct LiveAgentSessionSource final {
	//! The product name the presence reported (for example `Claude Code`), the
	//! same value `TerminalAgentDisplayName` returns for the agent's kind.
	std::wstring name;
	//! The session Sakura itself resumed into this terminal, as an
	//! AgentSessionIdentity (`<provider>:<id>`). It is trusted, never inferred,
	//! but only counts while it names the provider the live agent still is.
	std::optional<std::wstring> knownSessionIdentity;
	//! When the agent process started: the observed creation time of the
	//! presence-owning process, or the moment a title-only agent's presence was
	//! first resolved. 0 or negative means the anchor is unknown, in which case
	//! no session is ever inferred for this agent (never guess).
	std::int64_t startedUnixSeconds{};
};

//! The provider an identity's `<provider>:` prefix names; nullopt when the
//! identity is not valid (IsValidAgentSessionIdentity covers the id part).
[[nodiscard]] std::optional<EAgentSessionProvider> AgentSessionIdentityProvider(
	std::wstring_view identity) noexcept;

//! The session identities to record for one folder, in terminal order.
//!
//! Known identities go first in input order. Then, per provider, every other
//! live agent claims the newest session whose last activity is at or after the
//! agent's start anchor; the agent that started latest claims first (the same
//! arbitration CountAgentsAwaitingSessionHistory uses), and a session covers
//! at most one agent, including one already named by a known identity. An
//! agent with no qualifying unclaimed session is simply not recorded.
[[nodiscard]] std::vector<std::wstring> RecordLiveAgentSessions(
	std::span<const LiveAgentSessionSource> agents,
	std::span<const AgentSessionRecord> sessions,
	std::size_t maximum = kMaximumAgentSessionRestoreSessions);

//! What one folder's record holds: the sessions live when a window last left
//! it, as AgentSessionIdentity values in record order.
struct AgentSessionRestoreEntry final {
	std::wstring folder;
	std::vector<std::wstring> sessions;

	[[nodiscard]] bool operator==(const AgentSessionRestoreEntry&) const noexcept = default;
};

//! The whole persisted record, ordered oldest entry first.
struct AgentSessionRestoreState final {
	std::vector<AgentSessionRestoreEntry> entries;

	[[nodiscard]] bool operator==(const AgentSessionRestoreState&) const noexcept = default;
};

//! The recorded session identities for `folder`; empty when none are stored.
//! Folder comparison is IsSameAgentSessionFolder's.
[[nodiscard]] std::span<const std::wstring> FindAgentSessionRestoreEntry(
	const AgentSessionRestoreState& state, std::wstring_view folder) noexcept;

//! Replaces `folder`'s record, or removes it when `sessions` is empty. Every
//! identity must satisfy IsValidAgentSessionIdentity with no duplicates and at
//! most kMaximumAgentSessionRestoreSessions entries; an invalid input changes
//! nothing and returns false. A folder that IsSameAgentSessionFolder-matches
//! an existing entry replaces it and becomes the most recently written; at the
//! entry cap the least recently written entry is dropped. Returns whether the
//! state changed.
bool SetAgentSessionRestoreEntry(AgentSessionRestoreState& state, std::wstring_view folder,
	std::vector<std::wstring> sessions);

//! `{"version":1,"projects":[{"folder":"C:\\a","sessions":["claude-code:x"]}]}`.
//! Encode returns nullopt for a state Decode would reject.
[[nodiscard]] std::optional<std::string> EncodeAgentSessionRestoreState(
	const AgentSessionRestoreState& state);
//! Rejects (nullopt) a wrong version, an invalid or folder-duplicate entry, an
//! invalid or duplicated identity, an oversized list, and malformed payloads.
[[nodiscard]] std::optional<AgentSessionRestoreState> DecodeAgentSessionRestoreState(
	std::string_view payload);

enum class EAgentSessionRestoreStatus : std::uint8_t {
	//! The `sakura.projects.restoreAgentSessions` setting is off.
	Disabled,
	//! The store holds no sessions for this folder.
	NothingRecorded,
	//! The folder's sessions could not be listed. Nothing is resumed, and the
	//! caller never retries this attempt (a later Project open is a new one).
	DiscoveryFailed,
	Ready,
};

//! What one Project open does with its recorded sessions.
struct AgentSessionRestorePlan final {
	EAgentSessionRestoreStatus status{ EAgentSessionRestoreStatus::Ready };
	//! The recorded sessions that still exist, in record order, at most
	//! `maximum`; each is the freshly discovered record (valid id by
	//! construction), never the stored copy.
	std::vector<AgentSessionRecord> resume;
	//! Recorded identities the discovery did not return: skipped, not retried.
	std::vector<std::wstring> skipped;
	//! Recorded identities past `maximum`, dropped by the cap.
	std::size_t dropped{};
};

//! `recorded` is FindAgentSessionRestoreEntry's list for the folder being
//! opened; `sessions` is that folder's fresh discovery (nullopt = cancelled or
//! unavailable). Ready planning matches identities, never guesses titles.
[[nodiscard]] AgentSessionRestorePlan PlanAgentSessionRestore(
	bool enabled,
	std::span<const std::wstring> recorded,
	const std::optional<std::vector<AgentSessionRecord>>& sessions,
	std::size_t maximum = kMaximumAgentSessionRestoreSessions);

struct AgentSessionRestoreOutcome final {
	EAgentSessionRestoreStatus status{ EAgentSessionRestoreStatus::Ready };
	std::size_t resumed{};
	//! Resume operations that returned failure; each failed session is counted
	//! and the rest still run.
	std::size_t failed{};
	std::size_t skipped{};
	std::size_t dropped{};
};

//! Runs a Ready plan through the exact resume operation a Sessions row
//! double-click uses (a terminal in the folder plus its resume command), bound
//! by the caller. A non-Ready plan dispatches nothing and reports its status.
[[nodiscard]] AgentSessionRestoreOutcome DispatchAgentSessionRestore(
	const AgentSessionRestorePlan& plan,
	const std::function<bool(const AgentSessionRecord& session)>& resume);

} // namespace workbench::projects
