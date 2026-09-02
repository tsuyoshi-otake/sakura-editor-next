/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include "_main/ControlPlatformProjectCatalogStore.h"
#include "workbench/projects/CProjectsPage.h"
#include "workbench/projects/ProjectCatalogService.h"
#include "workbench/projects/ProjectsModel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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

TEST(ProjectsPage, ShowsUnselectedProjectBranchWithoutActivatingOrMutatingWorkspace)
{
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
