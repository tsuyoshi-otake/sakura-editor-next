/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "debug/launch/LaunchConfigurationCatalog.h"

namespace {

using debug::launch::CLaunchConfigurationCatalog;
using debug::launch::ELaunchConfigurationCatalogState;
using debug::launch::ELaunchConfigurationCatalogStatus;
using workbench::workspace::EWorkspaceArtifactDocumentKind;
using workbench::workspace::EWorkspaceArtifactDocumentSource;
using workbench::workspace::LaunchDocumentSnapshot;
using workbench::workspace::WorkspaceArtifactDocument;

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

LaunchDocumentSnapshot FolderSnapshot(const platform::uri::Uri& resource, std::uint64_t generation,
	std::uint64_t revision, const std::string& rawJsonc)
{
	auto parsed = platform::serialization::CJsoncDocument::Parse(rawJsonc);
	EXPECT_TRUE(parsed.Succeeded());
	const auto* root = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
	EXPECT_NE(nullptr, root);
	return { WorkspaceArtifactDocument {
		.kind = EWorkspaceArtifactDocumentKind::Launch,
		.source = EWorkspaceArtifactDocumentSource::Folder,
		.resource = resource,
		.generation = generation,
		.revision = revision,
		.rawJsonc = rawJsonc,
		.root = *root,
		.artifact = *root,
	} };
}

LaunchDocumentSnapshot WorkspaceSnapshot(const platform::uri::Uri& resource, std::uint64_t generation,
	std::uint64_t revision, const std::string& rawJsonc)
{
	auto parsed = platform::serialization::CJsoncDocument::Parse(rawJsonc);
	EXPECT_TRUE(parsed.Succeeded());
	const auto* root = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
	EXPECT_NE(nullptr, root);
	const auto launch = root->find(L"launch");
	EXPECT_NE(root->end(), launch);
	const auto* artifact = std::get_if<platform::serialization::JsoncValue::Object>(&launch->second.Value());
	EXPECT_NE(nullptr, artifact);
	return { WorkspaceArtifactDocument {
		.kind = EWorkspaceArtifactDocumentKind::Launch,
		.source = EWorkspaceArtifactDocumentSource::WorkspaceFile,
		.resource = resource,
		.generation = generation,
		.revision = revision,
		.rawJsonc = rawJsonc,
		.root = *root,
		.artifact = *artifact,
	} };
}

std::string Configuration(const char* name, const char* type = "cppdbg", const char* request = "launch")
{
	return std::string(R"json({ "name": ")json") + name + R"json(", "type": ")json" + type
		+ R"json(", "request": ")json" + request + R"json(" })json";
}

TEST(LaunchConfigurationCatalog, BuildsImmutableCatalogsFromFolderAndWorkspaceLaunchDocuments)
{
	CLaunchConfigurationCatalog catalog;
	const auto folder = ParseUri(L"file:///C:/Work/demo/.vscode/launch.json");
	const auto workspace = ParseUri(L"file:///C:/Work/demo.code-workspace");

	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Applied, catalog.Apply(FolderSnapshot(folder, 1, 1,
		R"json({ "configurations": [{ "name": "Folder", "type": "cppdbg", "request": "launch", "program": "a.exe" }], "compounds": [{ "name": "All", "configurations": ["Folder"] }] })json")).status);
	auto folderCatalog = catalog.Snapshot();
	ASSERT_EQ(ELaunchConfigurationCatalogState::Ready, folderCatalog.state);
	ASSERT_TRUE(folderCatalog.catalog);
	EXPECT_EQ(folder.ToString(), folderCatalog.catalog->source.ToString());
	ASSERT_EQ(1U, folderCatalog.catalog->configurations.size());
	EXPECT_EQ(L"Folder", folderCatalog.catalog->configurations.front().name);
	EXPECT_TRUE(folderCatalog.catalog->configurations.front().raw.contains(L"program"));
	const auto compound = catalog.FindCompound(L"All");
	ASSERT_TRUE(compound);
	EXPECT_EQ(L"All", compound->name);

	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Applied, catalog.Apply(WorkspaceSnapshot(workspace, 2, 1,
		R"json({ "folders": [], "launch": { "configurations": [{ "name": "Workspace", "type": "coreclr", "request": "attach" }], "compounds": [] } })json")).status);
	const auto workspaceCatalog = catalog.Snapshot();
	ASSERT_TRUE(workspaceCatalog.catalog);
	EXPECT_EQ(workspace.ToString(), workspaceCatalog.catalog->source.ToString());
	EXPECT_EQ(2U, workspaceCatalog.catalog->generation);
	EXPECT_EQ(L"Workspace", workspaceCatalog.catalog->configurations.front().name);
	EXPECT_FALSE(catalog.FindConfiguration(L"Folder"));
}

TEST(LaunchConfigurationCatalog, RejectsDuplicateInvalidAndOversizedEntriesWithoutReplacingLastGoodCatalog)
{
	CLaunchConfigurationCatalog catalog;
	const auto resource = ParseUri(L"file:///C:/Work/demo/.vscode/launch.json");
	ASSERT_TRUE(catalog.Apply(FolderSnapshot(resource, 3, 1,
		R"json({ "configurations": [{ "name": "Good", "type": "cppdbg", "request": "launch" }] })json")).Succeeded());

	EXPECT_EQ(ELaunchConfigurationCatalogStatus::DuplicateName, catalog.Apply(FolderSnapshot(resource, 3, 2,
		R"json({ "configurations": [{ "name": "Same", "type": "cppdbg", "request": "launch" }, { "name": "Same", "type": "coreclr", "request": "attach" }] })json")).status);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::InvalidConfiguration, catalog.Apply(FolderSnapshot(resource, 3, 3,
		R"json({ "configurations": [{ "name": "Missing request", "type": "cppdbg" }] })json")).status);

	std::string oversized = R"json({ "configurations": [)json";
	for (std::size_t index = 0; index <= debug::launch::LaunchConfigurationCatalogLimits::kMaximumConfigurations; ++index) {
		if (index != 0) oversized += ',';
		oversized += Configuration(("Many" + std::to_string(index)).c_str());
	}
	oversized += R"json(] })json";
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::MaximumConfigurationsExceeded,
		catalog.Apply(FolderSnapshot(resource, 3, 4, oversized)).status);

	const auto retained = catalog.Snapshot();
	ASSERT_TRUE(retained.catalog);
	EXPECT_EQ(3U, retained.catalog->generation);
	EXPECT_EQ(1U, retained.catalog->revision);
	ASSERT_EQ(1U, retained.catalog->configurations.size());
	EXPECT_EQ(L"Good", retained.catalog->configurations.front().name);
}

TEST(LaunchConfigurationCatalog, FencesStaleGenerationAndRevision)
{
	CLaunchConfigurationCatalog catalog;
	const auto resource = ParseUri(L"file:///C:/Work/demo/.vscode/launch.json");
	ASSERT_TRUE(catalog.Apply(FolderSnapshot(resource, 7, 2,
		R"json({ "configurations": [{ "name": "Current", "type": "cppdbg", "request": "launch" }] })json")).Succeeded());

	EXPECT_EQ(ELaunchConfigurationCatalogStatus::StaleRevision, catalog.Apply(FolderSnapshot(resource, 7, 2,
		R"json({ "configurations": [{ "name": "Same revision", "type": "cppdbg", "request": "launch" }] })json")).status);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::StaleGeneration, catalog.Apply(FolderSnapshot(resource, 6, 99,
		R"json({ "configurations": [{ "name": "Older", "type": "cppdbg", "request": "launch" }] })json")).status);
	ASSERT_TRUE(catalog.Snapshot().catalog);
	EXPECT_EQ(L"Current", catalog.Snapshot().catalog->configurations.front().name);
}

TEST(LaunchConfigurationCatalog, CorruptSemanticUpdateRetainsLastGoodCatalog)
{
	CLaunchConfigurationCatalog catalog;
	const auto resource = ParseUri(L"file:///C:/Work/demo/.vscode/launch.json");
	ASSERT_TRUE(catalog.Apply(FolderSnapshot(resource, 9, 1,
		R"json({ "configurations": [{ "name": "Good", "type": "cppdbg", "request": "launch" }] })json")).Succeeded());

	EXPECT_EQ(ELaunchConfigurationCatalogStatus::UnknownCompoundReference, catalog.Apply(FolderSnapshot(resource, 9, 2,
		R"json({ "configurations": [{ "name": "Good", "type": "cppdbg", "request": "launch" }], "compounds": [{ "name": "Corrupt", "configurations": ["Missing"] }] })json")).status);
	const auto snapshot = catalog.Snapshot();
	ASSERT_EQ(ELaunchConfigurationCatalogState::Ready, snapshot.state);
	ASSERT_TRUE(snapshot.catalog);
	EXPECT_EQ(1U, snapshot.catalog->revision);
	EXPECT_FALSE(catalog.FindCompound(L"Corrupt"));
	EXPECT_TRUE(catalog.FindConfiguration(L"Good"));
}

TEST(LaunchConfigurationCatalog, ClearStopAndInvalidInputHaveOnlyExplicitTerminalStates)
{
	CLaunchConfigurationCatalog catalog;
	const auto resource = ParseUri(L"file:///C:/Work/demo/.vscode/launch.json");
	EXPECT_EQ(ELaunchConfigurationCatalogState::Empty, catalog.Snapshot().state);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::InvalidSnapshot, catalog.Apply({}).status);
	EXPECT_EQ(ELaunchConfigurationCatalogState::Empty, catalog.Snapshot().state);
	ASSERT_TRUE(catalog.Apply(FolderSnapshot(resource, 11, 1,
		R"json({ "configurations": [{ "name": "Ready", "type": "cppdbg", "request": "launch" }] })json")).Succeeded());
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Cleared, catalog.Clear(11, 2).status);
	EXPECT_EQ(ELaunchConfigurationCatalogState::Empty, catalog.Snapshot().state);
	EXPECT_FALSE(catalog.Snapshot().catalog);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Cleared, catalog.Stop().status);
	EXPECT_EQ(ELaunchConfigurationCatalogState::Stopped, catalog.Snapshot().state);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Stopped, catalog.Clear(11, 3).status);
	EXPECT_EQ(ELaunchConfigurationCatalogStatus::Stopped, catalog.Apply(FolderSnapshot(resource, 12, 1,
		R"json({ "configurations": [] })json")).status);
}

} // namespace
