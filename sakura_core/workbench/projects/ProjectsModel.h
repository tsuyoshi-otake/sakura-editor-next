/*! @file
 * @brief Pure hierarchical Projects sidebar projection.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/agent/AgentWorkspacesModel.h"
#include "workbench/projects/ProjectCatalogService.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace workbench::projects {

enum class EProjectsRowKind : std::uint8_t {
	Project,
	CurrentWorktree,
	WorktreesToggle,
	Worktree,
};

struct ProjectsRow final {
	EProjectsRowKind kind{ EProjectsRowKind::Project };
	std::size_t projectIndex{};
	std::optional<std::size_t> worktreeIndex;
	std::wstring label;
	std::wstring description;
	std::size_t hiddenWorktreeCount{};
	bool currentProject = false;
	bool primaryWorktree = false;
	bool expanded = false;
	bool enabled = true;

	[[nodiscard]] bool operator==(const ProjectsRow&) const noexcept = default;
};

struct ProjectsProjection final {
	std::vector<ProjectsRow> rows;
	std::optional<std::size_t> currentProjectIndex;
	std::optional<std::size_t> selectedRowIndex;
};

//! Projects are stable catalog entries. Only the active Project receives Git
//! worktree children, so discovery remains one bounded load path per window.
[[nodiscard]] ProjectsProjection ProjectProjects(
	std::span<const ProjectEntry> projects,
	const config::WorkspaceContextSnapshot& workspace,
	const agent::AgentWorkspacesProjectionResult* worktrees,
	bool worktreesExpanded,
	std::optional<EProjectsRowKind> preferredKind = std::nullopt,
	std::wstring_view preferredWorktreeIdentity = {});

[[nodiscard]] std::wstring ProjectDisplayName(const ProjectEntry& project);
//! Owner-drawn LISTBOX rows retain a complete text value for screen readers.
[[nodiscard]] std::wstring ProjectsAccessibleLabel(const ProjectsRow& row);

} // namespace workbench::projects
