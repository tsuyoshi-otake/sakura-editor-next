/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/WorkspaceTrustPolicy.h"

#include <algorithm>
#include <string>

namespace config {

namespace {

[[nodiscard]] bool IsSameUri(const platform::uri::Uri& left, const platform::uri::Uri& right)
{
	return platform::uri::UriIdentityService::MakeComparisonKey(left)
		== platform::uri::UriIdentityService::MakeComparisonKey(right);
}

/*!
	@brief Rebuild @p resource with its path truncated to @p path.

	Every other component is carried over unchanged, so an ancestor candidate differs
	from the resource only in path depth.
 */
[[nodiscard]] std::optional<platform::uri::Uri> WithPath(const platform::uri::Uri& resource, std::wstring path)
{
	auto rebuilt = platform::uri::Uri::FromComponents(
		resource.Scheme(),
		resource.Authority(),
		std::move(path),
		resource.Query(),
		resource.Fragment(),
		resource.HasAuthority());
	return rebuilt.value;
}

} // namespace

bool WorkspaceTrustEntryCovers(const WorkspaceTrustEntry& entry, const platform::uri::Uri& resource)
{
	if (IsSameUri(entry.uri, resource)) {
		return true;
	}
	if (!entry.includesDescendants) {
		return false;
	}

	// Ancestor containment is decided by rebuilding the resource at each of its own path
	// segment boundaries and asking the identity service whether that rebuilt URI is the
	// entry. Every case-folding, authority-aliasing, and escaping rule therefore stays in
	// one place. A direct string comparison against a comparison key would be wrong:
	// MakeComparisonKey emits length-prefixed structured parts, so a prefix test on one
	// carries no ancestor meaning at all.
	const std::wstring& path = resource.Path();
	for (std::size_t index = 0; index < path.size(); ++index) {
		if (path[index] != L'/') {
			continue;
		}
		// A stored ancestor may or may not carry a trailing separator, and both spellings
		// address the same directory.
		const std::wstring candidates[] = { path.substr(0, index), path.substr(0, index + 1) };
		for (const std::wstring& candidatePath : candidates) {
			if (candidatePath.empty()) {
				continue;
			}
			const auto candidate = WithPath(resource, candidatePath);
			if (candidate && IsSameUri(entry.uri, *candidate)) {
				return true;
			}
		}
	}
	return false;
}

WorkspaceTrustResolution ResolveWorkspaceTrust(const WorkspaceTrustResolveRequest& request)
{
	// A disabled feature means everything is trusted, and it outranks every other rule
	// including an explicit empty-window opt-out. This is the documented way to turn
	// Workspace Trust off entirely.
	if (!request.settings.enabled) {
		return { EWorkspaceTrustState::Trusted, EWorkspaceTrustReason::FeatureDisabled };
	}

	if (request.kind == EWorkspaceKind::Empty) {
		if (request.settings.emptyWindow) {
			return { EWorkspaceTrustState::Trusted, EWorkspaceTrustReason::EmptyWindowTrustedByDefault };
		}
		// Unknown, not Untrusted: the user declined nothing here, the default simply
		// withholds trust until it is granted.
		return { EWorkspaceTrustState::Unknown, EWorkspaceTrustReason::EmptyWindowNotTrustedByDefault };
	}

	const auto isCovered = [&request](const platform::uri::Uri& resource) {
		return std::any_of(
			request.trustedEntries.begin(),
			request.trustedEntries.end(),
			[&resource](const WorkspaceTrustEntry& entry) { return WorkspaceTrustEntryCovers(entry, resource); });
	};

	// A .code-workspace file is itself a trustable item, and trusting it covers the whole
	// multi-root workspace regardless of where its folders live.
	if (request.kind == EWorkspaceKind::Workspace
		&& request.workspaceConfigUri
		&& isCovered(*request.workspaceConfigUri)) {
		return { EWorkspaceTrustState::Trusted, EWorkspaceTrustReason::WorkspaceFileTrusted };
	}

	if (request.folderUris.empty()) {
		// A workspace file that exists but is not trusted is a resolvable, untrusted root.
		// A non-empty kind with no root at all is a different fact and gets its own reason.
		return { EWorkspaceTrustState::Unknown,
			request.workspaceConfigUri ? EWorkspaceTrustReason::RootNotTrusted : EWorkspaceTrustReason::NoResolvableRoot };
	}

	// Trust is not the union of the roots. One untrusted root leaves the whole window
	// untrusted, because extension code runs once for all of them.
	for (const auto& folder : request.folderUris) {
		if (!isCovered(folder)) {
			return { EWorkspaceTrustState::Unknown, EWorkspaceTrustReason::RootNotTrusted };
		}
	}
	return { EWorkspaceTrustState::Trusted, EWorkspaceTrustReason::AllRootsTrusted };
}

} // namespace config
