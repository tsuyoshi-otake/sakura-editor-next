/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "workbench/ports/PortForwardingService.h"

namespace workbench::ports {
namespace {

PortOwner Owner(const char* id, const std::uint64_t generation = 1)
{
	return { .id = id, .generation = generation };
}

PortOperation Operation(std::string id, const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operationId = std::move(id), .expectedRevision = expectedRevision };
}

PortEndpoint Endpoint(std::string host, const std::uint16_t port)
{
	return { .host = std::move(host), .port = port };
}

DiscoverPortRequest DiscoverRequest(std::string operationId, PortOwner owner, std::string portId, const std::uint16_t port = 3000)
{
	return {
		.operation = Operation(std::move(operationId)),
		.owner = std::move(owner),
		.portId = std::move(portId),
		.remoteEndpoint = Endpoint("127.0.0.1", port),
		.privacy = EPortPrivacy::Private,
		.protocol = EPortProtocol::Http,
		.source = EPortSource::AutoForward,
		.sourceDescription = "Auto Forwarded",
		.label = std::string("Development server"),
		.processDescription = std::string("node server.js"),
	};
}

PortMutationRequest Mutation(std::string operationId, PortOwner owner, std::string portId,
	const std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operation = Operation(std::move(operationId), expectedRevision), .owner = std::move(owner), .portId = std::move(portId) };
}

PortSnapshot FindPort(const PortForwardingServiceSnapshot& snapshot, const std::string_view portId)
{
	const auto found = std::find_if(snapshot.ports.begin(), snapshot.ports.end(), [portId](const PortSnapshot& port) {
		return port.portId == portId;
	});
	EXPECT_NE(snapshot.ports.end(), found);
	return found == snapshot.ports.end() ? PortSnapshot{} : *found;
}

TEST(PortForwardingService, DiscoversBoundedMetadataAndReturnsStablePortIdOrder)
{
	PortForwardingService service;
	ASSERT_EQ(EPortOperationStatus::Succeeded, service.Discover(DiscoverRequest("discover-z", Owner("extension.z"), "z.port", 8080)).status);
	ASSERT_EQ(EPortOperationStatus::Succeeded, service.Discover(DiscoverRequest("discover-a", Owner("extension.a"), "a.port", 3000)).status);

	const auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.ports.size());
	EXPECT_EQ("a.port", snapshot.ports[0].portId);
	EXPECT_EQ("z.port", snapshot.ports[1].portId);
	const auto& port = FindPort(snapshot, "a.port");
	EXPECT_EQ(3000, port.remoteEndpoint.port);
	EXPECT_EQ(EPortPrivacy::Private, port.privacy);
	EXPECT_EQ(EPortProtocol::Http, port.protocol);
	EXPECT_EQ(EPortSource::AutoForward, port.source);
	EXPECT_EQ("Auto Forwarded", port.sourceDescription);
	EXPECT_EQ("Development server", *port.label);
	EXPECT_EQ("node server.js", *port.processDescription);
	EXPECT_TRUE(port.closeable);
	EXPECT_EQ(EPortForwardingState::Discovered, port.state);
	EXPECT_FALSE(port.tunnelId);
	EXPECT_FALSE(port.localAddress);
	EXPECT_FALSE(port.localPort);
}

TEST(PortForwardingService, DrivesOnlyExplicitForwardingAndStopTransitions)
{
	PortForwardingService service;
	const auto owner = Owner("extension.lifecycle");
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover", owner, "lifecycle.port")).Succeeded());
	ASSERT_TRUE(service.StartForwarding(Mutation("start", owner, "lifecycle.port")).Succeeded());
	EXPECT_EQ(EPortOperationStatus::NotApplicable, service.CompleteStop(Mutation("early-stop", owner, "lifecycle.port")).status);

	const CompletePortForwardingRequest complete{
		.mutation = Mutation("complete", owner, "lifecycle.port"),
		.tunnelId = "tunnel.lifecycle",
		.localAddress = "https://127.0.0.1:5443",
		.localPort = 5443,
	};
	ASSERT_TRUE(service.CompleteForwarding(complete).Succeeded());
	const auto forwarded = FindPort(service.Snapshot(), "lifecycle.port");
	EXPECT_EQ(EPortForwardingState::Forwarded, forwarded.state);
	EXPECT_EQ("tunnel.lifecycle", *forwarded.tunnelId);
	EXPECT_EQ("https://127.0.0.1:5443", *forwarded.localAddress);
	EXPECT_EQ(5443, *forwarded.localPort);

	ASSERT_TRUE(service.RequestStop(Mutation("request-stop", owner, "lifecycle.port")).Succeeded());
	EXPECT_EQ(EPortForwardingState::Stopping, FindPort(service.Snapshot(), "lifecycle.port").state);
	ASSERT_TRUE(service.CompleteStop(Mutation("complete-stop", owner, "lifecycle.port")).Succeeded());
	EXPECT_EQ(EPortForwardingState::Stopped, FindPort(service.Snapshot(), "lifecycle.port").state);
	EXPECT_TRUE(IsTerminalPortForwardingState(FindPort(service.Snapshot(), "lifecycle.port").state));
}

TEST(PortForwardingService, RejectsInvalidVscodeProjectionMetadataWithoutPartialState)
{
	PortForwardingService service;

	auto invalidEndpoint = DiscoverRequest("invalid-endpoint", Owner("extension.invalid"), "invalid.endpoint");
	invalidEndpoint.remoteEndpoint.host.clear();
	EXPECT_EQ(EPortOperationReason::InvalidEndpoint, service.Discover(invalidEndpoint).reason);

	auto invalidPrivacy = DiscoverRequest("invalid-privacy", Owner("extension.invalid"), "invalid.privacy");
	invalidPrivacy.privacy = static_cast<EPortPrivacy>(0xff);
	EXPECT_EQ(EPortOperationReason::InvalidPrivacy, service.Discover(invalidPrivacy).reason);

	auto invalidSource = DiscoverRequest("invalid-source", Owner("extension.invalid"), "invalid.source");
	invalidSource.sourceDescription.clear();
	EXPECT_EQ(EPortOperationReason::InvalidSourceDescription, service.Discover(invalidSource).reason);
	EXPECT_TRUE(service.Snapshot().ports.empty());

	const auto owner = Owner("extension.valid");
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover-valid", owner, "valid.port")).Succeeded());
	ASSERT_TRUE(service.StartForwarding(Mutation("start-valid", owner, "valid.port")).Succeeded());
	EXPECT_EQ(EPortOperationReason::InvalidLocalAddress,
		service.CompleteForwarding({ .mutation = Mutation("complete-invalid", owner, "valid.port"),
			.tunnelId = "tunnel.valid", .localAddress = "" }).reason);
	EXPECT_EQ(EPortForwardingState::Forwarding, FindPort(service.Snapshot(), "valid.port").state);
}

TEST(PortForwardingService, RecordsFailureAsATerminalSnapshotAndAllowsAnExplicitNewForwardAttempt)
{
	PortForwardingService service;
	const auto owner = Owner("extension.failure");
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover", owner, "failure.port")).Succeeded());
	ASSERT_TRUE(service.StartForwarding(Mutation("start", owner, "failure.port")).Succeeded());
	ASSERT_TRUE(service.FailForwarding({ .mutation = Mutation("failed", owner, "failure.port"), .error = { .code = 10061, .message = "Connection refused" } }).Succeeded());
	const auto failed = FindPort(service.Snapshot(), "failure.port");
	EXPECT_EQ(EPortForwardingState::Failed, failed.state);
	ASSERT_TRUE(failed.error);
	EXPECT_EQ(10061U, failed.error->code);
	EXPECT_TRUE(IsTerminalPortForwardingState(failed.state));

	ASSERT_TRUE(service.StartForwarding(Mutation("retry", owner, "failure.port")).Succeeded());
	const auto retrying = FindPort(service.Snapshot(), "failure.port");
	EXPECT_EQ(EPortForwardingState::Forwarding, retrying.state);
	EXPECT_FALSE(retrying.error);
}

TEST(PortForwardingService, EnforcesPayloadAndPortLimitsWithoutPartialState)
{
	PortForwardingServiceLimits limits;
	limits.maximumPorts = 1;
	limits.maximumPayloadBytes = 20;
	PortForwardingService service(limits);
	auto oversized = DiscoverRequest("oversized", Owner("owner"), "port");
	oversized.label = "This label exceeds the configured total payload limit";
	EXPECT_EQ(EPortOperationStatus::Rejected, service.Discover(oversized).status);
	EXPECT_EQ(EPortOperationReason::PayloadLimitExceeded, service.Discover(DiscoverRequest("oversized-2", Owner("owner"), "other")).reason);
	EXPECT_TRUE(service.Snapshot().ports.empty());

	PortForwardingServiceLimits oneLimit;
	oneLimit.maximumPorts = 1;
	PortForwardingService onePort(oneLimit);
	ASSERT_TRUE(onePort.Discover(DiscoverRequest("first", Owner("first"), "first")).Succeeded());
	EXPECT_EQ(EPortOperationReason::PortLimitExceeded, onePort.Discover(DiscoverRequest("second", Owner("second"), "second")).reason);
	EXPECT_EQ(1U, onePort.Snapshot().ports.size());
}

TEST(PortForwardingService, FencesStaleReplayedAndConflictingOperations)
{
	PortForwardingService service;
	const auto owner = Owner("extension.fence", 3);
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover", owner, "fence.port")).Succeeded());
	const auto before = service.Snapshot();
	EXPECT_EQ(EPortOperationStatus::StaleRevision, service.StartForwarding(Mutation("stale", owner, "fence.port", before.revision - 1)).status);
	ASSERT_EQ(EPortOperationStatus::Succeeded, service.StartForwarding(Mutation("start", owner, "fence.port")).status);
	EXPECT_EQ(EPortOperationStatus::Replayed, service.StartForwarding(Mutation("start", owner, "fence.port")).status);
	EXPECT_EQ(EPortOperationStatus::Conflict, service.StartForwarding(Mutation("start", owner, "different.port")).status);
	EXPECT_EQ(EPortOperationStatus::Conflict, service.StartForwarding(Mutation("late-generation", Owner("extension.fence", 2), "fence.port")).status);
	EXPECT_EQ(EPortOperationReason::OwnerGenerationConflict,
		service.StartForwarding(Mutation("late-generation-2", Owner("extension.fence", 2), "fence.port")).reason);
}

TEST(PortForwardingService, ReplacesOnlyWithANewerOwnerGenerationAndDisposalFencesLateWork)
{
	PortForwardingService service;
	const auto oldOwner = Owner("extension.replace", 1);
	ASSERT_TRUE(service.Discover(DiscoverRequest("old-a", oldOwner, "old.a")).Succeeded());
	ASSERT_TRUE(service.Discover(DiscoverRequest("old-b", oldOwner, "old.b")).Succeeded());
	ASSERT_TRUE(service.Discover(DiscoverRequest("new", Owner("extension.replace", 2), "old.a")).Succeeded());
	const auto replaced = service.Snapshot();
	ASSERT_EQ(1U, replaced.ports.size());
	EXPECT_EQ("old.a", replaced.ports.front().portId);
	EXPECT_EQ(2U, replaced.ports.front().owner.generation);
	EXPECT_EQ(EPortOperationStatus::Conflict, service.Discover(DiscoverRequest("late", oldOwner, "late")).status);

	ASSERT_TRUE(service.DisposeOwner({ .operation = Operation("dispose"), .owner = Owner("extension.replace", 2) }).Succeeded());
	EXPECT_TRUE(service.Snapshot().ports.empty());
	EXPECT_EQ(EPortOperationReason::OwnerDisposed, service.Discover(DiscoverRequest("late-current", Owner("extension.replace", 2), "late-current")).reason);
	ASSERT_TRUE(service.Discover(DiscoverRequest("newer", Owner("extension.replace", 3), "newer")).Succeeded());
}

TEST(PortForwardingService, RetainsDisposedOwnerFencesWithinAnExplicitLifetimeCapacity)
{
	PortForwardingServiceLimits limits;
	limits.maximumOwners = 1;
	PortForwardingService service(limits);
	const auto original = Owner("extension.original", 4);
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover-original", original, "original.port")).Succeeded());
	ASSERT_TRUE(service.DisposeOwner({ .operation = Operation("dispose-original"), .owner = original }).Succeeded());

	// A disposed identity still occupies the bounded registry; accepting another
	// ID by evicting it would allow a late generation-four producer to revive.
	const auto capacity = service.Discover(DiscoverRequest("discover-other", Owner("extension.other"), "other.port"));
	EXPECT_EQ(EPortOperationStatus::Rejected, capacity.status);
	EXPECT_EQ(EPortOperationReason::OwnerLimitExceeded, capacity.reason);
	EXPECT_EQ(EPortOperationReason::OwnerDisposed,
		service.Discover(DiscoverRequest("late-original", original, "late.port")).reason);
	EXPECT_EQ(EPortOperationStatus::Succeeded,
		service.Discover(DiscoverRequest("newer-original", Owner("extension.original", 5), "newer.port")).status);
}

TEST(PortForwardingService, DeliversOrderedNotificationsOutsideTheLockAndContainsListenerFaults)
{
	PortForwardingService service;
	std::vector<std::uint64_t> revisions;
	ASSERT_TRUE(service.Subscribe([&](const PortForwardingServiceChange& change) { revisions.push_back(change.revision); }));
	ASSERT_TRUE(service.Subscribe([](const PortForwardingServiceChange&) { throw std::runtime_error("expected listener fault"); }));
	EPortOperationStatus nestedStatus = EPortOperationStatus::Rejected;
	ASSERT_TRUE(service.Subscribe([&](const PortForwardingServiceChange& change) {
		if (change.portId && *change.portId == "one") {
			nestedStatus = service.Discover(DiscoverRequest("nested", Owner("nested"), "nested")).status;
		}
	}));
	ASSERT_TRUE(service.Discover(DiscoverRequest("one", Owner("one"), "one")).Succeeded());
	ASSERT_TRUE(service.Discover(DiscoverRequest("two", Owner("two"), "two")).Succeeded());
	EXPECT_EQ(EPortOperationStatus::Succeeded, nestedStatus);
	ASSERT_EQ(3U, revisions.size());
	EXPECT_LT(revisions[0], revisions[1]);
	EXPECT_LT(revisions[1], revisions[2]);
}

TEST(PortForwardingService, ExhaustedRevisionCannotPartiallyCommitAndStopIsTerminal)
{
	PortForwardingServiceLimits exhaustedLimits;
	exhaustedLimits.maximumRevision = 1;
	PortForwardingService exhausted(exhaustedLimits);
	EXPECT_EQ(EPortOperationStatus::RevisionExhausted, exhausted.Discover(DiscoverRequest("discover", Owner("owner"), "port")).status);
	EXPECT_TRUE(exhausted.Snapshot().ports.empty());

	PortForwardingService service;
	ASSERT_TRUE(service.Subscribe([](const PortForwardingServiceChange&) {}));
	ASSERT_TRUE(service.Discover(DiscoverRequest("discover", Owner("owner"), "port")).Succeeded());
	ASSERT_EQ(EPortOperationStatus::Succeeded, service.Stop().status);
	const auto stopped = service.Snapshot();
	EXPECT_TRUE(stopped.stopped);
	EXPECT_TRUE(stopped.ports.empty());
	EXPECT_EQ(EPortOperationStatus::Stopped, service.Discover(DiscoverRequest("after", Owner("owner"), "after")).status);
	EXPECT_FALSE(service.Subscribe([](const PortForwardingServiceChange&) {}));
	EXPECT_EQ(EPortOperationStatus::Succeeded, service.Stop().status);
}

TEST(PortForwardingService, ExternalAndRepeatedStopWaitForTheStartedListenerDispatch)
{
	using namespace std::chrono_literals;
	PortForwardingService service;
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	ASSERT_TRUE(service.Subscribe([&](const PortForwardingServiceChange& change) {
		if (change.kind != EPortChangeKind::Discovered) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	}));
	std::promise<PortOperationResult> discoverPromise;
	auto discoverFuture = discoverPromise.get_future();
	std::thread discoverer([&] { discoverPromise.set_value(service.Discover(DiscoverRequest("discover", Owner("owner"), "port"))); });
	{
		std::unique_lock lock(callbackMutex);
		EXPECT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	std::promise<PortOperationResult> firstPromise;
	std::promise<PortOperationResult> secondPromise;
	std::promise<void> firstStartedPromise;
	std::promise<void> secondStartedPromise;
	auto firstFuture = firstPromise.get_future();
	auto secondFuture = secondPromise.get_future();
	auto firstStarted = firstStartedPromise.get_future();
	auto secondStarted = secondStartedPromise.get_future();
	std::thread first([&] { firstStartedPromise.set_value(); firstPromise.set_value(service.Stop()); });
	std::thread second([&] { secondStartedPromise.set_value(); secondPromise.set_value(service.Stop()); });
	EXPECT_EQ(std::future_status::ready, firstStarted.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, secondStarted.wait_for(2s));
	EXPECT_EQ(std::future_status::timeout, firstFuture.wait_for(50ms));
	EXPECT_EQ(std::future_status::timeout, secondFuture.wait_for(50ms));
	{
		std::lock_guard lock(callbackMutex);
		release = true;
		callbackChanged.notify_all();
	}
	EXPECT_EQ(std::future_status::ready, discoverFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, firstFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, secondFuture.wait_for(2s));
	const auto discover = discoverFuture.get();
	const auto firstStop = firstFuture.get();
	const auto secondStop = secondFuture.get();
	discoverer.join();
	first.join();
	second.join();
	EXPECT_EQ(EPortOperationStatus::Succeeded, discover.status);
	EXPECT_EQ(EPortOperationStatus::Succeeded, firstStop.status);
	EXPECT_EQ(EPortOperationStatus::Succeeded, secondStop.status);
	EXPECT_FALSE(firstStop.callbackDrainDeferred);
	EXPECT_FALSE(secondStop.callbackDrainDeferred);
}

TEST(PortForwardingService, ReentrantListenerStopDefersItsOwnDrainWithoutDeadlock)
{
	PortForwardingService service;
	std::optional<PortOperationResult> stopResult;
	ASSERT_TRUE(service.Subscribe([&](const PortForwardingServiceChange&) {
		if (!stopResult) stopResult = service.Stop();
	}));
	EXPECT_EQ(EPortOperationStatus::Succeeded, service.Discover(DiscoverRequest("discover", Owner("owner"), "port")).status);
	ASSERT_TRUE(stopResult);
	EXPECT_EQ(EPortOperationStatus::Succeeded, stopResult->status);
	EXPECT_TRUE(stopResult->callbackDrainDeferred);
	EXPECT_TRUE(service.Snapshot().stopped);
}

} // namespace
} // namespace workbench::ports
