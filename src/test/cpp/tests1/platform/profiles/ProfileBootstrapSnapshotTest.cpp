/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/profiles/ProfileBootstrapSnapshot.h"

namespace platform::profiles {
namespace {

constexpr char kProfileId[] = "0123456789abcdef0123456789abcdef";

void ExpectSnapshotFor(std::wstring_view directory, std::wstring_view expectedHome)
{
	const auto result = ResolveProfileBootstrapSnapshot(kProfileId, 42, directory);

	ASSERT_TRUE(result.Resolved());
	ASSERT_TRUE(result.snapshot.has_value());
	const auto& snapshot = *result.snapshot;
	EXPECT_EQ(kProfileId, snapshot.ProfileId());
	EXPECT_EQ(42u, snapshot.AuthorityGeneration());
	EXPECT_EQ(ProfileBootstrapSnapshotSource::LegacyDirectoryBridge, snapshot.Source());
	EXPECT_EQ(0u, snapshot.RegistryRevision());
	EXPECT_EQ(expectedHome, snapshot.Resources().ProfileHome().ToString());
	EXPECT_EQ(std::wstring(expectedHome) + L"/settings.json", snapshot.Resources().Settings().ToString());
	EXPECT_EQ(std::wstring(expectedHome) + L"/tasks.json", snapshot.Resources().Tasks().ToString());
	EXPECT_EQ(std::wstring(expectedHome) + L"/keybindings.json", snapshot.Resources().Keybindings().ToString());
	EXPECT_EQ(std::wstring(expectedHome) + L"/snippets", snapshot.Resources().Snippets().ToString());
	EXPECT_EQ(std::wstring(expectedHome) + L"/.sakura-platform/globalStorage", snapshot.Resources().GlobalStorage().ToString());
}

} // namespace

TEST(ProfileBootstrapSnapshot, ResolvesStableUriResourcesForDriveDirectoryWithoutFilesystemAccess)
{
	ExpectSnapshotFor(L"C:\\Profiles\\Sakura User", L"file:///C:/Profiles/Sakura%20User");
}

TEST(ProfileBootstrapSnapshot, ResolvesStableUriResourcesForUncDirectoryWithoutFilesystemAccess)
{
	ExpectSnapshotFor(L"\\\\server\\profiles\\Sakura User", L"file://server/profiles/Sakura%20User");
}

TEST(ProfileBootstrapSnapshot, RejectsInvalidAuthorityAndDirectoryInputsWithTypedTerminalResults)
{
	struct Case {
		std::string_view profileId;
		std::uint64_t generation;
		std::wstring_view directory;
		ProfileBootstrapSnapshotStatus status;
	};
	const Case cases[] = {
		{ "not-an-opaque-profile-id", 1, L"C:\\Profiles\\Sakura", ProfileBootstrapSnapshotStatus::InvalidProfileId },
		{ kProfileId, 0, L"C:\\Profiles\\Sakura", ProfileBootstrapSnapshotStatus::InvalidAuthorityGeneration },
		{ kProfileId, 1, L"Profiles\\Sakura", ProfileBootstrapSnapshotStatus::InvalidProfileDirectory },
		{ kProfileId, 1, L"C:\\Profiles\\.\\Sakura", ProfileBootstrapSnapshotStatus::InvalidProfileDirectory },
		{ kProfileId, 1, L"\\\\server\\profiles\\..\\Sakura", ProfileBootstrapSnapshotStatus::InvalidProfileDirectory },
	};
	for (const auto& testCase : cases) {
		const auto result = ResolveProfileBootstrapSnapshot(
			std::string(testCase.profileId), testCase.generation, testCase.directory);
		EXPECT_EQ(testCase.status, result.status);
		EXPECT_FALSE(result.Resolved());
		EXPECT_FALSE(result.snapshot.has_value());
	}
}

TEST(ProfileBootstrapSnapshot, ReturnedSnapshotsAreIndependentValueCopies)
{
	const auto original = ResolveProfileBootstrapSnapshot(kProfileId, 9, L"C:\\Profiles\\Sakura");
	ASSERT_TRUE(original.Resolved());
	const auto copied = original;
	ASSERT_TRUE(copied.Resolved());

	EXPECT_NE(&*original.snapshot, &*copied.snapshot);
	EXPECT_NE(&original.snapshot->Resources(), &copied.snapshot->Resources());
	EXPECT_EQ(original.snapshot->ProfileId(), copied.snapshot->ProfileId());
}

} // namespace platform::profiles
