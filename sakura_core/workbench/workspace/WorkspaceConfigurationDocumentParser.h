/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "workbench/workspace/WorkspaceConfigurationTypes.h"

#include <optional>
#include <string_view>

namespace workbench::workspace {

//! Parses a .code-workspace document against its already resolved resource URI.
//! It performs lexical URI resolution only and never probes the filesystem.
class CWorkspaceConfigurationDocumentParser final {
public:
	[[nodiscard]] static WorkspaceConfigurationParseResult Parse(
		std::string_view utf8,
		const platform::uri::Uri& workspaceConfigUri);

	//! Performs the lexical resource normalization shared by workspace parsing
	//! and workspace-file rewriting.  It deliberately preserves a file URI's
	//! authority syntax: `file://localhost/...` and `file:///...` compare as the
	//! same resource, but VS Code's storage rewrite uses that authority when it
	//! decides whether a relative path is possible.
	[[nodiscard]] static std::optional<platform::uri::Uri> NormalizeFolderUri(
		const platform::uri::Uri& uri);
	//! Resolves one workspace `path` entry against the workspace resource using
	//! the same lexical rules as Parse().  Filesystem probing is never performed.
	[[nodiscard]] static std::optional<platform::uri::Uri> ResolveFolderPath(
		const platform::uri::Uri& workspaceConfigUri, std::wstring_view path);
};

} // namespace workbench::workspace
