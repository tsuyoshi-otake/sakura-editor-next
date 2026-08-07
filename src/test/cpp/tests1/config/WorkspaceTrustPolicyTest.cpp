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
using config::WorkspaceTrustParentFolder;
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

//! Upstream's `_canonicalStartupFiles` short-circuit: a file opened on the command
//! line into an otherwise-empty window must not silently ride the empty-window
//! default when the file itself is not covered by the trusted list.
TEST(WorkspaceTrustPolicy, EmptyWindowWithUncoveredStartupFileResolvesUnknown)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = true;
	request.startupFileUris.push_back(ParseUri(L"file:///c:/codes/notes.txt"));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::StartupFileNotTrusted, resolution.reason);
}

//! The rule is decided per startup file: every one of them must already be
//! covered, not merely one.
TEST(WorkspaceTrustPolicy, EmptyWindowWithOneUncoveredStartupFileAmongSeveralResolvesUnknown)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = true;
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", true));
	request.startupFileUris.push_back(ParseUri(L"file:///c:/codes/app/readme.txt"));
	request.startupFileUris.push_back(ParseUri(L"file:///d:/other/notes.txt"));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Unknown, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::StartupFileNotTrusted, resolution.reason);
}

TEST(WorkspaceTrustPolicy, EmptyWindowWithEveryStartupFileCoveredResolvesTrusted)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = false;
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", true));
	request.startupFileUris.push_back(ParseUri(L"file:///c:/codes/app/readme.txt"));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::StartupFilesTrusted, resolution.reason);
}

//! The startup-files rule runs before -- and can override -- the empty-window
//! default in either direction: it must not be skipped just because
//! `emptyWindow` would otherwise have trusted the window anyway.
TEST(WorkspaceTrustPolicy, StartupFilesRuleOverridesEmptyWindowDefaultTrusted)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = true;
	request.startupFileUris.push_back(ParseUri(L"file:///c:/codes/notes.txt"));

	EXPECT_EQ(EWorkspaceTrustState::Unknown, ResolveWorkspaceTrust(request).state);
}

//! With no startup files at all, the empty-window default is unaffected.
TEST(WorkspaceTrustPolicy, EmptyWindowWithNoStartupFilesFallsBackToDefault)
{
	WorkspaceTrustResolveRequest request;
	request.kind = EWorkspaceKind::Empty;
	request.settings.emptyWindow = true;

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::EmptyWindowTrustedByDefault, resolution.reason);
}

//! Startup files are meaningless for a non-Empty kind: a folder/workspace
//! window's trust is decided by its roots, and the field must be ignored there.
TEST(WorkspaceTrustPolicy, StartupFilesAreIgnoredForAFolderWindow)
{
	auto request = FolderRequest(L"file:///c:/codes/app");
	request.trustedEntries.push_back(Entry(L"file:///c:/codes/app", false));
	request.startupFileUris.push_back(ParseUri(L"file:///d:/other/notes.txt"));

	const auto resolution = ResolveWorkspaceTrust(request);
	EXPECT_EQ(EWorkspaceTrustState::Trusted, resolution.state);
	EXPECT_EQ(EWorkspaceTrustReason::AllRootsTrusted, resolution.reason);
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

// ---------------------------------------------------------------------------
// WorkspaceTrustParentFolder
// ---------------------------------------------------------------------------

TEST(WorkspaceTrustPolicy, ParentFolderOfNestedResourceIsItsImmediateAncestor)
{
	const auto parent = WorkspaceTrustParentFolder(ParseUri(L"file:///c:/codes/app/sub"));
	ASSERT_TRUE(parent);
	EXPECT_EQ(L"file:///c:/codes/app", parent->ToString());
}

//! A stored trailing separator must not make the folder its own parent: the
//! implementation strips it before finding the last path-segment boundary, so
//! both spellings of the same folder resolve to the same, next-level-up parent.
TEST(WorkspaceTrustPolicy, ParentFolderIsSameRegardlessOfTrailingSeparatorSpelling)
{
	const auto withSeparator = WorkspaceTrustParentFolder(ParseUri(L"file:///c:/codes/app/"));
	const auto withoutSeparator = WorkspaceTrustParentFolder(ParseUri(L"file:///c:/codes/app"));
	ASSERT_TRUE(withSeparator);
	ASSERT_TRUE(withoutSeparator);
	EXPECT_EQ(L"file:///c:/codes", withSeparator->ToString());
	EXPECT_EQ(withoutSeparator->ToString(), withSeparator->ToString());
}

TEST(WorkspaceTrustPolicy, ParentFolderOfADriveRootIsNullopt)
{
	EXPECT_FALSE(WorkspaceTrustParentFolder(ParseUri(L"file:///c:/")));
}

TEST(WorkspaceTrustPolicy, ParentFolderOfAPathThatIsOnlyASeparatorIsNullopt)
{
	EXPECT_FALSE(WorkspaceTrustParentFolder(ParseUri(L"file:///")));
}

TEST(WorkspaceTrustPolicy, ParentFolderOfASingleTopLevelSegmentIsNullopt)
{
	EXPECT_FALSE(WorkspaceTrustParentFolder(ParseUri(L"file:///c:")));
}

//! The authority is carried over unchanged, so a UNC-style resource's parent
//! stays on the same share.
TEST(WorkspaceTrustPolicy, ParentFolderCarriesUncAuthorityUnchanged)
{
	const auto parent = WorkspaceTrustParentFolder(ParseUri(L"file://server-a/share/sub"));
	ASSERT_TRUE(parent);
	EXPECT_EQ(L"file://server-a/share", parent->ToString());
}

//! A share root has no parent within the share, and that must not be papered
//! over by widening the answer to the bare host.
TEST(WorkspaceTrustPolicy, ParentFolderOfAShareRootIsNulloptNotTheHost)
{
	EXPECT_FALSE(WorkspaceTrustParentFolder(ParseUri(L"file://server-a/share")));
}

//! WithPath rebuilds through FromComponents, so query and fragment ride along
//! unchanged while only the path is truncated.
TEST(WorkspaceTrustPolicy, ParentFolderCarriesQueryAndFragmentUnchanged)
{
	const auto parent = WorkspaceTrustParentFolder(ParseUri(L"file:///c:/codes/app/sub?ref=1#section"));
	ASSERT_TRUE(parent);
	EXPECT_EQ(L"file:///c:/codes/app?ref=1#section", parent->ToString());
}

//! This is what backs "Trust the authors of all files in the parent folder":
//! the computed parent, stored as a descendants-including entry, must cover
//! the very resource it was derived from.
TEST(WorkspaceTrustPolicy, ParentFolderAsATrustedEntryCoversItsOriginalResource)
{
	const auto resource = ParseUri(L"file:///c:/codes/app/sub");
	const auto parent = WorkspaceTrustParentFolder(resource);
	ASSERT_TRUE(parent);

	const WorkspaceTrustEntry entry{ *parent, true };
	EXPECT_TRUE(WorkspaceTrustEntryCovers(entry, resource));
}
