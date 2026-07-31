/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "config/WorkspaceContextTypes.h"
#include "workbench/tasks/FolderTaskCatalogRegistry.h"
#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

namespace {

using config::EWorkspaceKind;
using config::WorkspaceContextSnapshot;
using config::WorkspaceFolderDescriptor;
using workbench::tasks::CFolderTaskCatalogRegistry;
using workbench::tasks::EFolderTaskCatalogRegistryStatus;
using workbench::workspace::CWorkspaceArtifactDocumentService;
using workbench::workspace::EWorkspaceArtifactDocumentKind;
using workbench::workspace::EWorkspaceArtifactDocumentSource;
using workbench::workspace::WorkspaceArtifactDocumentRemoval;
using workbench::workspace::WorkspaceArtifactDocumentUpdate;

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceContextSnapshot Context(
	EWorkspaceKind kind, std::uint64_t generation, std::initializer_list<platform::uri::Uri> folders)
{
	WorkspaceContextSnapshot context;
	context.kind = kind;
	context.generation = generation;
	for (const auto& folder : folders) context.folders.push_back({ folder, folder.ToString() });
	return context;
}

WorkspaceArtifactDocumentUpdate Update(
	EWorkspaceArtifactDocumentSource source, const platform::uri::Uri& resource,
	std::uint64_t generation, std::uint64_t revision, std::string json,
	std::optional<platform::uri::Uri> folder = std::nullopt)
{
	return { EWorkspaceArtifactDocumentKind::Tasks, source, std::move(folder), resource, generation, revision, std::move(json) };
}

WorkspaceArtifactDocumentRemoval Removal(
	EWorkspaceArtifactDocumentSource source, const platform::uri::Uri& resource,
	std::uint64_t generation, std::uint64_t revision, std::optional<platform::uri::Uri> folder = std::nullopt)
{
	return { EWorkspaceArtifactDocumentKind::Tasks, source, std::move(folder), resource, generation, revision };
}

const workbench::tasks::FolderTaskCatalogSnapshot& Folder(
	const workbench::tasks::FolderTaskCatalogRegistrySnapshot& snapshot, std::size_t index)
{
	EXPECT_LT(index, snapshot.folders.size());
	return snapshot.folders[index];
}

TEST(FolderTaskCatalogRegistry, EmptyFolderAndMultiRootContextsHaveOnlyExplicitFolderSlots)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto tools = ParseUri(L"file:///C:/Work/tools");

	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Empty, 3, {}), artifacts).status);
	EXPECT_TRUE(registry.Snapshot().folders.empty());

	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 4, { engine }), artifacts).status);
	ASSERT_EQ(1U, registry.Snapshot().folders.size());
	EXPECT_TRUE(Folder(registry.Snapshot(), 0).catalog.definitions.empty());
	EXPECT_FALSE(Folder(registry.Snapshot(), 0).catalog.sourceUri);

	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Workspace, 5, { engine, tools }), artifacts).status);
	const auto multiRoot = registry.Snapshot();
	ASSERT_EQ(2U, multiRoot.folders.size());
	EXPECT_TRUE(registry.SnapshotForFolder(engine).has_value());
	EXPECT_TRUE(registry.SnapshotForFolder(tools).has_value());
	EXPECT_FALSE(registry.SnapshotForFolder(ParseUri(L"file:///C:/Work/other")).has_value());
}

TEST(FolderTaskCatalogRegistry, FolderOverrideAndWorkspaceFallbackRemainIndependentPerFolder)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto tools = ParseUri(L"file:///C:/Work/tools");
	const auto engineTasks = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 8, 1,
		R"json({"tasks":{"version":"2.0.0","tasks":[{"label":"workspace","command":"workspace"}]}})json")).Succeeded());
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::Folder, engineTasks, 8, 1,
		R"json({"version":"2.0.0","tasks":[{"label":"engine","command":"engine"}]})json", engine)).Succeeded());

	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Workspace, 8, { engine, tools }), artifacts).Succeeded());
	const auto engineSnapshot = registry.SnapshotForFolder(engine);
	const auto toolsSnapshot = registry.SnapshotForFolder(tools);
	ASSERT_TRUE(engineSnapshot);
	ASSERT_TRUE(toolsSnapshot);
	ASSERT_EQ(1U, engineSnapshot->catalog.definitions.size());
	ASSERT_EQ(1U, toolsSnapshot->catalog.definitions.size());
	EXPECT_EQ(L"engine", engineSnapshot->catalog.definitions.front().label);
	EXPECT_EQ(L"workspace", toolsSnapshot->catalog.definitions.front().label);
	EXPECT_EQ(engineTasks.ToString(), engineSnapshot->catalog.sourceUri->ToString());
	EXPECT_EQ(workspace.ToString(), toolsSnapshot->catalog.sourceUri->ToString());
}

TEST(FolderTaskCatalogRegistry, FolderOrderIsIdentityNoOpAndTopologyChangesOnlyAffectedSlots)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto tools = ParseUri(L"file:///C:/Work/tools");
	const auto docs = ParseUri(L"file:///C:/Work/docs");
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Workspace, 11, { engine, tools }), artifacts).Succeeded());
	const auto before = registry.Snapshot();
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::NoChange,
		registry.Reconcile(Context(EWorkspaceKind::Workspace, 11, { tools, engine }), artifacts).status);
	EXPECT_EQ(before.revision, registry.Snapshot().revision);

	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Workspace, 11, { engine, docs }), artifacts).status);
	EXPECT_TRUE(registry.SnapshotForFolder(engine));
	EXPECT_FALSE(registry.SnapshotForFolder(tools));
	EXPECT_TRUE(registry.SnapshotForFolder(docs));
}

TEST(FolderTaskCatalogRegistry, RemovingFolderDocumentCanFallBackToOlderWorkspaceRevisionInSameGeneration)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto folderTasks = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 16, 1,
		R"json({"tasks":{"version":"2.0.0","tasks":[{"label":"workspace-old","command":"workspace"}]}})json")).Succeeded());
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::Folder, folderTasks, 16, 9,
		R"json({"version":"2.0.0","tasks":[{"label":"folder-new","command":"folder"}]})json", engine)).Succeeded());
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Folder, 16, { engine }), artifacts).Succeeded());
	EXPECT_EQ(L"folder-new", registry.SnapshotForFolder(engine)->catalog.definitions.front().label);

	ASSERT_TRUE(artifacts.Remove(Removal(EWorkspaceArtifactDocumentSource::Folder, folderTasks, 16, 10, engine)).Succeeded());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 16, { engine }), artifacts).status);
	const auto fallback = registry.SnapshotForFolder(engine);
	ASSERT_TRUE(fallback);
	ASSERT_EQ(1U, fallback->catalog.definitions.size());
	EXPECT_EQ(L"workspace-old", fallback->catalog.definitions.front().label);
	EXPECT_EQ(1U, fallback->catalog.revision);
}

TEST(FolderTaskCatalogRegistry, GenerationResetAndKnownAbsencePublishEmptyPresentFolderSlot)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 20, 1,
		R"json({"tasks":{"version":"2.0.0","tasks":[{"label":"old","command":"old"}]}})json")).Succeeded());
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Folder, 20, { engine }), artifacts).Succeeded());
	ASSERT_TRUE(artifacts.BeginGeneration(21).Succeeded());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 21, { engine }), artifacts).status);
	const auto reset = registry.SnapshotForFolder(engine);
	ASSERT_TRUE(reset);
	EXPECT_TRUE(reset->catalog.definitions.empty());
	EXPECT_FALSE(reset->catalog.sourceUri);
	EXPECT_EQ(21U, registry.Snapshot().generation);
}

TEST(FolderTaskCatalogRegistry, InvalidArtifactRetainsServiceLastGoodAndRegistryDoesNotClearIt)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 25, 1,
		R"json({"tasks":{"version":"2.0.0","tasks":[{"label":"good","command":"good"}]}})json")).Succeeded());
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Folder, 25, { engine }), artifacts).Succeeded());
	EXPECT_FALSE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 25, 2,
		R"json({"tasks":{"version":"2.0.0","tasks":"bad"}})json")).Succeeded());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::NoChange,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 25, { engine }), artifacts).status);
	EXPECT_EQ(L"good", registry.SnapshotForFolder(engine)->catalog.definitions.front().label);
}

TEST(FolderTaskCatalogRegistry, StoppedArtifactBatchDoesNotPublishASeparatelyReadEmptyTopology)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 29, 1,
		R"json({"tasks":{"version":"2.0.0","tasks":[{"label":"good","command":"good"}]}})json")).Succeeded());
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Folder, 29, { engine }), artifacts).Succeeded());
	const auto accepted = registry.Snapshot();
	ASSERT_TRUE(artifacts.Stop().Succeeded());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::CatalogRejected,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 29, { engine }), artifacts).status);
	EXPECT_EQ(accepted.revision, registry.Snapshot().revision);
	EXPECT_EQ(L"good", registry.SnapshotForFolder(engine)->catalog.definitions.front().label);
}

TEST(FolderTaskCatalogRegistry, StopClearsSlotsAndRejectsLaterReconcile)
{
	CWorkspaceArtifactDocumentService artifacts;
	CFolderTaskCatalogRegistry registry;
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	ASSERT_TRUE(registry.Reconcile(Context(EWorkspaceKind::Folder, 31, { engine }), artifacts).Succeeded());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Applied, registry.Stop().status);
	EXPECT_TRUE(registry.Snapshot().stopped);
	EXPECT_TRUE(registry.Snapshot().folders.empty());
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Stopped,
		registry.Reconcile(Context(EWorkspaceKind::Folder, 31, { engine }), artifacts).status);
	EXPECT_EQ(EFolderTaskCatalogRegistryStatus::Stopped, registry.Stop().status);
}

} // namespace
