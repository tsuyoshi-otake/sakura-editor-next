/*! @file
 * @brief Immutable process-to-workbench bootstrap value.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/WorkspaceContextTypes.h"
#include "platform/profiles/ProfileBootstrapSnapshot.h"
#include "platform/profiles/UserDataProfileBootstrap.h"
#include "platform/uri/UriIdentity.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace workbench {

//! A bootstrap request is resolved once, before a native workbench window is
//! created. It contains no raw path and performs no filesystem work.
struct WorkbenchBootstrapRequest final {
	//! Pinned control-process authority and its legacy control-owned resources.
	//! This is not the selected VS Code-compatible user-data profile.
	platform::profiles::ProfileBootstrapSnapshot controlProfile;
	//! Selected user-data profile and URI-only resources.  This must be pinned to
	//! controlProfile's authority identity and generation.
	platform::profiles::UserDataProfileBootstrapSnapshot userDataProfile;
	std::wstring windowInstanceIdentity;
	std::optional<platform::uri::Uri> explicitFolderUri;
	std::optional<platform::uri::Uri> explicitWorkspaceConfigUri;
	std::vector<config::WorkspaceFolderDescriptor> workspaceFolders;
	std::optional<platform::uri::Uri> initialDocumentUri;
	std::optional<platform::uri::Uri> terminalLaunchDirectoryUri;
};

//! Every bootstrap resolution ends in one typed terminal state.
enum class EWorkbenchBootstrapStatus : std::uint8_t {
	Resolved,
	InvalidProfileSnapshot,
	InvalidUserDataProfileSnapshot,
	InvalidWindowInstanceIdentity,
	InvalidWorkspaceShape,
	InvalidFolderUri,
	InvalidWorkspaceConfigUri,
	InvalidFolderDisplayName,
	DuplicateWorkspaceFolderIdentity,
	InvalidInitialDocumentUri,
	InvalidTerminalLaunchDirectoryUri,
};

struct WorkbenchBootstrapResult;

//! The workbench's stable startup boundary. Workspace identity is determined
//! only by the explicit folder or workspace request. In particular, opening a
//! file never establishes a workspace and never authorizes .vscode discovery.
class WorkbenchBootstrapContext final {
public:
	[[nodiscard]] const platform::profiles::ProfileBootstrapSnapshot& ControlProfile() const noexcept { return m_controlProfile; }
	[[nodiscard]] const platform::profiles::UserDataProfileBootstrapSnapshot& UserDataProfile() const noexcept { return m_userDataProfile; }
	//! Transitional compatibility alias for ControlProfile() only.  New Settings,
	//! extensions, global-state, working-copy, and layout consumers must use
	//! UserDataProfile() explicitly rather than this accessor.
	[[nodiscard]] const platform::profiles::ProfileBootstrapSnapshot& Profile() const noexcept { return ControlProfile(); }
	[[nodiscard]] const std::wstring& WindowInstanceIdentity() const noexcept { return m_windowInstanceIdentity; }
	[[nodiscard]] const config::WorkspaceContextSnapshot& Workspace() const noexcept { return m_workspace; }
	[[nodiscard]] const std::optional<platform::uri::Uri>& InitialDocumentUri() const noexcept { return m_initialDocumentUri; }
	[[nodiscard]] const std::optional<platform::uri::Uri>& TerminalLaunchDirectoryUri() const noexcept { return m_terminalLaunchDirectoryUri; }

private:
	WorkbenchBootstrapContext(
		platform::profiles::ProfileBootstrapSnapshot controlProfile,
		platform::profiles::UserDataProfileBootstrapSnapshot userDataProfile,
		std::wstring windowInstanceIdentity,
		config::WorkspaceContextSnapshot workspace,
		std::optional<platform::uri::Uri> initialDocumentUri,
		std::optional<platform::uri::Uri> terminalLaunchDirectoryUri) noexcept;

	platform::profiles::ProfileBootstrapSnapshot m_controlProfile;
	platform::profiles::UserDataProfileBootstrapSnapshot m_userDataProfile;
	std::wstring m_windowInstanceIdentity;
	config::WorkspaceContextSnapshot m_workspace;
	std::optional<platform::uri::Uri> m_initialDocumentUri;
	std::optional<platform::uri::Uri> m_terminalLaunchDirectoryUri;

	friend WorkbenchBootstrapResult ResolveWorkbenchBootstrapContext(WorkbenchBootstrapRequest request);
};

struct WorkbenchBootstrapResult final {
	EWorkbenchBootstrapStatus status = EWorkbenchBootstrapStatus::InvalidProfileSnapshot;
	std::optional<WorkbenchBootstrapContext> context;

	[[nodiscard]] bool Resolved() const noexcept;
};

//! Resolves a fully typed, immutable bootstrap context. No I/O, mutable
//! service, current directory, or active document participates in resolution.
[[nodiscard]] WorkbenchBootstrapResult ResolveWorkbenchBootstrapContext(WorkbenchBootstrapRequest request);

} // namespace workbench
