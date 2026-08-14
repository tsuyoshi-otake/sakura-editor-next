/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/workspace/WorkspaceResourceDescriptors.h"

#include <array>
#include <utility>

namespace workbench::workspace {

std::optional<std::vector<WorkspaceResourceDescriptor>> DescribeWorkspaceFolderResources(const platform::uri::Uri& folderUri)
{
	if (folderUri.Scheme().empty() || folderUri.Path().empty() || folderUri.Query() || folderUri.Fragment()) return std::nullopt;
	std::wstring basePath = folderUri.Path();
	if (basePath.back() != L'/') basePath.push_back(L'/');
	std::vector<WorkspaceResourceDescriptor> result;
	result.reserve(3);
	for (const auto [member, name] : std::array {
		std::pair { EWorkspaceFileMember::Settings, L"settings.json" },
		std::pair { EWorkspaceFileMember::Tasks, L"tasks.json" },
		std::pair { EWorkspaceFileMember::Launch, L"launch.json" },
	}) {
		auto resource = platform::uri::Uri::FromComponents(
			folderUri.Scheme(), folderUri.Authority(), basePath + L".vscode/" + name,
			std::nullopt, std::nullopt, folderUri.HasAuthority());
		if (!resource) return std::nullopt;
		result.push_back({ member, std::move(*resource.value), EWorkspaceResourceContentMode::WholeDocument });
	}
	return result;
}

} // namespace workbench::workspace
