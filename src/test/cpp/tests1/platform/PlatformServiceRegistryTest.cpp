/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/foundation/PlatformLifecycleCoordinator.h"
#include "platform/foundation/PlatformServiceRegistry.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace platform::foundation {
namespace {

struct TestService {
	int value = 0;
};

LifecycleParticipant Participant(std::string serviceId, std::vector<ServiceId> dependencies,
	std::vector<std::string>& events, bool startSucceeds = true, bool shutdownSucceeds = true)
{
	const auto eventId = serviceId;
	return {
		.serviceId = std::move(serviceId),
		.dependencies = std::move(dependencies),
		.start = [&events, startSucceeds, id = eventId] {
			events.emplace_back("start:" + id);
			return startSucceeds;
		},
		.shutdown = [&events, shutdownSucceeds, id = eventId] {
			events.emplace_back("stop:" + id);
			return shutdownSucceeds;
		},
	};
}

} // namespace

TEST(PlatformServiceRegistry, UnregisteredLookupHasExplicitMissingOutcome)
{
	PlatformServiceRegistry registry;

	const auto result = registry.Lookup<TestService>("platform.test.service");

	EXPECT_EQ(ServiceLookupOutcome::ServiceNotRegistered, result.outcome);
	EXPECT_FALSE(result.Found());
	EXPECT_EQ(nullptr, result.service);
}

TEST(PlatformServiceRegistry, RejectsDuplicateRegistrationWithoutReplacingOriginal)
{
	PlatformServiceRegistry registry;
	auto first = std::make_shared<TestService>(TestService{ .value = 7 });
	auto replacement = std::make_shared<TestService>(TestService{ .value = 99 });

	EXPECT_EQ(ServiceRegistrationOutcome::Registered, registry.Register("platform.test.service", first));
	EXPECT_EQ(ServiceRegistrationOutcome::DuplicateServiceId, registry.Register("platform.test.service", replacement));

	const auto lookup = registry.Lookup<TestService>("platform.test.service");
	ASSERT_TRUE(lookup.Found());
	EXPECT_EQ(first.get(), lookup.service);
	EXPECT_EQ(7, lookup.service->value);
}

TEST(PlatformLifecycleCoordinator, StartsDependenciesBeforeDependentsAndStopsInReverseOrder)
{
	PlatformLifecycleCoordinator coordinator;
	std::vector<std::string> events;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered, coordinator.Register(Participant("filesystem", {}, events)));
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		coordinator.Register(Participant("storage", { "filesystem" }, events)));
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		coordinator.Register(Participant("configuration", { "storage" }, events)));

	const auto start = coordinator.Start();
	EXPECT_EQ(LifecycleOperationOutcome::Started, start.outcome);
	EXPECT_EQ((std::vector<ServiceId>{ "filesystem", "storage", "configuration" }), start.startedServices);
	EXPECT_EQ(LifecycleState::Running, coordinator.State());

	const auto shutdown = coordinator.Shutdown();
	EXPECT_EQ(LifecycleOperationOutcome::ShutdownSucceeded, shutdown.outcome);
	EXPECT_EQ((std::vector<ServiceId>{ "configuration", "storage", "filesystem" }), shutdown.stoppedServices);
	EXPECT_EQ((std::vector<std::string>{
		"start:filesystem", "start:storage", "start:configuration",
		"stop:configuration", "stop:storage", "stop:filesystem" }), events);
	EXPECT_EQ(LifecycleState::Stopped, coordinator.State());
}

TEST(PlatformLifecycleCoordinator, FailedStartRollsBackSuccessfullyStartedParticipantsAndStops)
{
	PlatformLifecycleCoordinator coordinator;
	std::vector<std::string> events;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered, coordinator.Register(Participant("first", {}, events)));
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		coordinator.Register(Participant("second", { "first" }, events, false)));

	const auto start = coordinator.Start();
	EXPECT_EQ(LifecycleOperationOutcome::StartupFailedRolledBack, start.outcome);
	EXPECT_EQ("second", start.failedServiceId);
	EXPECT_EQ((std::vector<ServiceId>{ "first" }), start.startedServices);
	EXPECT_EQ((std::vector<ServiceId>{ "first" }), start.stoppedServices);
	EXPECT_EQ((std::vector<std::string>{ "start:first", "start:second", "stop:first" }), events);
	EXPECT_EQ(LifecycleState::Stopped, coordinator.State());

	const auto repeatedShutdown = coordinator.Shutdown();
	EXPECT_EQ(LifecycleOperationOutcome::AlreadyStopped, repeatedShutdown.outcome);
}

TEST(PlatformLifecycleCoordinator, DependencyResolutionFailuresReachStoppedTerminalState)
{
	PlatformLifecycleCoordinator missingDependency;
	std::vector<std::string> events;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		missingDependency.Register(Participant("configuration", { "storage" }, events)));
	const auto missing = missingDependency.Start();
	EXPECT_EQ(LifecycleOperationOutcome::DependencyMissing, missing.outcome);
	EXPECT_EQ("storage", missing.failedServiceId);
	EXPECT_EQ(LifecycleState::Stopped, missingDependency.State());

	PlatformLifecycleCoordinator dependencyCycle;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		dependencyCycle.Register(Participant("first", { "second" }, events)));
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		dependencyCycle.Register(Participant("second", { "first" }, events)));
	const auto cycle = dependencyCycle.Start();
	EXPECT_EQ(LifecycleOperationOutcome::DependencyCycle, cycle.outcome);
	EXPECT_EQ(LifecycleState::Stopped, dependencyCycle.State());
}

TEST(PlatformLifecycleCoordinator, ShutdownFailureStillReachesStoppedTerminalState)
{
	PlatformLifecycleCoordinator coordinator;
	std::vector<std::string> events;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered,
		coordinator.Register(Participant("service", {}, events, true, false)));
	ASSERT_EQ(LifecycleOperationOutcome::Started, coordinator.Start().outcome);

	const auto shutdown = coordinator.Shutdown();
	EXPECT_EQ(LifecycleOperationOutcome::ShutdownCompletedWithFailures, shutdown.outcome);
	EXPECT_EQ("service", shutdown.failedServiceId);
	EXPECT_EQ(LifecycleState::Stopped, coordinator.State());
}

TEST(PlatformLifecycleCoordinator, ReentrantRequestsHaveExplicitInProgressOutcomeAndStillTerminate)
{
	PlatformLifecycleCoordinator coordinator;
	std::vector<std::string> events;
	LifecycleOperationOutcome nestedStart = LifecycleOperationOutcome::Started;
	LifecycleRegistrationOutcome nestedRegistration = LifecycleRegistrationOutcome::Registered;
	LifecycleOperationOutcome nestedShutdown = LifecycleOperationOutcome::ShutdownSucceeded;
	ASSERT_EQ(LifecycleRegistrationOutcome::Registered, coordinator.Register({
		.serviceId = "service",
		.start = [&] {
			nestedStart = coordinator.Start().outcome;
			nestedRegistration = coordinator.Register(Participant("late", {}, events));
			return true;
		},
		.shutdown = [&] {
			nestedShutdown = coordinator.Shutdown().outcome;
			return true;
		},
	}));

	EXPECT_EQ(LifecycleOperationOutcome::Started, coordinator.Start().outcome);
	EXPECT_EQ(LifecycleOperationOutcome::OperationInProgress, nestedStart);
	EXPECT_EQ(LifecycleRegistrationOutcome::OperationInProgress, nestedRegistration);
	EXPECT_EQ(LifecycleOperationOutcome::ShutdownSucceeded, coordinator.Shutdown().outcome);
	EXPECT_EQ(LifecycleOperationOutcome::OperationInProgress, nestedShutdown);
	EXPECT_EQ(LifecycleState::Stopped, coordinator.State());
}

} // namespace platform::foundation
