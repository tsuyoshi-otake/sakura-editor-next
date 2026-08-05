/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include <utility>

#include <sakura/uri/UriIdentity.h>
#include "workbench/workspace/WorkspaceResourceDescriptors.h"

namespace {

platform::uri::Uri ParseUri(const wchar_t* text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

TEST(WorkspaceResourceDescriptors, SuppliesAllFolderWorkspaceFilesAsWholeDocuments)
{
	auto descriptors = workbench::workspace::DescribeWorkspaceFolderResources(ParseUri(L"file:///C:/Work/project"));
	ASSERT_TRUE(descriptors.has_value());
	ASSERT_EQ(4U, descriptors->size());
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Settings, (*descriptors)[0].member);
	EXPECT_EQ(L"file:///C:/Work/project/.vscode/settings.json", (*descriptors)[0].resource.ToString());
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Tasks, (*descriptors)[1].member);
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Launch, (*descriptors)[2].member);
	EXPECT_EQ(workbench::workspace::EWorkspaceFileMember::Extensions, (*descriptors)[3].member);
	for (const auto& descriptor : *descriptors) {
		EXPECT_EQ(workbench::workspace::EWorkspaceResourceContentMode::WholeDocument, descriptor.contentMode);
	}
}

TEST(WorkspaceResourceDescriptors, DoesNotConstructPartialDescriptorsForFolderMetadataUris)
{
	EXPECT_FALSE(workbench::workspace::DescribeWorkspaceFolderResources(ParseUri(L"file:///C:/Work/project?revision=7")).has_value());
}

} // namespace
