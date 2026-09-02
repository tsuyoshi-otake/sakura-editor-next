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

enum class EProjectBranchSummaryStatus : std::uint8_t {
	Loading,
	Ready,
	Mixed,
	NoRepository,
	Unavailable,
	Bounded,
};

struct ProjectRepositoryBranchObservation final {
	std::wstring label;
	bool succeeded = false;
	bool unavailable = false;

	[[nodiscard]] bool operator==(const ProjectRepositoryBranchObservation&) const noexcept = default;
};

struct ProjectBranchSummary final {
	EProjectBranchSummaryStatus status{ EProjectBranchSummaryStatus::Loading };
	std::wstring label{ L"Loading Git..." };
	std::size_t repositoryCount{};

	[[nodiscard]] bool operator==(const ProjectBranchSummary&) const noexcept = default;
};

struct ProjectBranchDiscoveryTarget final {
	std::wstring identity;
	std::vector<std::wstring> repositoryRoots;
	bool currentProject = false;
};

struct ProjectBranchDiscoveryRequest final {
	std::size_t projectIndex{};
	std::size_t repositoryIndex{};
	std::wstring identity;
	std::wstring repositoryRoot;

	[[nodiscard]] bool operator==(const ProjectBranchDiscoveryRequest&) const noexcept = default;
};

struct ProjectBranchDiscoveryPlan final {
	std::vector<ProjectBranchDiscoveryRequest> requests;
	std::vector<bool> truncatedProjects;
};

struct ProjectsRow final {
	EProjectsRowKind kind{ EProjectsRowKind::Project };
	std::size_t projectIndex{};
	std::optional<std::size_t> worktreeIndex;
	std::wstring label;
	std::wstring description;
	std::wstring trailing;
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

//! Projects are stable catalog entries. Every Project can expose a passive Git
//! summary, while only the active Project receives actionable worktree children.
[[nodiscard]] ProjectsProjection ProjectProjects(
	std::span<const ProjectEntry> projects,
	const config::WorkspaceContextSnapshot& workspace,
	const agent::AgentWorkspacesProjectionResult* worktrees,
	bool worktreesExpanded,
	std::span<const ProjectBranchSummary> branchSummaries = {},
	std::optional<EProjectsRowKind> preferredKind = std::nullopt,
	std::wstring_view preferredWorktreeIdentity = {});

//! Aggregates completed repository observations without inventing a branch for
//! failed or truncated inputs.  A mixed result counts distinct branch labels.
[[nodiscard]] ProjectBranchSummary SummarizeProjectBranches(
	std::span<const ProjectRepositoryBranchObservation> observations,
	bool complete,
	bool truncated = false);

//! Builds one deterministic, bounded queue.  The current Project is scheduled
//! first, duplicate roots within a Project are removed case-insensitively, and
//! every Project can contribute at least its first root before optional extra
//! roots consume the remaining global budget.
[[nodiscard]] ProjectBranchDiscoveryPlan PlanProjectBranchDiscovery(
	std::span<const ProjectBranchDiscoveryTarget> targets,
	std::size_t maximumRepositoriesPerProject,
	std::size_t maximumRequests);

[[nodiscard]] std::wstring ProjectDisplayName(const ProjectEntry& project);
[[nodiscard]] std::wstring ProjectWorktreeBranchLabel(
	const agent::AgentWorktreeRow& worktree);
//! Owner-drawn LISTBOX rows retain a complete text value for screen readers.
[[nodiscard]] std::wstring ProjectsAccessibleLabel(const ProjectsRow& row);

} // namespace workbench::projects
