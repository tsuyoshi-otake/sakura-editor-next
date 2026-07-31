/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/profiles/DurableUserDataProfileRegistryService.h"
#include "platform/storage/CInMemoryStorageService.h"

#include <algorithm>
#include <stdexcept>

namespace platform::profiles {
namespace {

UserDataProfileCreateRequest Profile(UserDataProfileId id, std::wstring name, UserDataProfileKind kind = UserDataProfileKind::Normal)
{
	return { std::move(id), std::move(name), kind, {}, { .settings = true, .extensions = true } };
}
WorkspaceUri Workspace(std::wstring_view text)
{
	auto uri = ::platform::uri::Uri::Parse(text);
	if (!uri) throw std::logic_error("test URI is invalid");
	return std::move(*uri.value);
}
::platform::storage::StorageAddress Address()
{
	return { ::platform::storage::EStorageScope::Application, {}, "platform.profiles", "user-data-profile-registry-v1" };
}

} // namespace

TEST(DurableUserDataProfileRegistryService, RoundTripsRenameAssociationsAndInheritanceAcrossRestart)
{
	::platform::storage::CInMemoryStorageService storage;
	UserDataProfileRegistry writer;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, writer.Create(Profile(L"opaque-a", L"Before")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, writer.Rename(L"opaque-a", L"After").status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, writer.AssociateWorkspace(L"opaque-a", Workspace(L"file:///C:/Work/Project")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, writer.AssociateEmptyWindow(L"opaque-a", L"empty-a").status);
	DurableUserDataProfileRegistryService writerService(writer, storage);
	ASSERT_EQ(DurableUserDataProfileRegistryStatus::Saved, writerService.Save("profile-save-a", 0).status);

	UserDataProfileRegistry restarted;
	DurableUserDataProfileRegistryService readerService(restarted, storage);
	ASSERT_EQ(DurableUserDataProfileRegistryStatus::Loaded, readerService.Load().status);
	ASSERT_TRUE(restarted.FindProfile(L"opaque-a").has_value());
	EXPECT_EQ(L"After", restarted.FindProfile(L"opaque-a")->displayName);
	EXPECT_TRUE(restarted.FindProfile(L"opaque-a")->resourceInheritance.settings);
	EXPECT_TRUE(restarted.FindProfile(L"opaque-a")->resourceInheritance.extensions);
	EXPECT_EQ(L"opaque-a", *restarted.FindProfileForWorkspace(Workspace(L"FILE://localhost/c:/work/project")));
	EXPECT_EQ(L"opaque-a", *restarted.FindProfileForEmptyWindow(L"empty-a"));
}

TEST(DurableUserDataProfileRegistryService, MapsCasConflictAndExactOperationReplay)
{
	::platform::storage::CInMemoryStorageService storage;
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"opaque-a", L"A")).status);
	DurableUserDataProfileRegistryService service(registry, storage);
	const auto saved = service.Save("replayable-save", 0);
	ASSERT_EQ(DurableUserDataProfileRegistryStatus::Saved, saved.status);
	EXPECT_EQ(DurableUserDataProfileRegistryStatus::Replayed, service.Save("replayable-save", 0).status);
	EXPECT_EQ(DurableUserDataProfileRegistryStatus::Conflict, service.Save("conflicting-save", 0).status);
}

TEST(DurableUserDataProfileRegistryService, CorruptBytesRemainStoredAndDoNotReplaceLiveRegistry)
{
	::platform::storage::CInMemoryStorageService storage;
	const auto put = storage.Apply({ "put-corrupt-profile-registry", 0,
		{ { Address(), ::platform::storage::EStorageTarget::Machine, std::string("not-a-profile-registry") } } });
	ASSERT_EQ(::platform::storage::EStorageMutationStatus::Succeeded, put.status);
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"live", L"Live")).status);
	DurableUserDataProfileRegistryService service(registry, storage);
	EXPECT_EQ(DurableUserDataProfileRegistryStatus::CorruptPreserved, service.Load().status);
	EXPECT_TRUE(registry.FindProfile(L"live").has_value());
	const auto stored = storage.Snapshot();
	ASSERT_EQ(1u, stored.entries.size());
	EXPECT_EQ("not-a-profile-registry", stored.entries.front().value);
}

TEST(DurableUserDataProfileRegistryService, TransientProfilesAreExcludedFromDurableAndPortableDocuments)
{
	::platform::storage::CInMemoryStorageService storage;
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"normal", L"Normal")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"transient", L"Transient", UserDataProfileKind::Transient)).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.AssociateEmptyWindow(L"transient", L"transient-window").status);
	DurableUserDataProfileRegistryService service(registry, storage);
	const auto portable = service.ExportPortableDocument();
	const auto decoded = DecodeUserDataProfileRegistryDocument(portable);
	ASSERT_TRUE(decoded.Decoded());
	EXPECT_FALSE(std::any_of(decoded.snapshot.profiles.begin(), decoded.snapshot.profiles.end(), [](const auto& profile) {
		return profile.kind == UserDataProfileKind::Transient;
	}));
	ASSERT_EQ(DurableUserDataProfileRegistryStatus::Saved, service.Save("save-with-transient", 0).status);
	UserDataProfileRegistry restarted;
	DurableUserDataProfileRegistryService reader(restarted, storage);
	ASSERT_EQ(DurableUserDataProfileRegistryStatus::Loaded, reader.Load().status);
	EXPECT_FALSE(restarted.FindProfile(L"transient").has_value());
}

TEST(DurableUserDataProfileRegistryService, ImportReportsStableIdAndDisplayNameCollisions)
{
	::platform::storage::CInMemoryStorageService sourceStorage;
	UserDataProfileRegistry source;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, source.Create(Profile(L"portable-id", L"Portable")).status);
	DurableUserDataProfileRegistryService sourceService(source, sourceStorage);
	const auto document = sourceService.ExportPortableDocument();

	::platform::storage::CInMemoryStorageService targetStorage;
	UserDataProfileRegistry target;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, target.Create(Profile(L"portable-id", L"Other")).status);
	DurableUserDataProfileRegistryService targetService(target, targetStorage);
	EXPECT_EQ(DurableUserDataProfileRegistryStatus::DuplicateProfileId,
		targetService.ImportPortableDocument(document, "import-id", 0).status);

	UserDataProfileRegistry displayTarget;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, displayTarget.Create(Profile(L"other-id", L"Portable")).status);
	DurableUserDataProfileRegistryService displayTargetService(displayTarget, targetStorage);
	EXPECT_EQ(DurableUserDataProfileRegistryStatus::DuplicateDisplayName,
		displayTargetService.ImportPortableDocument(document, "import-name", 0).status);
}

TEST(DurableUserDataProfileRegistryService, ResolvesWithPureExplicitWorkspaceEmptyAndDefaultPrecedence)
{
	::platform::storage::CInMemoryStorageService storage;
	UserDataProfileRegistry registry;
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"workspace", L"Workspace")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"empty", L"Empty")).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.Create(Profile(L"explicit", L"Explicit")).status);
	const auto workspace = Workspace(L"file:///C:/Work/Selection");
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.AssociateWorkspace(L"workspace", workspace).status);
	ASSERT_EQ(UserDataProfileOperationStatus::Applied, registry.AssociateEmptyWindow(L"empty", L"empty-window").status);
	DurableUserDataProfileRegistryService service(registry, storage);

	EXPECT_EQ(L"explicit", service.Resolve({ L"explicit", workspace, L"empty-window" }).profile->profileId);
	EXPECT_EQ(L"workspace", service.Resolve({ std::nullopt, workspace, L"empty-window" }).profile->profileId);
	EXPECT_EQ(L"empty", service.Resolve({ std::nullopt, std::nullopt, L"empty-window" }).profile->profileId);
	EXPECT_EQ(L"default", service.Resolve({}).profile->profileId);
}

} // namespace platform::profiles
