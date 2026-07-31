/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/workspace/WorkspaceConfigurationTypes.h"

#include <string_view>

namespace workbench::workspace {

//! Parses a .code-workspace document against its already resolved resource URI.
//! It performs lexical URI resolution only and never probes the filesystem.
class CWorkspaceConfigurationDocumentParser final {
public:
	[[nodiscard]] static WorkspaceConfigurationParseResult Parse(
		std::string_view utf8,
		const platform::uri::Uri& workspaceConfigUri);
};

} // namespace workbench::workspace
