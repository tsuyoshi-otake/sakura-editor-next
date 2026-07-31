/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/tasks/TaskConfigurationCatalog.h"
#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

namespace {

using workbench::tasks::CTaskConfigurationCatalog;
using workbench::tasks::ETaskConfigurationCatalogStatus;
using workbench::tasks::ETaskExecutionKind;
using workbench::tasks::ETaskUnsupportedCapability;
using workbench::tasks::HasUnsupportedCapability;
using workbench::workspace::CWorkspaceArtifactDocumentService;
using workbench::workspace::EWorkspaceArtifactDocumentKind;
using workbench::workspace::EWorkspaceArtifactDocumentSource;
using workbench::workspace::WorkspaceArtifactDocumentUpdate;

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceArtifactDocumentUpdate Update(
	EWorkspaceArtifactDocumentSource source,
	const platform::uri::Uri& resource,
	std::uint64_t generation,
	std::uint64_t revision,
	std::string utf8,
	std::optional<platform::uri::Uri> folderUri = std::nullopt)
{
	return { EWorkspaceArtifactDocumentKind::Tasks, source, std::move(folderUri), resource, generation, revision, std::move(utf8) };
}

TEST(TaskConfigurationCatalog, EnumeratesRunnableVscodeTasksWithBoundedMetadata)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto folder = ParseUri(L"file:///C:/Work/engine");
	const auto resource = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::Folder, resource, 4, 7, R"json({
  "version": "2.0.0",
  "tasks": [{
    "label": "build",
    "type": "process",
    "command": "ninja",
    "args": ["-C", "out"],
    "options": { "cwd": "${workspaceFolder}" },
    "group": { "kind": "build", "isDefault": true },
    "presentation": { "reveal": "always", "panel": "shared", "focus": false, "clear": true, "close": true }
  }]
})json", folder)).Succeeded());

	EXPECT_EQ(ETaskConfigurationCatalogStatus::Applied, catalog.Apply(artifacts.Tasks(folder)).status);
	const auto snapshot = catalog.Snapshot();
	ASSERT_EQ(1U, snapshot.definitions.size());
	const auto& task = snapshot.definitions.front();
	EXPECT_EQ(L"build", task.label);
	EXPECT_EQ(ETaskExecutionKind::Process, task.executionKind);
	EXPECT_EQ(L"ninja", task.command);
	ASSERT_EQ(2U, task.arguments.size());
	EXPECT_EQ(L"-C", task.arguments[0]);
	ASSERT_TRUE(task.workingDirectory);
	EXPECT_EQ(L"${workspaceFolder}", *task.workingDirectory);
	ASSERT_TRUE(task.group);
	EXPECT_EQ(L"build", task.group->id);
	EXPECT_TRUE(task.group->isDefault);
	ASSERT_TRUE(task.presentation);
	EXPECT_EQ(L"shared", *task.presentation->panel);
	EXPECT_TRUE(*task.presentation->clear);
	ASSERT_TRUE(task.sourceUri);
	EXPECT_EQ(resource.ToString(), task.sourceUri->ToString());
	EXPECT_EQ(4U, task.generation);
	EXPECT_EQ(7U, task.revision);
	EXPECT_TRUE(task.IsRunnable());
}

TEST(TaskConfigurationCatalog, EnumeratesWorkspaceTasksAndAcceptsFolderPrecedenceSelection)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto engine = ParseUri(L"file:///C:/Work/engine");
	const auto tools = ParseUri(L"file:///C:/Work/tools");
	const auto folderResource = ParseUri(L"file:///C:/Work/engine/.vscode/tasks.json");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 9, 1, R"json({
  "folders": [{ "path": "engine" }],
  "tasks": { "version": "2.0.0", "tasks": [{ "label": "workspace", "command": "ws" }] }
})json")).Succeeded());
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::Folder, folderResource, 9, 1,
		R"json({ "version": "2.0.0", "tasks": [{ "label": "folder", "command": "folder" }] })json", engine)).Succeeded());

	EXPECT_EQ(ETaskConfigurationCatalogStatus::Applied, catalog.Apply(artifacts.Tasks(engine)).status);
	ASSERT_EQ(1U, catalog.Snapshot().definitions.size());
	EXPECT_EQ(L"folder", catalog.Snapshot().definitions.front().label);
	EXPECT_EQ(folderResource.ToString(), catalog.Snapshot().sourceUri->ToString());

	EXPECT_EQ(ETaskConfigurationCatalogStatus::Applied, catalog.Apply(artifacts.Tasks(tools)).status);
	ASSERT_EQ(1U, catalog.Snapshot().definitions.size());
	EXPECT_EQ(L"workspace", catalog.Snapshot().definitions.front().label);
	EXPECT_EQ(workspace.ToString(), catalog.Snapshot().sourceUri->ToString());
}

TEST(TaskConfigurationCatalog, RejectsDuplicateInvalidAndOversizedEntriesWithoutReplacingLastGoodCatalog)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 11, 1,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "good", "command": "ok" }] } })json")).Succeeded());
	ASSERT_TRUE(catalog.Apply(artifacts.Tasks()).Succeeded());

	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 11, 2,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "same", "command": "a" }, { "label": "same", "command": "b" }] } })json")).Succeeded());
	EXPECT_EQ(ETaskConfigurationCatalogStatus::DuplicateLabel, catalog.Apply(artifacts.Tasks()).status);
	ASSERT_EQ(1U, catalog.Snapshot().definitions.size());
	EXPECT_EQ(L"good", catalog.Snapshot().definitions.front().label);

	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 11, 3,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "bad", "type": "process" }] } })json")).Succeeded());
	EXPECT_EQ(ETaskConfigurationCatalogStatus::InvalidSchema, catalog.Apply(artifacts.Tasks()).status);
	EXPECT_EQ(L"good", catalog.Snapshot().definitions.front().label);

	const std::string oversized(257U, 'x');
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 11, 4,
		std::string("{ \"tasks\": { \"version\": \"2.0.0\", \"tasks\": [{ \"label\": \"") + oversized + "\", \"command\": \"x\" }] } }")).Succeeded());
	EXPECT_EQ(ETaskConfigurationCatalogStatus::EntryTooLarge, catalog.Apply(artifacts.Tasks()).status);
	EXPECT_EQ(L"good", catalog.Snapshot().definitions.front().label);
}

TEST(TaskConfigurationCatalog, EnumeratesUnsupportedCapabilitiesButNeverMarksThemRunnable)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 13, 1, R"json({
  "tasks": { "version": "2.0.0", "tasks": [
    { "label": "extension", "type": "npm", "dependsOn": ["prepare"], "isBackground": true, "problemMatcher": "$tsc" },
    { "label": "shell-with-problem", "type": "shell", "command": "build", "problemMatcher": ["$gcc"] }
  ] }
})json")).Succeeded());
	ASSERT_TRUE(catalog.Apply(artifacts.Tasks()).Succeeded());
	const auto snapshot = catalog.Snapshot();
	ASSERT_EQ(2U, snapshot.definitions.size());
	const auto& custom = snapshot.definitions[0];
	EXPECT_EQ(L"extension", custom.label);
	EXPECT_EQ(ETaskExecutionKind::Custom, custom.executionKind);
	EXPECT_FALSE(custom.IsRunnable());
	EXPECT_TRUE(HasUnsupportedCapability(custom.unsupportedCapabilities, ETaskUnsupportedCapability::CustomExecution));
	EXPECT_TRUE(HasUnsupportedCapability(custom.unsupportedCapabilities, ETaskUnsupportedCapability::Dependencies));
	EXPECT_TRUE(HasUnsupportedCapability(custom.unsupportedCapabilities, ETaskUnsupportedCapability::Background));
	EXPECT_TRUE(HasUnsupportedCapability(custom.unsupportedCapabilities, ETaskUnsupportedCapability::ProblemMatcher));
	const auto& shell = snapshot.definitions[1];
	EXPECT_EQ(L"shell-with-problem", shell.label);
	EXPECT_FALSE(shell.IsRunnable());
	EXPECT_TRUE(HasUnsupportedCapability(shell.unsupportedCapabilities, ETaskUnsupportedCapability::ProblemMatcher));
}

TEST(TaskConfigurationCatalog, FencesStaleRevisionsAndPreservesAcceptedDefinitions)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 17, 2,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "new", "command": "new" }] } })json")).Succeeded());
	ASSERT_TRUE(catalog.Apply(artifacts.Tasks()).Succeeded());
	EXPECT_EQ(ETaskConfigurationCatalogStatus::StaleRevision, catalog.Apply(artifacts.Tasks()).status);
	EXPECT_EQ(L"new", catalog.Snapshot().definitions.front().label);

	workbench::workspace::TasksDocumentSnapshot stale = artifacts.Tasks();
	ASSERT_TRUE(stale.document);
	stale.document->generation = 16;
	stale.document->revision = 99;
	EXPECT_EQ(ETaskConfigurationCatalogStatus::StaleGeneration, catalog.Apply(stale).status);
	EXPECT_EQ(17U, catalog.Snapshot().generation);
}

TEST(TaskConfigurationCatalog, ClearAndStopAreExplicitTerminalStates)
{
	CWorkspaceArtifactDocumentService artifacts;
	CTaskConfigurationCatalog catalog;
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");
	ASSERT_TRUE(artifacts.Apply(Update(EWorkspaceArtifactDocumentSource::WorkspaceFile, workspace, 19, 1,
		R"json({ "tasks": { "version": "2.0.0", "tasks": [{ "label": "one", "command": "one" }] } })json")).Succeeded());
	ASSERT_TRUE(catalog.Apply(artifacts.Tasks()).Succeeded());
	EXPECT_EQ(ETaskConfigurationCatalogStatus::StaleRevision, catalog.Clear(19, 1).status);
	EXPECT_EQ(ETaskConfigurationCatalogStatus::Cleared, catalog.Clear(19, 2).status);
	EXPECT_TRUE(catalog.Snapshot().definitions.empty());
	EXPECT_FALSE(catalog.Snapshot().sourceUri);
	EXPECT_EQ(19U, catalog.Snapshot().generation);
	EXPECT_EQ(2U, catalog.Snapshot().revision);

	EXPECT_TRUE(catalog.Stop().Succeeded());
	EXPECT_TRUE(catalog.Snapshot().stopped);
	EXPECT_EQ(ETaskConfigurationCatalogStatus::Stopped, catalog.Apply(artifacts.Tasks()).status);
	EXPECT_EQ(ETaskConfigurationCatalogStatus::Stopped, catalog.Clear(20, 1).status);
}

} // namespace
