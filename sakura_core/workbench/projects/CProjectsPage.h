/*! @file
 * @brief Native contributed page for hierarchical Projects navigation.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/projects/ProjectCatalogService.h"
#include "workbench/viewcontainer/CViewContainerPages.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace workbench::projects {

enum class EProjectsActivationStatus : std::uint8_t {
	FocusedCurrentWindow,
	OpenedNewWindow,
	Failed,
};

enum class EProjectsRemovalStatus : std::uint8_t {
	Removed,
	Failed,
};

using ProjectsSnapshotProvider =
	std::function<std::optional<std::vector<ProjectEntry>>() >;
using ProjectsWorkspaceSnapshotProvider = std::function<config::WorkspaceContextSnapshot()>;
using ProjectsWorkspaceRootProvider = std::function<std::wstring()>;
using ProjectActivationCallback =
	std::function<EProjectsActivationStatus(const ProjectEntry&, bool)>;
using ProjectWorktreeActivationCallback =
	std::function<EProjectsActivationStatus(std::wstring_view, bool)>;
using ProjectRemovalCallback =
	std::function<EProjectsRemovalStatus(const ProjectEntry&)>;

struct ProjectsPageOptions final {
	ProjectsSnapshotProvider projects;
	ProjectsWorkspaceSnapshotProvider workspace;
	ProjectsWorkspaceRootProvider workspaceRoot;
	ProjectActivationCallback activateProject;
	ProjectWorktreeActivationCallback activateWorktree;
	ProjectRemovalCallback removeProject;
};

[[nodiscard]] std::unique_ptr<viewcontainer::IViewContainerPage>
CreateProjectsPage(HWND parkingParent, ProjectsPageOptions options) noexcept;

} // namespace workbench::projects
