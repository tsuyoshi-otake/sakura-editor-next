/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/CWorkspaceContextService.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using config::CWorkspaceContextService;
using config::EWorkspaceContextOutcome;
using config::EWorkspaceKind;
using config::SetFolderRequest;
using config::SetWorkspaceRequest;
using config::WorkspaceContextOperation;
using config::WorkspaceContextSnapshot;
using config::WorkspaceContextSubscription;
using config::WorkspaceFolderDescriptor;
using platform::uri::Uri;

Uri ParseUri(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceContextOperation Operation(const char* operationId, std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { operationId, expectedRevision };
}

SetFolderRequest Folder(const char* operationId, const wchar_t* uri, const wchar_t* name, std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { Operation(operationId, expectedRevision), ParseUri(uri), name };
}

WorkspaceFolderDescriptor WorkspaceFolder(const wchar_t* uri, const wchar_t* name)
{
	return { ParseUri(uri), name };
}

void ExpectStable(const WorkspaceContextSnapshot& expected, const WorkspaceContextSnapshot& actual)
{
	EXPECT_EQ(expected.generation, actual.generation);
	EXPECT_EQ(expected.revision, actual.revision);
	EXPECT_EQ(expected.kind, actual.kind);
	EXPECT_EQ(expected.workspaceIdentityKey, actual.workspaceIdentityKey);
	EXPECT_EQ(expected.workspaceConfigUri.has_value(), actual.workspaceConfigUri.has_value());
	EXPECT_EQ(expected.folders.size(), actual.folders.size());
}

TEST(WorkspaceContextService, InitialEmptySnapshotUsesOnlyTheSuppliedEmptyWindowIdentity)
{
	CWorkspaceContextService service(L"window-alpha", 7);
	const auto snapshot = service.Snapshot();

	EXPECT_EQ(7U, snapshot.generation);
	EXPECT_EQ(0U, snapshot.revision);
	EXPECT_EQ(EWorkspaceKind::Empty, snapshot.kind);
	EXPECT_FALSE(snapshot.workspaceConfigUri);
	EXPECT_TRUE(snapshot.folders.empty());
	EXPECT_EQ(L"empty:12:window-alpha", snapshot.workspaceIdentityKey);
}

TEST(WorkspaceContextService, FolderAliasesHaveOneIdentityAndContextChangesResetIdentity)
{
	CWorkspaceContextService service(L"window-alpha");
	auto first = service.SetFolder(Folder("folder", L"file:///C:/Repo", L"Repo"));
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, first.outcome);
	const auto identity = first.snapshot.workspaceIdentityKey;
	ASSERT_EQ(1U, first.snapshot.folders.size());
	EXPECT_EQ(L"/C:/Repo", first.snapshot.folders.front().uri.Path());
	auto alias = service.SetFolder(Folder("folder-alias", L"file://localhost/c:/repo", L"Repo", 1));
	EXPECT_EQ(EWorkspaceContextOutcome::NotApplicable, alias.outcome);
	EXPECT_EQ(1U, alias.revision);
	EXPECT_EQ(identity, alias.snapshot.workspaceIdentityKey);
	ASSERT_EQ(1U, alias.snapshot.folders.size());
	EXPECT_EQ(L"/C:/Repo", alias.snapshot.folders.front().uri.Path());

	auto changed = service.SetFolder(Folder("changed", L"file:///C:/Other", L"Other", 1));
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, changed.outcome);
	EXPECT_EQ(2U, changed.revision);
	EXPECT_NE(identity, changed.snapshot.workspaceIdentityKey);
}

TEST(WorkspaceContextService, ValidatesWorkspaceShapeAndKeepsTheLastStableSnapshot)
{
	CWorkspaceContextService service(L"window-alpha");
	const auto stable = service.Snapshot();

	SetWorkspaceRequest noConfig { Operation("no-config"), std::nullopt, { WorkspaceFolder(L"file:///C:/one", L"one") } };
	EXPECT_EQ(EWorkspaceContextOutcome::Failed, service.SetWorkspace(noConfig).outcome);
	ExpectStable(stable, service.Snapshot());

	SetWorkspaceRequest settingsOnly { Operation("settings-only"), ParseUri(L"file:///C:/work/test.code-workspace"), {} };
	auto settingsOnlyResult = service.SetWorkspace(settingsOnly);
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, settingsOnlyResult.outcome);
	EXPECT_TRUE(settingsOnlyResult.snapshot.folders.empty());
	EXPECT_EQ(EWorkspaceKind::Workspace, settingsOnlyResult.snapshot.kind);
	const auto stableWorkspace = service.Snapshot();

	SetWorkspaceRequest duplicate { Operation("duplicate"), ParseUri(L"file:///C:/work/test.code-workspace"),
		{ WorkspaceFolder(L"file:///C:/one", L"one"), WorkspaceFolder(L"file://localhost/c:/one", L"alias") } };
	EXPECT_EQ(EWorkspaceContextOutcome::Failed, service.SetWorkspace(duplicate).outcome);
	ExpectStable(stableWorkspace, service.Snapshot());

	std::vector<WorkspaceFolderDescriptor> tooManyFolders;
	tooManyFolders.reserve(257);
	for (int index = 0; index < 257; ++index) {
		tooManyFolders.push_back(WorkspaceFolder(L"file:///C:/one", L"one"));
	}
	SetWorkspaceRequest tooMany { Operation("too-many"), ParseUri(L"file:///C:/work/test.code-workspace"), std::move(tooManyFolders) };
	EXPECT_EQ(EWorkspaceContextOutcome::Failed, service.SetWorkspace(tooMany).outcome);
	ExpectStable(stableWorkspace, service.Snapshot());

	const std::wstring tooLongName(257, L'x');
	EXPECT_EQ(EWorkspaceContextOutcome::Failed,
		service.SetFolder({ Operation("too-long-name"), ParseUri(L"file:///C:/one"), tooLongName }).outcome);
	ExpectStable(stableWorkspace, service.Snapshot());

	const std::string tooLongOperationId(129, 'o');
	EXPECT_EQ(EWorkspaceContextOutcome::Failed,
		service.SetFolder({ Operation(tooLongOperationId.c_str()), ParseUri(L"file:///C:/one"), L"one" }).outcome);
	ExpectStable(stableWorkspace, service.Snapshot());
}

TEST(WorkspaceContextService, WorkspaceIdentityFollowsConfigUriRatherThanFolderMembershipOrOrder)
{
	CWorkspaceContextService service(L"window-alpha");
	const auto config = ParseUri(L"file:///C:/work/project.code-workspace");
	SetWorkspaceRequest initial { Operation("initial"), config, { WorkspaceFolder(L"file:///C:/work/one", L"one") } };
	auto first = service.SetWorkspace(initial);
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, first.outcome);
	const auto identity = first.snapshot.workspaceIdentityKey;
	SetWorkspaceRequest reordered { Operation("reordered", 1), ParseUri(L"file://localhost/c:/work/project.code-workspace"),
		{ WorkspaceFolder(L"file:///C:/work/two", L"two"), WorkspaceFolder(L"file:///C:/work/one", L"one") } };
	auto changedFolders = service.SetWorkspace(reordered);
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, changedFolders.outcome);
	EXPECT_EQ(2U, changedFolders.revision);
	EXPECT_EQ(identity, changedFolders.snapshot.workspaceIdentityKey);

	SetWorkspaceRequest noFolders { Operation("no-folders", 2), ParseUri(L"file:///C:/work/project.code-workspace"), {} };
	auto settingsOnly = service.SetWorkspace(noFolders);
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, settingsOnly.outcome);
	EXPECT_EQ(3U, settingsOnly.revision);
	EXPECT_EQ(identity, settingsOnly.snapshot.workspaceIdentityKey);

	SetWorkspaceRequest differentConfig { Operation("different-config", 3), ParseUri(L"file:///C:/other/project.code-workspace"), {} };
	auto changedConfig = service.SetWorkspace(differentConfig);
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, changedConfig.outcome);
	EXPECT_NE(identity, changedConfig.snapshot.workspaceIdentityKey);
}

TEST(WorkspaceContextService, InvalidPreparationDoesNotReserveAnOperationId)
{
	CWorkspaceContextService service(L"window-alpha");
	const std::wstring tooLongName(257, L'x');
	EXPECT_EQ(EWorkspaceContextOutcome::Failed,
		service.SetFolder({ Operation("retry"), ParseUri(L"file:///C:/one"), tooLongName }).outcome);
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded,
		service.SetFolder(Folder("retry", L"file:///C:/one", L"one")).outcome);
	EXPECT_EQ(1U, service.Snapshot().revision);
}

TEST(WorkspaceContextService, BuildsMultiRootIdentityAndSetEmptyNeverFabricatesAPathIdentity)
{
	CWorkspaceContextService service(L"explicit-window-id");
	SetWorkspaceRequest workspace { Operation("workspace"), ParseUri(L"file:///C:/work/project.code-workspace"),
		{ WorkspaceFolder(L"file:///C:/work/one", L"one"), WorkspaceFolder(L"file:///C:/work/two", L"two") } };
	auto selected = service.SetWorkspace(workspace);
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, selected.outcome);
	EXPECT_EQ(EWorkspaceKind::Workspace, selected.snapshot.kind);
	EXPECT_EQ(2U, selected.snapshot.folders.size());
	EXPECT_TRUE(selected.snapshot.workspaceConfigUri.has_value());
	auto empty = service.SetEmpty(Operation("empty", 1));
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, empty.outcome);
	EXPECT_EQ(2U, empty.revision);
	EXPECT_EQ(EWorkspaceKind::Empty, empty.snapshot.kind);
	EXPECT_FALSE(empty.snapshot.workspaceConfigUri);
	EXPECT_TRUE(empty.snapshot.folders.empty());
	EXPECT_EQ(L"empty:18:explicit-window-id", empty.snapshot.workspaceIdentityKey);
}

TEST(WorkspaceContextService, EnforcesRevisionAndOperationReplayWithoutExtraNotifications)
{
	CWorkspaceContextService service(L"window-alpha");
	int notifications = 0;
	auto subscription = service.Subscribe([&notifications](const auto&) { ++notifications; });

	auto first = service.SetFolder(Folder("operation", L"file:///C:/Repo", L"Repo", 0));
	ASSERT_EQ(EWorkspaceContextOutcome::Succeeded, first.outcome);
	EXPECT_EQ(1U, first.revision);
	EXPECT_EQ(1, notifications);

	auto replay = service.SetFolder(Folder("operation", L"file:///C:/Repo", L"Repo", 0));
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, replay.outcome);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(1U, replay.revision);
	EXPECT_FALSE(replay.change);
	EXPECT_EQ(1, notifications);

	auto collision = service.SetFolder(Folder("operation", L"file:///C:/Other", L"Other", 1));
	EXPECT_EQ(EWorkspaceContextOutcome::Conflict, collision.outcome);
	EXPECT_FALSE(collision.replayed);
	EXPECT_EQ(1U, collision.revision);
	EXPECT_EQ(1, notifications);
}

TEST(WorkspaceContextService, DeliversInOrderAfterCommitAndIsolatesThrowingReentrantAndRaiiListeners)
{
	CWorkspaceContextService service(L"window-alpha");
	std::vector<int> ordered;
	std::vector<std::uint64_t> firstRevisions;
	std::vector<std::uint64_t> lastRevisions;
	int reentrantCalls = 0;
	WorkspaceContextSubscription removed;
	auto first = service.Subscribe([&](const auto& change) {
		ordered.push_back(1);
		firstRevisions.push_back(change.revision);
		++reentrantCalls;
		removed.Reset();
	});
	removed = service.Subscribe([&](const auto&) { ordered.push_back(2); });
	auto throwing = service.Subscribe([&](const auto&) {
		ordered.push_back(3);
		throw std::runtime_error("expected listener boundary failure");
	});
	auto last = service.Subscribe([&](const auto& change) {
		ordered.push_back(4);
		lastRevisions.push_back(change.revision);
	});

	auto result = service.SetFolder(Folder("outer", L"file:///C:/Repo", L"Repo"));
	EXPECT_EQ(EWorkspaceContextOutcome::Succeeded, result.outcome);
	EXPECT_EQ(1, reentrantCalls);
	EXPECT_EQ((std::vector<int> { 1, 3, 4 }), ordered);
	EXPECT_EQ((std::vector<std::uint64_t> { 1 }), firstRevisions);
	EXPECT_EQ((std::vector<std::uint64_t> { 1 }), lastRevisions);
	EXPECT_FALSE(removed.IsActive());
}

} // namespace
