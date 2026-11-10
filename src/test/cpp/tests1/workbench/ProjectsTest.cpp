/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "CSelectLang.h"
#include "ScopedLanguageSelection.h"
#include "sakura_rc.h"
#include "_main/ControlPlatformProjectCatalogStore.h"
#include "workbench/projects/CProjectsPage.h"
#include "workbench/projects/ProjectCatalogService.h"
#include "workbench/projects/ProjectsModel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using platform::controlipc::EEditorControlPlatformRuntimeState;
using platform::controlipc::EEditorControlStorageApplyCode;
using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
using platform::controlipc::EditorControlStorageApplyResult;
using platform::controlipc::EditorControlStorageCacheCoordinateResult;
using platform::controlipc::EditorControlStorageCacheCoordinates;
using platform::storage::EStorageScope;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageMutationRequest;
using platform::uri::Uri;
using namespace workbench::projects;
using namespace std::chrono_literals;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

Uri FileUri(std::wstring path)
{
	auto parsed = Uri::FromWindowsPath(path);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

ProjectEntry ProjectFolder(std::wstring path, std::optional<std::wstring> label = std::nullopt)
{
	return { EProjectKind::Folder, FileUri(std::move(path)), std::move(label) };
}

ProjectEntry ProjectWorkspace(std::wstring path, std::optional<std::wstring> label = std::nullopt)
{
	return { EProjectKind::Workspace, FileUri(std::move(path)), std::move(label) };
}

class ScriptedProjectStore final : public IProjectCatalogStore {
public:
	ProjectCatalogStoreLoadResult Load() override
	{
		++loads;
		return load;
	}

	ProjectCatalogStoreSaveResult Save(std::string payload) override
	{
		saved.push_back(std::move(payload));
		if (saves.empty()) return { EProjectCatalogStoreSaveStatus::Succeeded, {} };
		auto result = std::move(saves.front());
		saves.pop_front();
		return result;
	}

	ProjectCatalogStoreLoadResult load{
		EProjectCatalogStoreLoadStatus::NotFound, std::nullopt, {} };
	std::deque<ProjectCatalogStoreSaveResult> saves;
	std::vector<std::string> saved;
	int loads = 0;
};

EditorControlStorageCacheCoordinateResult Coordinates(const std::uint64_t revision)
{
	return { EEditorControlStorageCacheCoordinateCode::Ready,
		EEditorControlPlatformRuntimeState::Ready,
		EditorControlStorageCacheCoordinates{ kProfileId, 7, revision }, {} };
}

workbench::agent::AgentWorkspacesProjectionResult Worktrees()
{
	using namespace workbench::agent;
	return {
		.status = EAgentWorkspacesProjectionStatus::Succeeded,
		.rows = {
			{ .path = L"C:\\repo", .identity = L"c:\\repo", .name = L"sakura-editor-next",
				.branch = L"main", .windowState = EAgentWorktreeWindowState::ThisWindow },
			{ .path = L"C:\\worktrees\\feature", .identity = L"c:\\worktrees\\feature",
				.name = L"feature", .branch = L"feature" },
			{ .path = L"C:\\worktrees\\fix", .identity = L"c:\\worktrees\\fix",
				.name = L"fix", .branch = L"fix" },
		},
		.currentIndex = 0,
		.selectedIndex = 0,
	};
}

class ScriptedBranchRunner final : public workbench::worktree::IGitWorktreeListRunner {
public:
	workbench::worktree::GitWorktreeCommandResult RunListPorcelainZ(
		const workbench::worktree::GitWorktreeListRequest& request,
		const HANDLE cancellation) override
	{
		const auto now = m_active.fetch_add(1) + 1;
		auto observed = m_maxActive.load();
		while (observed < now && !m_maxActive.compare_exchange_weak(observed, now)) {}
		struct ActiveGuard final {
			std::atomic<int>& active;
			~ActiveGuard() { --active; }
		} guard{ m_active };
		{
			std::lock_guard lock(m_mutex);
			m_calls.push_back(request.repositoryPath);
		}
		if (m_block) {
			const auto waited = ::WaitForSingleObject(cancellation, 5000);
			return { waited == WAIT_OBJECT_0
				? workbench::worktree::EGitWorktreeCommandStatus::Cancelled
				: workbench::worktree::EGitWorktreeCommandStatus::TimedOut };
		}
		const auto found = branches.find(request.repositoryPath);
		if (found == branches.end()) {
			return { workbench::worktree::EGitWorktreeCommandStatus::Failed, 128 };
		}
		std::string path;
		path.reserve(request.repositoryPath.size());
		for (const wchar_t character : request.repositoryPath) {
			if (static_cast<unsigned int>(character) > 0x7fU) {
				return { workbench::worktree::EGitWorktreeCommandStatus::Failed, 1 };
			}
			path.push_back(static_cast<char>(character));
		}
		std::string output = "worktree " + path;
		output.push_back('\0');
		output += "HEAD aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
		output.push_back('\0');
		output += "branch refs/heads/" + found->second;
		output.push_back('\0');
		output.push_back('\0');
		if (const auto extra = linked.find(request.repositoryPath); extra != linked.end()) {
			for (const auto& [linkedPath, branch] : extra->second) {
				output += "worktree " + linkedPath;
				output.push_back('\0');
				output += "HEAD bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
				output.push_back('\0');
				output += "branch refs/heads/" + branch;
				output.push_back('\0');
				output.push_back('\0');
			}
		}
		workbench::worktree::GitWorktreeCommandResult result;
		result.status = workbench::worktree::EGitWorktreeCommandStatus::Succeeded;
		result.exitCode = 0;
		result.standardOutput.assign(output.begin(), output.end());
		return result;
	}

	void Block() noexcept { m_block = true; }
	[[nodiscard]] int Active() const noexcept { return m_active.load(); }
	[[nodiscard]] int MaximumActive() const noexcept { return m_maxActive.load(); }
	[[nodiscard]] std::vector<std::wstring> Calls() const
	{
		std::lock_guard lock(m_mutex);
		return m_calls;
	}

	std::map<std::wstring, std::string> branches;
	//! Linked worktrees listed after a repository's own, as ASCII path and branch.
	std::map<std::wstring, std::vector<std::pair<std::string, std::string>>> linked;

private:
	mutable std::mutex m_mutex;
	std::vector<std::wstring> m_calls;
	std::atomic<int> m_active{};
	std::atomic<int> m_maxActive{};
	bool m_block = false;
};

class TemporaryProjectRoots final {
public:
	TemporaryProjectRoots()
	{
		base = std::filesystem::temp_directory_path()
			/ (L"sakura-project-branches-" + std::to_wstring(::GetCurrentProcessId())
				+ L"-" + std::to_wstring(::GetTickCount64()));
		current = base / L"current";
		other = base / L"other";
		std::filesystem::create_directories(current);
		std::filesystem::create_directories(other);
	}

	~TemporaryProjectRoots()
	{
		std::error_code error;
		std::filesystem::remove_all(base, error);
	}

	std::filesystem::path base;
	std::filesystem::path current;
	std::filesystem::path other;
};

ProjectGitDiscoveryFactory DiscoveryFactory(const std::shared_ptr<ScriptedBranchRunner>& runner)
{
	return [runner] {
		workbench::worktree::GitWorktreeDiscoveryLimits limits;
		limits.retry.maximumAttempts = 1;
		return std::make_unique<workbench::worktree::GitWorktreeDiscoverySource>(
			runner, std::make_shared<workbench::worktree::SystemGitWorktreeRetryJitterSource>(),
			limits);
	};
}

//! The tooltip control of this thread whose one tool is the whole list window.
HWND FindListTooltip(const HWND list)
{
	struct Search final {
		HWND list;
		HWND found;
	} search{ list, nullptr };
	(void)::EnumThreadWindows(::GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL {
		auto& state = *reinterpret_cast<Search*>(data);
		wchar_t className[64]{};
		if (::GetClassNameW(window, className, static_cast<int>(std::size(className))) == 0
			|| std::wstring_view(className) != TOOLTIPS_CLASSW) return TRUE;
		const auto count = static_cast<UINT>(::SendMessageW(window, TTM_GETTOOLCOUNT, 0, 0));
		for (UINT index = 0; index < count; ++index) {
			TOOLINFOW tool{};
			tool.cbSize = sizeof(tool);
			if (::SendMessageW(window, TTM_ENUMTOOLSW, index, reinterpret_cast<LPARAM>(&tool)) != FALSE
				&& (tool.uFlags & TTF_IDISHWND) != 0
				&& tool.uId == reinterpret_cast<UINT_PTR>(state.list)) {
				state.found = window;
				return FALSE;
			}
		}
		return TRUE;
	}, reinterpret_cast<LPARAM>(&search));
	return search.found;
}

std::wstring ListTooltipText(const HWND tooltip, const HWND list)
{
	std::wstring text(1024, L'\0');
	TOOLINFOW tool{};
	tool.cbSize = sizeof(tool);
	tool.hwnd = ::GetParent(list);
	tool.uId = reinterpret_cast<UINT_PTR>(list);
	tool.lpszText = text.data();
	(void)::SendMessageW(tooltip, TTM_GETTEXTW, text.size(), reinterpret_cast<LPARAM>(&tool));
	text.resize(std::wcslen(text.c_str()));
	return text;
}

std::vector<std::wstring> ListBoxRows(const HWND list)
{
	std::vector<std::wstring> result;
	const auto count = static_cast<int>(::SendMessageW(list, LB_GETCOUNT, 0, 0));
	for (int index = 0; index < count; ++index) {
		const auto length = static_cast<int>(::SendMessageW(list, LB_GETTEXTLEN, index, 0));
		if (length < 0) continue;
		std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
		const auto copied = static_cast<int>(::SendMessageW(list, LB_GETTEXT,
			index, reinterpret_cast<LPARAM>(text.data())));
		if (copied >= 0) {
			text.resize(static_cast<std::size_t>(copied));
			result.push_back(std::move(text));
		}
	}
	return result;
}

} // namespace

TEST(ProjectCatalogService, PersistsStableEntriesSeparatelyFromRecentHistory)
{
	auto store = std::make_unique<ScriptedProjectStore>();
	auto* script = store.get();
	CProjectCatalogService service(std::move(store));
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded, service.Load().outcome);
	EXPECT_EQ(EProjectCatalogOutcome::Succeeded,
		service.RecordSuccessfulOpen(ProjectFolder(L"C:\\repo", L"Repository")).outcome);
	EXPECT_EQ(EProjectCatalogOutcome::Succeeded,
		service.RecordSuccessfulOpen(ProjectFolder(L"c:\\repo", L"Renamed")).outcome);

	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.size());
	ASSERT_TRUE(snapshot[0].label.has_value());
	EXPECT_EQ(L"Renamed", *snapshot[0].label);
	ASSERT_EQ(2U, script->saved.size());
	EXPECT_NE(std::string::npos, script->saved.back().find("Renamed"));
}

TEST(ProjectCatalogService, RemovesOnlyTheSavedProjectAndTreatsARepeatAsIdempotent)
{
	auto store = std::make_unique<ScriptedProjectStore>();
	auto* script = store.get();
	CProjectCatalogService service(std::move(store));
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded, service.Load().outcome);
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded,
		service.RecordSuccessfulOpen(ProjectFolder(L"C:\\repo")).outcome);
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded,
		service.RecordSuccessfulOpen(ProjectFolder(L"D:\\other")).outcome);

	const auto savesBeforeRemove = script->saved.size();
	EXPECT_EQ(EProjectCatalogOutcome::Succeeded,
		service.Remove(FileUri(L"c:\\repo")).outcome);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.size());
	EXPECT_TRUE(platform::uri::UriIdentityService::IsEqual(
		FileUri(L"D:\\other"), snapshot[0].uri));
	EXPECT_EQ(savesBeforeRemove + 1U, script->saved.size());

	EXPECT_EQ(EProjectCatalogOutcome::Succeeded,
		service.Remove(FileUri(L"C:\\repo")).outcome);
	EXPECT_EQ(savesBeforeRemove + 1U, script->saved.size());
}

TEST(ProjectCatalogService, FailedRemovePreservesTheAcceptedCatalogSnapshot)
{
	auto store = std::make_unique<ScriptedProjectStore>();
	auto* script = store.get();
	CProjectCatalogService service(std::move(store));
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded, service.Load().outcome);
	ASSERT_EQ(EProjectCatalogOutcome::Succeeded,
		service.RecordSuccessfulOpen(ProjectFolder(L"C:\\repo")).outcome);
	script->saves.push_back({ EProjectCatalogStoreSaveStatus::Failed, "injected failure" });

	const auto removed = service.Remove(FileUri(L"C:\\repo"));
	EXPECT_EQ(EProjectCatalogOutcome::Failed, removed.outcome);
	EXPECT_EQ("injected failure", removed.diagnostic);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.size());
	EXPECT_TRUE(platform::uri::UriIdentityService::IsEqual(
		FileUri(L"C:\\repo"), snapshot[0].uri));
}

TEST(ProjectCatalogService, CorruptEntriesFailClosedWithoutReplacingAcceptedState)
{
	auto store = std::make_unique<ScriptedProjectStore>();
	auto* script = store.get();
	store->load = { EProjectCatalogStoreLoadStatus::Succeeded,
		R"({"version":1,"entries":[{"kind":"bad","uri":"file:///C:/repo"}]})", {} };
	CProjectCatalogService service(std::move(store));

	EXPECT_EQ(EProjectCatalogOutcome::Failed, service.Load().outcome);
	EXPECT_EQ(EProjectCatalogState::Unavailable, service.State());
	EXPECT_TRUE(service.Snapshot().empty());
	EXPECT_EQ(EProjectCatalogOutcome::Failed,
		service.RecordSuccessfulOpen(ProjectFolder(L"C:\\repo")).outcome);
	EXPECT_TRUE(script->saved.empty());
}

TEST(ProjectsModel, GroupsCurrentCheckoutAndCollapsesTheRemainingWorktrees)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"sakura-editor-next"),
		ProjectFolder(L"D:\\other", L"other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	auto worktrees = Worktrees();

	const auto collapsed = ProjectProjects(projects, workspace, &worktrees, false);
	ASSERT_EQ(4U, collapsed.rows.size());
	EXPECT_EQ(EProjectsRowKind::Project, collapsed.rows[0].kind);
	EXPECT_TRUE(collapsed.rows[0].currentProject);
	EXPECT_EQ(EProjectsRowKind::CurrentWorktree, collapsed.rows[1].kind);
	EXPECT_TRUE(collapsed.rows[1].primaryWorktree);
	EXPECT_EQ(L"main", collapsed.rows[1].label);
	EXPECT_TRUE(collapsed.rows[1].description.empty());
	EXPECT_EQ(EProjectsRowKind::WorktreesToggle, collapsed.rows[2].kind);
	EXPECT_EQ(2U, collapsed.rows[2].hiddenWorktreeCount);
	EXPECT_EQ(L"Show linked worktrees, 2 linked worktrees",
		ProjectsAccessibleLabel(collapsed.rows[2]));
	EXPECT_NE(std::wstring::npos,
		ProjectsAccessibleLabel(collapsed.rows[1]).find(L"Primary, This Window"));
	EXPECT_EQ(EProjectsRowKind::Project, collapsed.rows[3].kind);

	const auto expanded = ProjectProjects(projects, workspace, &worktrees, true);
	ASSERT_EQ(6U, expanded.rows.size());
	EXPECT_EQ(EProjectsRowKind::Worktree, expanded.rows[3].kind);
	EXPECT_EQ(L"feature", expanded.rows[3].label);
	EXPECT_EQ(EProjectsRowKind::Worktree, expanded.rows[4].kind);
	EXPECT_EQ(EProjectsRowKind::Project, expanded.rows[5].kind);

	ProjectsTexts texts;
	texts.currentFolder = L"Current localized";
	texts.folder = L"Folder localized";
	texts.showLinkedWorktrees = L"Show localized";
	texts.linkedWorktreesFormat = L"Linked count: {0}";
	texts.primary = L"Primary localized";
	texts.thisWindow = L"Window localized";
	const auto localized = ProjectProjects(projects, workspace, &worktrees, false,
		{}, std::nullopt, {}, texts);
	EXPECT_EQ(L"Current localized", localized.rows[0].description);
	EXPECT_EQ(L"Folder localized", localized.rows[3].description);
	EXPECT_EQ(L"main", localized.rows[1].label);
	EXPECT_EQ(L"Show localized, Linked count: 2", ProjectsAccessibleLabel(localized.rows[2], texts));
	EXPECT_EQ(L"main, Primary localized, Window localized",
		ProjectsAccessibleLabel(localized.rows[1], texts));
}

TEST(ProjectsModel, SummarizesCommonMixedAndUnavailableProjectBranches)
{
	const std::vector<ProjectRepositoryBranchObservation> common{
		{ .label = L"main", .succeeded = true },
		{ .label = L"MAIN", .succeeded = true },
	};
	const auto commonSummary = SummarizeProjectBranches(common, true);
	EXPECT_EQ(EProjectBranchSummaryStatus::Ready, commonSummary.status);
	EXPECT_EQ(L"main", commonSummary.label);

	const std::vector<ProjectRepositoryBranchObservation> mixed{
		{ .label = L"main", .succeeded = true },
		{ .label = L"feature/projects", .succeeded = true },
	};
	const auto mixedSummary = SummarizeProjectBranches(mixed, true);
	EXPECT_EQ(EProjectBranchSummaryStatus::Mixed, mixedSummary.status);
	EXPECT_EQ(L"2 branches", mixedSummary.label);

	const std::vector<ProjectRepositoryBranchObservation> unavailable{
		{ .label = L"main", .succeeded = true },
		{ .unavailable = true },
	};
	EXPECT_EQ(EProjectBranchSummaryStatus::Unavailable,
		SummarizeProjectBranches(unavailable, true).status);
	EXPECT_EQ(EProjectBranchSummaryStatus::Loading,
		SummarizeProjectBranches(common, false).status);
	EXPECT_EQ(EProjectBranchSummaryStatus::Bounded,
		SummarizeProjectBranches(common, true, true).status);
	workbench::agent::AgentWorktreeRow detached{
		.name = L"checkout", .head = L"abcdef0", .detached = true };
	EXPECT_EQ(L"Detached @ abcdef0", ProjectWorktreeBranchLabel(detached));

	ProjectsTexts texts;
	texts.loadingGit = L"Loading localized";
	texts.noGit = L"No repository localized";
	texts.gitUnavailable = L"Unavailable localized";
	texts.branchesFormat = L"Branches: {0}";
	texts.repositoriesFormat = L"Repositories: {0}+";
	texts.detached = L"Detached localized";
	EXPECT_EQ(L"main", SummarizeProjectBranches(common, true, false, texts).label);
	EXPECT_EQ(L"Branches: 2", SummarizeProjectBranches(mixed, true, false, texts).label);
	EXPECT_EQ(L"Repositories: 2+", SummarizeProjectBranches(common, true, true, texts).label);
	EXPECT_EQ(L"Loading localized", SummarizeProjectBranches(common, false, false, texts).label);
	EXPECT_EQ(L"No repository localized", SummarizeProjectBranches({}, true, false, texts).label);
	EXPECT_EQ(L"Unavailable localized", SummarizeProjectBranches(unavailable, true, false, texts).label);
	EXPECT_EQ(L"Detached localized @ abcdef0", ProjectWorktreeBranchLabel(detached, texts));
}

TEST(ProjectsModel, PlansCurrentProjectFirstWithCaseInsensitiveDedupeAndBounds)
{
	const std::vector<ProjectBranchDiscoveryTarget> targets{
		{ .identity = L"project-a", .repositoryRoots = { L"C:\\A", L"c:\\a", L"C:\\A2" } },
		{ .identity = L"project-b", .repositoryRoots = { L"C:\\B", L"C:\\B2", L"C:\\B3" },
			.currentProject = true },
	};
	const auto plan = PlanProjectBranchDiscovery(targets, 2, 3);
	ASSERT_EQ(3U, plan.requests.size());
	EXPECT_EQ(L"project-b", plan.requests[0].identity);
	EXPECT_EQ(L"C:\\B", plan.requests[0].repositoryRoot);
	EXPECT_EQ(L"project-a", plan.requests[1].identity);
	EXPECT_EQ(L"C:\\A", plan.requests[1].repositoryRoot);
	EXPECT_EQ(L"project-b", plan.requests[2].identity);
	EXPECT_EQ(L"C:\\B2", plan.requests[2].repositoryRoot);
	EXPECT_TRUE(plan.truncatedProjects[0]);
	EXPECT_TRUE(plan.truncatedProjects[1]);
}

TEST(ProjectsModel, GivesEveryCatalogProjectOneRequestBeforeUsingTheGlobalBudgetForExtras)
{
	std::vector<ProjectBranchDiscoveryTarget> targets;
	for (std::size_t index = 0; index < kMaximumProjects; ++index) {
		targets.push_back({
			.identity = L"project-" + std::to_wstring(index),
			.repositoryRoots = {
				L"C:\\repo-" + std::to_wstring(index),
				L"C:\\extra-" + std::to_wstring(index),
			},
			.currentProject = index == kMaximumProjects - 1U,
		});
	}
	const auto plan = PlanProjectBranchDiscovery(targets, 8, kMaximumProjects);
	ASSERT_EQ(kMaximumProjects, plan.requests.size());
	EXPECT_EQ(kMaximumProjects - 1U, plan.requests.front().projectIndex);
	std::vector<bool> scheduled(kMaximumProjects, false);
	for (const auto& request : plan.requests) {
		EXPECT_EQ(0U, request.repositoryIndex);
		scheduled[request.projectIndex] = true;
	}
	EXPECT_TRUE(std::ranges::all_of(scheduled, [](const bool value) { return value; }));
	EXPECT_TRUE(std::ranges::all_of(plan.truncatedProjects,
		[](const bool value) { return value; }));
}

TEST(ProjectsModel, ExposesBranchSummaryForUnselectedProjectWithoutChangingSelection)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"Current"),
		ProjectFolder(L"D:\\other", L"Other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	const std::vector<ProjectBranchSummary> summaries{
		{ .status = EProjectBranchSummaryStatus::Ready, .label = L"main", .repositoryCount = 1 },
		{ .status = EProjectBranchSummaryStatus::Ready, .label = L"feature/other", .repositoryCount = 1 },
	};

	const auto projection = ProjectProjects(projects, workspace, nullptr, false, summaries);
	ASSERT_EQ(2U, projection.rows.size());
	ASSERT_EQ(0U, projection.currentProjectIndex);
	ASSERT_EQ(0U, projection.selectedRowIndex);
	EXPECT_EQ(L"main", projection.rows[0].trailing);
	EXPECT_EQ(L"feature/other", projection.rows[1].trailing);
	EXPECT_NE(std::wstring::npos,
		ProjectsAccessibleLabel(projection.rows[1]).find(L"feature/other"));
}

TEST(ProjectsModel, UsesWorkspaceIdentityAndPreservesUnsafeWorktreeStates)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"folder"),
		ProjectWorkspace(L"C:\\workspaces\\product.code-workspace", L"product"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Workspace;
	workspace.workspaceConfigUri = FileUri(L"C:\\workspaces\\product.code-workspace");
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	auto worktrees = Worktrees();
	worktrees.rows[1].locked = true;
	worktrees.rows[2].prunable = true;

	const auto projection = ProjectProjects(projects, workspace, &worktrees, true);
	ASSERT_EQ(6U, projection.rows.size());
	ASSERT_EQ(1U, projection.currentProjectIndex);
	EXPECT_FALSE(projection.rows[0].currentProject);
	EXPECT_TRUE(projection.rows[1].currentProject);
	EXPECT_EQ(EProjectsRowKind::CurrentWorktree, projection.rows[2].kind);
	EXPECT_FALSE(projection.rows[4].enabled);
	EXPECT_NE(std::wstring::npos,
		ProjectsAccessibleLabel(projection.rows[4]).find(L"Unavailable"));
	EXPECT_FALSE(projection.rows[5].enabled);
}

TEST(ProjectsModel, AgentRowsSitBetweenCurrentWorktreeAndToggleOnTheCurrentProjectOnly)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"sakura-editor-next"),
		ProjectFolder(L"D:\\other", L"other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	auto worktrees = Worktrees();
	const std::vector<ProjectAgentEntry> agents{
		{ .identity = L"terminal-instance:3", .terminalId = 3,
			.name = L"Claude Code", .sessionTitle = L"fix login bug" },
		{ .identity = L"terminal-instance:9", .terminalId = 9, .name = L"Devin" },
	};

	const auto projection = ProjectProjects(projects, workspace, &worktrees, false,
		{}, std::nullopt, {}, {}, agents);
	ASSERT_EQ(6U, projection.rows.size());
	EXPECT_EQ(EProjectsRowKind::Project, projection.rows[0].kind);
	EXPECT_EQ(EProjectsRowKind::CurrentWorktree, projection.rows[1].kind);
	ASSERT_EQ(EProjectsRowKind::Agent, projection.rows[2].kind);
	ASSERT_TRUE(projection.rows[2].agentIndex.has_value());
	EXPECT_EQ(0U, *projection.rows[2].agentIndex);
	EXPECT_EQ(L"Claude Code", projection.rows[2].label);
	// The row shows the product name only (Herdr's default agent row); the
	// agent's session name is hover text, not a second visible column.
	EXPECT_TRUE(projection.rows[2].description.empty());
	EXPECT_EQ(L"fix login bug", projection.rows[2].tooltip);
	EXPECT_TRUE(projection.rows[2].currentProject);
	EXPECT_TRUE(projection.rows[2].enabled);
	EXPECT_EQ(L"Claude Code", ProjectsAccessibleLabel(projection.rows[2]));
	ASSERT_EQ(EProjectsRowKind::Agent, projection.rows[3].kind);
	ASSERT_TRUE(projection.rows[3].agentIndex.has_value());
	EXPECT_EQ(1U, *projection.rows[3].agentIndex);
	EXPECT_EQ(L"Devin", projection.rows[3].label);
	EXPECT_TRUE(projection.rows[3].description.empty());
	EXPECT_TRUE(projection.rows[3].tooltip.empty());
	EXPECT_EQ(EProjectsRowKind::WorktreesToggle, projection.rows[4].kind);
	EXPECT_EQ(EProjectsRowKind::Project, projection.rows[5].kind);
	EXPECT_FALSE(projection.rows[5].currentProject);
}

TEST(ProjectsModel, AgentRowsFollowTheProjectWithoutWorktreesAndRequireACurrentProject)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"repo"),
		ProjectFolder(L"D:\\other", L"other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	const std::vector<ProjectAgentEntry> agents{
		{ .identity = L"terminal-instance:3", .terminalId = 3, .name = L"Claude Code" },
	};

	const auto projection = ProjectProjects(projects, workspace, nullptr, false,
		{}, std::nullopt, {}, {}, agents);
	ASSERT_EQ(3U, projection.rows.size());
	EXPECT_EQ(EProjectsRowKind::Project, projection.rows[0].kind);
	EXPECT_EQ(EProjectsRowKind::Agent, projection.rows[1].kind);
	EXPECT_EQ(0U, projection.rows[1].projectIndex);
	EXPECT_EQ(EProjectsRowKind::Project, projection.rows[2].kind);
	EXPECT_EQ(1U, projection.rows[2].projectIndex);

	// A failed/empty worktree projection does not suppress the Agent rows.
	auto failedWorktrees = Worktrees();
	failedWorktrees.status = workbench::agent::EAgentWorkspacesProjectionStatus::CurrentWorktreeUnavailable;
	failedWorktrees.currentIndex.reset();
	failedWorktrees.rows.clear();
	const auto failed = ProjectProjects(projects, workspace, &failedWorktrees, false,
		{}, std::nullopt, {}, {}, agents);
	ASSERT_EQ(3U, failed.rows.size());
	EXPECT_EQ(EProjectsRowKind::Agent, failed.rows[1].kind);

	// No current project: agent rows are never emitted.
	config::WorkspaceContextSnapshot empty;
	empty.kind = config::EWorkspaceKind::Empty;
	const auto orphaned = ProjectProjects(projects, empty, nullptr, false,
		{}, std::nullopt, {}, {}, agents);
	ASSERT_EQ(2U, orphaned.rows.size());
	EXPECT_TRUE(std::ranges::none_of(orphaned.rows, [](const auto& row) {
		return row.kind == EProjectsRowKind::Agent;
	}));
}

TEST(ProjectsModel, AgentRowShowsOnlyTheNameAndKeepsTheSessionTitleForHover)
{
	const std::vector<ProjectEntry> projects{ ProjectFolder(L"C:\\repo", L"repo") };
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	const std::vector<ProjectAgentEntry> agents{
		{ .identity = L"terminal-instance:1", .terminalId = 1,
			.name = L"Claude Code", .sessionTitle = L"Claude Code" },
		{ .identity = L"terminal-instance:2", .terminalId = 2,
			.name = L"Devin", .sessionTitle = L"" },
		{ .identity = L"terminal-instance:3", .terminalId = 3,
			.name = L"Copilot", .sessionTitle = L"Fix login bug" },
	};

	const auto projection = ProjectProjects(projects, workspace, nullptr, false,
		{}, std::nullopt, {}, {}, agents);
	ASSERT_EQ(4U, projection.rows.size());
	for( std::size_t index = 1; index < projection.rows.size(); ++index ) {
		EXPECT_TRUE(projection.rows[index].description.empty()) << index;
		EXPECT_EQ(projection.rows[index].label, ProjectsAccessibleLabel(projection.rows[index])) << index;
	}
	// A title that only repeats the product name, or no title, has no hover.
	EXPECT_TRUE(projection.rows[1].tooltip.empty());
	EXPECT_TRUE(projection.rows[2].tooltip.empty());
	EXPECT_EQ(L"Fix login bug", projection.rows[3].tooltip);
}

TEST(ProjectsModel, PreferredAgentIdentitySelectsTheMatchingRowAndFallsBackWhenUnknown)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"repo"),
		ProjectFolder(L"D:\\other", L"other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	const std::vector<ProjectAgentEntry> agents{
		{ .identity = L"terminal-instance:3", .terminalId = 3, .name = L"Claude Code" },
		{ .identity = L"terminal-instance:9", .terminalId = 9, .name = L"Devin" },
	};

	const auto matched = ProjectProjects(projects, workspace, nullptr, false,
		{}, EProjectsRowKind::Agent, {}, {}, agents, L"terminal-instance:9");
	ASSERT_TRUE(matched.selectedRowIndex.has_value());
	EXPECT_EQ(EProjectsRowKind::Agent, matched.rows[*matched.selectedRowIndex].kind);
	EXPECT_EQ(L"Devin", matched.rows[*matched.selectedRowIndex].label);

	const auto unmatched = ProjectProjects(projects, workspace, nullptr, false,
		{}, EProjectsRowKind::Agent, {}, {}, agents, L"terminal-instance:404");
	ASSERT_TRUE(unmatched.selectedRowIndex.has_value());
	EXPECT_EQ(EProjectsRowKind::Project, unmatched.rows[*unmatched.selectedRowIndex].kind);
	EXPECT_TRUE(unmatched.rows[*unmatched.selectedRowIndex].currentProject);
}

// #411: the Sessions View lists the folder's sessions newest first, as top-level
// rows of their own; the Projects View's tree no longer carries them.
TEST(ProjectsModel, SessionsViewListsTheFolderSessionsApartFromTheProjectTree)
{
	const std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"repo"),
		ProjectFolder(L"D:\\other", L"other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	auto worktrees = Worktrees();
	const std::vector<ProjectAgentEntry> agents{
		{ .identity = L"terminal-instance:3", .terminalId = 3, .name = L"Claude Code" },
	};
	const auto tree = ProjectProjects(projects, workspace, &worktrees, false,
		{}, std::nullopt, {}, {}, agents, {});
	ASSERT_EQ(5U, tree.rows.size());
	EXPECT_EQ(EProjectsRowKind::Project, tree.rows[0].kind);
	EXPECT_EQ(EProjectsRowKind::CurrentWorktree, tree.rows[1].kind);
	EXPECT_EQ(EProjectsRowKind::Agent, tree.rows[2].kind);
	EXPECT_EQ(EProjectsRowKind::WorktreesToggle, tree.rows[3].kind);
	EXPECT_EQ(EProjectsRowKind::Project, tree.rows[4].kind);

	constexpr std::int64_t now = 1'000'000;
	const std::vector<AgentSessionRecord> sessions{
		{ EAgentSessionProvider::Codex, L"019a-codex", L"Add a sessions node", now - 120 },
		{ EAgentSessionProvider::ClaudeCode, L"0f1e-claude", L"", 0 },
	};
	const ProjectSessionsInput ready{ .state = EProjectSessionsState::Ready, .sessions = sessions,
		.nowUnixSeconds = now };
	const auto projection = ProjectAgentSessions(ready);
	ASSERT_EQ(2U, projection.rows.size());
	EXPECT_FALSE(projection.selectedRowIndex.has_value());
	EXPECT_TRUE(projection.emptyMessage.empty());

	const auto& codex = projection.rows[0];
	EXPECT_EQ(0U, codex.sessionIndex);
	EXPECT_EQ(L"Add a sessions node", codex.label);
	EXPECT_EQ(L"Codex", codex.description);
	EXPECT_EQ(L"2m", codex.trailing);
	EXPECT_EQ(L"Add a sessions node\nCodex - 019a-codex\nDouble-click to resume in a new terminal",
		codex.tooltip);
	EXPECT_EQ(L"Add a sessions node, Codex, 2m", AgentSessionsAccessibleLabel(codex));

	const auto& untitled = projection.rows[1];
	EXPECT_EQ(1U, untitled.sessionIndex);
	EXPECT_EQ(L"Untitled session", untitled.label);
	EXPECT_EQ(L"Claude Code", untitled.description);
	EXPECT_TRUE(untitled.trailing.empty());
	EXPECT_EQ(L"Untitled session, Claude Code", AgentSessionsAccessibleLabel(untitled));

	// Loading with nothing accepted yet says so in the View's body.
	const ProjectSessionsInput loading{ .state = EProjectSessionsState::Loading, .nowUnixSeconds = now };
	const auto first = ProjectAgentSessions(loading);
	EXPECT_TRUE(first.rows.empty());
	EXPECT_EQ(L"Loading...", first.emptyMessage);

	// Reloading keeps the last accepted sessions on screen.
	auto reloading = ready;
	reloading.state = EProjectSessionsState::Loading;
	const auto reloaded = ProjectAgentSessions(reloading);
	EXPECT_EQ(2U, reloaded.rows.size());
	EXPECT_TRUE(reloaded.emptyMessage.empty());

	// Ready with none says there are none; Unavailable lists nothing, even
	// sessions accepted earlier.
	const ProjectSessionsInput empty{ .state = EProjectSessionsState::Ready };
	EXPECT_TRUE(ProjectAgentSessions(empty).rows.empty());
	EXPECT_EQ(L"No sessions", ProjectAgentSessions(empty).emptyMessage);
	const ProjectSessionsInput unavailable{ .state = EProjectSessionsState::Unavailable, .sessions = sessions };
	EXPECT_TRUE(ProjectAgentSessions(unavailable).rows.empty());
	EXPECT_EQ(L"Unavailable", ProjectAgentSessions(unavailable).emptyMessage);

	// The texts come from the caller, so a language switch reprojects them.
	ProjectsTexts texts;
	texts.untitledSession = L"(untitled)";
	texts.noSessions = L"none";
	EXPECT_EQ(L"(untitled)", ProjectAgentSessions(ready, texts).rows[1].label);
	EXPECT_EQ(L"none", ProjectAgentSessions(empty, texts).emptyMessage);
}

// #410: an agent is waiting until a session of its own provider shows
// activity at or after the moment its row appeared; one session covers one agent.
TEST(ProjectsModel, CountsAgentsAwaitingTheirOwnSessionHistory)
{
	using Sighting = ProjectAgentSighting;
	const auto session = [](const EAgentSessionProvider provider, const std::int64_t activity) {
		return AgentSessionRecord{ provider, L"id" + std::to_wstring(activity), L"", activity };
	};
	const std::vector<Sighting> none;
	EXPECT_EQ(0U, CountAgentsAwaitingSessionHistory(none, {}));

	const std::vector<Sighting> codex{ { L"Codex", 100 } };
	EXPECT_EQ(1U, CountAgentsAwaitingSessionHistory(codex, {}));
	const std::vector<AgentSessionRecord> older{ session(EAgentSessionProvider::Codex, 99) };
	EXPECT_EQ(1U, CountAgentsAwaitingSessionHistory(codex, older));
	const std::vector<AgentSessionRecord> same{ session(EAgentSessionProvider::Codex, 100) };
	EXPECT_EQ(0U, CountAgentsAwaitingSessionHistory(codex, same));
	// Another provider's session, or one with an unknown time, covers nothing.
	const std::vector<AgentSessionRecord> foreign{
		session(EAgentSessionProvider::ClaudeCode, 500), session(EAgentSessionProvider::Codex, 0) };
	EXPECT_EQ(1U, CountAgentsAwaitingSessionHistory(codex, foreign));

	// Two Codex agents need two sessions; the later agent takes the newest.
	const std::vector<Sighting> two{ { L"Codex", 100 }, { L"Codex", 300 } };
	const std::vector<AgentSessionRecord> one{ session(EAgentSessionProvider::Codex, 350) };
	EXPECT_EQ(1U, CountAgentsAwaitingSessionHistory(two, one));
	const std::vector<AgentSessionRecord> newestTooOldForTheLatest{
		session(EAgentSessionProvider::Codex, 250), session(EAgentSessionProvider::Codex, 50) };
	EXPECT_EQ(1U, CountAgentsAwaitingSessionHistory(two, newestTooOldForTheLatest));
	const std::vector<AgentSessionRecord> both{
		session(EAgentSessionProvider::Codex, 120), session(EAgentSessionProvider::Codex, 310) };
	EXPECT_EQ(0U, CountAgentsAwaitingSessionHistory(two, both));

	// Each provider matches by its display name; other products never wait.
	const std::vector<Sighting> mixed{
		{ L"Claude Code", 10 }, { L"Devin", 10 }, { L"Pi", 10 }, { L"Cursor", 10 } };
	EXPECT_EQ(3U, CountAgentsAwaitingSessionHistory(mixed, {}));
	const std::vector<AgentSessionRecord> covered{
		session(EAgentSessionProvider::ClaudeCode, 10), session(EAgentSessionProvider::Devin, 11),
		session(EAgentSessionProvider::Pi, 12) };
	EXPECT_EQ(0U, CountAgentsAwaitingSessionHistory(mixed, covered));
}

TEST(ProjectsModel, PreferredSessionIdentitySelectsTheMatchingSessionRow)
{
	const std::vector<AgentSessionRecord> sessions{
		{ EAgentSessionProvider::Codex, L"same-id", L"Codex thread", 20 },
		{ EAgentSessionProvider::ClaudeCode, L"same-id", L"Claude session", 10 },
	};
	ProjectSessionsInput input{ .state = EProjectSessionsState::Ready, .sessions = sessions,
		.nowUnixSeconds = 100, .preferredIdentity = L"claude-code:same-id" };
	const auto matched = ProjectAgentSessions(input);
	ASSERT_TRUE(matched.selectedRowIndex.has_value());
	EXPECT_EQ(L"Claude session", matched.rows[*matched.selectedRowIndex].label);

	// An unknown identity selects nothing; the list keeps its own selection.
	input.preferredIdentity = L"pi:missing";
	EXPECT_FALSE(ProjectAgentSessions(input).selectedRowIndex.has_value());
}

TEST(ProjectsPage, DeleteRemovesTheCatalogEntryWithoutChangingWorkspaceState)
{
	std::vector<ProjectEntry> projects{
		ProjectFolder(L"C:\\repo", L"Repository"),
		ProjectFolder(L"D:\\other", L"Other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Empty;
	std::optional<Uri> removedUri;
	int removeCalls = 0;

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [] { return std::wstring{}; };
	options.repositoryRoots = [](const ProjectEntry&) {
		return std::optional(std::vector<std::wstring>{});
	};
	options.activateProject = [](const ProjectEntry&, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [](std::wstring_view, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [&projects, &removedUri, &removeCalls](const ProjectEntry& project) {
		++removeCalls;
		removedUri = project.uri;
		std::erase_if(projects, [&project](const auto& candidate) {
			return platform::uri::UriIdentityService::IsEqual(candidate.uri, project.uri);
		});
		return EProjectsRemovalStatus::Removed;
	};

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 320, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(
		hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar",
		workbench::layout::EViewContainerLocation::Sidebar,
		nativeHost,
	};
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(
		page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 320, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND list = ::FindWindowExW(pageWindow, nullptr, L"LISTBOX", nullptr);
	ASSERT_NE(nullptr, list);
	ASSERT_EQ(2, ::SendMessageW(list, LB_GETCOUNT, 0, 0));
	ASSERT_NE(LB_ERR, ::SendMessageW(list, LB_SETCURSEL, 1, 0));
	MSG message{};
	message.hwnd = list;
	message.message = WM_KEYDOWN;
	message.wParam = VK_DELETE;
	EXPECT_TRUE(nativePage->PreTranslateProjection(message));

	EXPECT_EQ(1, removeCalls);
	ASSERT_TRUE(removedUri.has_value());
	EXPECT_TRUE(platform::uri::UriIdentityService::IsEqual(FileUri(L"D:\\other"), *removedUri));
	ASSERT_EQ(1U, projects.size());
	EXPECT_TRUE(platform::uri::UriIdentityService::IsEqual(FileUri(L"C:\\repo"), projects[0].uri));
	EXPECT_EQ(config::EWorkspaceKind::Empty, workspace.kind);
	EXPECT_EQ(1, ::SendMessageW(list, LB_GETCOUNT, 0, 0));

	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	page.reset();
	::DestroyWindow(hostWindow);
}

// #407: the Projects list draws the same VS Code overlay the Explorer and
// Source Control lists draw, hides the platform bar, and still scrolls by wheel.
TEST(ProjectsPage, ListUsesTheSharedOverlayScrollbarAndScrollsByWheel)
{
	std::vector<ProjectEntry> projects;
	for (int index = 0; index < 40; ++index) {
		projects.push_back(ProjectFolder(L"C:\\repo" + std::to_wstring(index),
			L"Repository " + std::to_wstring(index)));
	}
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Empty;

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [] { return std::wstring{}; };
	options.repositoryRoots = [](const ProjectEntry&) {
		return std::optional(std::vector<std::wstring>{});
	};
	options.activateProject = [](const ProjectEntry&, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [](std::wstring_view, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 320, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(
		hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar",
		workbench::layout::EViewContainerLocation::Sidebar,
		nativeHost,
	};
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(
		page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 320, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND list = ::FindWindowExW(pageWindow, nullptr, L"LISTBOX", nullptr);
	ASSERT_NE(nullptr, list);
	ASSERT_EQ(40, ::SendMessageW(list, LB_GETCOUNT, 0, 0));

	// The overlay is the list's sibling, at the list's right edge, and the
	// list no longer shows the platform bar.
	const HWND scrollbar = ::FindWindowExW(
		pageWindow, nullptr, L"SakuraWorkbenchOverlayScrollbar", nullptr);
	ASSERT_NE(nullptr, scrollbar);
	EXPECT_NE(0L, ::GetWindowLongPtrW(scrollbar, GWL_STYLE) & WS_VISIBLE);
	EXPECT_EQ(0L, ::GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL);
	EXPECT_NE(0L, ::GetWindowLongPtrW(list, GWL_STYLE) & WS_CLIPSIBLINGS);
	RECT overlay{};
	ASSERT_TRUE(::GetWindowRect(scrollbar, &overlay));
	::MapWindowPoints(nullptr, pageWindow, reinterpret_cast<POINT*>(&overlay), 2);
	EXPECT_EQ(310, overlay.left);
	EXPECT_EQ(0, overlay.top);
	EXPECT_EQ(320, overlay.right);
	EXPECT_EQ(400, overlay.bottom);

	// A list box with a hidden bar ignores the wheel; the page scrolls it.
	UINT linesPerNotch = 3;
	(void)::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
	ASSERT_EQ(0, ::SendMessageW(list, LB_GETTOPINDEX, 0, 0));
	(void)::SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
	if (linesPerNotch != 0) {
		EXPECT_GT(::SendMessageW(list, LB_GETTOPINDEX, 0, 0), 0);
	}
	EXPECT_EQ(0L, ::GetWindowLongPtrW(list, GWL_STYLE) & WS_VSCROLL);

	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	EXPECT_FALSE(::IsWindow(scrollbar));
	page.reset();
	::DestroyWindow(hostWindow);
}

TEST(ProjectsPage, ShowsUnselectedProjectBranchWithoutActivatingOrMutatingWorkspace)
{
	tests1::ScopedLanguageSelection restoreLanguage;
	CSelectLang::InitializeLanguageEnvironment();
	CSelectLang::ChangeLang(L"sakura_lang_en_US.dll");
	ASSERT_EQ(L"Projects", CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_TITLE));
	TemporaryProjectRoots roots;
	std::vector<ProjectEntry> projects{
		ProjectFolder(roots.current.wstring(), L"Current"),
		ProjectFolder(roots.other.wstring(), L"Other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.workspaceIdentityKey = L"current-project";
	workspace.revision = 17;
	workspace.folders.push_back({ FileUri(roots.current.wstring()), L"repo" });
	const auto runner = std::make_shared<ScriptedBranchRunner>();
	runner->branches.emplace(roots.current.wstring(), "main");
	runner->branches.emplace(roots.other.wstring(), "feature/other");
	int activationCalls = 0;

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [&roots] { return roots.current.wstring(); };
	options.repositoryRoots = [](const ProjectEntry& project) {
		const auto path = project.uri.ToWindowsPath();
		return path.value ? std::optional(std::vector<std::wstring>{ *path.value })
			: std::nullopt;
	};
	options.activateProject = [&activationCalls](const ProjectEntry&, bool) {
		++activationCalls;
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [&activationCalls](std::wstring_view, bool) {
		++activationCalls;
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.gitDiscoveryFactory = DiscoveryFactory(runner);

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 420, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 420, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND list = ::FindWindowExW(pageWindow, nullptr, L"LISTBOX", nullptr);
	ASSERT_NE(nullptr, list);
	std::vector<std::wstring> rows;
	for (int attempt = 0; attempt < 200; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
		rows = ListBoxRows(list);
		if (std::ranges::any_of(rows, [](const auto& row) {
			return row.find(L"Other, Folder, feature/other") != std::wstring::npos;
		})) break;
		std::this_thread::sleep_for(5ms);
	}
	std::string rowDiagnostic;
	for (const auto& row : rows) {
		if (!rowDiagnostic.empty()) rowDiagnostic += " | ";
		for (const wchar_t character : row) {
			rowDiagnostic.push_back(static_cast<unsigned int>(character) <= 0x7fU
				? static_cast<char>(character) : '?');
		}
	}
	EXPECT_TRUE(std::ranges::any_of(rows, [](const auto& row) {
		return row.find(L"Current, Current Folder, main") != std::wstring::npos;
	})) << rowDiagnostic;
	EXPECT_TRUE(std::ranges::any_of(rows, [](const auto& row) {
		return row.find(L"Other, Folder, feature/other") != std::wstring::npos;
	})) << rowDiagnostic;
	EXPECT_EQ(0, activationCalls);
	EXPECT_EQ(17U, workspace.revision);
	EXPECT_EQ(L"current-project", workspace.workspaceIdentityKey);
	EXPECT_EQ(0, ::SendMessageW(list, LB_GETCURSEL, 0, 0));
	const auto calls = runner->Calls();
	ASSERT_EQ(2U, calls.size());
	EXPECT_EQ(roots.current.wstring(), calls[0]);
	EXPECT_EQ(roots.other.wstring(), calls[1]);
	EXPECT_EQ(1, runner->MaximumActive());
	nativePage->RefreshProjectionStrings();
	nativePage->LayoutProjection(bounds, bounds, 96);
	EXPECT_EQ(2U, runner->Calls().size());
	EXPECT_TRUE(std::ranges::any_of(ListBoxRows(list), [](const auto& row) {
		return row.find(L"Other, Folder, feature/other") != std::wstring::npos;
	}));

	// Language changes update the retained native rows without Git or workspace work.
	const auto otherIndex = ::SendMessageW(list, LB_GETCOUNT, 0, 0) - 1;
	(void)::SendMessageW(list, LB_SETCURSEL, otherIndex, 0);
	const auto originalRows = ListBoxRows(list);
	const std::pair<const wchar_t*, const wchar_t*> locales[] = {
		{ L"", L"\u30d7\u30ed\u30b8\u30a7\u30af\u30c8" },
		{ L"sakura_lang_zh_CN.dll", L"\u9879\u76ee" },
		{ L"sakura_lang_en_US.dll", L"Projects" },
	};
	for (const auto& [dll, title] : locales) {
		CSelectLang::ChangeLang(dll);
		EXPECT_EQ(title, CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_TITLE));
		// Direct selected-module reads reject silent Japanese fallback for a missing ID.
		for (UINT id = STR_WORKBENCH_PROJECTS_TITLE; id <= STR_WORKBENCH_PROJECTS_PROFILE_UNAVAILABLE; ++id) {
			wchar_t value[256]{};
			EXPECT_GT(::LoadStringW(CSelectLang::getLangRsrcInstance(), id, value, 256), 0) << id;
		}
		nativePage->RefreshProjectionStrings();
		const auto translatedRows = ListBoxRows(list);
		ASSERT_EQ(originalRows.size(), translatedRows.size());
		const auto expected = L"Other, " + std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_FOLDER))
			+ L", feature/other";
		EXPECT_EQ(expected, translatedRows.back());
		EXPECT_EQ(otherIndex, ::SendMessageW(list, LB_GETCURSEL, 0, 0));
		EXPECT_EQ(2U, runner->Calls().size());
		EXPECT_EQ(0, activationCalls);
		EXPECT_EQ(17U, workspace.revision);
	}
	EXPECT_EQ(originalRows, ListBoxRows(list));

	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	EXPECT_EQ(0, runner->Active());
	page.reset();
	::DestroyWindow(hostWindow);
}

TEST(ProjectsPage, RefreshSupersessionAndCloseFinalizeEachInFlightBranchDiscovery)
{
	std::vector<ProjectEntry> projects{ ProjectFolder(L"C:\\repo", L"Repository") };
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(L"C:\\repo"), L"repo" });
	const auto runner = std::make_shared<ScriptedBranchRunner>();
	runner->Block();

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [] { return std::wstring(L"C:\\repo"); };
	options.repositoryRoots = [](const ProjectEntry&) {
		return std::optional(std::vector<std::wstring>{ L"C:\\repo" });
	};
	options.activateProject = [](const ProjectEntry&, bool) { return EProjectsActivationStatus::Failed; };
	options.activateWorktree = [](std::wstring_view, bool) { return EProjectsActivationStatus::Failed; };
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.gitDiscoveryFactory = DiscoveryFactory(runner);

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 320, 300, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	nativePage->ActivateProjection();
	for (int attempt = 0; attempt < 200 && runner->Active() == 0; ++attempt) {
		std::this_thread::sleep_for(5ms);
	}
	ASSERT_EQ(1, runner->Active());
	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	nativePage->RefreshProjectionContent();
	for (int attempt = 0; attempt < 200 && runner->Calls().size() < 2U; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
		std::this_thread::sleep_for(5ms);
	}
	ASSERT_EQ(2U, runner->Calls().size());
	ASSERT_EQ(1, runner->Active());
	EXPECT_EQ(1, runner->MaximumActive());

	const auto started = std::chrono::steady_clock::now();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	const auto elapsed = std::chrono::steady_clock::now() - started;
	EXPECT_LT(elapsed, 2s);
	EXPECT_EQ(0, runner->Active());
	EXPECT_EQ(2U, runner->Calls().size());
	page.reset();
	::DestroyWindow(hostWindow);
}

TEST(ProjectsPage, AgentNotificationAddsRowsAndActivatesWithoutGitRediscovery)
{
	TemporaryProjectRoots roots;
	std::vector<ProjectEntry> projects{
		ProjectFolder(roots.current.wstring(), L"Current"),
		ProjectFolder(roots.other.wstring(), L"Other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(roots.current.wstring()), L"repo" });
	const auto runner = std::make_shared<ScriptedBranchRunner>();
	runner->branches.emplace(roots.current.wstring(), "main");
	// One linked worktree puts the collapsed worktrees group after the agent row.
	std::string linkedPath;
	for (const wchar_t character : (roots.base / L"linked").wstring()) {
		linkedPath.push_back(static_cast<char>(character));
	}
	runner->linked[roots.current.wstring()] = { { linkedPath, "feature" } };

	std::vector<ProjectAgentEntry> agents;
	std::vector<bool> refreshFlags;
	std::function<void()> agentsChanged;
	std::optional<std::uint64_t> activatedTerminal;

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [&roots] { return roots.current.wstring(); };
	options.repositoryRoots = [](const ProjectEntry& project) {
		const auto path = project.uri.ToWindowsPath();
		return path.value ? std::optional(std::vector<std::wstring>{ *path.value })
			: std::nullopt;
	};
	int agentActivations = 0;
	int workspaceActivations = 0;
	options.activateProject = [&workspaceActivations](const ProjectEntry&, bool) {
		++workspaceActivations;
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [&workspaceActivations](std::wstring_view, bool) {
		++workspaceActivations;
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.gitDiscoveryFactory = DiscoveryFactory(runner);
	options.agents = [&agents, &refreshFlags](bool refresh) {
		refreshFlags.push_back(refresh);
		return agents;
	};
	options.activateAgent = [&activatedTerminal, &agentActivations](std::uint64_t terminalId) {
		activatedTerminal = terminalId;
		++agentActivations;
		return EProjectsActivationStatus::FocusedCurrentWindow;
	};
	options.agentsChanged = [&agentsChanged](std::function<void()> listener) {
		agentsChanged = std::move(listener);
	};

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 420, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 420, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND list = ::FindWindowExW(pageWindow, nullptr, L"LISTBOX", nullptr);
	ASSERT_NE(nullptr, list);
	// Activation pulls agents(true) once; no agent rows exist yet.
	ASSERT_EQ(1U, refreshFlags.size());
	EXPECT_TRUE(refreshFlags.front());
	// The current worktree row's label is its branch; the rest of its text is
	// localized, so match the label only. Wait for both Projects' discovery so
	// the call count below is not racing the second request.
	const auto isCurrentWorktreeRow = [](const std::wstring& row) {
		return row.starts_with(L"main, ");
	};
	std::vector<std::wstring> rows;
	for (int attempt = 0; attempt < 400; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
		rows = ListBoxRows(list);
		if (runner->Calls().size() == 2U && runner->Active() == 0
			&& std::ranges::any_of(rows, isCurrentWorktreeRow)) break;
		std::this_thread::sleep_for(5ms);
	}
	std::string rowDiagnostic;
	for (const auto& row : rows) {
		if (!rowDiagnostic.empty()) rowDiagnostic += " | ";
		for (const wchar_t character : row) {
			rowDiagnostic.push_back(static_cast<unsigned int>(character) <= 0x7fU
				? static_cast<char>(character) : '?');
		}
	}
	ASSERT_TRUE(std::ranges::any_of(rows, isCurrentWorktreeRow)) << rowDiagnostic;
	ASSERT_EQ(2U, runner->Calls().size());
	(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);

	// A change notification posts one message, rebuilds with agents(false),
	// and never restarts Git discovery.
	agents.push_back({ .identity = L"terminal-instance:7", .terminalId = 7,
		.name = L"Claude Code", .sessionTitle = L"fix login bug" });
	ASSERT_TRUE(static_cast<bool>(agentsChanged));
	agentsChanged();
	const auto isAgentRow = [](const std::wstring& row) { return row == L"Claude Code"; };
	for (int attempt = 0; attempt < 100; ++attempt) {
		MSG pump{};
		while (::PeekMessageW(&pump, pageWindow, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&pump);
			::DispatchMessageW(&pump);
		}
		rows = ListBoxRows(list);
		if (std::ranges::any_of(rows, isAgentRow)) break;
		std::this_thread::sleep_for(5ms);
	}
	// The row is the product name alone; the session name never becomes row text.
	ASSERT_TRUE(std::ranges::any_of(rows, isAgentRow));
	EXPECT_TRUE(std::ranges::none_of(rows, [](const auto& row) {
		return row.find(L"fix login bug") != std::wstring::npos;
	}));
	EXPECT_EQ(2U, refreshFlags.size());
	EXPECT_FALSE(refreshFlags.back());
	EXPECT_EQ(2U, runner->Calls().size());
	// The agent row nests under the current worktree row.
	const auto worktreeRow = std::ranges::find_if(rows, isCurrentWorktreeRow);
	const auto agentRow = std::ranges::find_if(rows, isAgentRow);
	ASSERT_NE(rows.end(), worktreeRow);
	ASSERT_NE(rows.end(), agentRow);
	EXPECT_EQ(worktreeRow + 1, agentRow);

	// Hovering the agent row offers its session name as the list's tooltip;
	// hovering a row without hover text offers none.
	const HWND tooltip = FindListTooltip(list);
	ASSERT_NE(nullptr, tooltip);
	const auto hoverRow = [list](const std::ptrdiff_t index) {
		RECT item{};
		EXPECT_NE(LB_ERR, ::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
			reinterpret_cast<LPARAM>(&item)));
		(void)::SendMessageW(list, WM_MOUSEMOVE, 0,
			MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2));
	};
	hoverRow(agentRow - rows.begin());
	EXPECT_EQ(L"fix login bug", ListTooltipText(tooltip, list));
	hoverRow(worktreeRow - rows.begin());
	EXPECT_TRUE(ListTooltipText(tooltip, list).empty());
	hoverRow(agentRow - rows.begin());
	EXPECT_EQ(L"fix login bug", ListTooltipText(tooltip, list));
	// Below the last row is not a row, even though LISTBOX reports the nearest one.
	RECT client{};
	ASSERT_TRUE(::GetClientRect(list, &client));
	(void)::SendMessageW(list, WM_MOUSEMOVE, 0, MAKELPARAM(client.right / 2, client.bottom - 2));
	EXPECT_TRUE(ListTooltipText(tooltip, list).empty());

	// Enter on the Agent row activates it by terminal id.
	for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
		if (rows[static_cast<std::size_t>(index)].find(L"Claude Code") != std::wstring::npos) {
			ASSERT_NE(LB_ERR, ::SendMessageW(list, LB_SETCURSEL, index, 0));
			break;
		}
	}
	MSG message{};
	message.hwnd = list;
	message.message = WM_KEYDOWN;
	message.wParam = VK_RETURN;
	EXPECT_TRUE(nativePage->PreTranslateProjection(message));
	ASSERT_TRUE(activatedTerminal.has_value());
	EXPECT_EQ(7U, *activatedTerminal);
	EXPECT_EQ(1, agentActivations);

	// #412: VS Code's singleClick open mode. A press and release on the agent
	// row opens it once; a press and release on the current worktree row only
	// selects it, because opening it would switch the workspace.
	const auto rowCenter = [list](const std::ptrdiff_t index) {
		RECT item{};
		EXPECT_NE(LB_ERR, ::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
			reinterpret_cast<LPARAM>(&item)));
		return MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
	};
	const LPARAM agentPoint = rowCenter(agentRow - rows.begin());
	const LPARAM worktreePoint = rowCenter(worktreeRow - rows.begin());
	activatedTerminal.reset();
	(void)::SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, worktreePoint);
	(void)::SendMessageW(list, WM_LBUTTONUP, 0, worktreePoint);
	EXPECT_EQ(worktreeRow - rows.begin(), ::SendMessageW(list, LB_GETCURSEL, 0, 0));
	EXPECT_EQ(1, agentActivations);
	EXPECT_EQ(0, workspaceActivations);
	(void)::SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, agentPoint);
	(void)::SendMessageW(list, WM_LBUTTONUP, 0, agentPoint);
	EXPECT_EQ(agentRow - rows.begin(), ::SendMessageW(list, LB_GETCURSEL, 0, 0));
	EXPECT_EQ(2, agentActivations);
	ASSERT_TRUE(activatedTerminal.has_value());
	EXPECT_EQ(7U, *activatedTerminal);
	// A press that leaves the row before its release is not a click.
	(void)::SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, agentPoint);
	(void)::SendMessageW(list, WM_LBUTTONUP, 0, worktreePoint);
	EXPECT_EQ(2, agentActivations);
	// The second press of a double click belongs to LBN_DBLCLK; its release
	// must not open the row a third time.
	(void)::SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, agentPoint);
	(void)::SendMessageW(list, WM_LBUTTONUP, 0, agentPoint);
	EXPECT_EQ(3, agentActivations);
	(void)::SendMessageW(list, WM_LBUTTONDBLCLK, MK_LBUTTON, agentPoint);
	EXPECT_EQ(4, agentActivations);
	(void)::SendMessageW(list, WM_LBUTTONUP, 0, agentPoint);
	EXPECT_EQ(4, agentActivations);
	EXPECT_EQ(0, workspaceActivations);

	// VS Code's singleClick expand mode: one click on the worktrees group
	// toggles it, and the double click's second press does not toggle it back.
	const auto toggleIndex = (agentRow - rows.begin()) + 1;
	ASSERT_LT(static_cast<std::size_t>(toggleIndex), rows.size());
	const auto collapsedCount = rows.size();
	const LPARAM togglePoint = rowCenter(toggleIndex);
	const auto clickToggle = [list, togglePoint](const bool doubleClick) {
		(void)::SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, togglePoint);
		(void)::SendMessageW(list, WM_LBUTTONUP, 0, togglePoint);
		if (!doubleClick) return;
		(void)::SendMessageW(list, WM_LBUTTONDBLCLK, MK_LBUTTON, togglePoint);
		(void)::SendMessageW(list, WM_LBUTTONUP, 0, togglePoint);
	};
	clickToggle(false);
	EXPECT_EQ(collapsedCount + 1U, ListBoxRows(list).size());
	EXPECT_EQ(toggleIndex, ::SendMessageW(list, LB_GETCURSEL, 0, 0));
	clickToggle(false);
	EXPECT_EQ(collapsedCount, ListBoxRows(list).size());
	clickToggle(true);
	EXPECT_EQ(collapsedCount + 1U, ListBoxRows(list).size());
	clickToggle(false);
	EXPECT_EQ(collapsedCount, ListBoxRows(list).size());
	EXPECT_EQ(4, agentActivations);
	EXPECT_EQ(0, workspaceActivations);

	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	EXPECT_FALSE(static_cast<bool>(agentsChanged));
	page.reset();
	::DestroyWindow(hostWindow);
}

TEST(ProjectsPage, SessionsLoadOffTheUiThreadResumeFromTheRowAndCancelOnClose)
{
	tests1::ScopedLanguageSelection restoreLanguage;
	CSelectLang::InitializeLanguageEnvironment();
	for (const wchar_t* dll : { L"", L"sakura_lang_zh_CN.dll", L"sakura_lang_en_US.dll" }) {
		CSelectLang::ChangeLang(dll);
		// Direct selected-module reads reject silent Japanese fallback.
		for (UINT id = STR_WORKBENCH_PROJECTS_SESSIONS; id <= STR_WORKBENCH_PROJECTS_SESSION_AGE_DAYS; ++id) {
			wchar_t value[256]{};
			EXPECT_GT(::LoadStringW(CSelectLang::getLangRsrcInstance(), id, value, 256), 0) << id;
		}
	}
	ASSERT_EQ(L"Sessions", CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_SESSIONS));

	TemporaryProjectRoots roots;
	std::vector<ProjectEntry> projects{
		ProjectFolder(roots.current.wstring(), L"Current"),
		ProjectFolder(roots.other.wstring(), L"Other"),
	};
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(roots.current.wstring()), L"repo" });
	const auto runner = std::make_shared<ScriptedBranchRunner>();
	runner->branches.emplace(roots.current.wstring(), "main");

	// The provider runs on the page's worker. It blocks until the test
	// releases it, or returns nullopt as soon as the page signals stop.
	struct SessionProbe final {
		std::mutex mutex;
		std::vector<std::wstring> folders;
		std::vector<DWORD> threads;
		std::atomic<int> stopped{ 0 };
	};
	const auto probe = std::make_shared<SessionProbe>();
	const HANDLE release = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	ASSERT_NE(nullptr, release);
	constexpr std::int64_t now = 1'000'000;
	const std::wstring codexId = L"019a0000-0000-7000-8000-000000000001";
	const std::wstring claudeId = L"0f1e2d3c-4b5a-4968-8776-655443322110";
	const std::vector<AgentSessionRecord> available{
		{ EAgentSessionProvider::Codex, codexId, L"Add a sessions node", now - 120 },
		{ EAgentSessionProvider::ClaudeCode, claudeId, L"Fix login bug", now - 7200 },
	};
	std::vector<std::pair<AgentSessionRecord, std::wstring>> resumed;

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [&roots] { return roots.current.wstring(); };
	options.repositoryRoots = [](const ProjectEntry& project) {
		const auto path = project.uri.ToWindowsPath();
		return path.value ? std::optional(std::vector<std::wstring>{ *path.value })
			: std::nullopt;
	};
	options.activateProject = [](const ProjectEntry&, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [](std::wstring_view, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.gitDiscoveryFactory = DiscoveryFactory(runner);
	// The first discovery finds both sessions; later ones find only Claude's.
	options.agentSessions = [probe, release, available](ProjectAgentSessionsQuery query, HANDLE stop)
		-> std::optional<std::vector<AgentSessionRecord>> {
		std::size_t call = 0;
		{
			std::lock_guard lock(probe->mutex);
			probe->folders.push_back(std::move(query.folder));
			probe->threads.push_back(::GetCurrentThreadId());
			call = probe->folders.size();
		}
		const HANDLE handles[] = { release, stop };
		if (::WaitForMultipleObjects(2, handles, FALSE, 10'000) != WAIT_OBJECT_0) {
			++probe->stopped;
			return std::nullopt;
		}
		if (call == 1U) return available;
		return std::vector<AgentSessionRecord>{ available[1] };
	};
	options.resumeAgentSession = [&resumed](const AgentSessionRecord& session, std::wstring_view folder) {
		resumed.emplace_back(session, std::wstring(folder));
		return EProjectsActivationStatus::FocusedCurrentWindow;
	};
	options.nowUnixSeconds = [] { return std::int64_t{ 1'000'000 }; };

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 420, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 420, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	// #411: the container holds two Views, Projects and Sessions, each with its
	// own list under its own pane header.
	const HWND projectsList = ::GetDlgItem(pageWindow, 1);
	const HWND sessionsList = ::GetDlgItem(pageWindow, 2);
	ASSERT_NE(nullptr, projectsList);
	ASSERT_NE(nullptr, sessionsList);
	const auto contains = [](const std::vector<std::wstring>& rows, const std::wstring_view text) {
		return std::ranges::find(rows, text) != rows.end();
	};
	using Span = std::pair<LONG, LONG>;
	//! A list's top and bottom in page coordinates; every list spans the width.
	const auto span = [pageWindow](const HWND list) {
		RECT rect{};
		EXPECT_TRUE(::GetWindowRect(list, &rect));
		::MapWindowPoints(nullptr, pageWindow, reinterpret_cast<POINT*>(&rect), 2);
		EXPECT_EQ(0, rect.left);
		EXPECT_EQ(420, rect.right);
		return Span{ rect.top, rect.bottom };
	};
	const auto shown = [](const HWND list) {
		return (::GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0;
	};
	const auto rowPoint = [](const HWND list, const int index) {
		RECT row{};
		EXPECT_NE(LB_ERR, ::SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index),
			reinterpret_cast<LPARAM>(&row)));
		return MAKELPARAM((row.left + row.right) / 2, (row.top + row.bottom) / 2);
	};

	// VS Code's paneview at 96 DPI: two 22-DIP headers split the rest evenly,
	// and the Projects list stops at the top half of the 4-DIP sash.
	EXPECT_EQ(Span(22, 198), span(projectsList));
	EXPECT_EQ(Span(222, 400), span(sessionsList));

	// Activation starts discovery off the UI thread. Until it finishes, the
	// Sessions View has no rows (its body says it is loading); the Projects
	// tree never carries session rows.
	EXPECT_TRUE(ListBoxRows(sessionsList).empty());
	const auto isCurrentWorktreeRow = [](const std::wstring& row) { return row.starts_with(L"main, "); };
	std::vector<std::wstring> rows;
	for (int attempt = 0; attempt < 400; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
		rows = ListBoxRows(projectsList);
		if (runner->Active() == 0 && std::ranges::any_of(rows, isCurrentWorktreeRow)) break;
		std::this_thread::sleep_for(5ms);
	}
	ASSERT_TRUE(std::ranges::any_of(rows, isCurrentWorktreeRow));
	// Git discovery finishing does not finish session discovery.
	EXPECT_TRUE(ListBoxRows(sessionsList).empty());

	const std::wstring codexRow = L"Add a sessions node, Codex, 2m";
	const std::wstring claudeRow = L"Fix login bug, Claude Code, 2h";
	ASSERT_NE(FALSE, ::SetEvent(release));
	for (int attempt = 0; attempt < 400; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7073, 0);
		if (contains(ListBoxRows(sessionsList), claudeRow)) break;
		std::this_thread::sleep_for(5ms);
	}
	{
		std::lock_guard lock(probe->mutex);
		ASSERT_EQ(1U, probe->folders.size());
		EXPECT_TRUE(IsSameAgentSessionFolder(roots.current.wstring(), probe->folders.front()));
		EXPECT_NE(::GetCurrentThreadId(), probe->threads.front());
	}
	// The Sessions View lists them newest first as top-level rows; the Projects
	// tree goes from the current worktree straight to the next Project.
	EXPECT_EQ((std::vector<std::wstring>{ codexRow, claudeRow }), ListBoxRows(sessionsList));
	rows = ListBoxRows(projectsList);
	const auto worktreeRow = std::ranges::find_if(rows, isCurrentWorktreeRow);
	ASSERT_NE(rows.end(), worktreeRow);
	ASSERT_NE(rows.end(), worktreeRow + 1);
	EXPECT_TRUE((worktreeRow + 1)->starts_with(L"Other, "));
	constexpr int codexIndex = 0;
	constexpr int claudeIndex = 1;

	// Hover names the provider, the id, and how to resume. Both Views share
	// the page's one tooltip.
	const HWND tooltip = FindListTooltip(sessionsList);
	ASSERT_NE(nullptr, tooltip);
	EXPECT_EQ(tooltip, FindListTooltip(projectsList));
	(void)::SendMessageW(sessionsList, WM_MOUSEMOVE, 0, rowPoint(sessionsList, claudeIndex));
	EXPECT_EQ(L"Fix login bug\nClaude Code - " + claudeId + L"\nDouble-click to resume in a new terminal",
		ListTooltipText(tooltip, sessionsList));

	// Double-click resumes the session in its own folder, and so does Enter.
	ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_SETCURSEL, claudeIndex, 0));
	(void)::SendMessageW(pageWindow, WM_COMMAND,
		MAKEWPARAM(::GetDlgCtrlID(sessionsList), LBN_DBLCLK), reinterpret_cast<LPARAM>(sessionsList));
	ASSERT_EQ(1U, resumed.size());
	EXPECT_EQ(available[1], resumed[0].first);
	EXPECT_TRUE(IsSameAgentSessionFolder(roots.current.wstring(), resumed[0].second));
	MSG enter{};
	enter.hwnd = sessionsList;
	enter.message = WM_KEYDOWN;
	enter.wParam = VK_RETURN;
	ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_SETCURSEL, codexIndex, 0));
	EXPECT_TRUE(nativePage->PreTranslateProjection(enter));
	ASSERT_EQ(2U, resumed.size());
	EXPECT_EQ(available[0], resumed[1].first);

	// #412: a single click on a session row only selects it, because resuming
	// starts a process.
	const LPARAM claudePoint = rowPoint(sessionsList, claudeIndex);
	(void)::SendMessageW(sessionsList, WM_LBUTTONDOWN, MK_LBUTTON, claudePoint);
	(void)::SendMessageW(sessionsList, WM_LBUTTONUP, 0, claudePoint);
	EXPECT_EQ(claudeIndex, ::SendMessageW(sessionsList, LB_GETCURSEL, 0, 0));
	EXPECT_EQ(2U, resumed.size());

	// A click on a pane header collapses that View and gives its space to the
	// other one; the press and the release must both land on the header.
	const auto clickPage = [pageWindow](const int pressY, const int releaseY) {
		(void)::SendMessageW(pageWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, pressY));
		(void)::SendMessageW(pageWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, releaseY));
	};
	clickPage(211, 100);
	EXPECT_TRUE(shown(sessionsList));
	EXPECT_EQ(Span(222, 400), span(sessionsList));
	(void)::SetFocus(sessionsList);
	const bool sessionsFocused = ::GetFocus() == sessionsList;
	clickPage(211, 211);
	EXPECT_FALSE(shown(sessionsList));
	EXPECT_EQ(Span(22, 378), span(projectsList));
	// A collapsed body cannot keep the keyboard focus it hides.
	if (sessionsFocused) EXPECT_EQ(projectsList, ::GetFocus());
	// The collapsed Sessions header sits at the bottom; a click there expands
	// it again at the same split.
	clickPage(389, 389);
	EXPECT_TRUE(shown(sessionsList));
	EXPECT_EQ(Span(22, 198), span(projectsList));
	EXPECT_EQ(Span(222, 400), span(sessionsList));
	clickPage(11, 11);
	EXPECT_FALSE(shown(projectsList));
	EXPECT_EQ(Span(44, 400), span(sessionsList));
	clickPage(11, 11);
	EXPECT_TRUE(shown(projectsList));
	EXPECT_EQ(Span(22, 198), span(projectsList));

	// Dragging the sash moves the boundary, measured from the drag's origin;
	// neither body shrinks below VS Code's 120-DIP minimum.
	(void)::SendMessageW(pageWindow, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 200));
	(void)::SendMessageW(pageWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(100, 150));
	EXPECT_EQ(Span(22, 148), span(projectsList));
	EXPECT_EQ(Span(172, 400), span(sessionsList));
	(void)::SendMessageW(pageWindow, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(100, 0));
	EXPECT_EQ(Span(22, 140), span(projectsList));
	EXPECT_EQ(Span(164, 400), span(sessionsList));
	(void)::SendMessageW(pageWindow, WM_LBUTTONUP, 0, MAKELPARAM(100, 0));
	(void)::SendMessageW(pageWindow, WM_MOUSEMOVE, 0, MAKELPARAM(100, 300));
	EXPECT_EQ(Span(164, 400), span(sessionsList));
	// The split survives a collapse and an expand.
	clickPage(153, 153);
	EXPECT_FALSE(shown(sessionsList));
	clickPage(389, 389);
	EXPECT_EQ(Span(164, 400), span(sessionsList));

	// F5 in either View rediscovers; the accepted sessions stay on screen while
	// it runs, and its result replaces them.
	MSG refresh = enter;
	refresh.wParam = VK_F5;
	EXPECT_TRUE(nativePage->PreTranslateProjection(refresh));
	EXPECT_TRUE(contains(ListBoxRows(sessionsList), codexRow));
	for (int attempt = 0; attempt < 400; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7073, 0);
		rows = ListBoxRows(sessionsList);
		if (!contains(rows, codexRow) && runner->Active() == 0) break;
		std::this_thread::sleep_for(5ms);
	}
	EXPECT_FALSE(contains(rows, codexRow));
	EXPECT_TRUE(contains(rows, claudeRow));
	{
		std::lock_guard lock(probe->mutex);
		EXPECT_EQ(2U, probe->folders.size());
	}

	// Close cancels an in-flight discovery and waits for it.
	ASSERT_NE(FALSE, ::ResetEvent(release));
	refresh.hwnd = projectsList;
	EXPECT_TRUE(nativePage->PreTranslateProjection(refresh));
	for (int attempt = 0; attempt < 400; ++attempt) {
		{
			std::lock_guard lock(probe->mutex);
			if (probe->folders.size() == 3U) break;
		}
		std::this_thread::sleep_for(5ms);
	}
	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	EXPECT_EQ(1, probe->stopped.load());
	{
		std::lock_guard lock(probe->mutex);
		EXPECT_EQ(3U, probe->folders.size());
	}
	EXPECT_EQ(2U, resumed.size());
	EXPECT_EQ(0, runner->Active());
	page.reset();
	::DestroyWindow(hostWindow);
	::CloseHandle(release);
}

// #410: an agent CLI writes its session file only after the first message, so
// the agent row appears before its session exists. The page reruns discovery
// on a bounded backoff while an agent still has no session of its own, and
// stops as soon as one appears or the attempts run out.
TEST(ProjectsPage, CatchUpRediscoversUntilEachAgentHasItsOwnSessionWithinTheAttemptBound)
{
	tests1::ScopedLanguageSelection restoreLanguage;
	CSelectLang::InitializeLanguageEnvironment();
	CSelectLang::ChangeLang(L"sakura_lang_en_US.dll");
	ASSERT_EQ(L"Sessions", CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_SESSIONS));

	TemporaryProjectRoots roots;
	std::vector<ProjectEntry> projects{ ProjectFolder(roots.current.wstring(), L"Current") };
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(roots.current.wstring()), L"repo" });
	constexpr std::int64_t now = 1'000'000;
	std::vector<ProjectAgentEntry> agents{ { L"terminal-instance:1", 1, L"Codex", L"" } };
	std::function<void()> agentsChanged;
	std::atomic<int> calls{ 0 };
	std::atomic<bool> covering{ false };
	const std::wstring codexId = L"019a0000-0000-7000-8000-000000000001";

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [&roots] { return roots.current.wstring(); };
	options.repositoryRoots = [](const ProjectEntry&) {
		return std::optional(std::vector<std::wstring>{});
	};
	options.activateProject = [](const ProjectEntry&, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.activateWorktree = [](std::wstring_view, bool) {
		return EProjectsActivationStatus::Failed;
	};
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.agents = [&agents](bool) { return agents; };
	options.activateAgent = [](std::uint64_t) { return EProjectsActivationStatus::Failed; };
	options.agentsChanged = [&agentsChanged](std::function<void()> listener) {
		agentsChanged = std::move(listener);
	};
	// Until `covering`, the folder has no Codex session, only an unrelated
	// Claude Code one named after the call, which shows when each discovery
	// was accepted. Afterwards it has one Codex session as recent as the agent row.
	options.agentSessions = [&calls, &covering, codexId](ProjectAgentSessionsQuery, HANDLE)
		-> std::optional<std::vector<AgentSessionRecord>> {
		const int call = ++calls;
		if (!covering.load()) {
			return std::vector<AgentSessionRecord>{ { EAgentSessionProvider::ClaudeCode,
				L"claude-" + std::to_wstring(call), L"Unrelated " + std::to_wstring(call), now } };
		}
		return std::vector<AgentSessionRecord>{
			{ EAgentSessionProvider::Codex, codexId, L"Catch-up session", now } };
	};
	options.nowUnixSeconds = [] { return now; };

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 420, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented,
		page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached,
		page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 420, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();

	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND sessionsList = ::GetDlgItem(pageWindow, 2);
	ASSERT_NE(nullptr, sessionsList);
	const auto hasRow = [sessionsList](const std::wstring& prefix) {
		return std::ranges::any_of(ListBoxRows(sessionsList), [&prefix](const std::wstring& row) {
			return row.starts_with(prefix);
		});
	};
	// Completes the in-flight discovery the way the page's own poll does, until
	// the Sessions View shows the row that discovery returned.
	const auto settle = [pageWindow, &hasRow](const std::wstring& prefix) {
		for (int attempt = 0; attempt < 400; ++attempt) {
			(void)::SendMessageW(pageWindow, WM_TIMER, 0x7073, 0);
			if (hasRow(prefix)) return true;
			std::this_thread::sleep_for(5ms);
		}
		return false;
	};
	const auto unrelated = [](const int call) {
		return L"Unrelated " + std::to_wstring(call) + L", Claude Code";
	};
	const auto catchUpTick = [pageWindow] { (void)::SendMessageW(pageWindow, WM_TIMER, 0x7074, 0); };
	const std::wstring catchUpRow = L"Catch-up session, Codex";

	ASSERT_TRUE(settle(unrelated(1)));
	ASSERT_EQ(1, calls.load());
	// Each armed tick is one more discovery, up to the bound of twelve.
	for (int attempt = 1; attempt <= 12; ++attempt) {
		catchUpTick();
		ASSERT_TRUE(settle(unrelated(1 + attempt))) << attempt;
		ASSERT_EQ(1 + attempt, calls.load()) << attempt;
	}
	catchUpTick();
	std::this_thread::sleep_for(50ms);
	ASSERT_TRUE(settle(unrelated(13)));
	EXPECT_EQ(13, calls.load());
	EXPECT_FALSE(hasRow(catchUpRow));

	// A new agent set starts a fresh schedule; its discovery finds the
	// agent's session, after which no catch-up is armed.
	covering = true;
	agents = { { L"terminal-instance:2", 2, L"Codex", L"" } };
	ASSERT_TRUE(static_cast<bool>(agentsChanged));
	(void)::SendMessageW(pageWindow, WM_APP + 0x72, 0, 0);
	ASSERT_TRUE(settle(catchUpRow));
	EXPECT_EQ(14, calls.load());
	catchUpTick();
	std::this_thread::sleep_for(50ms);
	ASSERT_TRUE(settle(catchUpRow));
	EXPECT_EQ(14, calls.load());

	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	page.reset();
	::DestroyWindow(hostWindow);
}

TEST(ControlPlatformProjectCatalogStore, UsesDedicatedProfileUserAddress)
{
	std::optional<StorageMutationRequest> request;
	ControlPlatformProjectCatalogStoreDependencies dependencies{
		.storageCacheCoordinates = [] { return Coordinates(12); },
		.find = [](const StorageAddress& address) {
			EXPECT_EQ(StorageAddress(EStorageScope::Profile, kProfileId,
				"workbench.projects", "state"), address);
			return std::optional<platform::storage::StorageEntry>{};
		},
		.apply = [&request](const StorageMutationRequest& candidate) {
			request = candidate;
			return EditorControlStorageApplyResult{
				EEditorControlStorageApplyCode::Succeeded, std::nullopt, {} };
		},
		.operationIdFactory = [] { return std::string("project-catalog-test"); },
	};
	CControlPlatformProjectCatalogStore store(kProfileId, std::move(dependencies));
	EXPECT_EQ(EProjectCatalogStoreLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(EProjectCatalogStoreSaveStatus::Succeeded,
		store.Save("{\"version\":1,\"entries\":[]}").status);
	ASSERT_TRUE(request.has_value());
	ASSERT_EQ(1U, request->mutations.size());
	EXPECT_EQ(EStorageTarget::User, request->mutations[0].target);
	EXPECT_EQ(StorageAddress(EStorageScope::Profile, kProfileId,
		"workbench.projects", "state"), request->mutations[0].address);
}

// #408: pinned sessions lead, the display count bounds the unmarked ones, and
// archived sessions sit behind a collapsible group row.
TEST(ProjectsModel, SessionsViewOrdersPinnedThenListedThenTheArchivedGroup)
{
	constexpr std::int64_t now = 1'000'000;
	std::vector<AgentSessionRecord> sessions;
	for (int index = 0; index < 15; ++index) {
		sessions.push_back({ EAgentSessionProvider::Codex, L"s" + std::to_wstring(index),
			L"Session " + std::to_wstring(index), now - 60 * (index + 1) });
	}
	AgentSessionViewState view;
	view.displayCount = 10;
	view.pinned = { L"codex:s12", L"codex:s3", L"codex:gone" };
	view.archived = { L"codex:s1", L"codex:s13" };
	ProjectSessionsInput input{ .state = EProjectSessionsState::Ready, .sessions = sessions,
		.nowUnixSeconds = now, .view = &view };

	const auto labels = [](const AgentSessionsProjection& projection) {
		std::vector<std::wstring> result;
		for (const auto& row : projection.rows) result.push_back(row.label);
		return result;
	};
	const auto collapsed = ProjectAgentSessions(input);
	// Pinned rows keep the list's newest-first order and are extra to the ten
	// unmarked rows; s14 is the eleventh unmarked session and is cut.
	EXPECT_EQ((std::vector<std::wstring>{ L"Session 3", L"Session 12", L"Session 0", L"Session 2",
		L"Session 4", L"Session 5", L"Session 6", L"Session 7", L"Session 8", L"Session 9", L"Session 10",
		L"Session 11", L"Archived (2)" }), labels(collapsed));
	EXPECT_TRUE(collapsed.rows[0].pinned);
	EXPECT_TRUE(collapsed.rows[1].pinned);
	EXPECT_FALSE(collapsed.rows[2].pinned);
	EXPECT_EQ(3U, collapsed.rows[0].sessionIndex);
	EXPECT_EQ(L"Session 3, Codex, 4m, Pinned", AgentSessionsAccessibleLabel(collapsed.rows[0]));
	ProjectsTexts translated;
	translated.pinnedSession = L"PIN";
	EXPECT_EQ(L"Session 3, Codex, 4m, PIN", AgentSessionsAccessibleLabel(collapsed.rows[0], translated));
	const auto& toggle = collapsed.rows.back();
	EXPECT_EQ(EAgentSessionsRowKind::ArchivedToggle, toggle.kind);
	EXPECT_FALSE(toggle.expanded);
	EXPECT_FALSE(toggle.pinned);
	EXPECT_TRUE(toggle.description.empty());
	EXPECT_EQ(L"Archived (2)", AgentSessionsAccessibleLabel(toggle));
	EXPECT_TRUE(collapsed.emptyMessage.empty());

	input.archivedExpanded = true;
	input.preferredIdentity = L"codex:s13";
	const auto expanded = ProjectAgentSessions(input);
	ASSERT_EQ(15U, expanded.rows.size());
	EXPECT_TRUE(expanded.rows[12].expanded);
	EXPECT_EQ(L"Session 1", expanded.rows[13].label);
	EXPECT_EQ(L"Session 13", expanded.rows[14].label);
	EXPECT_TRUE(expanded.rows[13].archived);
	EXPECT_FALSE(expanded.rows[13].pinned);
	EXPECT_EQ(EAgentSessionsRowKind::Session, expanded.rows[14].kind);
	EXPECT_EQ(std::optional<std::size_t>(14U), expanded.selectedRowIndex);

	// A collapsed group hides its sessions, so one of them cannot be selected;
	// the group row itself can.
	input.archivedExpanded = false;
	EXPECT_FALSE(ProjectAgentSessions(input).selectedRowIndex.has_value());
	input.preferredIdentity = kArchivedAgentSessionsRowIdentity;
	EXPECT_EQ(std::optional<std::size_t>(12U), ProjectAgentSessions(input).selectedRowIndex);

	// Only archived sessions: the group row alone, and no empty message.
	const std::vector<AgentSessionRecord> one{ sessions[1] };
	const ProjectSessionsInput onlyArchived{ .state = EProjectSessionsState::Ready, .sessions = one,
		.nowUnixSeconds = now, .view = &view };
	const auto group = ProjectAgentSessions(onlyArchived);
	ASSERT_EQ(1U, group.rows.size());
	EXPECT_EQ(L"Archived (1)", group.rows[0].label);
	EXPECT_TRUE(group.emptyMessage.empty());

	// Without a view state: no marks and the default count of 20.
	std::vector<AgentSessionRecord> many;
	for (int index = 0; index < 25; ++index) {
		many.push_back({ EAgentSessionProvider::Pi, L"p" + std::to_wstring(index), L"", 0 });
	}
	const ProjectSessionsInput plain{ .state = EProjectSessionsState::Ready, .sessions = many,
		.nowUnixSeconds = now };
	EXPECT_EQ(kDefaultAgentSessionDisplayCount, ProjectAgentSessions(plain).rows.size());
}

TEST(ControlPlatformProjectCatalogStore, SessionViewStateUsesItsOwnKeyUnderTheProjectsOwner)
{
	std::optional<StorageMutationRequest> request;
	const StorageAddress expected(EStorageScope::Profile, kProfileId, "workbench.projects", "agentSessions");
	ControlPlatformProjectCatalogStoreDependencies dependencies{
		.storageCacheCoordinates = [] { return Coordinates(12); },
		.find = [expected](const StorageAddress& address) {
			EXPECT_EQ(expected, address);
			return std::optional<platform::storage::StorageEntry>{};
		},
		.apply = [&request](const StorageMutationRequest& candidate) {
			request = candidate;
			return EditorControlStorageApplyResult{
				EEditorControlStorageApplyCode::Succeeded, std::nullopt, {} };
		},
		.operationIdFactory = [] { return std::string("session-view-test"); },
	};
	CControlPlatformProjectCatalogStore store(kProfileId, std::move(dependencies),
		kAgentSessionViewStateStorageKey);
	EXPECT_EQ(EProjectCatalogStoreLoadStatus::NotFound, store.Load().status);
	const auto payload = EncodeAgentSessionViewState({});
	ASSERT_TRUE(payload.has_value());
	EXPECT_EQ(EProjectCatalogStoreSaveStatus::Succeeded, store.Save(*payload).status);
	ASSERT_TRUE(request.has_value());
	ASSERT_EQ(1U, request->mutations.size());
	EXPECT_EQ(EStorageTarget::User, request->mutations[0].target);
	EXPECT_EQ(expected, request->mutations[0].address);
}

namespace {

class MemorySessionViewState final : public IAgentSessionViewStateService {
public:
	void Load() override { ++loads; }
	[[nodiscard]] AgentSessionViewState Snapshot() const override { return state; }
	bool Update(const std::function<bool(AgentSessionViewState&)>& change) override
	{
		auto next = state;
		if (!change(next)) return false;
		state = std::move(next);
		return true;
	}

	AgentSessionViewState state;
	int loads = 0;
};

} // namespace

// #408: the Sessions view's context menu. The menu itself is modal, so the
// test answers it through the chooseMenuItem seam; everything before and after
// the popup is the production path.
TEST(ProjectsPage, SessionContextMenuManagesTheRowItWasOpenedFor)
{
	tests1::ScopedLanguageSelection restoreLanguage;
	CSelectLang::InitializeLanguageEnvironment();
	for (const wchar_t* dll : { L"", L"sakura_lang_zh_CN.dll", L"sakura_lang_en_US.dll" }) {
		CSelectLang::ChangeLang(dll);
		for (UINT id = STR_WORKBENCH_PROJECTS_SESSION_RESUME; id <= STR_WORKBENCH_PROJECTS_SESSION_NAME_INVALID; ++id) {
			wchar_t value[512]{};
			EXPECT_GT(::LoadStringW(CSelectLang::getLangRsrcInstance(), id, value, 512), 0) << id;
		}
	}
	const std::wstring pinnedSuffix = L", " + std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_SESSION_PINNED));
	ASSERT_EQ(L", Pinned", pinnedSuffix);
	ASSERT_EQ(L"Archived ({0})", std::wstring(CSelectLang::LoadStringW(STR_WORKBENCH_PROJECTS_SESSIONS_ARCHIVED)));

	TemporaryProjectRoots roots;
	std::vector<ProjectEntry> projects{ ProjectFolder(roots.current.wstring(), L"Current") };
	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Folder;
	workspace.folders.push_back({ FileUri(roots.current.wstring()), L"repo" });
	const auto runner = std::make_shared<ScriptedBranchRunner>();
	runner->branches.emplace(roots.current.wstring(), "main");

	constexpr std::int64_t now = 1'000'000;
	const std::wstring codexId = L"019a0000-0000-7000-8000-000000000001";
	const std::wstring claudeId = L"0f1e2d3c-4b5a-4968-8776-655443322110";
	const std::wstring piId = L"019a1111-2222-7333-8444-555566667777";
	const std::wstring codexIdentity = L"codex:" + codexId;
	const std::wstring claudeIdentity = L"claude-code:" + claudeId;
	const std::wstring piIdentity = L"pi:" + piId;

	//! The agents' stores as the worker threads see them.
	struct Stores final {
		std::mutex mutex;
		std::vector<AgentSessionRecord> sessions;
		std::vector<ProjectAgentSessionsQuery> queries;
		std::vector<std::wstring> renames;	//! "<identity>|<folder is current>|<title>"
		std::vector<std::wstring> removals;
		std::vector<DWORD> operationThreads;
		EAgentSessionOperationStatus renameStatus{ EAgentSessionOperationStatus::Succeeded };
		EAgentSessionOperationStatus removeStatus{ EAgentSessionOperationStatus::Succeeded };
	};
	const auto stores = std::make_shared<Stores>();
	stores->sessions = {
		{ EAgentSessionProvider::Codex, codexId, L"Add a sessions node", now - 120 },
		{ EAgentSessionProvider::ClaudeCode, claudeId, L"Fix login bug", now - 7200 },
		{ EAgentSessionProvider::Pi, piId, L"Port it", now - 2 * 86400 },
	};
	const auto queryCount = [stores] {
		std::lock_guard lock(stores->mutex);
		return stores->queries.size();
	};
	const auto lastQuery = [stores] {
		std::lock_guard lock(stores->mutex);
		return stores->queries.back();
	};

	MemorySessionViewState viewState;
	std::vector<std::pair<AgentSessionRecord, std::wstring>> resumed;
	std::vector<std::wstring> copied;
	std::vector<std::pair<std::wstring, bool>> confirmations;
	std::vector<std::pair<EAgentSessionCommand, EAgentSessionOperationStatus>> failures;
	std::vector<std::vector<AgentSessionMenuItem>> menus;
	std::optional<std::wstring> enteredTitle;
	bool confirmDelete = true;
	//! The entry the next menu answers with: a command and, for ShowCount, its count.
	std::optional<std::pair<EAgentSessionCommand, std::size_t>> choice;
	const std::wstring currentFolder = roots.current.wstring();

	ProjectsPageOptions options;
	options.projects = [&projects] { return std::optional(projects); };
	options.workspace = [&workspace] { return workspace; };
	options.workspaceRoot = [&roots] { return roots.current.wstring(); };
	options.repositoryRoots = [](const ProjectEntry& project) {
		const auto path = project.uri.ToWindowsPath();
		return path.value ? std::optional(std::vector<std::wstring>{ *path.value }) : std::nullopt;
	};
	options.activateProject = [](const ProjectEntry&, bool) { return EProjectsActivationStatus::Failed; };
	options.activateWorktree = [](std::wstring_view, bool) { return EProjectsActivationStatus::Failed; };
	options.removeProject = [](const ProjectEntry&) { return EProjectsRemovalStatus::Failed; };
	options.gitDiscoveryFactory = DiscoveryFactory(runner);
	options.agentSessions = [stores](ProjectAgentSessionsQuery query, HANDLE)
		-> std::optional<std::vector<AgentSessionRecord>> {
		std::lock_guard lock(stores->mutex);
		stores->queries.push_back(std::move(query));
		return stores->sessions;
	};
	options.resumeAgentSession = [&resumed](const AgentSessionRecord& session, std::wstring_view folder) {
		resumed.emplace_back(session, std::wstring(folder));
		return EProjectsActivationStatus::FocusedCurrentWindow;
	};
	options.nowUnixSeconds = [] { return std::int64_t{ 1'000'000 }; };
	auto& seams = options.sessionManagement;
	seams.viewState = [&viewState]() -> IAgentSessionViewStateService* { return &viewState; };
	seams.promptTitle = [&enteredTitle](const AgentSessionRecord&) { return enteredTitle; };
	seams.confirmDelete = [&confirmations, &confirmDelete](const AgentSessionRecord& session, const bool permanent) {
		confirmations.emplace_back(AgentSessionIdentity(session), permanent);
		return confirmDelete;
	};
	seams.copyText = [&copied](const std::wstring_view text) {
		copied.emplace_back(text);
		return true;
	};
	seams.rename = [stores, currentFolder](AgentSessionRecord session, std::wstring folder, std::wstring title, HANDLE) {
		std::lock_guard lock(stores->mutex);
		stores->operationThreads.push_back(::GetCurrentThreadId());
		stores->renames.push_back(AgentSessionIdentity(session)
			+ (IsSameAgentSessionFolder(folder, currentFolder) ? L"|current|" : L"|other|") + title);
		if (stores->renameStatus == EAgentSessionOperationStatus::Succeeded) {
			for (auto& stored : stores->sessions) {
				if (AgentSessionIdentity(stored) == AgentSessionIdentity(session)) stored.title = title;
			}
		}
		return stores->renameStatus;
	};
	seams.remove = [stores](AgentSessionRecord session, std::wstring, HANDLE) {
		std::lock_guard lock(stores->mutex);
		stores->operationThreads.push_back(::GetCurrentThreadId());
		stores->removals.push_back(AgentSessionIdentity(session));
		if (stores->removeStatus == EAgentSessionOperationStatus::Succeeded) {
			std::erase_if(stores->sessions, [&session](const auto& stored) {
				return AgentSessionIdentity(stored) == AgentSessionIdentity(session);
			});
		}
		return stores->removeStatus;
	};
	seams.reportFailure = [&failures](const EAgentSessionCommand command, const EAgentSessionOperationStatus status) {
		failures.emplace_back(command, status);
	};
	seams.chooseMenuItem = [&menus, &choice](const std::vector<AgentSessionMenuItem>& offered)
		-> std::optional<std::size_t> {
		menus.push_back(offered);
		if (!choice) return std::nullopt;
		for (std::size_t index = 0; index < offered.size(); ++index) {
			if (offered[index].command == choice->first && offered[index].displayCount == choice->second) return index;
		}
		ADD_FAILURE() << "menu " << menus.size() << " does not offer command "
			<< static_cast<int>(choice->first) << " among " << offered.size() << " entries";
		return std::nullopt;
	};

	const HWND hostWindow = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
		0, 0, 420, 400, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, hostWindow);
	auto page = CreateProjectsPage(hostWindow, std::move(options));
	ASSERT_NE(nullptr, page);
	const auto nativeHost = reinterpret_cast<workbench::viewcontainer::ViewContainerNativeHandle>(hostWindow);
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageReparentStatus::Reparented, page->Reparent(nativeHost));
	const workbench::viewcontainer::ViewContainerPageHost host{
		"workbench.parts.sidebar", workbench::layout::EViewContainerLocation::Sidebar, nativeHost };
	ASSERT_EQ(workbench::viewcontainer::EViewContainerPageAttachStatus::Attached, page->Attach(host));
	auto* nativePage = dynamic_cast<workbench::viewcontainer::IViewContainerPageProjection*>(page.get());
	ASSERT_NE(nullptr, nativePage);
	const RECT bounds{ 0, 0, 420, 400 };
	nativePage->LayoutProjection(bounds, bounds, 96);
	nativePage->SetProjectionVisible(true);
	nativePage->ActivateProjection();
	const HWND pageWindow = ::FindWindowExW(hostWindow, nullptr, L"SakuraProjectsPage", nullptr);
	ASSERT_NE(nullptr, pageWindow);
	const HWND sessionsList = ::GetDlgItem(pageWindow, 2);
	ASSERT_NE(nullptr, sessionsList);

	const std::wstring codexRow = L"Add a sessions node, Codex, 2m";
	const std::wstring claudeRow = L"Fix login bug, Claude Code, 2h";
	const std::wstring piRow = L"Port it, Pi, 2d";
	using Rows = std::vector<std::wstring>;
	//! Pumps the page's worker timers until the Sessions list shows `expected`
	//! and at least `queries` discoveries have run.
	const auto waitFor = [&](const Rows& expected, const std::size_t queries) {
		for (int attempt = 0; attempt < 600; ++attempt) {
			(void)::SendMessageW(pageWindow, WM_TIMER, 0x7072, 0);
			(void)::SendMessageW(pageWindow, WM_TIMER, 0x7075, 0);
			(void)::SendMessageW(pageWindow, WM_TIMER, 0x7073, 0);
			if (queryCount() >= queries && ListBoxRows(sessionsList) == expected) return;
			std::this_thread::sleep_for(5ms);
		}
		EXPECT_EQ(expected, ListBoxRows(sessionsList));
		EXPECT_GE(queryCount(), queries);
	};
	const auto rowIndex = [sessionsList](const std::wstring& text) {
		const auto rows = ListBoxRows(sessionsList);
		const auto found = std::ranges::find(rows, text);
		EXPECT_NE(rows.end(), found) << "row not listed";
		return static_cast<int>(found - rows.begin());
	};
	//! A right click on the row with this text, in screen coordinates.
	const auto rightClick = [&](const std::wstring& text) {
		RECT row{};
		ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_GETITEMRECT, static_cast<WPARAM>(rowIndex(text)),
			reinterpret_cast<LPARAM>(&row)));
		POINT point{ (row.left + row.right) / 2, (row.top + row.bottom) / 2 };
		ASSERT_NE(FALSE, ::ClientToScreen(sessionsList, &point));
		(void)::SendMessageW(sessionsList, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(sessionsList),
			MAKELPARAM(point.x, point.y));
	};
	const auto commands = [](const std::vector<AgentSessionMenuItem>& menu) {
		std::vector<EAgentSessionCommand> result;
		for (const auto& item : menu) result.push_back(item.command);
		return result;
	};
	using Command = EAgentSessionCommand;

	waitFor({ codexRow, claudeRow, piRow }, 1);
	EXPECT_GE(viewState.loads, 1);
	EXPECT_EQ(kDefaultAgentSessionDisplayCount, lastQuery().displayCount);
	EXPECT_TRUE(lastQuery().markedIdentities.empty());
	EXPECT_TRUE(IsSameAgentSessionFolder(currentFolder, lastQuery().folder));

	// A right click selects the row under it and offers that row's menu.
	ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_SETCURSEL, 0, 0));
	rightClick(claudeRow);
	EXPECT_EQ(1, ::SendMessageW(sessionsList, LB_GETCURSEL, 0, 0));
	ASSERT_EQ(1U, menus.size());
	EXPECT_EQ((std::vector<Command>{ Command::Resume, Command::Rename, Command::Pin, Command::Archive,
		Command::CopyId, Command::CopyResumeCommand, Command::Delete, Command::ShowCount, Command::ShowCount,
		Command::ShowCount, Command::ShowCount }), commands(menus.back()));
	// Dismissing the menu does nothing.
	EXPECT_EQ(AgentSessionViewState{}, viewState.state);
	EXPECT_EQ(1U, queryCount());

	// Pin: the row moves to the top, and discovery now requires the session.
	choice = { Command::Pin, 0 };
	rightClick(claudeRow);
	EXPECT_EQ((std::vector<std::wstring>{ claudeIdentity }), viewState.state.pinned);
	waitFor({ claudeRow + pinnedSuffix, codexRow, piRow }, 2);
	EXPECT_EQ((std::vector<std::wstring>{ claudeIdentity }), lastQuery().markedIdentities);
	// The pinned row's own menu offers Unpin.
	choice.reset();
	rightClick(claudeRow + pinnedSuffix);
	EXPECT_EQ(Command::Unpin, menus.back()[2].command);

	// Shift+F10 (position -1) opens the menu of the selected row: archive Pi.
	ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_SETCURSEL, rowIndex(piRow), 0));
	choice = { Command::Archive, 0 };
	(void)::SendMessageW(sessionsList, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(sessionsList), static_cast<LPARAM>(-1));
	EXPECT_EQ((std::vector<std::wstring>{ piIdentity }), viewState.state.archived);
	const std::wstring archivedRow = L"Archived (1)";
	waitFor({ claudeRow + pinnedSuffix, codexRow, archivedRow }, 3);
	EXPECT_EQ((std::vector<std::wstring>{ claudeIdentity, piIdentity }), lastQuery().markedIdentities);

	// The group row's menu holds the display counts only, the current one checked.
	choice.reset();
	rightClick(archivedRow);
	EXPECT_EQ((std::vector<Command>(4, Command::ShowCount)), commands(menus.back()));
	EXPECT_TRUE(menus.back()[1].checked);
	choice = { Command::ShowCount, 10 };
	rightClick(archivedRow);
	EXPECT_EQ(10U, viewState.state.displayCount);
	waitFor({ claudeRow + pinnedSuffix, codexRow, archivedRow }, 4);
	EXPECT_EQ(10U, lastQuery().displayCount);
	choice.reset();
	rightClick(archivedRow);
	EXPECT_TRUE(menus.back()[0].checked);
	EXPECT_FALSE(menus.back()[1].checked);

	// A single click opens the group; its rows offer Unarchive and no Pin.
	const auto clickRow = [&](const std::wstring& text) {
		RECT row{};
		ASSERT_NE(LB_ERR, ::SendMessageW(sessionsList, LB_GETITEMRECT, static_cast<WPARAM>(rowIndex(text)),
			reinterpret_cast<LPARAM>(&row)));
		const LPARAM point = MAKELPARAM((row.left + row.right) / 2, (row.top + row.bottom) / 2);
		(void)::SendMessageW(sessionsList, WM_LBUTTONDOWN, MK_LBUTTON, point);
		(void)::SendMessageW(sessionsList, WM_LBUTTONUP, 0, point);
	};
	clickRow(archivedRow);
	EXPECT_EQ((Rows{ claudeRow + pinnedSuffix, codexRow, archivedRow, piRow }), ListBoxRows(sessionsList));
	rightClick(piRow);
	EXPECT_EQ((std::vector<Command>{ Command::Resume, Command::Rename, Command::Unarchive, Command::CopyId,
		Command::CopyResumeCommand, Command::Delete, Command::ShowCount, Command::ShowCount, Command::ShowCount,
		Command::ShowCount }), commands(menus.back()));
	choice = { Command::Unarchive, 0 };
	rightClick(piRow);
	EXPECT_TRUE(viewState.state.archived.empty());
	waitFor({ claudeRow + pinnedSuffix, codexRow, piRow }, 5);

	// Copy and Resume act on the row the menu was opened for.
	choice = { Command::CopyId, 0 };
	rightClick(codexRow);
	choice = { Command::CopyResumeCommand, 0 };
	rightClick(piRow);
	EXPECT_EQ((std::vector<std::wstring>{ codexId, L"pi --session " + piId }), copied);
	choice = { Command::Resume, 0 };
	rightClick(codexRow);
	ASSERT_EQ(1U, resumed.size());
	EXPECT_EQ(codexId, resumed[0].first.id);
	EXPECT_TRUE(IsSameAgentSessionFolder(currentFolder, resumed[0].second));
	EXPECT_EQ(5U, queryCount());

	// Rename: a cancelled prompt does nothing; an entered title is normalised,
	// written off the UI thread, and the list is read again.
	choice = { Command::Rename, 0 };
	rightClick(codexRow);
	enteredTitle = L"  Sessions \t menu ";
	rightClick(codexRow);
	waitFor({ claudeRow + pinnedSuffix, L"Sessions menu, Codex, 2m", piRow }, 6);
	{
		std::lock_guard lock(stores->mutex);
		EXPECT_EQ((std::vector<std::wstring>{ codexIdentity + L"|current|Sessions menu" }), stores->renames);
		ASSERT_EQ(1U, stores->operationThreads.size());
		EXPECT_NE(::GetCurrentThreadId(), stores->operationThreads[0]);
	}
	EXPECT_TRUE(failures.empty());
	const std::wstring renamedRow = L"Sessions menu, Codex, 2m";

	// A name that is empty after normalising never reaches the agent's store.
	enteredTitle = L" \t ";
	rightClick(piRow);
	EXPECT_EQ((std::vector<std::pair<Command, EAgentSessionOperationStatus>>{
		{ Command::Rename, EAgentSessionOperationStatus::Failed } }), failures);
	// A refused rename is reported and leaves the list alone.
	enteredTitle = L"Other";
	{
		std::lock_guard lock(stores->mutex);
		stores->renameStatus = EAgentSessionOperationStatus::SessionRunning;
	}
	rightClick(piRow);
	for (int attempt = 0; attempt < 600 && failures.size() < 2U; ++attempt) {
		(void)::SendMessageW(pageWindow, WM_TIMER, 0x7075, 0);
		std::this_thread::sleep_for(5ms);
	}
	ASSERT_EQ(2U, failures.size());
	EXPECT_EQ(std::pair(Command::Rename, EAgentSessionOperationStatus::SessionRunning), failures[1]);
	EXPECT_EQ(6U, queryCount());
	EXPECT_EQ((Rows{ claudeRow + pinnedSuffix, renamedRow, piRow }), ListBoxRows(sessionsList));

	// Delete asks first. Codex deletes for good; Claude Code goes to the Recycle Bin.
	choice = { Command::Delete, 0 };
	confirmDelete = false;
	rightClick(renamedRow);
	EXPECT_EQ((std::vector<std::pair<std::wstring, bool>>{ { codexIdentity, true } }), confirmations);
	confirmDelete = true;
	rightClick(claudeRow + pinnedSuffix);
	ASSERT_EQ(2U, confirmations.size());
	EXPECT_EQ(std::pair(claudeIdentity, false), confirmations[1]);
	// The deleted session leaves the list and keeps no pin.
	waitFor({ renamedRow, piRow }, 7);
	EXPECT_TRUE(viewState.state.pinned.empty());
	EXPECT_TRUE(lastQuery().markedIdentities.empty());
	{
		std::lock_guard lock(stores->mutex);
		EXPECT_EQ((std::vector<std::wstring>{ claudeIdentity }), stores->removals);
	}
	EXPECT_EQ(2U, failures.size());

	// A session that is already gone is forgotten and the list is read again.
	choice = { Command::Pin, 0 };
	rightClick(piRow);
	waitFor({ piRow + pinnedSuffix, renamedRow }, 8);
	{
		std::lock_guard lock(stores->mutex);
		stores->removeStatus = EAgentSessionOperationStatus::NotFound;
	}
	choice = { Command::Delete, 0 };
	rightClick(piRow + pinnedSuffix);
	waitFor({ renamedRow, piRow }, 9);
	EXPECT_TRUE(viewState.state.pinned.empty());

	nativePage->DeactivateProjection();
	EXPECT_EQ(workbench::viewcontainer::EViewContainerPageCloseStatus::Closed, page->Close());
	EXPECT_EQ(0, runner->Active());
	page.reset();
	::DestroyWindow(hostWindow);
}
