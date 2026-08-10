/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <sakura/controlipc/ControlIpcSecurity.h>
#include "platform/controlipc/ControlPlatformEndpoint.h"

#include <filesystem>
#include <limits>
#include <string>

namespace platform::controlipc {
namespace {

constexpr char kProfileAuthorityId[] = "0123456789abcdef0123456789abcdef";

std::filesystem::path UniqueProfilePath()
{
	return std::filesystem::temp_directory_path() /
		("sakura-controlipc-endpoint-" + std::to_string(::GetCurrentProcessId()) + "-" +
			std::to_string(::GetTickCount64()));
}

ControlPlatformEndpointSnapshot AcceptingSnapshot(const CControlPlatformEndpoint& endpoint, std::uint64_t generation)
{
	return {
		.controlProcessId = ::GetCurrentProcessId(),
		.generation = generation,
		.lifecycle = ControlPlatformEndpointLifecycle::Accepting,
		.profileHash = endpoint.ProfileHash(),
		.pipeName = BuildControlPipeName(endpoint.ProfileHash()),
		.profileId = kProfileAuthorityId,
	};
}

} // namespace

TEST(ControlPlatformEndpoint, ReaderObtainsAConsistentLiveSnapshotAndHonorsMinimumGeneration)
{
	const auto profile = UniqueProfilePath();
	CControlPlatformEndpoint control;
	CControlPlatformEndpoint editor;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(editor.OpenForEditor(profile, diagnostic)) << diagnostic.c_str();
	auto starting = AcceptingSnapshot(control, 8);
	starting.lifecycle = ControlPlatformEndpointLifecycle::Starting;
	ASSERT_TRUE(control.Publish(starting, diagnostic)) << diagnostic.c_str();
	EXPECT_FALSE(editor.Read({ .minimumGeneration = 0, .requireLiveControlProcess = true }).has_value());
	ASSERT_TRUE(control.Publish(AcceptingSnapshot(control, 9), diagnostic)) << diagnostic.c_str();

	const auto snapshot = editor.Read({ .minimumGeneration = 9, .requireLiveControlProcess = true });
	ASSERT_TRUE(snapshot.has_value());
	EXPECT_EQ(::GetCurrentProcessId(), snapshot->controlProcessId);
	EXPECT_EQ(9u, snapshot->generation);
	EXPECT_EQ(ControlPlatformEndpointLifecycle::Accepting, snapshot->lifecycle);
	EXPECT_EQ(kProfileAuthorityId, snapshot->profileId);
	EXPECT_EQ(control.ProfileHash(), snapshot->profileHash);
	EXPECT_EQ(BuildControlPipeName(control.ProfileHash()), snapshot->pipeName);
	EXPECT_FALSE(editor.Read({ .minimumGeneration = 10, .requireLiveControlProcess = true }).has_value());
	auto stopping = AcceptingSnapshot(control, 10);
	stopping.lifecycle = ControlPlatformEndpointLifecycle::Stopping;
	EXPECT_TRUE(control.Publish(stopping, diagnostic)) << diagnostic.c_str();
	EXPECT_FALSE(editor.Read({ .minimumGeneration = 0, .requireLiveControlProcess = true }).has_value());
	auto stopped = AcceptingSnapshot(control, 11);
	stopped.lifecycle = ControlPlatformEndpointLifecycle::Stopped;
	EXPECT_TRUE(control.Publish(stopped, diagnostic)) << diagnostic.c_str();
	EXPECT_FALSE(editor.Read({ .minimumGeneration = 0, .requireLiveControlProcess = true }).has_value());
}

TEST(ControlPlatformEndpoint, PublishesOnlyCanonicalAuthorityIdentityAndReaderRejectsMutatedIdentity)
{
	const auto profile = UniqueProfilePath();
	CControlPlatformEndpoint control;
	CControlPlatformEndpoint editor;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	ASSERT_TRUE(editor.OpenForEditor(profile, diagnostic)) << diagnostic.c_str();

	auto valid = AcceptingSnapshot(control, 1);
	EXPECT_TRUE(control.Publish(valid, diagnostic)) << diagnostic.c_str();
	const auto roundTrip = editor.Read({ .minimumGeneration = 1, .requireLiveControlProcess = true });
	ASSERT_TRUE(roundTrip.has_value());
	EXPECT_EQ(kProfileAuthorityId, roundTrip->profileId);

	auto empty = valid;
	empty.profileId.clear();
	EXPECT_FALSE(control.Publish(empty, diagnostic));

	auto malformed = valid;
	malformed.profileId[0] = 'A';
	EXPECT_FALSE(control.Publish(malformed, diagnostic));

	auto oversized = valid;
	oversized.profileId.push_back('0');
	EXPECT_FALSE(control.Publish(oversized, diagnostic));

	auto mutated = valid;
	mutated.profileId[31] = 'g';
	EXPECT_FALSE(CControlPlatformEndpoint::IsSnapshotUsable(mutated, control.ProfileHash(),
		{ .minimumGeneration = 1, .requireLiveControlProcess = true }));
}

TEST(ControlPlatformEndpoint, RejectsInvalidWriterSnapshotAndUntrustedReaderSnapshot)
{
	const auto profile = UniqueProfilePath();
	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	auto invalid = AcceptingSnapshot(control, 1);
	invalid.pipeName += L"-unexpected";
	EXPECT_FALSE(control.Publish(invalid, diagnostic));

	const auto valid = AcceptingSnapshot(control, 2);
	EXPECT_TRUE(CControlPlatformEndpoint::IsSnapshotUsable(valid, control.ProfileHash(),
		{ .minimumGeneration = 2, .requireLiveControlProcess = true }));
	EXPECT_FALSE(CControlPlatformEndpoint::IsSnapshotUsable(valid, control.ProfileHash(),
		{ .minimumGeneration = 3, .requireLiveControlProcess = false }));

	auto stale = valid;
	stale.controlProcessId = (std::numeric_limits<std::uint32_t>::max)();
	EXPECT_FALSE(CControlPlatformEndpoint::IsSnapshotUsable(stale, control.ProfileHash(),
		{ .minimumGeneration = 0, .requireLiveControlProcess = true }));
	EXPECT_TRUE(CControlPlatformEndpoint::IsSnapshotUsable(stale, control.ProfileHash(),
		{ .minimumGeneration = 0, .requireLiveControlProcess = false }));

	auto stopping = valid;
	stopping.lifecycle = ControlPlatformEndpointLifecycle::Stopping;
	EXPECT_FALSE(CControlPlatformEndpoint::IsSnapshotUsable(stopping, control.ProfileHash(),
		{ .minimumGeneration = 0, .requireLiveControlProcess = false }));
}

TEST(ControlPlatformEndpoint, ClassifiesSnapshotReadinessWithoutCollapsingFailureModes)
{
	const auto profile = UniqueProfilePath();
	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	ASSERT_TRUE(control.CreateForControl(profile, diagnostic)) << diagnostic.c_str();
	const auto accepting = AcceptingSnapshot(control, 8);
	const ControlPlatformEndpointReadRequirements requirements{ .minimumGeneration = 8, .requireLiveControlProcess = true };

	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::Discovered,
		CControlPlatformEndpoint::ClassifySnapshot(accepting, control.ProfileHash(), requirements));
	auto starting = accepting;
	starting.lifecycle = ControlPlatformEndpointLifecycle::Starting;
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::NotAccepting,
		CControlPlatformEndpoint::ClassifySnapshot(starting, control.ProfileHash(), requirements));
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::DeadOrStale,
		CControlPlatformEndpoint::ClassifySnapshot(accepting, control.ProfileHash(), { .minimumGeneration = 9, .requireLiveControlProcess = true }));
	auto dead = accepting;
	dead.controlProcessId = (std::numeric_limits<std::uint32_t>::max)();
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::DeadOrStale,
		CControlPlatformEndpoint::ClassifySnapshot(dead, control.ProfileHash(), requirements));
	auto malformed = accepting;
	malformed.pipeName += L"-unexpected";
	EXPECT_EQ(EControlPlatformEndpointDiscoveryDisposition::UnsupportedOrMalformedAbi,
		CControlPlatformEndpoint::ClassifySnapshot(malformed, control.ProfileHash(), requirements));
}

} // namespace platform::controlipc
