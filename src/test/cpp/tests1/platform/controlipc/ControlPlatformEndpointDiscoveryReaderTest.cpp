/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/controlipc/ControlIpcSecurity.h"
#include "platform/controlipc/ControlPlatformEndpointDiscoveryReader.h"

#include <atomic>
#include <filesystem>
#include <string>

namespace platform::controlipc {
namespace {

constexpr char kProfileAuthorityId[] = "0123456789abcdef0123456789abcdef";

std::filesystem::path UniqueProfilePath()
{
	static std::atomic_uint64_t sequence{ 0 };
	return std::filesystem::temp_directory_path() /
		("sakura-controlipc-discovery-reader-" + std::to_string(::GetCurrentProcessId()) + "-" +
			std::to_string(::GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
}

ControlPlatformEndpointSnapshot Snapshot(
	const CControlPlatformEndpoint& endpoint, std::uint64_t generation,
	ControlPlatformEndpointLifecycle lifecycle = ControlPlatformEndpointLifecycle::Accepting)
{
	return {
		.controlProcessId = ::GetCurrentProcessId(),
		.generation = generation,
		.lifecycle = lifecycle,
		.profileHash = endpoint.ProfileHash(),
		.pipeName = BuildControlPipeName(endpoint.ProfileHash()),
		.profileId = kProfileAuthorityId,
	};
}

} // namespace

TEST(ControlPlatformEndpointDiscoveryReader, PrePublicationReadFailsAndLaterPublicationSucceeds)
{
	const auto profile = UniqueProfilePath();
	const auto profileHash = ComputeCanonicalProfileHash(profile);
	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	CControlPlatformEndpointDiscoveryReader reader(profile, profileHash);

	const auto notPublished = reader.ReadDetailed({ .minimumGeneration = 0, .requireLiveControlProcess = true });
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::NotPublished, notPublished.disposition);
	EXPECT_FALSE(notPublished.snapshot.has_value());
	ASSERT_TRUE(control.Publish(Snapshot(control, 3), diagnostic)) << diagnostic.c_str();

	const auto discovered = reader.Read({ .minimumGeneration = 3, .requireLiveControlProcess = true });
	ASSERT_TRUE(discovered.has_value());
	EXPECT_EQ(3u, discovered->generation);
	EXPECT_EQ(profileHash, discovered->profileHash);
}

TEST(ControlPlatformEndpointDiscoveryReader, InvalidPathHashDescriptorDoesNotPinEitherMapping)
{
	const auto profile = UniqueProfilePath();
	const auto otherProfile = UniqueProfilePath();
	const auto otherHash = ComputeCanonicalProfileHash(otherProfile);
	CControlPlatformEndpoint control;
	CControlPlatformEndpoint otherControl;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(otherControl.CreateForControl(otherProfile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(control.Publish(Snapshot(control, 1), diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(otherControl.Publish(Snapshot(otherControl, 1), diagnostic)) << diagnostic.c_str();

	CControlPlatformEndpointDiscoveryReader mismatched(profile, otherHash);
	CControlPlatformEndpointDiscoveryReader malformed(profile, L"not-a-canonical-profile-hash");
	CControlPlatformEndpointDiscoveryReader empty({}, otherHash);
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor, mismatched.ReadDetailed({}).disposition);
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor, malformed.ReadDetailed({}).disposition);
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::InvalidDescriptor, empty.ReadDetailed({}).disposition);

	control.Close();
	otherControl.Close();
	CControlPlatformEndpoint replacement;
	CControlPlatformEndpoint otherReplacement;
	EXPECT_TRUE(replacement.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	EXPECT_TRUE(otherReplacement.CreateForControl(otherProfile, diagnostic)) << diagnostic.c_str();
}

TEST(ControlPlatformEndpointDiscoveryReader, StoppedMappingIsReleasedAndReplacementGenerationIsDiscovered)
{
	const auto profile = UniqueProfilePath();
	const auto profileHash = ComputeCanonicalProfileHash(profile);
	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(control.Publish(Snapshot(control, 4), diagnostic)) << diagnostic.c_str();
	CControlPlatformEndpointDiscoveryReader reader(profile, profileHash);
	ASSERT_TRUE(reader.Read({ .minimumGeneration = 4, .requireLiveControlProcess = true }).has_value());

	ASSERT_TRUE(control.Publish(
		Snapshot(control, 5, ControlPlatformEndpointLifecycle::Stopped), diagnostic)) << diagnostic.c_str();
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::NotAccepting,
		reader.ReadDetailed({ .minimumGeneration = 4, .requireLiveControlProcess = true }).disposition);
	control.Close();

	CControlPlatformEndpoint replacement;
	ASSERT_TRUE(replacement.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(replacement.Publish(Snapshot(replacement, 6), diagnostic)) << diagnostic.c_str();
	const auto discovered = reader.Read({ .minimumGeneration = 6, .requireLiveControlProcess = true });
	ASSERT_TRUE(discovered.has_value());
	EXPECT_EQ(6u, discovered->generation);
}

TEST(ControlPlatformEndpointDiscoveryReader, RequirementsAreHonoredAndCloseIsTerminalAndIdempotent)
{
	const auto profile = UniqueProfilePath();
	const auto profileHash = ComputeCanonicalProfileHash(profile);
	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(control.Publish(Snapshot(control, 8), diagnostic)) << diagnostic.c_str();
	CControlPlatformEndpointDiscoveryReader reader(profile, profileHash);

	EXPECT_FALSE(reader.Read({ .minimumGeneration = 9, .requireLiveControlProcess = false }).has_value());
	const auto accepted = reader.Read({ .minimumGeneration = 8, .requireLiveControlProcess = true });
	ASSERT_TRUE(accepted.has_value());
	EXPECT_EQ(8u, accepted->generation);

	reader.Close();
	reader.Close();
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::Closed,
		reader.ReadDetailed({ .minimumGeneration = 0, .requireLiveControlProcess = false }).disposition);
	control.Close();
	CControlPlatformEndpoint replacement;
	EXPECT_TRUE(replacement.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
}

} // namespace platform::controlipc
