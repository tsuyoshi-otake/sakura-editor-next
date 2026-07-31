/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/workspace/WorkspaceConfigurationTypes.h"

#include <optional>
#include <vector>

namespace workbench::workspace {

//! Returns the four non-merged .vscode resources for an explicit folder. An
//! invalid folder URI returns no descriptors, rather than a partly usable list.
[[nodiscard]] std::optional<std::vector<WorkspaceResourceDescriptor>> DescribeWorkspaceFolderResources(
	const platform::uri::Uri& folderUri);

} // namespace workbench::workspace
