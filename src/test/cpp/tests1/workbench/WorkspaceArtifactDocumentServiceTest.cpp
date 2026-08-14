/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

namespace {

using workbench::workspace::CWorkspaceArtifactDocumentService;
using workbench::workspace::EWorkspaceArtifactDocumentKind;
using workbench::workspace::EWorkspaceArtifactDocumentSource;
using workbench::workspace::EWorkspaceArtifactDocumentStatus;
using workbench::workspace::ETasksDocumentBatchStatus;
using workbench::workspace::WorkspaceArtifactDocumentUpdate;

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceArtifactDocumentUpdate Update(
	EWorkspaceArtifactDocumentKind kind,
	EWorkspaceArtifactDocumentSource source,
	const platform::uri::Uri& resource,
	std::uint64_t generation,
	std::uint64_t revision,
	std::string utf8,
	std::optional<platform::uri::Uri> folderUri = std::nullopt)
{
	return { kind, source, std::move(folderUri), resource, generation, revision, std::move(utf8) };
}

TEST(WorkspaceArtifactDocumentService, FolderDocumentWinsOverWorkspaceFileForOnlyItsFolderAndPreservesWorkspaceSource)
{
	CWorkspaceArtifactDocumentService service;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto folder = ParseUri(L"file:///C:/Work/engine");
	const auto otherFolder = ParseUri(L"file:///C:/Work/tools");
	const auto folderTasks = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");

	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::Applied, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 1, 1,
		R"json({ // retain this whole workspace document
  "folders": [{ "path": "engine" }],
  "settings": { "editor.tabSize": 3 },
  "tasks": { "version": "2.0.0", "tasks": [] },
  "unrelated": true
})json")).status);
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::Applied, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::Folder, folderTasks, 1, 1,
		R"json({ "version": "2.0.0", "tasks": [{ "label": "folder" }] })json", folder)).status);

	const auto selected = service.Tasks(folder);
	ASSERT_TRUE(selected.document);
	EXPECT_EQ(EWorkspaceArtifactDocumentSource::Folder, selected.document->source);
	EXPECT_EQ(folderTasks.ToString(), selected.document->resource.ToString());
	EXPECT_NE(nullptr, std::get_if<platform::serialization::JsoncValue::Array>(
		&selected.document->artifact.at(L"tasks").Value()));

	const auto fallback = service.Tasks(otherFolder);
	ASSERT_TRUE(fallback.document);
	EXPECT_EQ(EWorkspaceArtifactDocumentSource::WorkspaceFile, fallback.document->source);
	EXPECT_EQ(workspace.ToString(), fallback.document->resource.ToString());
	EXPECT_TRUE(fallback.document->root.contains(L"folders"));
	EXPECT_TRUE(fallback.document->root.contains(L"settings"));
	EXPECT_TRUE(fallback.document->root.contains(L"unrelated"));
	EXPECT_NE(std::string::npos, fallback.document->rawJsonc.find("retain this whole"));
}

TEST(WorkspaceArtifactDocumentService, TasksBatchCopiesOrderedFolderSelectionsAndServiceStateAtomically)
{
	CWorkspaceArtifactDocumentService service;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto tools = ParseUri(L"file:///C:/Work/tools");
	const auto engineTasks = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	ASSERT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::WorkspaceFile,
		workspace, 2, 1, R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "workspace" }] } })json")).Succeeded());
	ASSERT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::Folder,
		engineTasks, 2, 1, R"json({ "version": "2.0.0", "tasks": [{ "label": "engine" }] })json", engine)).Succeeded());

	const auto batch = service.TasksForFolders({ tools, engine });
	ASSERT_EQ(ETasksDocumentBatchStatus::Applied, batch.status);
	EXPECT_TRUE(batch.Succeeded());
	EXPECT_EQ(2U, batch.service.generation);
	EXPECT_FALSE(batch.service.stopped);
	EXPECT_EQ(2U, batch.service.acceptedDocuments);
	ASSERT_EQ(2U, batch.documents.size());
	ASSERT_TRUE(batch.documents[0].document);
	ASSERT_TRUE(batch.documents[1].document);
	EXPECT_EQ(EWorkspaceArtifactDocumentSource::WorkspaceFile, batch.documents[0].document->source);
	EXPECT_EQ(EWorkspaceArtifactDocumentSource::Folder, batch.documents[1].document->source);
	EXPECT_EQ(workspace.ToString(), batch.documents[0].document->resource.ToString());
	EXPECT_EQ(engineTasks.ToString(), batch.documents[1].document->resource.ToString());

	const auto empty = service.TasksForFolders({});
	EXPECT_EQ(ETasksDocumentBatchStatus::Applied, empty.status);
	EXPECT_TRUE(empty.documents.empty());
	EXPECT_EQ(2U, empty.service.generation);
	EXPECT_FALSE(empty.service.stopped);
}

TEST(WorkspaceArtifactDocumentService, TasksBatchBoundsInvalidFoldersAndStoppedStateWithoutImplicitFallback)
{
	CWorkspaceArtifactDocumentService service;
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto invalid = ParseUri(L"file:///C:/Work/engine?not-a-folder");
	std::vector<platform::uri::Uri> tooMany;
	tooMany.reserve(65U);
	for (std::size_t index = 0; index < 65U; ++index) {
		tooMany.push_back(ParseUri((L"file:///C:/Work/f" + std::to_wstring(index)).c_str()));
	}
	EXPECT_EQ(ETasksDocumentBatchStatus::TooManyFolders, service.TasksForFolders(tooMany).status);
	const auto invalidBatch = service.TasksForFolders({ invalid });
	EXPECT_EQ(ETasksDocumentBatchStatus::InvalidFolder, invalidBatch.status);
	EXPECT_TRUE(invalidBatch.documents.empty());
	EXPECT_FALSE(invalidBatch.service.stopped);

	ASSERT_TRUE(service.Stop().Succeeded());
	const auto stopped = service.TasksForFolders({ engine });
	EXPECT_EQ(ETasksDocumentBatchStatus::Stopped, stopped.status);
	EXPECT_TRUE(stopped.service.stopped);
	EXPECT_EQ(0U, stopped.service.acceptedDocuments);
	ASSERT_EQ(1U, stopped.documents.size());
	EXPECT_FALSE(stopped.documents.front().document);
}

TEST(WorkspaceArtifactDocumentService, KeepsTasksAndLaunchAsSeparateTypedResources)
{
	CWorkspaceArtifactDocumentService service;
	const auto folder = ParseUri(L"file:///C:/Work/engine");
	const auto tasks = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	const auto launch = ParseUri(L"file:///C:/Work/engine/.vscode/launch.json");

	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::Folder,
		tasks, 3, 1, R"json({ "version": "2.0.0", "tasks": [] })json", folder)).Succeeded());
	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::Folder,
		launch, 3, 1, R"json({ "version": "0.2.0", "configurations": [] })json", folder)).Succeeded());
	const auto taskDocument = service.Tasks(folder).document;
	const auto launchDocument = service.Launch(folder).document;
	ASSERT_TRUE(taskDocument);
	ASSERT_TRUE(launchDocument);
	EXPECT_EQ(EWorkspaceArtifactDocumentKind::Tasks, taskDocument->kind);
	EXPECT_EQ(EWorkspaceArtifactDocumentKind::Launch, launchDocument->kind);
	EXPECT_EQ(tasks.ToString(), taskDocument->resource.ToString());
	EXPECT_EQ(launch.ToString(), launchDocument->resource.ToString());
}

TEST(WorkspaceArtifactDocumentService, InvalidDocumentsPreserveLastAcceptedSnapshotAndReturnTypedFailures)
{
	CWorkspaceArtifactDocumentService service;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::WorkspaceFile,
		workspace, 5, 1, R"json({ "launch": { "configurations": [] } })json")).Succeeded());

	const auto invalidSchema = service.Apply(Update(EWorkspaceArtifactDocumentKind::Launch,
		EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 5, 2,
		R"json({ "launch": { "configurations": {} } })json"));
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::InvalidSchema, invalidSchema.status);
	const auto retained = service.Launch().document;
	ASSERT_TRUE(retained);
	EXPECT_EQ(1U, retained->revision);
	EXPECT_EQ(std::string::npos, retained->rawJsonc.find("[1]"));

	const auto invalidUtf8 = service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks,
		EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 5, 1,
		std::string("{\"tasks\": {\"label\": \"") + static_cast<char>(0xc3) + "\"}}"));
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::InvalidUtf8, invalidUtf8.status);
	const auto malformedJsonc = service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks,
		EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 5, 1,
		R"json({ "tasks": [ })json"));
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::JsoncParseFailed, malformedJsonc.status);
	const auto duplicate = service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks,
		EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 5, 1,
		R"json({ "tasks": {}, "tasks": {} })json"));
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::DuplicateKey, duplicate.status);
}

TEST(WorkspaceArtifactDocumentService, RejectsStaleGenerationAndRevisionWithoutChangingAcceptedDocument)
{
	CWorkspaceArtifactDocumentService service;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::WorkspaceFile,
		workspace, 7, 2, R"json({ "launch": { "configurations": [] } })json")).Succeeded());
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::StaleRevision, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 7, 2,
		R"json({ "launch": { "configurations": [{ "name": "old" }] } })json")).status);
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::StaleGeneration, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 6, 99,
		R"json({ "launch": { "configurations": [{ "name": "older generation" }] } })json")).status);
	const auto retained = service.Launch().document;
	ASSERT_TRUE(retained);
	EXPECT_EQ(7U, retained->generation);
	EXPECT_EQ(2U, retained->revision);

	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentSource::WorkspaceFile,
		workspace, 8, 1, R"json({ "launch": { "configurations": [{ "name": "new generation" }] } })json")).Succeeded());
	const auto advanced = service.Launch().document;
	ASSERT_TRUE(advanced);
	EXPECT_EQ(8U, advanced->generation);
	EXPECT_EQ(1U, advanced->revision);
}

TEST(WorkspaceArtifactDocumentService, RejectsDocumentLimitWithTypedTerminalStatus)
{
	CWorkspaceArtifactDocumentService service;
	for (std::size_t index = 0; index < 514U; ++index) {
		const auto folder = ParseUri((L"file:///C:/Work/f" + std::to_wstring(index)).c_str());
		const auto resource = ParseUri((L"file:///C:/Work/f" + std::to_wstring(index) + L"/.vscode/tasks.json").c_str());
		const auto result = service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks,
			EWorkspaceArtifactDocumentSource::Folder, resource, 11, 1,
			R"json({ "version": "2.0.0", "tasks": [] })json", folder));
		ASSERT_EQ(EWorkspaceArtifactDocumentStatus::Applied, result.status) << index;
	}
	const auto overflowFolder = ParseUri(L"file:///C:/Work/overflow");
	const auto overflow = ParseUri(L"file:///C:/Work/overflow/.vscode/tasks.json");
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::MaximumDocumentsExceeded, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::Folder, overflow, 11, 1,
		R"json({ "version": "2.0.0", "tasks": [] })json", overflowFolder)).status);
}

TEST(WorkspaceArtifactDocumentService, StopClearsSubscribersAndRejectsLaterDelivery)
{
	CWorkspaceArtifactDocumentService service;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	int deliveries = 0;
	ASSERT_TRUE(service.Subscribe([&deliveries](const auto&) { ++deliveries; }));
	EXPECT_TRUE(service.Apply(Update(EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::WorkspaceFile,
		workspace, 13, 1, R"json({ "tasks": { "version": "2.0.0", "tasks": [] } })json")).Succeeded());
	EXPECT_EQ(1, deliveries);
	EXPECT_TRUE(service.Stop().Succeeded());
	EXPECT_EQ(EWorkspaceArtifactDocumentStatus::Stopped, service.Apply(Update(
		EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 13, 2,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "later" }] } })json")).status);
	EXPECT_EQ(1, deliveries);
	EXPECT_TRUE(service.Snapshot().stopped);
	EXPECT_EQ(0U, service.Snapshot().acceptedDocuments);
	EXPECT_FALSE(service.Tasks().document);
}

} // namespace
