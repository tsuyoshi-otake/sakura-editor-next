/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/profiles/UserDataProfileBootstrap.h"

#include "platform/profiles/UserDataProfileRegistryCodec.h"

#include <type_traits>

namespace platform::profiles {
namespace {

constexpr char kAuthorityId[] = "0123456789abcdef0123456789abcdef";

UserDataProfileCreateRequest Profile(UserDataProfileId id, std::wstring displayName,
	UserDataProfileKind kind = UserDataProfileKind::Normal)
{
	return { std::move(id), std::move(displayName), kind, {}, {} };
}

WorkspaceUri Workspace(std::wstring_view value)
{
	auto parsed = ::platform::uri::Uri::Parse(value);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

UserDataProfileBootstrapRequest Request()
{
	return { { kAuthorityId, 17 }, L"C:\\Sakura\\ControlProfile", {}, UserDataProfileResourceRootMode::ProfileIdNamespace };
}

template<class T>
constexpr bool HasSecretAccessor = requires(const T& value) { value.Secrets(); };

static_assert(!HasSecretAccessor<UserDataProfileResourceUris>);

} // namespace

TEST(UserDataProfileBootstrap, SeparatesPinnedControlAuthorityFromSelectedUserProfile)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"work-profile-01", L"Work profile")).status);
	auto request = Request();
	request.selection.explicitProfileId = L"work-profile-01";

	const auto result = ResolveUserDataProfileBootstrap(request, registry);

	ASSERT_TRUE(result.Resolved());
	EXPECT_EQ(kAuthorityId, result.snapshot->ControlAuthority().authorityId);
	EXPECT_EQ(17u, result.snapshot->ControlAuthority().generation);
	EXPECT_EQ(L"work-profile-01", result.snapshot->SelectedProfileId());
	EXPECT_NE(result.snapshot->ControlAuthority().authorityId, "work-profile-01");
	EXPECT_EQ(UserDataProfileResolveSource::ExplicitProfile, result.snapshot->SelectionSource());
}

TEST(UserDataProfileBootstrap, AppliesRegistrySelectionPrecedenceAgainstImmutableSnapshot)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"explicit-profile", L"Explicit")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"workspace-profile", L"Workspace")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"empty-profile", L"Empty")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.AssociateWorkspace(L"workspace-profile", Workspace(L"file:///C:/work/project")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied,
		registry.AssociateEmptyWindow(L"empty-profile", L"empty-window:stable-token-0001").status);

	auto request = Request();
	request.selection = { L"explicit-profile", Workspace(L"file:///c:/WORK/project"), L"empty-window:stable-token-0001" };
	const auto explicitResult = ResolveUserDataProfileBootstrap(request, registry.Snapshot(true));
	ASSERT_TRUE(explicitResult.Resolved());
	EXPECT_EQ(L"explicit-profile", explicitResult.snapshot->SelectedProfileId());
	EXPECT_EQ(UserDataProfileResolveSource::ExplicitProfile, explicitResult.snapshot->SelectionSource());

	request.selection.explicitProfileId.reset();
	const auto workspaceResult = ResolveUserDataProfileBootstrap(request, registry.Snapshot(true));
	ASSERT_TRUE(workspaceResult.Resolved());
	EXPECT_EQ(L"workspace-profile", workspaceResult.snapshot->SelectedProfileId());
	EXPECT_EQ(UserDataProfileResolveSource::WorkspaceAssociation, workspaceResult.snapshot->SelectionSource());

	request.selection.workspaceUri.reset();
	const auto emptyResult = ResolveUserDataProfileBootstrap(request, registry.Snapshot(true));
	ASSERT_TRUE(emptyResult.Resolved());
	EXPECT_EQ(L"empty-profile", emptyResult.snapshot->SelectedProfileId());
	EXPECT_EQ(UserDataProfileResolveSource::EmptyWindowAssociation, emptyResult.snapshot->SelectionSource());
}

TEST(UserDataProfileBootstrap, ResourceRootsAreRenameStableAndIsolatedByOpaqueId)
{
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"opaque-alpha-01", L"Before rename")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"opaque-beta-02", L"Other profile")).status);
	auto request = Request();
	request.selection.explicitProfileId = L"opaque-alpha-01";
	const auto before = ResolveUserDataProfileBootstrap(request, registry);
	ASSERT_TRUE(before.Resolved());
	const auto beforeHome = before.snapshot->Resources().ProfileHome().ToString();
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Rename(L"opaque-alpha-01", L"After rename").status);
	const auto after = ResolveUserDataProfileBootstrap(request, registry);
	ASSERT_TRUE(after.Resolved());
	EXPECT_EQ(beforeHome, after.snapshot->Resources().ProfileHome().ToString());
	EXPECT_EQ(L"file:///C:/Sakura/ControlProfile/user-data-profiles/opaque-alpha-01", beforeHome);

	request.selection.explicitProfileId = L"opaque-beta-02";
	const auto other = ResolveUserDataProfileBootstrap(request, registry);
	ASSERT_TRUE(other.Resolved());
	EXPECT_NE(after.snapshot->Resources().Settings().ToString(), other.snapshot->Resources().Settings().ToString());
}

TEST(UserDataProfileBootstrap, RejectsInvalidSelectorsAndProcessDerivedEmptyWindowMarkers)
{
	UserDataProfileRegistry registry;
	auto request = Request();
	request.selection.explicitProfileId = L"..\\escape";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidProfileId, ResolveUserDataProfileBootstrap(request, registry).status);

	request.selection.explicitProfileId = L"missing-profile";
	EXPECT_EQ(UserDataProfileBootstrapStatus::ProfileNotFound, ResolveUserDataProfileBootstrap(request, registry).status);

	request.selection.explicitProfileId.reset();
	request.selection.emptyWindowId = L"editor-process:1729";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidEmptyWindowIdentity, ResolveUserDataProfileBootstrap(request, registry).status);
	EXPECT_FALSE(IsStableEmptyWindowIdentity(L"editor-process:1729"));
	EXPECT_TRUE(IsStableEmptyWindowIdentity(L"empty-window:durable-token-1234"));
}

TEST(UserDataProfileBootstrap, HasNoSecretResourceSurfaceAndFailsClosedForMalformedAuthorityRootOrSnapshot)
{
	UserDataProfileRegistry registry;
	auto request = Request();
	request.controlAuthority.authorityId = "not-an-authority";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidControlAuthorityId, ResolveUserDataProfileBootstrap(request, registry).status);

	request = Request();
	request.controlAuthority.generation = 0;
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidControlAuthorityGeneration, ResolveUserDataProfileBootstrap(request, registry).status);

	request = Request();
	request.controlProfileRoot = L"C:\\Sakura\\..\\escape";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidControlProfileRoot, ResolveUserDataProfileBootstrap(request, registry).status);

	auto malformed = registry.Snapshot(true);
	malformed.profiles.front().profileId = L"..";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidRegistrySnapshot, ResolveUserDataProfileBootstrap(Request(), malformed).status);
}

TEST(UserDataProfileBootstrap, AllowsOnlyExplicitLegacyDefaultRootCompatibilityMode)
{
	UserDataProfileRegistry registry;
	auto request = Request();
	request.resourceRootMode = UserDataProfileResourceRootMode::LegacyControlRootForDefault;
	const auto defaultResult = ResolveUserDataProfileBootstrap(request, registry);
	ASSERT_TRUE(defaultResult.Resolved());
	EXPECT_EQ(L"file:///C:/Sakura/ControlProfile", defaultResult.snapshot->Resources().ProfileHome().ToString());

	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"not-default", L"Not default")).status);
	request.selection.explicitProfileId = L"not-default";
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidResourceRootMode, ResolveUserDataProfileBootstrap(request, registry).status);
}

TEST(UserDataProfileBootstrap, DecodesOnlyAValidBoundedRegistryDocument)
{
	UserDataProfileRegistry registry;
	const auto document = EncodeUserDataProfileRegistryDocument(registry.Snapshot(true));

	const auto resolved = ResolveUserDataProfileBootstrap(Request(), std::string_view(document));
	ASSERT_TRUE(resolved.Resolved());
	EXPECT_EQ(registry.DefaultProfileId(), resolved.snapshot->SelectedProfileId());

	const auto malformed = ResolveUserDataProfileBootstrap(Request(), std::string_view("not-a-profile-document"));
	EXPECT_EQ(UserDataProfileBootstrapStatus::InvalidRegistryDocument, malformed.status);
	EXPECT_FALSE(malformed.snapshot);
}

} // namespace platform::profiles
