/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/profiles/UserDataProfileRegistry.h"

#include <stdexcept>

namespace platform::profiles {
namespace {

UserDataProfileCreateRequest Profile(UserDataProfileId id, std::wstring name,
	UserDataProfileKind kind = UserDataProfileKind::Normal)
{
	return { std::move(id), std::move(name), kind, {}, {} };
}

WorkspaceUri Workspace(std::wstring_view value)
{
	auto parsed = ::platform::uri::Uri::Parse(value);
	if (!parsed) {
		throw std::logic_error("invalid workspace URI in test");
	}
	return std::move(*parsed.value);
}

} // namespace

TEST(UserDataProfileRegistry, DefaultProfileCannotBeRenamedOrRemoved)
{
	UserDataProfileRegistry registry;
	const auto defaultId = registry.DefaultProfileId();

	EXPECT_EQ(UserDataProfileOperationStatus::DefaultProfileProtected, registry.Rename(defaultId, L"Changed").status);
	EXPECT_EQ(UserDataProfileOperationStatus::DefaultProfileProtected, registry.Remove(defaultId).status);
	EXPECT_EQ(0u, registry.Revision());
	ASSERT_TRUE(registry.FindProfile(defaultId).has_value());
	EXPECT_EQ(L"Default", registry.FindProfile(defaultId)->displayName);
}

TEST(UserDataProfileRegistry, RejectsLegacyAliasCollisions)
{
	UserDataProfileRegistry registry;
	auto first = Profile(L"one", L"One");
	first.legacyAliases = { L"legacy-one" };
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(first).status);

	auto second = Profile(L"two", L"Two");
	second.legacyAliases = { L"legacy-one" };
	EXPECT_EQ(UserDataProfileOperationStatus::DuplicateLegacyAlias, registry.Create(second).status);
	EXPECT_EQ(L"one", *registry.FindProfileByLegacyAlias(L"legacy-one"));
}

TEST(UserDataProfileRegistry, RenameKeepsStableProfileId)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"stable-id", L"Before")).status);
	const auto revisionBeforeRename = registry.Revision();

	const auto renamed = registry.Rename(L"stable-id", L"After");

	EXPECT_EQ(UserDataProfileOperationStatus::Applied, renamed.status);
	EXPECT_EQ(revisionBeforeRename + 1, renamed.revision);
	ASSERT_TRUE(registry.FindProfile(L"stable-id").has_value());
	EXPECT_EQ(L"After", registry.FindProfile(L"stable-id")->displayName);
	EXPECT_FALSE(registry.FindProfile(L"After").has_value());
}

TEST(UserDataProfileRegistry, LegacyMigrationIsIdempotent)
{
	UserDataProfileRegistry registry;
	LegacyUserDataProfileMigrationRequest migration{
		.profile = Profile(L"legacy-id", L"Migrated"),
		.workspaceUris = { Workspace(L"file:///workspace") },
		.emptyWindowIds = { L"empty-window-1" },
	};
	migration.profile.legacyAliases = { L"-PROF:Legacy" };

	const auto first = registry.ApplyLegacyMigration(migration);
	const auto second = registry.ApplyLegacyMigration(migration);

	EXPECT_EQ(UserDataProfileOperationStatus::Applied, first.status);
	EXPECT_EQ(UserDataProfileOperationStatus::NoChange, second.status);
	EXPECT_EQ(first.revision, second.revision);
	EXPECT_EQ(L"legacy-id", *registry.FindProfileByLegacyAlias(L"-PROF:Legacy"));
	EXPECT_EQ(L"legacy-id", *registry.FindProfileForWorkspace(Workspace(L"file:///workspace")));
	EXPECT_EQ(L"legacy-id", *registry.FindProfileForEmptyWindow(L"empty-window-1"));
}

TEST(UserDataProfileRegistry, LastTransientAssociationRemovalCleansUpProfile)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.Create(Profile(L"temporary", L"Temporary", UserDataProfileKind::Transient)).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.AssociateEmptyWindow(L"temporary", L"empty-window").status);

	const auto removed = registry.DisassociateEmptyWindow(L"empty-window");

	EXPECT_EQ(UserDataProfileOperationStatus::Applied, removed.status);
	EXPECT_EQ(1u, removed.removedTransientProfiles);
	EXPECT_FALSE(registry.FindProfile(L"temporary").has_value());
}

TEST(UserDataProfileRegistry, ResolveUsesExplicitThenWorkspaceThenEmptyWindowThenDefault)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"explicit", L"Explicit")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"workspace", L"Workspace")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"empty", L"Empty")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.AssociateWorkspace(L"workspace", Workspace(L"file:///workspace")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.AssociateEmptyWindow(L"empty", L"empty-window").status);

	const auto explicitProfile = registry.Resolve({
		.explicitProfileId = L"explicit", .workspaceUri = Workspace(L"file:///workspace"), .emptyWindowId = L"empty-window" });
	const auto workspaceProfile = registry.Resolve({
		.workspaceUri = Workspace(L"file:///workspace"), .emptyWindowId = L"empty-window" });
	const auto emptyWindowProfile = registry.Resolve({ .emptyWindowId = L"empty-window" });
	const auto defaultProfile = registry.Resolve({});

	ASSERT_TRUE(explicitProfile.Resolved());
	EXPECT_EQ(UserDataProfileResolveSource::ExplicitProfile, explicitProfile.source);
	EXPECT_EQ(L"explicit", explicitProfile.profile->profileId);
	ASSERT_TRUE(workspaceProfile.Resolved());
	EXPECT_EQ(UserDataProfileResolveSource::WorkspaceAssociation, workspaceProfile.source);
	EXPECT_EQ(L"workspace", workspaceProfile.profile->profileId);
	ASSERT_TRUE(emptyWindowProfile.Resolved());
	EXPECT_EQ(UserDataProfileResolveSource::EmptyWindowAssociation, emptyWindowProfile.source);
	EXPECT_EQ(L"empty", emptyWindowProfile.profile->profileId);
	ASSERT_TRUE(defaultProfile.Resolved());
	EXPECT_EQ(UserDataProfileResolveSource::DefaultProfile, defaultProfile.source);
	EXPECT_EQ(registry.DefaultProfileId(), defaultProfile.profile->profileId);
}

TEST(UserDataProfileRegistry, WorkspaceAssociationUsesCanonicalUriIdentity)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"workspace", L"Workspace")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.AssociateWorkspace(L"workspace", Workspace(L"file:///C:/Work/Project")).status);

	const auto resolved = registry.FindProfileForWorkspace(Workspace(L"FILE://localhost/c:/work/project"));
	ASSERT_TRUE(resolved.has_value());
	EXPECT_EQ(L"workspace", *resolved);
}

} // namespace platform::profiles
