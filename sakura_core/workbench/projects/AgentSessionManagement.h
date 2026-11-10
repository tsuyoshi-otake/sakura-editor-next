/*! @file
 * @brief Pure Sessions View management model: the right-click menu and the
 * Sakura-owned pin, archive, and display-count state (#408).
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/projects/AgentSessionHistory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

//! The counts the Sessions View offers in its right-click menu.
inline constexpr std::array<std::size_t, 4> kAgentSessionDisplayCounts{ 10, 20, 30, 50 };
inline constexpr std::size_t kDefaultAgentSessionDisplayCount = 20;
//! Bound of each identity list. A pin or archive past it is refused.
inline constexpr std::size_t kMaximumAgentSessionMarks = 256;
inline constexpr std::uint32_t kAgentSessionViewStateSchemaVersion = 1;

[[nodiscard]] bool IsAgentSessionDisplayCount(std::size_t count) noexcept;

//! `<provider>:<id>` exactly as AgentSessionIdentity writes it, with a known
//! provider key and an id that IsValidAgentSessionId accepts.
[[nodiscard]] bool IsValidAgentSessionIdentity(std::wstring_view identity) noexcept;

//! Sessions View state that belongs to Sakura, not to any agent CLI. It is
//! persisted per profile. Each list holds AgentSessionIdentity values in the
//! order they were added; one identity is never both pinned and archived.
struct AgentSessionViewState final {
	std::size_t displayCount{ kDefaultAgentSessionDisplayCount };
	std::vector<std::wstring> pinned;
	std::vector<std::wstring> archived;

	[[nodiscard]] bool operator==(const AgentSessionViewState&) const noexcept = default;
};

[[nodiscard]] bool IsAgentSessionPinned(const AgentSessionViewState& state, std::wstring_view identity) noexcept;
[[nodiscard]] bool IsAgentSessionArchived(const AgentSessionViewState& state, std::wstring_view identity) noexcept;

//! Each mutation returns true when the state changed. An invalid identity, a
//! no-op, and a list at kMaximumAgentSessionMarks all return false and leave
//! the state untouched. Pinning unarchives, archiving unpins.
bool PinAgentSession(AgentSessionViewState& state, std::wstring_view identity);
bool UnpinAgentSession(AgentSessionViewState& state, std::wstring_view identity);
bool ArchiveAgentSession(AgentSessionViewState& state, std::wstring_view identity);
bool UnarchiveAgentSession(AgentSessionViewState& state, std::wstring_view identity);
//! Drops a deleted session from both lists.
bool ForgetAgentSession(AgentSessionViewState& state, std::wstring_view identity);
bool SetAgentSessionDisplayCount(AgentSessionViewState& state, std::size_t count);

//! `{"version":1,"displayCount":20,"pinned":[...],"archived":[...]}`. Encode
//! returns nullopt for a state Decode would reject.
[[nodiscard]] std::optional<std::string> EncodeAgentSessionViewState(const AgentSessionViewState& state);
//! Rejects (nullopt) a wrong version, an unsupported display count, an invalid
//! or duplicated identity, an identity in both lists, and oversized lists.
[[nodiscard]] std::optional<AgentSessionViewState> DecodeAgentSessionViewState(std::string_view payload);

enum class EAgentSessionCommand : std::uint8_t {
	Resume,
	Rename,
	Pin,
	Unpin,
	Archive,
	Unarchive,
	CopyId,
	CopyResumeCommand,
	Delete,
	ShowCount,
};

//! VS Code's command ID where VS Code has the same Agent Sessions action
//! (`agentSession.rename`, `.pin`, `.unpin`, `.archive`, `.unarchive`,
//! `.delete`), a `sakura.` ID otherwise.
[[nodiscard]] std::wstring_view AgentSessionCommandId(EAgentSessionCommand command) noexcept;

struct AgentSessionMenuItem final {
	EAgentSessionCommand command{ EAgentSessionCommand::Resume };
	std::size_t displayCount{};	//! ShowCount only
	bool checked = false;	//! ShowCount only: the current count
	bool separatorBefore = false;

	[[nodiscard]] bool operator==(const AgentSessionMenuItem&) const noexcept = default;
};

//! Devin has no rename command or flag, so its rows offer no Rename.
[[nodiscard]] bool CanRenameAgentSession(EAgentSessionProvider provider) noexcept;
//! Codex and Devin keep sessions in their own databases; only their CLIs
//! (`codex delete`, `devin rm`) delete one, and that delete is permanent.
//! Claude Code and Pi sessions are files that go to the Recycle Bin.
[[nodiscard]] bool DeletesAgentSessionPermanently(EAgentSessionProvider provider) noexcept;

//! A session row's menu: Resume | Rename, Pin or Unpin, Archive or Unarchive |
//! Copy Session ID, Copy Resume Command | Delete | the ShowCount entries. An
//! archived row offers Unarchive and no Pin.
[[nodiscard]] std::vector<AgentSessionMenuItem> BuildAgentSessionMenu(
	EAgentSessionProvider provider, bool pinned, bool archived, std::size_t displayCount);
//! The menu of a row that is not a session (the Archived group): the
//! ShowCount entries only.
[[nodiscard]] std::vector<AgentSessionMenuItem> BuildAgentSessionsGroupMenu(std::size_t displayCount);

//! How a rename or delete in an agent's own store ended
//! (AgentSessionStoreOperations).
enum class EAgentSessionOperationStatus : std::uint8_t {
	Succeeded,
	Unsupported,	//! the agent has no such operation, or its CLI was not found
	SessionRunning,	//! the agent CLI is running this session; nothing was changed
	NotFound,	//! the session's files are gone
	Cancelled,
	Failed,
};

} // namespace workbench::projects
