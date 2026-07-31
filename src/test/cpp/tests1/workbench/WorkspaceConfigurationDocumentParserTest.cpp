/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include <utility>

#include "platform/uri/UriIdentity.h"
#include "workbench/workspace/WorkspaceConfigurationDocumentParser.h"

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

TEST(WorkspaceConfigurationDocumentParser, RejectsNonObjectSettingsBeforeAnyConfigurationAdapterCanRun)
{
	const auto result = workbench::workspace::CWorkspaceConfigurationDocumentParser::Parse(
		R"json({ "settings": [] })json", ParseUri(L"file:///C:/Work/demo.code-workspace"));
	EXPECT_FALSE(result.Succeeded());
	ASSERT_EQ(1U, result.diagnostics.size());
	EXPECT_EQ(workbench::workspace::EWorkspaceConfigurationDiagnosticCode::SettingsMustBeObject, result.diagnostics.front().code);
}

} // namespace
