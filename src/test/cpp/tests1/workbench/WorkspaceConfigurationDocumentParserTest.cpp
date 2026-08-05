/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <sakura/uri/UriIdentity.h>
#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"
#include "workbench/workspace/WorkspaceFolderLimits.h"

namespace {

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

TEST(WorkspaceConfigurationDocumentParser, ResolvesRelativeFoldersAndRetainsTypedNonSettingsMembers)
{
	const auto result = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(R"json(
{
  // comments and trailing commas are JSONC
  "folders": [ { "path": "src/../engine" }, ],
  "settings": { "editor.tabSize": 3, },
  "tasks": { "version": "2.0.0" },
  "launch": { "configurations": [] },
  "extensions": { "recommendations": ["sample.extension"] },
}
)json", ParseUri(L"file:///C:/Work/demo.code-workspace"));

	ASSERT_TRUE(result.Succeeded());
	ASSERT_TRUE(result.document->settings.has_value());
	ASSERT_EQ(1U, result.document->folders.size());
	EXPECT_EQ(L"file:///C:/Work/engine", result.document->folders.front().uri.ToString());
	EXPECT_EQ(L"engine", result.document->folders.front().displayName);
	ASSERT_EQ(3U, result.document->fileMembers.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Tasks, result.document->fileMembers[0].member);
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Launch, result.document->fileMembers[1].member);
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Extensions, result.document->fileMembers[2].member);
}

TEST(WorkspaceConfigurationDocumentParser, RequiresExactlyOneFolderLocation)
{
	const auto both = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [{ "path": "one", "uri": "file:///C:/two" }] })json",
		ParseUri(L"file:///C:/Work/demo.code-workspace"));
	EXPECT_FALSE(both.Succeeded());
	ASSERT_EQ(1U, both.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::FolderMustSpecifyExactlyOneLocation, both.diagnostics.front().code);

	const auto missing = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [{}] })json", ParseUri(L"file:///C:/Work/demo.code-workspace"));
	EXPECT_FALSE(missing.Succeeded());
	ASSERT_EQ(1U, missing.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::FolderMustSpecifyExactlyOneLocation, missing.diagnostics.front().code);
}

TEST(WorkspaceConfigurationDocumentParser, CanonicalUriDuplicatesKeepFirstFolderWithPathFreeDiagnostic)
{
	const auto result = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [
  { "uri": "file:///C:/Work/Engine", "name": "first" },
  { "uri": "file://localhost/c:/work/engine", "name": "second" }
] })json", ParseUri(L"file:///C:/Work/demo.code-workspace"));

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, result.document->folders.size());
	EXPECT_EQ(L"first", result.document->folders.front().displayName);
	ASSERT_EQ(1U, result.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::DuplicateFolderUri, result.diagnostics.front().code);
	EXPECT_EQ(std::string::npos, result.diagnostics.front().message.find("C:"));
}

TEST(WorkspaceConfigurationDocumentParser, EmptyExplicitNameUsesDerivedDisplayName)
{
	const auto result = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [{ "path": "D:/Absolute", "name": "" }] })json",
		ParseUri(L"file:///C:/Work/demo.code-workspace"));

	ASSERT_TRUE(result.Succeeded());
	ASSERT_EQ(1U, result.document->folders.size());
	EXPECT_EQ(L"file:///D:/Absolute", result.document->folders.front().uri.ToString());
	EXPECT_EQ(L"Absolute", result.document->folders.front().displayName);
}

TEST(WorkspaceConfigurationDocumentParser, ResolvesUncPathsWithoutBorrowingWorkspaceAuthority)
{
	const auto localWorkspace = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [{ "path": "\\\\server\\share\\Repo", "name": "" }] })json",
		ParseUri(L"file:///C:/Work/demo.code-workspace"));
	ASSERT_TRUE(localWorkspace.Succeeded());
	ASSERT_EQ(1U, localWorkspace.document->folders.size());
	EXPECT_EQ(L"file://server/share/Repo", localWorkspace.document->folders.front().uri.ToString());
	EXPECT_EQ(L"Repo", localWorkspace.document->folders.front().displayName);

	const auto uncWorkspace = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "folders": [{ "path": "..\\Sibling" }, { "path": "\\\\other\\share\\Repo" }] })json",
		ParseUri(L"file://host/workspaces/team/demo.code-workspace"));
	ASSERT_TRUE(uncWorkspace.Succeeded());
	ASSERT_EQ(2U, uncWorkspace.document->folders.size());
	EXPECT_EQ(L"file://host/workspaces/Sibling", uncWorkspace.document->folders[0].uri.ToString());
	EXPECT_EQ(L"file://other/share/Repo", uncWorkspace.document->folders[1].uri.ToString());
}

TEST(WorkspaceConfigurationDocumentParser, RejectsNonObjectSettingsBeforeAnyConfigurationAdapterCanRun)
{
	const auto result = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "settings": [] })json", ParseUri(L"file:///C:/Work/demo.code-workspace"));
	EXPECT_FALSE(result.Succeeded());
	ASSERT_EQ(1U, result.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::SettingsMustBeObject, result.diagnostics.front().code);
}

TEST(WorkspaceConfigurationDocumentParser, SharesTheSixtyFourFolderBoundWithWorkspaceEditing)
{
	auto workspaceDocument = [](std::size_t count) {
		std::string text = "{ \"folders\": [";
		for (std::size_t index = 0; index < count; ++index) {
			text += "{ \"path\": \"Folder" + std::to_string(index) + "\" },";
		}
		return text + "] }";
	};
	const auto resource = ParseUri(L"file:///C:/Work/demo.code-workspace");
	const auto accepted = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		workspaceDocument(workbench::workspace::kMaximumWorkspaceFolders), resource);
	ASSERT_TRUE(accepted.Succeeded());
	EXPECT_EQ(workbench::workspace::kMaximumWorkspaceFolders, accepted.document->folders.size());

	const auto rejected = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		workspaceDocument(workbench::workspace::kMaximumWorkspaceFolders + 1U), resource);
	ASSERT_FALSE(rejected.Succeeded());
	ASSERT_EQ(1U, rejected.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::MaximumFoldersExceeded, rejected.diagnostics.front().code);
}

} // namespace
