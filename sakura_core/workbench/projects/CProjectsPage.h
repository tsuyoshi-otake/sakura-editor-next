/*! @file
 * @brief Native contributed page for hierarchical Projects navigation.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "config/WorkspaceContextTypes.h"
#include "workbench/projects/AgentSessionManagement.h"
#include "workbench/projects/AgentSessionViewStateService.h"
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
//! #408: what one discovery must find: enough unmarked sessions to fill the
//! view, plus every pinned or archived session however old it is.
struct ProjectAgentSessionsQuery final {
	std::wstring folder;
	std::size_t displayCount = kDefaultAgentSessionDisplayCount;
	std::vector<std::wstring> markedIdentities;	//! pinned and archived
};
//! #406: called on a page-owned worker thread for the current folder. It
//! must return promptly once `stop` (a manual-reset event) is signalled and
//! must not touch window state. nullopt means cancelled or unavailable; the
//! page then keeps the sessions it last accepted for the same folder.
using ProjectAgentSessionsProvider =
	std::function<std::optional<std::vector<AgentSessionRecord>>(ProjectAgentSessionsQuery query, HANDLE stop)>;
using ProjectAgentSessionResumeCallback =
	std::function<EProjectsActivationStatus(const AgentSessionRecord& session, std::wstring_view folder)>;
using ProjectsClock = std::function<std::int64_t()>;

//! #408 seams of the Sessions view's context menu. Every member is optional:
//! an entry whose seam is absent is not offered. Resume uses
//! resumeAgentSession.
struct ProjectAgentSessionManagement final {
	//! UI thread. The profile's pins, archive marks, and display count; null
	//! while the runtime has none.
	std::function<IAgentSessionViewStateService*()> viewState;
	//! UI thread, modal. The title the user entered, nullopt when cancelled.
	std::function<std::optional<std::wstring>(const AgentSessionRecord& session)> promptTitle;
	//! UI thread, modal. `permanent`: the agent's CLI deletes the session for
	//! good; otherwise its files go to the Recycle Bin.
	std::function<bool(const AgentSessionRecord& session, bool permanent)> confirmDelete;
	//! UI thread. Puts text on the clipboard.
	std::function<bool(std::wstring_view text)> copyText;
	//! Worker thread, like agentSessions: return promptly once `stop` is
	//! signalled and never touch window state.
	std::function<EAgentSessionOperationStatus(AgentSessionRecord session, std::wstring folder,
		std::wstring title, HANDLE stop)> rename;
	std::function<EAgentSessionOperationStatus(AgentSessionRecord session, std::wstring folder,
		HANDLE stop)> remove;
	//! UI thread. A rename or delete that ended without success or cancel.
	std::function<void(EAgentSessionCommand command, EAgentSessionOperationStatus status)> reportFailure;
	//! Test seam; absent = the native popup menu. Receives the entries the
	//! menu shows, in order, and returns the index of the chosen one, nullopt
	//! when the menu is dismissed.
	std::function<std::optional<std::size_t>(const std::vector<AgentSessionMenuItem>& offered)> chooseMenuItem;
};

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
	ProjectAgentSessionManagement sessionManagement;
	//! Unix seconds for the age column; absent = the system clock.
	ProjectsClock nowUnixSeconds;
};

[[nodiscard]] std::unique_ptr<viewcontainer::IViewContainerPage>
CreateProjectsPage(HWND parkingParent, ProjectsPageOptions options) noexcept;

struct ProjectsRowTextSplit final {
	int labelWidth{};
	int trailingWidth{};

	[[nodiscard]] bool operator==(const ProjectsRowTextSplit&) const noexcept = default;
};

//! Shares one row's text width between the label (project name, session title)
//! and the right-aligned trailing text (branch, agent and age), `gap` apart.
//! The label comes first (#414): when both do not fit it keeps its own width up
//! to three fifths of the row and the trailing text is the part that is cut.
//! Either part takes whatever the other does not need. Widths are pixels.
[[nodiscard]] constexpr ProjectsRowTextSplit SplitProjectsRowText(
	const int available, const int label, const int trailing, const int gap) noexcept
{
	if (available <= 0) return {};
	if (trailing <= 0) return { available, 0 };
	const int usable = available > gap ? available - (gap > 0 ? gap : 0) : 0;
	const int wanted = label > 0 ? label : 0;
	if (trailing >= usable - wanted) {
		const int guaranteed = usable / 5 * 3 < wanted ? usable / 5 * 3 : wanted;
		const int left = usable - trailing > guaranteed ? usable - trailing : guaranteed;
		return { left, usable - left };
	}
	return { usable - trailing, trailing };
}

} // namespace workbench::projects
