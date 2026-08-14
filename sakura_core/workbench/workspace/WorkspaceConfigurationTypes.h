/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/serialization/JsoncDocument.h>
#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench::workspace {

//! The well-known files in a VS Code workspace. Their contents are never
//! implicitly configuration settings merely because they are JSON/JSONC.
enum class EWorkspaceFileMember : std::uint8_t {
	Settings,
	Tasks,
	Launch,
};

//! Every document read for these members must retain the whole document. This
//! prevents task/debug data from entering IConfigurationService.
enum class EWorkspaceResourceContentMode : std::uint8_t {
	WholeDocument,
};

struct WorkspaceConfigurationFolder final {
	platform::uri::Uri uri;
	std::wstring displayName;
};

//! A typed value retained from a .code-workspace document. Settings is held
//! separately as an object because it alone can be adapted to configuration.
struct WorkspaceFileMemberDescriptor final {
	EWorkspaceFileMember member = EWorkspaceFileMember::Tasks;
	platform::serialization::JsoncValue value;
};

struct WorkspaceResourceDescriptor final {
	EWorkspaceFileMember member = EWorkspaceFileMember::Settings;
	platform::uri::Uri resource;
	EWorkspaceResourceContentMode contentMode = EWorkspaceResourceContentMode::WholeDocument;
};

enum class EWorkspaceConfigurationDiagnosticCode : std::uint8_t {
	JsoncParseFailed,
	RootMustBeObject,
	FoldersMustBeArray,
	FolderMustBeObject,
	FolderMustSpecifyExactlyOneLocation,
	FolderPathMustBeString,
	FolderUriMustBeString,
	FolderNameMustBeString,
	UnsupportedFolderMember,
	InvalidFolderUri,
	InvalidFolderName,
	SettingsMustBeObject,
	MaximumFoldersExceeded,
	DuplicateFolderUri,
};

//! Diagnostics contain no raw path or URI text so workspace details never leak
//! into logs. Duplicate folders are non-fatal and keep the first descriptor.
struct WorkspaceConfigurationDiagnostic final {
	EWorkspaceConfigurationDiagnosticCode code = EWorkspaceConfigurationDiagnosticCode::JsoncParseFailed;
	std::string message;
};

struct WorkspaceConfigurationDocument final {
	std::vector<WorkspaceConfigurationFolder> folders;
	std::optional<platform::serialization::JsoncValue::Object> settings;
	std::vector<WorkspaceFileMemberDescriptor> fileMembers;
};

//! The runtime-owned, read-only view of one accepted `.code-workspace`
//! document. It deliberately retains settings and task/debug members in
//! different fields: only `document.settings` is eligible for the
//! configuration service.  Folder resource descriptors are metadata; reading
//! one never authorizes interpreting a sibling JSON document as settings.
struct WorkspaceFolderResourceSnapshot final {
	WorkspaceConfigurationFolder folder;
	std::vector<WorkspaceResourceDescriptor> resources;
};

struct WorkspaceConfigurationRuntimeSnapshot final {
	std::uint64_t revision = 0;
	std::optional<platform::uri::Uri> resource;
	std::optional<WorkspaceConfigurationDocument> document;
	std::vector<WorkspaceFolderResourceSnapshot> folderResources;
};

struct WorkspaceConfigurationParseResult final {
	std::optional<WorkspaceConfigurationDocument> document;
	std::vector<WorkspaceConfigurationDiagnostic> diagnostics;

	[[nodiscard]] bool Succeeded() const noexcept { return document.has_value(); }
};

} // namespace workbench::workspace
