/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/problems/MarkerService.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace workbench::problems {
namespace {

platform::uri::Uri Resource(const wchar_t* value)
{
	auto parsed = platform::uri::Uri::Parse(value);
	EXPECT_TRUE(parsed);
	return std::move(*parsed.value);
}

MarkerCollectionIdentity Collection(const char* owner = "sample.extension", const std::uint64_t generation = 1,
	const char* collection = "compiler")
{
	return { { owner, generation }, collection };
}

ProblemMarker Marker(const std::uint32_t line, std::string message = "Unexpected token",
	const EMarkerSeverity severity = EMarkerSeverity::Error)
{
	return { { line, 0, line, 4 }, severity, std::move(message), std::string("E100"), std::string("compiler") };
}

ReplaceMarkersRequest ReplaceRequest(MarkerCollectionIdentity collection, const platform::uri::Uri& resource,
	std::vector<ProblemMarker> markers, std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { std::move(collection), resource, expectedRevision, std::move(markers) };
}

ClearCollectionRequest ClearRequest(MarkerCollectionIdentity collection,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { std::move(collection), expectedRevision };
}

TEST(MarkerService, ReplacesOneStableResourceAndDeletesItWithAnEmptyReplace)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto collection = Collection();

	const auto replaced = service.Replace(ReplaceRequest(collection, resource, { Marker(8), Marker(2, "First", EMarkerSeverity::Warning) }));
	ASSERT_EQ(EMarkerOperationStatus::Replaced, replaced.status);
	EXPECT_EQ(1U, replaced.revision);
	const auto afterReplace = service.Snapshot();
	ASSERT_EQ(1U, afterReplace.resources.size());
	ASSERT_EQ(2U, afterReplace.resources.front().markers.size());
	EXPECT_EQ(2U, afterReplace.resources.front().markers.front().range.startLine);

	const auto cleared = service.Replace(ReplaceRequest(collection, resource, {}));
	EXPECT_EQ(EMarkerOperationStatus::Deleted, cleared.status);
	EXPECT_EQ(2U, cleared.revision);
	EXPECT_TRUE(service.Snapshot().resources.empty());
}

TEST(MarkerService, KeepsCollectionsSeparateAcrossOwnersAndGenerations)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("owner.one"), resource, { Marker(1, "one") })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("owner.two"), resource, { Marker(2, "two") })).status);
	ASSERT_EQ(2U, service.Snapshot().resources.size());

	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("owner.one", 2), resource, { Marker(3, "replacement") })).status);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(2U, snapshot.resources.size());
	EXPECT_EQ("owner.one", snapshot.resources[0].collection.owner.id);
	EXPECT_EQ(2U, snapshot.resources[0].collection.owner.generation);
	EXPECT_EQ("owner.two", snapshot.resources[1].collection.owner.id);
	EXPECT_EQ(EMarkerOperationStatus::StaleGeneration,
		service.Replace(ReplaceRequest(Collection("owner.one", 1), resource, { Marker(4) })).status);
}

TEST(MarkerService, RejectsStaleRevisionAndPreservesTheLastGoodMarkers)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto collection = Collection();
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(collection, resource, { Marker(1, "good") })).status);

	const auto stale = service.Replace(ReplaceRequest(collection, resource, { Marker(2, "bad") }, 0));
	EXPECT_EQ(EMarkerOperationStatus::StaleRevision, stale.status);
	EXPECT_EQ(1U, stale.revision);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ("good", snapshot.resources.front().markers.front().message);
}

TEST(MarkerService, RejectsInvalidRangesAndBoundViolationsWithoutChangingTheSnapshot)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto collection = Collection();
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(collection, resource, { Marker(1, "good") })).status);

	auto backwards = Marker(3);
	backwards.range.endLine = 2;
	EXPECT_EQ(EMarkerOperationStatus::InvalidMarker,
		service.Replace(ReplaceRequest(collection, resource, { backwards })).status);
	EXPECT_EQ(EMarkerOperationStatus::MaximumPayloadExceeded,
		service.Replace(ReplaceRequest(collection, resource, { Marker(2, std::string(4'097U, 'x')) })).status);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ("good", snapshot.resources.front().markers.front().message);
}

TEST(MarkerService, DisposesOnlyTheExactOwnerGeneration)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto owner = MarkerOwner { "owner", 7 };
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest({ owner, "compiler" }, resource, { Marker(1) })).status);
	EXPECT_EQ(EMarkerOperationStatus::StaleGeneration, service.DisposeOwner({ { "owner", 8 }, std::nullopt }).status);
	EXPECT_FALSE(service.Snapshot().resources.empty());
	EXPECT_EQ(EMarkerOperationStatus::OwnerDisposed, service.DisposeOwner({ owner, std::nullopt }).status);
	EXPECT_TRUE(service.Snapshot().resources.empty());
	EXPECT_EQ(EMarkerOperationStatus::NotApplicable, service.DisposeOwner({ owner, std::nullopt }).status);
	EXPECT_EQ(EMarkerOperationStatus::StaleGeneration,
		service.Replace(ReplaceRequest({ owner, "compiler" }, resource, { Marker(2) })).status);
	EXPECT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("owner", 8), resource, { Marker(3) })).status);
}

TEST(MarkerService, ClearsAllResourcesInOneCollectionAtomicallyAndPreservesSiblingCollections)
{
	MarkerService service;
	const auto first = Resource(L"file:///C:/Work/first.cpp");
	const auto second = Resource(L"file:///C:/Work/second.cpp");
	const auto target = Collection("owner", 1, "compiler");
	const auto sibling = Collection("owner", 1, "linter");
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(target, first, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(target, second, { Marker(2) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(sibling, first, { Marker(3) })).status);

	std::vector<MarkerChange> changes;
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([&](const MarkerChange& change) {
		changes.push_back(change);
	}).status);
	const auto cleared = service.ClearCollection(ClearRequest(target));
	EXPECT_EQ(EMarkerOperationStatus::CollectionCleared, cleared.status);
	EXPECT_EQ(4U, cleared.revision);
	EXPECT_TRUE(cleared.Succeeded());
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ(sibling, snapshot.resources.front().collection);
	EXPECT_EQ(1U, changes.size());
	EXPECT_EQ(EMarkerChangeKind::CollectionCleared, changes.front().kind);
	EXPECT_EQ(target, *changes.front().collection);
	EXPECT_FALSE(changes.front().resource.has_value());
}

TEST(MarkerService, ClearCollectionRejectsAnExpectedRevisionMismatchWithoutChangingData)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto collection = Collection();
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(collection, resource, { Marker(1) })).status);

	const auto cleared = service.ClearCollection(ClearRequest(collection, 0));
	EXPECT_EQ(EMarkerOperationStatus::StaleRevision, cleared.status);
	EXPECT_EQ(1U, cleared.revision);
	EXPECT_EQ(1U, service.Snapshot().resources.size());
}

TEST(MarkerService, ClearCollectionRejectsAStaleGenerationWithoutClearingNewerData)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto oldCollection = Collection("owner", 1);
	const auto currentCollection = Collection("owner", 2);
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(oldCollection, resource, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(currentCollection, resource, { Marker(2, "current") })).status);

	const auto cleared = service.ClearCollection(ClearRequest(oldCollection));
	EXPECT_EQ(EMarkerOperationStatus::StaleGeneration, cleared.status);
	const auto snapshot = service.Snapshot();
	ASSERT_EQ(1U, snapshot.resources.size());
	EXPECT_EQ("current", snapshot.resources.front().markers.front().message);
}

TEST(MarkerService, ClearCollectionReturnsNotApplicableForAMissingCollectionWithoutAdvancingRevision)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto present = Collection("owner", 1, "present");
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(present, resource, { Marker(1) })).status);

	const auto cleared = service.ClearCollection(ClearRequest(Collection("owner", 1, "missing")));
	EXPECT_EQ(EMarkerOperationStatus::NotApplicable, cleared.status);
	EXPECT_EQ(1U, cleared.revision);
	EXPECT_EQ(1U, service.Revision());
	EXPECT_EQ(1U, service.Snapshot().resources.size());
}

TEST(MarkerService, ClearCollectionNotificationObservesOnlyThePostCommitSnapshotDuringReentrancy)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto target = Collection("owner", 1, "compiler");
	const auto sibling = Collection("owner", 1, "linter");
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(target, resource, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(sibling, resource, { Marker(2) })).status);
	std::optional<ProblemsSnapshot> observed;
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([&](const MarkerChange& change) {
		if (change.kind == EMarkerChangeKind::CollectionCleared) observed = service.Snapshot();
	}).status);

	EXPECT_EQ(EMarkerOperationStatus::CollectionCleared, service.ClearCollection(ClearRequest(target)).status);
	ASSERT_TRUE(observed);
	ASSERT_EQ(1U, observed->resources.size());
	EXPECT_EQ(sibling, observed->resources.front().collection);
}

TEST(MarkerService, ClearCollectionIsRejectedAfterStop)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto collection = Collection();
	ASSERT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(collection, resource, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Stopped, service.Stop().status);
	EXPECT_EQ(EMarkerOperationStatus::Stopped, service.ClearCollection(ClearRequest(collection)).status);
}

TEST(MarkerService, RetainsDisposedOwnerFencesWithinAnExplicitLifetimeCapacity)
{
	MarkerServiceLimits limits;
	limits.maximumOwners = 1;
	MarkerService service(limits);
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	const auto original = Collection("owner.original", 4);
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(original, resource, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::OwnerDisposed,
		service.DisposeOwner({ original.owner, std::nullopt }).status);

	// Disposed identities remain in the bounded registry, so a new identity cannot
	// evict its tombstone and let a delayed generation-four callback return later.
	EXPECT_EQ(EMarkerOperationStatus::MaximumOwnersExceeded,
		service.Replace(ReplaceRequest(Collection("owner.other"), resource, { Marker(2) })).status);
	EXPECT_EQ(EMarkerOperationStatus::StaleGeneration,
		service.Replace(ReplaceRequest(original, resource, { Marker(3) })).status);
	EXPECT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("owner.original", 5), resource, { Marker(4) })).status);
}

TEST(MarkerService, ReturnsAStableFilteredProblemsOrdering)
{
	MarkerService service;
	const auto zResource = Resource(L"file:///C:/Work/z.cpp");
	const auto aResource = Resource(L"file:///C:/Work/a.cpp");
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("z.owner"), zResource, { Marker(4, "z", EMarkerSeverity::Hint) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection("a.owner"), aResource, { Marker(8, "late"), Marker(2, "early", EMarkerSeverity::Warning) })).status);

	const auto all = service.Snapshot();
	ASSERT_EQ(2U, all.resources.size());
	EXPECT_EQ(aResource.ToString(), all.resources[0].resource.ToString());
	EXPECT_EQ(2U, all.resources[0].markers[0].range.startLine);
	const auto errorsOnly = service.Snapshot({ .maximumSeverity = EMarkerSeverity::Error });
	ASSERT_EQ(1U, errorsOnly.resources.size());
	EXPECT_EQ("late", errorsOnly.resources.front().markers.front().message);
	const auto ownerOnly = service.Snapshot({ .owner = MarkerOwner { "z.owner", 1 } });
	ASSERT_EQ(1U, ownerOnly.resources.size());
	EXPECT_EQ(zResource.ToString(), ownerOnly.resources.front().resource.ToString());
}

TEST(MarkerService, DeliversCommittedNotificationsInOrderOutsideTheModelLockAndContainsExceptions)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	std::vector<std::uint64_t> revisions;
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([&](const MarkerChange& change) {
		revisions.push_back(change.revision);
		if (change.revision == 1) {
			EXPECT_EQ(EMarkerOperationStatus::Replaced,
				service.Replace(ReplaceRequest(Collection("nested"), resource, { Marker(3) })).status);
		}
	}).status);
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([](const MarkerChange&) { throw std::runtime_error("listener"); }).status);

	EXPECT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection(), resource, { Marker(1) })).status);
	ASSERT_EQ(2U, revisions.size());
	EXPECT_EQ(1U, revisions[0]);
	EXPECT_EQ(2U, revisions[1]);
}

TEST(MarkerService, StopClearsStateAndRejectsFurtherMutationAndSubscriptions)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/main.cpp");
	ASSERT_EQ(EMarkerOperationStatus::Replaced,
		service.Replace(ReplaceRequest(Collection(), resource, { Marker(1) })).status);
	ASSERT_EQ(EMarkerOperationStatus::Stopped, service.Stop().status);
	const auto stopped = service.Snapshot();
	EXPECT_TRUE(stopped.stopped);
	EXPECT_TRUE(stopped.resources.empty());
	EXPECT_EQ(EMarkerOperationStatus::Stopped,
		service.Replace(ReplaceRequest(Collection(), resource, { Marker(2) })).status);
	EXPECT_EQ(EMarkerSubscriptionStatus::Stopped, service.Subscribe([](const MarkerChange&) {}).status);
	EXPECT_EQ(EMarkerOperationStatus::Stopped, service.Stop().status);
}

TEST(MarkerService, ExternalAndRepeatedStopWaitForTheStartedListenerDispatch)
{
	using namespace std::chrono_literals;
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/stop.cpp");
	std::mutex callbackMutex;
	std::condition_variable callbackChanged;
	bool entered{};
	bool release{};
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([&](const MarkerChange& change) {
		if (change.kind != EMarkerChangeKind::Replaced) return;
		std::unique_lock lock(callbackMutex);
		entered = true;
		callbackChanged.notify_all();
		callbackChanged.wait(lock, [&] { return release; });
	}).status);

	std::promise<MarkerOperationResult> replacePromise;
	auto replaceFuture = replacePromise.get_future();
	std::thread replacer([&] { replacePromise.set_value(service.Replace(ReplaceRequest(Collection(), resource, { Marker(1) }))); });
	{
		std::unique_lock lock(callbackMutex);
		EXPECT_TRUE(callbackChanged.wait_for(lock, 2s, [&] { return entered; }));
	}
	std::promise<MarkerOperationResult> firstPromise;
	std::promise<MarkerOperationResult> secondPromise;
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
	EXPECT_EQ(std::future_status::ready, replaceFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, firstFuture.wait_for(2s));
	EXPECT_EQ(std::future_status::ready, secondFuture.wait_for(2s));
	const auto replace = replaceFuture.get();
	const auto firstStop = firstFuture.get();
	const auto secondStop = secondFuture.get();
	replacer.join();
	first.join();
	second.join();
	EXPECT_EQ(EMarkerOperationStatus::Replaced, replace.status);
	EXPECT_EQ(EMarkerOperationStatus::Stopped, firstStop.status);
	EXPECT_EQ(EMarkerOperationStatus::Stopped, secondStop.status);
	EXPECT_FALSE(firstStop.callbackDrainDeferred);
	EXPECT_FALSE(secondStop.callbackDrainDeferred);
}

TEST(MarkerService, ReentrantListenerStopDefersItsOwnDrainWithoutDeadlock)
{
	MarkerService service;
	const auto resource = Resource(L"file:///C:/Work/reentrant-stop.cpp");
	std::optional<MarkerOperationResult> stopResult;
	ASSERT_EQ(EMarkerSubscriptionStatus::Subscribed, service.Subscribe([&](const MarkerChange&) {
		if (!stopResult) stopResult = service.Stop();
	}).status);
	EXPECT_EQ(EMarkerOperationStatus::Replaced, service.Replace(ReplaceRequest(Collection(), resource, { Marker(1) })).status);
	ASSERT_TRUE(stopResult);
	EXPECT_EQ(EMarkerOperationStatus::Stopped, stopResult->status);
	EXPECT_TRUE(stopResult->callbackDrainDeferred);
	EXPECT_TRUE(service.Snapshot().stopped);
}

} // namespace
} // namespace workbench::problems
