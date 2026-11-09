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
#include "workbench/projects/ProjectsModel.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/worktree/GitWorktreeDiscoverySource.h"

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
//! Returns local repository roots for passive branch inspection.  nullopt is
//! an unavailable/invalid Project; an empty vector is a valid Project with no
//! local repository roots.
using ProjectRepositoryRootsProvider =
	std::function<std::optional<std::vector<std::wstring>>(const ProjectEntry&)>;
using ProjectActivationCallback =
	std::function<EProjectsActivationStatus(const ProjectEntry&, bool)>;
using ProjectNewWindowActivationCallback =
	std::function<EProjectsActivationStatus(const ProjectEntry&)>;
using ProjectWorktreeActivationCallback =
	std::function<EProjectsActivationStatus(std::wstring_view, bool)>;
using ProjectRemovalCallback =
	std::function<EProjectsRemovalStatus(const ProjectEntry&)>;
using ProjectGitDiscoveryFactory =
	std::function<std::unique_ptr<worktree::GitWorktreeDiscoverySource>()>;
//! refresh=true only from the page's own Refresh() (activation / explicit
//! refresh); false when reacting to a change notification.
using ProjectAgentsProvider =
	std::function<std::vector<ProjectAgentEntry>(bool refresh)>;
using ProjectAgentActivationCallback =
	std::function<EProjectsActivationStatus(std::uint64_t terminalId)>;
//! The page registers its listener in Create, and calls it with an empty
//! function in Cleanup.
using ProjectAgentsChangedRegistrar =
	std::function<void(std::function<void()> listener)>;
//! #406: called on a page-owned worker thread with the current folder. It
//! must return promptly once `stop` (a manual-reset event) is signalled and
//! must not touch window state. nullopt means cancelled or unavailable; the
//! page then keeps the sessions it last accepted for the same folder.
using ProjectAgentSessionsProvider =
	std::function<std::optional<std::vector<AgentSessionRecord>>(std::wstring folder, HANDLE stop)>;
using ProjectAgentSessionResumeCallback =
	std::function<EProjectsActivationStatus(const AgentSessionRecord& session, std::wstring_view folder)>;
using ProjectsClock = std::function<std::int64_t()>;

struct ProjectsPageOptions final {
	ProjectsSnapshotProvider projects;
	ProjectsWorkspaceSnapshotProvider workspace;
	ProjectsWorkspaceRootProvider workspaceRoot;
	ProjectRepositoryRootsProvider repositoryRoots;
	ProjectActivationCallback activateProject;
	ProjectNewWindowActivationCallback activateProjectInNewWindow;
	ProjectWorktreeActivationCallback activateWorktree;
	ProjectRemovalCallback removeProject;
	//! Optional test/composition seam. Production constructs the hardened
	//! passive Git source when this factory is absent.
	ProjectGitDiscoveryFactory gitDiscoveryFactory;
	//! Optional #402 Phase A seams. Absent = no agent rows; never required
	//! for Create to succeed.
	ProjectAgentsProvider agents;
	ProjectAgentActivationCallback activateAgent;
	ProjectAgentsChangedRegistrar agentsChanged;
	//! Optional #406 seams. Absent agentSessions = no Sessions group.
	ProjectAgentSessionsProvider agentSessions;
	ProjectAgentSessionResumeCallback resumeAgentSession;
	//! Unix seconds for the age column; absent = the system clock.
	ProjectsClock nowUnixSeconds;
};

[[nodiscard]] std::unique_ptr<viewcontainer::IViewContainerPage>
CreateProjectsPage(HWND parkingParent, ProjectsPageOptions options) noexcept;

} // namespace workbench::projects
