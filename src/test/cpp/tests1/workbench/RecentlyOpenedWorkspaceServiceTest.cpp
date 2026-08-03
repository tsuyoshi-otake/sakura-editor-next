/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "_main/ControlPlatformRecentlyOpenedWorkspaceStore.h"
#include "workbench/commands/WorkbenchCommandRegistry.h"
#include "workbench/editor/EditorCommandIds.h"
#include "workbench/layout/WorkbenchLayoutStateTypes.h"
#include "workbench/recent/RecentlyOpenedWorkspaceMenuProjection.h"
#include "workbench/recent/RecentlyOpenedWorkspaceService.h"

#include <gtest/gtest.h>

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
using platform::controlipc::EEditorControlStorageCacheWaitCode;
using platform::controlipc::EditorControlStorageApplyResult;
using platform::controlipc::EditorControlStorageCacheCoordinateResult;
using platform::controlipc::EditorControlStorageCacheCoordinates;
using platform::controlipc::EditorControlStorageCacheWaitResult;
using platform::storage::EStorageScope;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageMutationRequest;
using platform::uri::Uri;
using workbench::recent::CRecentlyOpenedWorkspaceMenuProjection;
using workbench::recent::CRecentlyOpenedWorkspaceService;
using workbench::recent::ControlPlatformRecentlyOpenedWorkspaceStoreDependencies;
using workbench::recent::ERecentlyOpenedWorkspaceKind;
using workbench::recent::ERecentlyOpenedWorkspaceMenuRowKind;
using workbench::recent::ERecentlyOpenedWorkspaceOutcome;
using workbench::recent::ERecentlyOpenedWorkspaceStoreLoadStatus;
using workbench::recent::ERecentlyOpenedWorkspaceStoreSaveStatus;
using workbench::recent::IRecentlyOpenedWorkspaceStore;
using workbench::recent::RecentlyOpenedWorkspaceEntry;
using workbench::recent::RecentlyOpenedWorkspaceStoreLoadResult;
using workbench::recent::RecentlyOpenedWorkspaceStoreSaveResult;

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

Uri FolderUri(std::wstring path)
{
	auto parsed = Uri::FromWindowsPath(path);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

RecentlyOpenedWorkspaceEntry RecentFolderEntry(std::wstring path, std::optional<std::wstring> label = std::nullopt)
{
	return { ERecentlyOpenedWorkspaceKind::Folder, FolderUri(std::move(path)), std::move(label) };
}

RecentlyOpenedWorkspaceEntry Workspace(std::wstring path, std::optional<std::wstring> label = std::nullopt)
{
	return { ERecentlyOpenedWorkspaceKind::Workspace, FolderUri(std::move(path)), std::move(label) };
}

class ScriptedRecentStore final : public IRecentlyOpenedWorkspaceStore {
public:
	RecentlyOpenedWorkspaceStoreLoadResult Load() override
	{
		++loads;
		return load;
	}

	RecentlyOpenedWorkspaceStoreSaveResult Save(std::string payload) override
	{
		savedPayloads.push_back(std::move(payload));
		if (saves.empty()) return { ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded, {} };
		auto result = std::move(saves.front());
		saves.pop_front();
		return result;
	}

	RecentlyOpenedWorkspaceStoreLoadResult load{
		ERecentlyOpenedWorkspaceStoreLoadStatus::NotFound, std::nullopt, {} };
	std::deque<RecentlyOpenedWorkspaceStoreSaveResult> saves;
	std::vector<std::string> savedPayloads;
	int loads = 0;
};

EditorControlStorageCacheCoordinateResult Coordinates(std::uint64_t revision)
{
	return { EEditorControlStorageCacheCoordinateCode::Ready, EEditorControlPlatformRuntimeState::Ready,
		EditorControlStorageCacheCoordinates{ kProfileId, 7, revision }, {} };
}

} // namespace

TEST(RecentlyOpenedWorkspaceService, LoadsVersionOneSkipsOnlyMalformedRecordsAndPreservesValidEntries)
{
	auto store = std::make_unique<ScriptedRecentStore>();
	store->load = { ERecentlyOpenedWorkspaceStoreLoadStatus::Succeeded,
		R"({"version":1,"entries":[{"kind":"folder","uri":"file:///c%3A/Repo"},{"kind":"bad","uri":"file:///c%3A/Ignored"},{"kind":"workspace","uri":"file:///d%3A/Team.code-workspace","label":"Team \"alpha\""}]})", {} };
	CRecentlyOpenedWorkspaceService service(std::move(store));

	const auto loaded = service.Load();
	const auto snapshot = service.Snapshot();

	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded, loaded.outcome);
	EXPECT_FALSE(loaded.diagnostic.empty());
	ASSERT_EQ(2U, snapshot.size());
	EXPECT_EQ(ERecentlyOpenedWorkspaceKind::Folder, snapshot[0].kind);
	EXPECT_EQ(L"file:///C:/Repo", snapshot[0].uri.ToString());
	ASSERT_TRUE(snapshot[1].label.has_value());
	EXPECT_EQ(L"Team \"alpha\"", *snapshot[1].label);
}

TEST(RecentlyOpenedWorkspaceService, PersistsSuccessOnlyMruDedupeAndSharedCapacity)
{
	auto store = std::make_unique<ScriptedRecentStore>();
	auto* script = store.get();
	CRecentlyOpenedWorkspaceService service(std::move(store));
	ASSERT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded, service.Load().outcome);
	for (int index = 0; index < 65; ++index) {
		EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
			service.RecordSuccessfulOpen(RecentFolderEntry(L"c:\\repo" + std::to_wstring(index))).outcome);
	}
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
		service.RecordSuccessfulOpen(RecentFolderEntry(L"C:\\REPO64")).outcome);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(workbench::recent::kMaximumRecentlyOpenedWorkspaces, snapshot.size());
	EXPECT_EQ(L"file:///C:/REPO64", snapshot.front().uri.ToString());
	EXPECT_NE(std::string::npos, script->savedPayloads.back().find("\"version\":1"));
	EXPECT_NE(std::string::npos, script->savedPayloads.back().find("file:///C:/REPO64"));
}

TEST(RecentlyOpenedWorkspaceService, StoreAndTransientFailuresNeverPromoteOrRemoveAcceptedHistory)
{
	auto store = std::make_unique<ScriptedRecentStore>();
	auto* script = store.get();
	CRecentlyOpenedWorkspaceService service(std::move(store));
	ASSERT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
		service.RecordSuccessfulOpen(RecentFolderEntry(L"c:\\stable")).outcome);
	const auto stable = service.Snapshot();
	ASSERT_EQ(1U, stable.size());

	script->saves.push_back({ ERecentlyOpenedWorkspaceStoreSaveStatus::Unavailable, "temporary transport loss" });
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Failed,
		service.RecordSuccessfulOpen(RecentFolderEntry(L"c:\\new")).outcome);
	EXPECT_EQ(stable[0].uri.ToString(), service.Snapshot()[0].uri.ToString());

	script->load = { ERecentlyOpenedWorkspaceStoreLoadStatus::Succeeded, "{not JSON", {} };
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Failed, service.Load().outcome);
	EXPECT_EQ(stable[0].uri.ToString(), service.Snapshot()[0].uri.ToString());

	script->saves.push_back({ ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict, "resnapshot required" });
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Failed,
		service.RemoveConfirmedNotFound(stable[0].uri).outcome);
	EXPECT_EQ(stable[0].uri.ToString(), service.Snapshot()[0].uri.ToString());
}

TEST(RecentlyOpenedWorkspaceService, ConflictReloadsAndMergesConcurrentHistoryBeforeOneBoundedRetry)
{
	auto store = std::make_unique<ScriptedRecentStore>();
	auto* script = store.get();
	CRecentlyOpenedWorkspaceService service(std::move(store));
	ASSERT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded, service.Load().outcome);

	script->load = { ERecentlyOpenedWorkspaceStoreLoadStatus::Succeeded,
		R"({"version":1,"entries":[{"kind":"folder","uri":"file:///D:/concurrent"}]})", {} };
	script->saves.push_back({ ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict, "resnapshot required" });
	script->saves.push_back({ ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded, {} });

	const auto result = service.RecordSuccessfulOpen(RecentFolderEntry(L"c:\\opened"));
	ASSERT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded, result.outcome);
	ASSERT_EQ(2, script->loads);
	ASSERT_EQ(2U, script->savedPayloads.size());
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.size());
	EXPECT_EQ(L"file:///C:/opened", snapshot[0].uri.ToString());
	EXPECT_EQ(L"file:///D:/concurrent", snapshot[1].uri.ToString());
	EXPECT_NE(std::string::npos, script->savedPayloads.back().find("file:///D:/concurrent"));
}

TEST(RecentlyOpenedWorkspaceService, RemovesOnlyOnExplicitConfirmedNotFoundAndNormalizesUriIdentity)
{
	auto store = std::make_unique<ScriptedRecentStore>();
	CRecentlyOpenedWorkspaceService service(std::move(store));
	ASSERT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
		service.RecordSuccessfulOpen(RecentFolderEntry(L"c:\\Repo\\Folder", L"Folder & label")).outcome);
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
		service.RecordSuccessfulOpen(RecentFolderEntry(L"C:\\REPO\\FOLDER")).outcome);
	ASSERT_EQ(1U, service.Snapshot().size());
	EXPECT_EQ(ERecentlyOpenedWorkspaceOutcome::Succeeded,
		service.RemoveConfirmedNotFound(FolderUri(L"c:\\repo\\folder")).outcome);
	EXPECT_TRUE(service.Snapshot().empty());

	auto unc = CRecentlyOpenedWorkspaceService::Normalize(RecentFolderEntry(L"\\\\server\\share\\folder"));
	ASSERT_TRUE(unc.has_value());
	EXPECT_EQ(L"file://server/share/folder", unc->uri.ToString());
	auto nonFile = Uri::Parse(L"vscode-remote://ssh-remote/home/me");
	ASSERT_TRUE(nonFile.value.has_value());
	EXPECT_TRUE(CRecentlyOpenedWorkspaceService::Normalize(
		{ ERecentlyOpenedWorkspaceKind::Folder, *nonFile.value, std::nullopt }).has_value());
}

TEST(RecentlyOpenedWorkspaceMenuProjection, GroupsTypedRowsBeforeFilesWithOneBoundedSnapshotRange)
{
	std::vector<RecentlyOpenedWorkspaceEntry> entries {
		Workspace(L"c:\\repo\\team.code-workspace", L"Team"), RecentFolderEntry(L"d:\\folder") };
	const auto withFiles = CRecentlyOpenedWorkspaceMenuProjection::Build(entries, true);
	ASSERT_EQ(3U, withFiles.size());
	EXPECT_EQ(ERecentlyOpenedWorkspaceMenuRowKind::Entry, withFiles[0].kind);
	EXPECT_EQ(13000, withFiles[0].commandId);
	EXPECT_EQ(ERecentlyOpenedWorkspaceMenuRowKind::Separator, withFiles[2].kind);
	EXPECT_EQ(13001, withFiles[1].commandId);
	EXPECT_EQ(1U, *CRecentlyOpenedWorkspaceMenuProjection::Resolve(13001, entries));
	EXPECT_FALSE(CRecentlyOpenedWorkspaceMenuProjection::Resolve(13002, entries).has_value());
	EXPECT_FALSE(CRecentlyOpenedWorkspaceMenuProjection::Resolve(12999, entries).has_value());
	EXPECT_FALSE(CRecentlyOpenedWorkspaceMenuProjection::Resolve(13064, entries).has_value());
	EXPECT_EQ(13063, workbench::recent::kRecentlyOpenedWorkspaceDynamicLast);

	const auto typedOnly = CRecentlyOpenedWorkspaceMenuProjection::Build(entries, false);
	ASSERT_EQ(2U, typedOnly.size());
	EXPECT_EQ(ERecentlyOpenedWorkspaceMenuRowKind::Entry, typedOnly.back().kind);

	// The native menu owner appends legacy recent files after this typed
	// projection.  It must remain usable when it has only those file rows, but
	// neither group may create an empty Open Recent menu.
	const std::vector<RecentlyOpenedWorkspaceEntry> noTypedEntries;
	EXPECT_TRUE(CRecentlyOpenedWorkspaceMenuProjection::HasItems(entries, false));
	EXPECT_TRUE(CRecentlyOpenedWorkspaceMenuProjection::HasItems(noTypedEntries, true));
	EXPECT_FALSE(CRecentlyOpenedWorkspaceMenuProjection::HasItems(noTypedEntries, false));
	EXPECT_TRUE(CRecentlyOpenedWorkspaceMenuProjection::Build(noTypedEntries, true).empty());
	EXPECT_TRUE(CRecentlyOpenedWorkspaceMenuProjection::Build(noTypedEntries, false).empty());
}

TEST(ControlPlatformRecentlyOpenedWorkspaceStore, UsesProfileUserAddressAndRevisionedControlApply)
{
	std::optional<StorageMutationRequest> request;
	ControlPlatformRecentlyOpenedWorkspaceStoreDependencies dependencies {
		.storageCacheCoordinates = [] { return Coordinates(12); },
		.find = [](const StorageAddress& address) {
			EXPECT_EQ(StorageAddress(EStorageScope::Profile, kProfileId, "workbench.recentlyOpened", "state"), address);
			return std::optional<platform::storage::StorageEntry>{};
		},
		.apply = [&request](const StorageMutationRequest& candidate) {
			request = candidate;
			return EditorControlStorageApplyResult { EEditorControlStorageApplyCode::Succeeded, std::nullopt, {} };
		},
		.operationIdFactory = [] { return std::string("recent-workspace-test"); },
	};
	CControlPlatformRecentlyOpenedWorkspaceStore store(kProfileId, std::move(dependencies));
	EXPECT_EQ(ERecentlyOpenedWorkspaceStoreLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded, store.Save("{\"version\":1,\"entries\":[]}").status);
	ASSERT_TRUE(request.has_value());
	ASSERT_EQ(1U, request->mutations.size());
	EXPECT_EQ(EStorageTarget::User, request->mutations[0].target);
	EXPECT_EQ(StorageAddress(EStorageScope::Profile, kProfileId, "workbench.recentlyOpened", "state"), request->mutations[0].address);
}

TEST(ControlPlatformRecentlyOpenedWorkspaceStore, WaitsForAnAlreadyScheduledResnapshotBeforeReading)
{
	int coordinateReads = 0;
	int waits = 0;
	ControlPlatformRecentlyOpenedWorkspaceStoreDependencies dependencies {
		.storageCacheCoordinates = [&coordinateReads] {
			if (coordinateReads++ == 0) {
				return EditorControlStorageCacheCoordinateResult{
					EEditorControlStorageCacheCoordinateCode::Resynchronizing,
					EEditorControlPlatformRuntimeState::Ready, std::nullopt, {} };
			}
			return Coordinates(19);
		},
		.waitForStorageCacheReady = [&waits](std::chrono::milliseconds timeout) {
			++waits;
			EXPECT_EQ(std::chrono::seconds(2), timeout);
			return EditorControlStorageCacheWaitResult{
				EEditorControlStorageCacheWaitCode::Ready,
				EEditorControlPlatformRuntimeState::Ready,
				EditorControlStorageCacheCoordinates{ kProfileId, 7, 19 }, {} };
		},
		.find = [](const StorageAddress&) {
			return std::optional<platform::storage::StorageEntry>{};
		},
		.apply = [](const StorageMutationRequest&) {
			return EditorControlStorageApplyResult{ EEditorControlStorageApplyCode::Succeeded, std::nullopt, {} };
		},
		.operationIdFactory = [] { return std::string("recent-workspace-resnapshot-test"); },
	};
	CControlPlatformRecentlyOpenedWorkspaceStore store(kProfileId, std::move(dependencies));

	EXPECT_EQ(ERecentlyOpenedWorkspaceStoreLoadStatus::NotFound, store.Load().status);
	EXPECT_EQ(1, waits);
	EXPECT_EQ(2, coordinateReads);
}

TEST(RecentlyOpenedWorkspaceCommandContext, OpenRecentAlwaysReachesAnExplicitTerminal)
{
	workbench::commands::WorkbenchContextKeyService context;
	workbench::layout::WorkbenchLayoutStateSnapshot layout;
	workbench::commands::WorkbenchCommandRegistry registry;
	int calls = 0;
	workbench::commands::WorkbenchBuiltinCommandExecutors executors;
	executors.openRecent = [&calls] {
		++calls;
		return workbench::commands::WorkbenchCommandExecutionResult{
			workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded, {} };
	};
	ASSERT_TRUE(registry.RegisterBuiltinCommands(std::move(executors)).Succeeded());
	ASSERT_TRUE(context.SetCoreProjection(layout, {}, {}, false).Succeeded());
	EXPECT_EQ(workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute(workbench::editor::command_ids::OpenRecent, context.Snapshot()).status);
	EXPECT_EQ(1, calls);
	ASSERT_TRUE(context.SetCoreProjection(layout, {}, {}, true).Succeeded());
	EXPECT_EQ(workbench::commands::EWorkbenchCommandExecutionStatus::Succeeded,
		registry.Execute(workbench::editor::command_ids::OpenRecent, context.Snapshot()).status);
	EXPECT_EQ(2, calls);
}
