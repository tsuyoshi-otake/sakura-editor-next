/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/WorkspaceTrustPolicy.h"

#include <utility>
#include <vector>

namespace {

using config::EWorkspaceKind;
using config::EWorkspaceTrustReason;
using config::EWorkspaceTrustState;
using config::ResolveWorkspaceTrust;
using config::WorkspaceTrustEntry;
using config::WorkspaceTrustEntryCovers;
using config::WorkspaceTrustResolveRequest;
using platform::uri::Uri;

Uri ParseUri(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

WorkspaceTrustEntry Entry(const wchar_t* text, bool includesDescendants)
{
	return { ParseUri(text), includesDescendants };
}

//! A folder workspace with one root and no trusted entries.
WorkspaceTrustResolveRequest FolderRequest(const wchar_t* folder)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Folder;
	request.folderUris.push_back(ParseUri(folder));
	return request;
}

} // namespace

// ---------------------------------------------------------------------------
// WorkspaceTrustEntryCovers
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustPolicy, EntryCoversItselfWithoutDescendants)
{
	const auto entry = Entry(L"file:///c:/codes/app", false);
	EXPECT_TRUE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/app")));
}

TEST(WorkspaceTrustPolicy, EntryWithoutDescendantsDoesNotCoverChild)
{
	const auto entry = Entry(L"file:///c:/codes/app", false);
	EXPECT_FALSE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/app/src")));
}

TEST(WorkspaceTrustPolicy, EntryWithDescendantsCoversNestedChild)
{
	const auto entry = Entry(L"file:///c:/codes", true);
	EXPECT_TRUE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/app/src")));
}

TEST(WorkspaceTrustPolicy, EntryWithTrailingSeparatorCoversNestedChild)
{
	const auto entry = Entry(L"file:///c:/codes/", true);
	EXPECT_TRUE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/app")));
}

//! The containment rule is a segment-boundary rule, not a string-prefix rule.
TEST(WorkspaceTrustPolicy, EntryDoesNotCoverSiblingSharingAPathPrefix)
{
	const auto entry = Entry(L"file:///c:/codes/app", true);
	EXPECT_FALSE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/application")));
}

TEST(WorkspaceTrustPolicy, EntryDoesNotCoverAncestor)
{
	const auto entry = Entry(L"file:///c:/codes/app", true);
	EXPECT_FALSE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes")));
}

//! Windows file paths compare case-insensitively, matching the identity service.
TEST(WorkspaceTrustPolicy, FileEntryCoversDescendantIgnoringCase)
{
	const auto entry = Entry(L"file:///C:/Codes", true);
	EXPECT_TRUE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file:///c:/codes/app")));
}

TEST(WorkspaceTrustPolicy, EntryDoesNotCoverDifferentScheme)
{
	const auto entry = Entry(L"file:///c:/codes", true);
	EXPECT_FALSE(WorkspaceTrustEntryCovers(entry, ParseUri(L"vscode-remote:///c:/codes/app")));
}

TEST(WorkspaceTrustPolicy, EntryDoesNotCoverDifferentAuthority)
{
	const auto entry = Entry(L"file://server-a/share", true);
	EXPECT_FALSE(WorkspaceTrustEntryCovers(entry, ParseUri(L"file://server-b/share/app")));
}

// ---------------------------------------------------------------------------
// ResolveWorkspaceTrust
// ---------------------------------------------------------------------------

//! Disabling the feature trusts everything, and outranks every other rule.
TEST(WorkspaceTrustPolicy, DisabledFeatureTrustsAnUntrustedFolder)
{
	auto request = FolderRequest(L"file:///c:/codes/app");
	request.settings.enabled = false;
	request.settings.emptyWindow = false;

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::FeatureDisabled, resolution.reason);
}

TEST(WorkspaceTrustPolicy, EmptyWindowIsTrustedByDefault)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::EmptyWindowTrustedByDefault, resolution.reason);
}

//! Opting out of the empty-window default withholds trust; it does not deny it.
TEST(WorkspaceTrustPolicy, EmptyWindowOptOutResolvesUnknownNotUntrusted)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = false;

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::EmptyWindowNotTrustedByDefault, resolution.reason);
}

TEST(WorkspaceTrustPolicy, UngrantedFolderResolvesUnknown)
{
	const auto resolution = ResolveWorkspaceTrust(FolderRequest(L"file:///c:/codes/app"));
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::RootNotTrusted, resolution.reason);
}

//! The empty-window default never leaks into a folder window.
TEST(WorkspaceTrustPolicy, EmptyWindowDefaultDoesNotTrustAFolder)
{
	auto request = FolderRequest(L"file:///c:/codes/app");
	request.settings.emptyWindow = true;

	EXPECT_EQ(EWorkspaceTrustState::Unknown, ResolveWorkspaceTrust(request).state);
}

TEST(WorkspaceTrustPolicy, ExactlyTrustedFolderResolvesTrusted)
{
	auto request = FolderRequest(L"file:///c:/codes/app");
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", false));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::AllRootsTrusted, resolution.reason);
}

TEST(WorkspaceTrustPolicy, TrustedParentFolderResolvesTrusted)
{
	auto request = FolderRequest(L"file:///c:/codes/app/src");
	request.trustedEntries.push_back(Entry(L"file:///c:/codes", true));

	EXPECT_EQ(EWorkspaceTrustState::Trusted, ResolveWorkspaceTrust(request).state);
}

//! Trust is not the union of the roots: one untrusted root leaves the window untrusted.
TEST(WorkspaceTrustPolicy, OneUntrustedRootLeavesMultiRootWorkspaceUnknown)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Workspace;
	request.workspaceConfigUri = ParseUri(L"file:///c:/codes/team.code-workspace");
	request.folderUris.push_back(ParseUri(L"file:///c:/codes/app"));
	request.folderUris.push_back(ParseUri(L"file:///d:/vendor/lib"));
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", false));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::RootNotTrusted, resolution.reason);
}

TEST(WorkspaceTrustPolicy, EveryTrustedRootResolvesTrusted)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Workspace;
	request.workspaceConfigUri = ParseUri(L"file:///c:/codes/team.code-workspace");
	request.folderUris.push_back(ParseUri(L"file:///c:/codes/app"));
	request.folderUris.push_back(ParseUri(L"file:///d:/vendor/lib"));
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", false));
	request.trustedEntries.push_back(Entry(L"file:///d:/vendor", true));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::AllRootsTrusted, resolution.reason);
}

//! Trusting the workspace file covers the whole workspace, wherever its folders live.
TEST(WorkspaceTrustPolicy, TrustedWorkspaceFileCoversUntrustedFolders)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Workspace;
	request.workspaceConfigUri = ParseUri(L"file:///c:/codes/team.code-workspace");
	request.folderUris.push_back(ParseUri(L"file:///d:/vendor/lib"));
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/team.code-workspace", false));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::WorkspaceFileTrusted, resolution.reason);
}

//! A workspace file is a resolvable root even with no folders, so it is not "no root".
TEST(WorkspaceTrustPolicy, UntrustedWorkspaceFileWithoutFoldersReportsRootNotTrusted)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Workspace;
	request.workspaceConfigUri = ParseUri(L"file:///c:/codes/team.code-workspace");

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::RootNotTrusted, resolution.reason);
}

TEST(WorkspaceTrustPolicy, FolderKindWithoutAnyRootReportsNoResolvableRoot)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Folder;

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::NoResolvableRoot, resolution.reason);
}

//! Untrusted denotes an explicit denial, which no amount of state alone can establish.
TEST(WorkspaceTrustPolicy, ResolutionNeverProducesUntrusted)
{
	const WorkspaceTrustResolveRequest requests[] = {
		[] { WorkspaceTrustResolveRequest r; r.settings.emptyWindow = false; return r; }(),
		FolderRequest(L"file:///c:/codes/app"),
		[] {
			WorkspaceTrustResolveRequest r;
			r.kind = EWorkspaceKind::Workspace;
			r.workspaceConfigUri = ParseUri(L"file:///c:/codes/team.code-workspace");
			return r;
		}(),
	};
	for (const auto& request : requests) {
		EXPECT_NE(EWorkspaceTrustState::Untrusted, ResolveWorkspaceTrust(request).state);
	}
}
