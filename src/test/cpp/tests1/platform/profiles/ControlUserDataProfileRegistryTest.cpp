/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/profiles/ControlUserDataProfileRegistry.h"
#include "platform/storage/CInMemoryStorageService.h"

namespace platform::profiles {
namespace {

UserDataProfileCreateRequest Profile(UserDataProfileId id, std::wstring displayName)
{
	return { std::move(id), std::move(displayName), UserDataProfileKind::Normal, {}, { .settings = true } };
}

WorkspaceUri Workspace(std::wstring_view text)
{
	auto uri = ::platform::uri::Uri::Parse(text);
	if (!uri) throw std::logic_error("test URI is invalid");
	return std::move(*uri.value);
}

ControlUserDataProfileRegistryMutation Mutation(std::string operationId, std::uint64_t revision)
{
	return { std::move(operationId), revision };
}

} // namespace

TEST(ControlUserDataProfileRegistry, ControlLifecyclePersistsNamedProfilesAndFencesTransientProfiles)
{
	auto storage = std::make_shared<::platform::storage::CInMemoryStorageService>();
	ControlUserDataProfileRegistry control(storage);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Started, control.Start().status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Applied,
		control.CreateNamed(Profile(L"opaque-named", L"Named"), Mutation("create-named", 0)).status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Applied,
		control.AssociateWorkspace(L"opaque-named", Workspace(L"file:///C:/profile-project"), Mutation("associate-workspace", 1)).status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::AppliedTransient,
		control.CreateTransient(Profile(L"temporary", L"Temporary"), Mutation("create-transient", 2)).status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Resolved, control.SwitchTo(L"temporary").status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Applied,
		control.Rename(L"opaque-named", L"Renamed", Mutation("rename-named", 2)).status);
	const auto portable = control.ExportPortableDocument();
	EXPECT_FALSE(portable.empty());
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Stopped,
		control.Stop(Mutation("shutdown-profile-registry", 3)).status);

	ControlUserDataProfileRegistry restarted(storage);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Started, restarted.Start().status);
	const auto resolved = restarted.Resolve({ std::nullopt, Workspace(L"FILE://localhost/c:/profile-project"), std::nullopt });
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Resolved, resolved.status);
	EXPECT_EQ(L"opaque-named", resolved.resolved->profile->profileId);
	EXPECT_EQ(L"Renamed", resolved.resolved->profile->displayName);
	EXPECT_EQ(ControlUserDataProfileRegistryStatus::ProfileNotFound, restarted.SwitchTo(L"temporary").status);
	EXPECT_EQ(ControlUserDataProfileRegistryStatus::Stopped,
		restarted.Stop(Mutation("shutdown-profile-registry-restarted", 3)).status);
}

TEST(ControlUserDataProfileRegistry, ConflictRollsBackMutationAndReplayRemainsObservable)
{
	auto storage = std::make_shared<::platform::storage::CInMemoryStorageService>();
	ControlUserDataProfileRegistry control(storage);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Started, control.Start().status);
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::Applied,
		control.CreateNamed(Profile(L"first", L"First"), Mutation("create-first", 0)).status);
	const auto conflict = control.CreateNamed(Profile(L"second", L"Second"), Mutation("create-second", 0));
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::PersistConflict, conflict.status);
	ASSERT_TRUE(conflict.durable.has_value());
	EXPECT_FALSE(conflict.durable->replayed);
	EXPECT_EQ(ControlUserDataProfileRegistryStatus::ProfileNotFound, control.SwitchTo(L"second").status);
	const auto replay = control.CreateNamed(Profile(L"second", L"Second"), Mutation("create-second", 0));
	ASSERT_EQ(ControlUserDataProfileRegistryStatus::PersistConflict, replay.status);
	ASSERT_TRUE(replay.durable.has_value());
	EXPECT_TRUE(replay.durable->replayed);
	EXPECT_EQ(ControlUserDataProfileRegistryStatus::ProfileNotFound, control.SwitchTo(L"second").status);
}

} // namespace platform::profiles
