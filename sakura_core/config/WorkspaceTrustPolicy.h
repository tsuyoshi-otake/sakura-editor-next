/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include "config/WorkspaceContextTypes.h"

#include <sakura/uri/UriIdentity.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace config {

/*!
	@brief One decision from the Trusted Folders and Workspaces list.

	@c includesDescendants is VS Code's "trust the parent folder" option, which
	extends one decision to every resource beneath that folder.
 */
struct WorkspaceTrustEntry final {
	platform::uri::Uri uri;
	bool includesDescendants = false;
};

//! The @c security.workspace.trust.* values this policy consumes.
struct WorkspaceTrustSettings final {
	bool enabled = true;
	bool emptyWindow = true;
};

//! Everything the policy is allowed to look at. It reads nothing else.
struct WorkspaceTrustResolveRequest final {
	EWorkspaceKind kind = EWorkspaceKind::Empty;
	std::optional<platform::uri::Uri> workspaceConfigUri;
	std::vector<platform::uri::Uri> folderUris;
	WorkspaceTrustSettings settings;
	std::vector<WorkspaceTrustEntry> trustedEntries;
};

//! Why the policy answered the way it did. Diagnostic only; never a second answer.
enum class EWorkspaceTrustReason : std::uint8_t {
	FeatureDisabled,
	EmptyWindowTrustedByDefault,
	EmptyWindowNotTrustedByDefault,
	WorkspaceFileTrusted,
	AllRootsTrusted,
	RootNotTrusted,
	NoResolvableRoot,
};

struct WorkspaceTrustResolution final {
	EWorkspaceTrustState state = EWorkspaceTrustState::Unknown;
	EWorkspaceTrustReason reason = EWorkspaceTrustReason::NoResolvableRoot;
};

/*!
	@brief Does @p entry cover @p resource?

	Exact URI identity always covers. An entry that includes descendants also covers
	any resource strictly beneath it, decided on path-segment boundaries so that
	@c /c:/codes/foo never covers @c /c:/codes/foobar.
 */
[[nodiscard]] bool WorkspaceTrustEntryCovers(const WorkspaceTrustEntry& entry, const platform::uri::Uri& resource);

/*!
	@brief The parent folder of @p resource, or nothing when it has none.

	This is what backs VS Code's "Trust the authors of all files in the parent
	folder" choice. Pure and path-only: it rebuilds @p resource at its own last
	path-segment boundary and performs no filesystem lookup, so it cannot follow a
	link or resolve a relative segment.

	A resource whose parent would be the scheme root -- a drive root, a UNC share
	root, a path that is only a separator -- returns nothing. "Trust the parent"
	must never silently widen into a whole volume or host.
 */
[[nodiscard]] std::optional<platform::uri::Uri> WorkspaceTrustParentFolder(const platform::uri::Uri& resource);

/*!
	@brief Resolve workspace trust from state alone.

	Pure: no I/O, no clock, no global state, no window. Trust is never inferred from a
	path shape, a file extension, or a successful configuration load.

	@c Untrusted is never produced here. It denotes an explicit user denial, which only
	an interactive grant/deny surface can establish.
 */
[[nodiscard]] WorkspaceTrustResolution ResolveWorkspaceTrust(const WorkspaceTrustResolveRequest& request);

} // namespace config
