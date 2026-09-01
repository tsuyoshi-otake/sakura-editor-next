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
#include <deque>
#include <memory>
#include <optional>
#include <string>
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
