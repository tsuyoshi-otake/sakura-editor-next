/*! @file
 * @brief Pure read-only projection for one Project's worktree children.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "workbench/worktree/GitWorktreePorcelainParser.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::agent {

enum class EAgentWorktreeWindowState : std::uint8_t {
	ThisWindow,
	OpenInNewWindow,
};

struct AgentWorktreeRow final {
	std::wstring path;
	std::wstring identity;
	std::wstring name;
	std::wstring branch;
	std::wstring head;
	EAgentWorktreeWindowState windowState{ EAgentWorktreeWindowState::OpenInNewWindow };
	bool detached = false;
	bool bare = false;
	bool locked = false;
	bool prunable = false;

	[[nodiscard]] bool operator==(const AgentWorktreeRow&) const noexcept = default;
};

enum class EAgentWorkspacesProjectionStatus : std::uint8_t {
	Succeeded,
	NoWorkspace,
	InvalidWorkspacePath,
	CurrentWorktreeUnavailable,
	AmbiguousCurrentWorktree,
};

struct AgentWorkspacesProjectionResult final {
	EAgentWorkspacesProjectionStatus status{ EAgentWorkspacesProjectionStatus::InvalidWorkspacePath };
	std::vector<AgentWorktreeRow> rows;
	std::optional<std::size_t> currentIndex;
	std::optional<std::size_t> selectedIndex;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EAgentWorkspacesProjectionStatus::Succeeded;
	}
};

//! True when `candidateIdentity` names `worktreeIdentity` itself or one of its
//! descendants. Both arguments must be identities emitted by the worktree path
//! normalizer, never display paths.
[[nodiscard]] bool IsWorktreeIdentityAtOrBelow(
	std::wstring_view candidateIdentity, std::wstring_view worktreeIdentity) noexcept;

//! Resolves an existing directory through junctions, symlinks, and volume
//! aliases into a comparison-only final-path identity. Missing or inaccessible
//! directories fail closed.
[[nodiscard]] std::optional<std::wstring> ResolvePhysicalDirectoryIdentity(
	std::wstring_view path) noexcept;

using AgentDirectoryIdentityResolver =
	std::function<std::optional<std::wstring>(std::wstring_view)>;

//! Builds the navigation-only row model. It does not read status, SCM provider
//! state, terminals, or other windows. The longest containing worktree wins for
//! an opened subdirectory; two equally specific matches fail closed.
[[nodiscard]] AgentWorkspacesProjectionResult ProjectAgentWorkspaces(
	std::span<const worktree::GitWorktreeRecord> records,
	std::wstring_view workspacePath,
	std::wstring_view selectedIdentity = {},
	const AgentDirectoryIdentityResolver& identityResolver = ResolvePhysicalDirectoryIdentity);

} // namespace workbench::agent
