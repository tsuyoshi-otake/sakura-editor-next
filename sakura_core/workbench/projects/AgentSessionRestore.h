/*! @file
 * @brief Pure Phase-2/3 agent session restore model (#406/#415): what a
 * closing window records per Project folder -- the session identities plus
 * the terminal groups and split trees they lived in -- how that state
 * round-trips through the profile store, and what the next Project open
 * resumes.
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
//! At most this many plain-shell leaves a folder's recorded layout carries.
inline constexpr std::size_t kMaximumAgentSessionRestoreShellLeaves = 8;
//! One recorded group's tree holds at most this many nodes, which also bounds
//! its depth.
inline constexpr std::size_t kMaximumAgentSessionRestoreLayoutNodes = 32;
//! One store record covers at most this many folders; the least recently
//! written entry is dropped to make room (the entries are write-ordered).
inline constexpr std::size_t kMaximumAgentSessionRestoreProjects = 64;
//! Encode writes version 2; Decode accepts version 1 (no layout: each session
//! restores into its own single-leaf group in record order) and version 2.
inline constexpr std::uint32_t kAgentSessionRestoreSchemaVersion = 2;
inline constexpr std::uint32_t kMinimumAgentSessionRestoreSchemaVersion = 1;

//! One live agent terminal observed when its window closes or switches away.
struct LiveAgentSessionSource final {
	//! The product name the presence reported (for example `Claude Code`), the
	//! same value `TerminalAgentDisplayName` returns for the agent's kind.
	std::wstring name;
	//! The session Sakura itself resumed into this terminal, as an
	//! AgentSessionIdentity (`<provider>:<id>`). It is trusted, never inferred,
	//! but only counts while it names the provider the live agent still is.
	std::optional<std::wstring> knownSessionIdentity;
	//! The session the running agent says it is in right now, read from its own
	//! live record (ReadLiveAgentSessionIdentity). A CLI can move to another
	//! session without restarting, so this replaces `knownSessionIdentity`
	//! while it names the provider the live agent still is.
	const std::optional<std::wstring> liveSessionIdentity{};
	//! When the agent process started: the observed creation time of the
	//! presence-owning process, or the moment a title-only agent's presence was
	//! first resolved. 0 or negative means the anchor is unknown, in which case
	//! no session is ever inferred for this agent (never guess).
	std::int64_t startedUnixSeconds{};
	//! The terminal tab hosting the agent, so the persisted layout can put the
	//! recorded identity on the right leaf. 0 names no tab.
	std::uint64_t tabId{};
};

//! The provider an identity's `<provider>:` prefix names; nullopt when the
//! identity is not valid (IsValidAgentSessionIdentity covers the id part).
[[nodiscard]] std::optional<EAgentSessionProvider> AgentSessionIdentityProvider(
	std::wstring_view identity) noexcept;

//! One claim produced by recording: which terminal tab ended up with which
//! session identity.
struct RecordedAgentSessionClaim final {
	std::uint64_t tabId{};
	std::wstring identity;
};

//! What recording these agents needs from session discovery.
struct AgentSessionRecordDiscovery final {
	//! The providers whose session list recording would use: to check a live
	//! identity, or to infer an agent that nothing else identifies. Empty means
	//! no discovery can change the result.
	const std::vector<EAgentSessionProvider> providers;
	//! Every live and known identity that fits its agent, so that a discovery
	//! bound cannot hide one from the check.
	const std::vector<std::wstring> requiredIdentities;
};

[[nodiscard]] AgentSessionRecordDiscovery PlanAgentSessionRecordDiscovery(
	std::span<const LiveAgentSessionSource> agents);

//! The session identities to record for one folder, each paired with the
//! source's tab id so the caller can place the identity on a layout leaf.
//!
//! Live identities go first in input order, and each replaces that agent's
//! known identity. A provider in `answeredProviders` completed its discovery:
//! when `sessions` then lacks an agent's live identity, nothing can resume that
//! session, so the agent produces no claim at all -- an older session must not
//! stand in for it. A provider that did not answer cannot deny a live identity.
//!
//! Known identities of the remaining agents go next in input order. Then, per
//! provider, every agent with neither claims the newest unclaimed session
//! whose last activity is at or after the agent's start anchor; the agent that
//! started latest claims first (the same arbitration
//! CountAgentsAwaitingSessionHistory uses). A session covers at most one
//! agent. An agent without a claim is recorded by the caller as a plain shell
//! leaf.
[[nodiscard]] std::vector<RecordedAgentSessionClaim> RecordLiveAgentSessionsByTab(
	std::span<const LiveAgentSessionSource> agents,
	std::span<const AgentSessionRecord> sessions,
	std::size_t maximum = kMaximumAgentSessionRestoreSessions,
	std::span<const EAgentSessionProvider> answeredProviders = {});

//! Orientation of a persisted split node: Horizontal puts children side by
//! side, Vertical stacks them top to bottom (TerminalPaneOrientation).
enum class AgentSessionRestoreOrientation : std::uint8_t {
	Horizontal,
	Vertical,
};

//! One leaf of a persisted terminal group (#415 Phase 3): exactly one member
//! is set. `session` holds an agent session identity validated by
//! `IsValidAgentSessionIdentity`; `shell` holds the start directory of a
//! plain terminal (TerminalTabSnapshot::initialWorkingDirectory). An agent
//! whose session cannot be identified is recorded as a shell, never guessed.
struct AgentSessionRestoreLeaf final {
	std::optional<std::wstring> session;
	std::optional<std::wstring> shell;

	[[nodiscard]] bool operator==(const AgentSessionRestoreLeaf&) const noexcept = default;
};

//! One node of a persisted recursive split tree: a leaf (a `leaf` index into
//! the entry's `leaves`) or a split (`children` node indices plus a `weights`
//! list of the same size, `orientation`). A leaf node leaves `children`
//! empty; a split leaves `leaf` unset.
struct AgentSessionRestoreLayoutNode final {
	std::optional<std::size_t> leaf;
	AgentSessionRestoreOrientation orientation = AgentSessionRestoreOrientation::Horizontal;
	std::vector<std::size_t> children;
	std::vector<int> weights;

	[[nodiscard]] bool operator==(const AgentSessionRestoreLayoutNode&) const noexcept = default;
};

//! One persisted terminal group in panel order. A valid tree has `root` in
//! range, reaches every node from it (no dead nodes), and references each
//! leaf exactly once.
struct AgentSessionRestoreLayoutGroup final {
	std::vector<AgentSessionRestoreLayoutNode> nodes;
	std::size_t root{};

	[[nodiscard]] bool operator==(const AgentSessionRestoreLayoutGroup&) const noexcept = default;
};

//! One folder's persisted terminal layout (schema version 2, #415 Phase 3):
//! `leaves` holds every terminal of `groups` in their trees' depth-first
//! leaf order, `groups` holds the split trees in panel order, and `active`
//! indexes the leaf that was the active terminal. Serialized as:
//! `{"leaves":[{"session":"claude-code:x"}|{"shell":"C:\\dir"},...],
//! "groups":[{"nodes":[{"leaf":0}|{"split":"horizontal","weights":[600,400],
//! "children":[0,1]},...],"root":N},...],"active":M}`.
struct AgentSessionRestoreLayout final {
	std::vector<AgentSessionRestoreLeaf> leaves;
	std::vector<AgentSessionRestoreLayoutGroup> groups;
	std::optional<std::size_t> active;

	[[nodiscard]] bool operator==(const AgentSessionRestoreLayout&) const noexcept = default;
};

//! What one folder's record holds: the sessions live when a window last left
//! it, as AgentSessionIdentity values in the layout's leaf order (record
//! order for a version-1 record with no layout), plus the terminal layout
//! (#415 Phase 3). `sessions` and `layout` are consistent: a layout's
//! session leaves in leaf order are exactly `sessions`.
struct AgentSessionRestoreEntry final {
	std::wstring folder;
	std::vector<std::wstring> sessions;
	std::optional<AgentSessionRestoreLayout> layout;

	[[nodiscard]] bool operator==(const AgentSessionRestoreEntry&) const noexcept = default;
};

//! The whole persisted record, ordered oldest entry first.
struct AgentSessionRestoreState final {
	std::vector<AgentSessionRestoreEntry> entries;

	[[nodiscard]] bool operator==(const AgentSessionRestoreState&) const noexcept = default;
};

//! `folder`'s recorded entry; nullptr when none is stored. Folder comparison
//! is IsSameAgentSessionFolder's.
[[nodiscard]] const AgentSessionRestoreEntry* FindAgentSessionRestoreEntry(
	const AgentSessionRestoreState& state, std::wstring_view folder) noexcept;

//! Replaces `folder`'s record, or removes it when `sessions` is empty. Every
//! identity must satisfy IsValidAgentSessionIdentity with no duplicates and at
//! most kMaximumAgentSessionRestoreSessions entries. When `layout` is given
//! it must hold one leaf per recorded terminal (at most
//! kMaximumAgentSessionRestoreShellLeaves shells and
//! kMaximumAgentSessionRestoreLayoutNodes nodes per group), each group's tree
//! must be well-formed (root in range, child indices in range and acyclic,
//! splits with at least two children and matching positive weights, every
//! leaf referenced exactly once, every node reachable from the root), and its
//! session leaves in leaf order must equal `sessions`; an invalid input
//! changes nothing and returns false. A folder that
//! IsSameAgentSessionFolder-matches an existing entry replaces it and becomes
//! the most recently written; at the entry cap the least recently written
//! entry is dropped. The encoded record also stays inside its payload bound:
//! least recently written entries are dropped until it fits, and a written
//! entry that does not fit alone keeps its sessions without its layout. A
//! write that cannot fit even so changes nothing and returns false. Returns
//! whether the state changed.
bool SetAgentSessionRestoreEntry(AgentSessionRestoreState& state, std::wstring_view folder, std::vector<std::wstring> sessions, std::optional<AgentSessionRestoreLayout> layout = std::nullopt);

//! Whether SetAgentSessionRestoreEntry would accept `layout` for `sessions`
//! (the layout rules above). A recorder whose live layout exceeds a bound
//! records `sessions` without a layout instead of recording nothing.
[[nodiscard]] bool IsValidAgentSessionRestoreLayout(const AgentSessionRestoreLayout& layout,
	const std::vector<std::wstring>& sessions);

//! `{"version":2,"projects":[{"folder":"C:\\a","sessions":["claude-code:x"],
//! "layout":{"leaves":[{"session":"claude-code:x"}],"groups":[{"nodes":
//! [{"leaf":0}],"root":0}],"active":0}}]}`; `layout` is omitted for an entry
//! without one. Encode returns nullopt for a state Decode would reject.
[[nodiscard]] std::optional<std::string> EncodeAgentSessionRestoreState(
	const AgentSessionRestoreState& state);
//! Accepts version 1 (no layout; each session restores into its own
//! single-leaf group in record order) and version 2. Rejects (nullopt) any
//! other version, an invalid or folder-duplicate entry, an invalid or
//! duplicated identity, an oversized list, a malformed tree (child index out
//! of range, a cycle, a split with fewer than two children, a non-positive
//! weight, a leaf used twice, an unknown orientation, a session-leaf order
//! that differs from `sessions`), and malformed payloads.
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

//! One leaf a restore installs (#415 Phase 3): exactly one member is set.
//! `session` is the freshly discovered record whose identity the recorded
//! leaf named; `shellDirectory` is the plain shell's recorded start
//! directory.
struct AgentSessionRestorePlanLeaf final {
	std::optional<AgentSessionRecord> session;
	std::optional<std::wstring> shellDirectory;
};

//! One node of a planned group tree: a leaf (index into the group's
//! `leaves`) or a split (`children`/`weights`, `orientation`).
struct AgentSessionRestorePlanNode final {
	std::optional<std::size_t> leaf;
	AgentSessionRestoreOrientation orientation = AgentSessionRestoreOrientation::Horizontal;
	std::vector<std::size_t> children;
	std::vector<int> weights;
};

//! One group to create, in panel order. A group that loses every leaf to a
//! missing session is never planned, and a group left holding shells alone
//! is dropped (a shell-only group is never recreated).
struct AgentSessionRestorePlanGroup final {
	std::vector<AgentSessionRestorePlanLeaf> leaves;
	std::vector<AgentSessionRestorePlanNode> nodes;
	std::size_t root{};
};

//! What one Project open does with its recorded sessions.
struct AgentSessionRestorePlan final {
	EAgentSessionRestoreStatus status{ EAgentSessionRestoreStatus::Ready };
	//! The groups to install, in panel order; each keeps the recorded tree
	//! minus leaves whose session no longer exists. A session leaf holds the
	//! freshly discovered record (valid id by construction), never the stored
	//! copy, and at most `maximum` session leaves survive, in record order.
	std::vector<AgentSessionRestorePlanGroup> groups;
	//! Which planned leaf to select: the recorded active leaf when it
	//! survived, else the first session leaf of the first group. Absent when
	//! nothing resumes.
	std::optional<std::size_t> activeGroup;
	std::optional<std::size_t> activeLeaf;
	//! Recorded identities the discovery did not return: skipped, not retried.
	std::vector<std::wstring> skipped;
	//! Recorded identities past `maximum`, dropped by the cap.
	std::size_t dropped{};
};

//! `recorded` is FindAgentSessionRestoreEntry's entry for the folder being
//! opened; `sessions` is that folder's fresh discovery (nullopt = cancelled or
//! unavailable). Ready planning matches identities, never guesses titles; a
//! version-1 record without a layout plans one single-leaf group per
//! recorded session in record order.
[[nodiscard]] AgentSessionRestorePlan PlanAgentSessionRestore(bool enabled,
	const AgentSessionRestoreEntry& recorded,
	const std::optional<std::vector<AgentSessionRecord>>& sessions,
	std::size_t maximum = kMaximumAgentSessionRestoreSessions);

struct AgentSessionRestoreInstallResult final {
	//! Planned session leaves that got a terminal and their resume input.
	std::size_t resumed{};
	//! Planned leaves that got no terminal (creation or input failure);
	//! each is counted and the rest of the plan still installs.
	std::size_t failed{};
};

struct AgentSessionRestoreOutcome final {
	EAgentSessionRestoreStatus status{ EAgentSessionRestoreStatus::Ready };
	std::size_t resumed{};
	//! Leaves the install did not place; each is counted and the rest still
	//! runs.
	std::size_t failed{};
	std::size_t skipped{};
	std::size_t dropped{};
};

//! Runs a Ready plan through the caller-bound install operation, which
//! creates the planned groups' terminals and queues each session leaf's
//! resume command in one transaction. A non-Ready plan dispatches nothing
//! and reports its status; a throwing install counts every leaf as failed.
[[nodiscard]] AgentSessionRestoreOutcome DispatchAgentSessionRestore(
	const AgentSessionRestorePlan& plan,
	const std::function<AgentSessionRestoreInstallResult(const AgentSessionRestorePlan& plan)>& install);

} // namespace workbench::projects
