/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "platform/storage/CInMemoryStorageService.h"

#include <atomic>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace platform::storage {
namespace {

StorageAddress ProfileAddress(std::string profileId, std::string key)
{
	return { .scope = EStorageScope::Profile, .scopeId = std::move(profileId),
		.owner = "workbench.layout", .key = std::move(key) };
}

StorageMutationRequest Put(std::string operationId, StorageAddress address, std::string value,
	std::optional<std::uint64_t> expectedRevision = std::nullopt)
{
	return { .operationId = std::move(operationId), .expectedRevision = expectedRevision,
		.mutations = { StorageMutation{ .address = std::move(address),
			.target = EStorageTarget::Machine, .value = std::move(value) } } };
}

TEST(StorageAddress, RequiresScopeIdentityOnlyForProfileAndWorkspace)
{
	const StorageAddress application{ EStorageScope::Application, {}, "owner", "key" };
	const StorageAddress invalidApplication{ EStorageScope::Application, "unexpected", "owner", "key" };
	const StorageAddress profile{ EStorageScope::Profile, "profile-a", "owner", "key" };
	const StorageAddress invalidWorkspace{ EStorageScope::Workspace, {}, "owner", "key" };
	const StorageAddress invalidOwner{ EStorageScope::Profile, "profile-a", {}, "key" };

	EXPECT_TRUE(application.IsValid());
	EXPECT_FALSE(invalidApplication.IsValid());
	EXPECT_TRUE(profile.IsValid());
	EXPECT_FALSE(invalidWorkspace.IsValid());
	EXPECT_FALSE(invalidOwner.IsValid());
}

TEST(StorageAddress, RejectsUnknownScopesOversizedPartsAndMalformedUtf8)
{
	StorageAddress unknownScope = ProfileAddress("profile-a", "key");
	unknownScope.scope = static_cast<EStorageScope>(0xff);
	StorageAddress oversized = ProfileAddress("profile-a", std::string(kMaximumStorageAddressPartBytes + 1, 'k'));
	StorageAddress malformed = ProfileAddress("profile-a", std::string("\xc0\xaf", 2));

	EXPECT_FALSE(unknownScope.IsValid());
	EXPECT_FALSE(oversized.IsValid());
	EXPECT_FALSE(malformed.IsValid());
	EXPECT_TRUE(IsValidStorageUtf8("\xe8\xa8\xad\xe5\xae\x9a"));
}

TEST(StorageService, CompareAndSetPreventsLostUpdate)
{
	CInMemoryStorageService storage;
	const auto address = ProfileAddress("profile-a", "panel.position");

	const auto first = storage.Apply(Put("writer-a", address, "bottom", 0));
	const auto stale = storage.Apply(Put("writer-b", address, "right", 0));

	EXPECT_EQ(EStorageMutationStatus::Succeeded, first.status);
	EXPECT_EQ(1u, first.revision);
	EXPECT_EQ(EStorageMutationStatus::Conflict, stale.status);
	EXPECT_EQ(1u, stale.revision);
	const auto stored = storage.Find(address);
	ASSERT_TRUE(stored);
	EXPECT_EQ("bottom", stored->value);
}

TEST(StorageService, ReplayedOperationIsIdempotentAndMismatchedReuseFails)
{
	CInMemoryStorageService storage;
	const auto address = ProfileAddress("profile-a", "sidebar.visible");
	const auto request = Put("operation-1", address, "true", 0);

	const auto first = storage.Apply(request);
	const auto replay = storage.Apply(request);
	const auto mismatch = storage.Apply(Put("operation-1", address, "false", 1));

	EXPECT_EQ(EStorageMutationStatus::Succeeded, first.status);
	EXPECT_FALSE(first.replayed);
	EXPECT_EQ(EStorageMutationStatus::Succeeded, replay.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(first.revision, replay.revision);
	EXPECT_EQ(EStorageMutationStatus::Failed, mismatch.status);
	EXPECT_EQ(1u, storage.Snapshot().revision);
}

TEST(StorageService, BatchIsAtomicAndPublishesOneRevision)
{
	CInMemoryStorageService storage(7);
	const auto first = ProfileAddress("profile-a", "one");
	const auto second = ProfileAddress("profile-a", "two");
	StorageMutationRequest batch{ .operationId = "batch", .expectedRevision = 0,
		.mutations = {
			{ .address = first, .target = EStorageTarget::User, .value = "1" },
			{ .address = second, .target = EStorageTarget::Machine, .value = "2" },
		} };

	const auto result = storage.Apply(batch);
	ASSERT_EQ(EStorageMutationStatus::Succeeded, result.status);
	ASSERT_TRUE(result.changeBatch);
	EXPECT_EQ(0u, result.changeBatch->baseRevision);
	EXPECT_EQ(1u, result.changeBatch->revision);
	EXPECT_EQ(7u, result.changeBatch->generation);
	EXPECT_EQ(2u, result.changeBatch->changes.size());
	const auto firstEntry = storage.Find(first);
	const auto secondEntry = storage.Find(second);
	ASSERT_TRUE(firstEntry);
	ASSERT_TRUE(secondEntry);
	EXPECT_EQ(1u, firstEntry->revision);
	EXPECT_EQ(EStorageTarget::User, firstEntry->target);
	EXPECT_EQ(1u, secondEntry->revision);
}

TEST(StorageService, InvalidMutationLeavesWholeBatchUnchanged)
{
	CInMemoryStorageService storage;
	StorageMutationRequest batch{ .operationId = "invalid-batch",
		.mutations = {
			{ .address = ProfileAddress("profile-a", "valid"), .value = "1" },
			{ .address = StorageAddress{ EStorageScope::Workspace, {}, "owner", "invalid" }, .value = "2" },
		} };

	const auto result = storage.Apply(batch);
	EXPECT_EQ(EStorageMutationStatus::Failed, result.status);
	EXPECT_EQ(0u, storage.Snapshot().revision);
	EXPECT_TRUE(storage.Snapshot().entries.empty());
}

TEST(StorageService, RejectsUnserializableRequestsBeforeChangingState)
{
	CInMemoryStorageService storage;
	const auto address = ProfileAddress("profile-a", "bounded");

	auto badOperation = Put(std::string(kMaximumStorageOperationIdBytes + 1, 'o'), address, "value");
	auto badTarget = Put("bad-target", address, "value");
	badTarget.mutations.front().target = static_cast<EStorageTarget>(0xff);
	auto badValue = Put("bad-value", address, std::string("\xc0\xaf", 2));
	auto tooMany = Put("too-many", address, "value");
	tooMany.mutations.assign(kMaximumStorageItems + 1,
		StorageMutation{ .address = address, .target = EStorageTarget::Machine, .value = "value" });

	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(badOperation).status);
	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(badTarget).status);
	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(badValue).status);
	EXPECT_EQ(EStorageMutationStatus::Failed, storage.Apply(tooMany).status);
	EXPECT_EQ(0U, storage.Snapshot().revision);
	EXPECT_TRUE(storage.Snapshot().entries.empty());
}

TEST(StorageService, EnforcesSnapshotCapacityBeforeCommit)
{
	CInMemoryStorageService storage;
	StorageMutationRequest fill{ .operationId = "fill", .expectedRevision = 0 };
	fill.mutations.reserve(kMaximumStorageItems);
	for (std::size_t index = 0; index < kMaximumStorageItems; ++index) {
		fill.mutations.push_back({
			.address = ProfileAddress("profile-a", "key-" + std::to_string(index)),
			.target = EStorageTarget::Machine,
			.value = "value",
		});
	}
	ASSERT_EQ(EStorageMutationStatus::Succeeded, storage.Apply(fill).status);
	ASSERT_EQ(kMaximumStorageItems, storage.Snapshot().entries.size());

	const auto overflow = storage.Apply(Put(
		"overflow", ProfileAddress("profile-a", "one-more"), "value", 1));
	EXPECT_EQ(EStorageMutationStatus::Failed, overflow.status);
	EXPECT_EQ(1U, overflow.revision);
	EXPECT_EQ(kMaximumStorageItems, storage.Snapshot().entries.size());
	EXPECT_FALSE(storage.Find(ProfileAddress("profile-a", "one-more")));
}

TEST(StorageService, RevisionAndGenerationExhaustionAreExplicitAndDoNotWrap)
{
	constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
	CInMemoryStorageService revisionExhausted(9, 8, maximum);
	const auto request = Put("revision-exhausted", ProfileAddress("profile-a", "key"), "value", maximum);
	const auto failed = revisionExhausted.Apply(request);
	const auto replay = revisionExhausted.Apply(request);

	EXPECT_EQ(EStorageMutationStatus::Failed, failed.status);
	EXPECT_EQ(maximum, failed.revision);
	EXPECT_FALSE(failed.replayed);
	EXPECT_TRUE(replay.replayed);
	EXPECT_TRUE(revisionExhausted.Snapshot().entries.empty());

	CInMemoryStorageService generationExhausted(maximum);
	EXPECT_EQ(0U, generationExhausted.RestartGeneration());
	EXPECT_EQ(maximum, generationExhausted.Snapshot().generation);
}

TEST(StorageService, ChangeSubscriptionsPublishOnlyCommittedBatchesInRegistrationAndMutationOrder)
{
	CInMemoryStorageService storage(9);
	std::vector<std::string> firstListener;
	std::vector<std::string> secondListener;
	const auto first = storage.Subscribe([&firstListener](const StorageChangeBatch& batch) {
		for (const auto& change : batch.changes) {
			firstListener.push_back(change.address.key + ":"
				+ (change.target == EStorageTarget::User ? "user" : "machine"));
		}
	});
	const auto second = storage.Subscribe([&secondListener](const StorageChangeBatch& batch) {
		secondListener.push_back(std::to_string(batch.generation) + ":"
			+ std::to_string(batch.baseRevision) + ":" + std::to_string(batch.revision));
	});
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_TRUE(first->IsSubscribed());
	EXPECT_TRUE(second->IsSubscribed());

	StorageMutationRequest batch{ .operationId = "subscribed-batch", .expectedRevision = 0,
		.mutations = {
			{ .address = ProfileAddress("profile-a", "first"), .target = EStorageTarget::User, .value = "1" },
			{ .address = ProfileAddress("profile-a", "second"), .target = EStorageTarget::Machine, .value = "2" },
		} };
	const auto committed = storage.Apply(batch);
	const auto replay = storage.Apply(batch);
	StorageMutationRequest sameValue{ .operationId = "same-value", .expectedRevision = 1,
		.mutations = { { .address = ProfileAddress("profile-a", "first"),
			.target = EStorageTarget::User, .value = "1" } } };
	const auto noChange = storage.Apply(sameValue);
	const auto conflict = storage.Apply(Put("stale", ProfileAddress("profile-a", "first"), "3", 0));
	StorageMutationRequest invalid{ .operationId = "invalid", .mutations = {
		{ .address = StorageAddress{ EStorageScope::Workspace, {}, "owner", "bad" }, .value = "no" } } };
	const auto failed = storage.Apply(invalid);

	ASSERT_EQ(EStorageMutationStatus::Succeeded, committed.status);
	EXPECT_TRUE(replay.replayed);
	EXPECT_EQ(EStorageMutationStatus::NotApplicable, noChange.status);
	EXPECT_EQ(EStorageMutationStatus::Conflict, conflict.status);
	EXPECT_EQ(EStorageMutationStatus::Failed, failed.status);
	EXPECT_EQ((std::vector<std::string>{ "first:user", "second:machine" }), firstListener);
	EXPECT_EQ((std::vector<std::string>{ "9:0:1" }), secondListener);
}

TEST(StorageService, ChangeSubscriptionSupportsReentrantMutationAndSelfUnsubscribe)
{
	CInMemoryStorageService storage;
	std::vector<std::string> deliveryOrder;
	std::unique_ptr<IStorageChangeSubscription> first;
	first = storage.Subscribe([&](const StorageChangeBatch& batch) {
		deliveryOrder.push_back("first:" + std::to_string(batch.revision));
		if (batch.revision == 1) {
			first->Unsubscribe();
			const auto nested = storage.Apply(Put("nested", ProfileAddress("profile-a", "nested"), "yes", 1));
			EXPECT_EQ(EStorageMutationStatus::Succeeded, nested.status);
		}
	});
	const auto second = storage.Subscribe([&](const StorageChangeBatch& batch) {
		deliveryOrder.push_back("second:" + std::to_string(batch.revision));
	});
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);

	const auto outer = storage.Apply(Put("outer", ProfileAddress("profile-a", "outer"), "yes", 0));

	EXPECT_EQ(EStorageMutationStatus::Succeeded, outer.status);
	EXPECT_FALSE(first->IsSubscribed());
	EXPECT_EQ((std::vector<std::string>{ "first:1", "second:1", "second:2" }), deliveryOrder);
	EXPECT_EQ("yes", storage.Find(ProfileAddress("profile-a", "nested"))->value);
}

TEST(StorageService, SubscriptionHandleCanOutliveServiceAndUnsubscribeTerminally)
{
	std::unique_ptr<IStorageChangeSubscription> subscription;
	{
		auto storage = std::make_unique<CInMemoryStorageService>();
		subscription = storage->Subscribe([](const StorageChangeBatch&) {});
		ASSERT_TRUE(subscription);
		EXPECT_TRUE(subscription->IsSubscribed());
	}

	EXPECT_FALSE(subscription->IsSubscribed());
	subscription->Unsubscribe();
	EXPECT_FALSE(subscription->IsSubscribed());
}

TEST(StorageService, ThrowingListenerDoesNotSuppressLaterListenersOrCommitResult)
{
	CInMemoryStorageService storage;
	const auto throwing = storage.Subscribe([](const StorageChangeBatch&) {
		throw std::runtime_error("test listener failure");
	});
	std::uint64_t deliveredRevision = 0;
	const auto later = storage.Subscribe([&deliveredRevision](const StorageChangeBatch& batch) {
		deliveredRevision = batch.revision;
	});
	ASSERT_TRUE(throwing);
	ASSERT_TRUE(later);

	const auto result = storage.Apply(Put("listener-fault", ProfileAddress("profile-a", "value"), "yes", 0));

	EXPECT_EQ(EStorageMutationStatus::Succeeded, result.status);
	EXPECT_EQ(result.revision, deliveredRevision);
}

TEST(StorageSnapshotCache, RequiresSnapshotOnGenerationOrRevisionGap)
{
	CInMemoryStorageService storage(11);
	const auto address = ProfileAddress("profile-a", "panel.size");
	const auto result = storage.Apply(Put("put", address, "220", 0));
	ASSERT_TRUE(result.changeBatch);

	CStorageSnapshotCache cache;
	EXPECT_EQ(EStorageChangeApplyStatus::ResyncRequired, cache.Apply(*result.changeBatch));
	cache.Replace(StorageSnapshot{ .generation = 11, .revision = 0 });
	EXPECT_EQ(EStorageChangeApplyStatus::Applied, cache.Apply(*result.changeBatch));
	const auto cached = cache.Find(address);
	ASSERT_TRUE(cached);
	EXPECT_EQ("220", cached->value);
	EXPECT_EQ(EStorageChangeApplyStatus::IgnoredStale, cache.Apply(*result.changeBatch));

	StorageChangeBatch gap{ .generation = 11, .baseRevision = 2, .revision = 3 };
	EXPECT_EQ(EStorageChangeApplyStatus::ResyncRequired, cache.Apply(gap));
	StorageChangeBatch nextGeneration{ .generation = 12, .baseRevision = 1, .revision = 2 };
	EXPECT_EQ(EStorageChangeApplyStatus::ResyncRequired, cache.Apply(nextGeneration));
	cache.Replace(StorageSnapshot{ .generation = 12, .revision = 1 });
	EXPECT_EQ(EStorageChangeApplyStatus::IgnoredStale, cache.Apply(*result.changeBatch));
}

TEST(StorageSnapshotCache, MatchesTestsGenerationAndRevisionAsOneCacheState)
{
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = 11, .revision = 0 });
	EXPECT_TRUE(cache.Matches(11, 0));
	EXPECT_FALSE(cache.Matches(11, 1));
	EXPECT_FALSE(cache.Matches(12, 0));

	const auto address = ProfileAddress("profile-a", "panel.position");
	const StorageChangeBatch batch{ .generation = 11, .baseRevision = 0, .revision = 1,
		.changes = { StorageChange{ .address = address, .target = EStorageTarget::Machine,
			.entry = StorageEntry{ .address = address, .target = EStorageTarget::Machine,
				.value = "bottom", .revision = 1 } } } };
	ASSERT_EQ(EStorageChangeApplyStatus::Applied, cache.Apply(batch));
	EXPECT_FALSE(cache.Matches(11, 0));
	EXPECT_TRUE(cache.Matches(11, 1));

	cache.Replace(StorageSnapshot{ .generation = 0, .revision = 1 });
	EXPECT_FALSE(cache.Matches(11, 1));
	EXPECT_TRUE(cache.Matches(0, 0));
}

TEST(StorageSnapshotCache, MalformedSnapshotsAndBatchesFailClosedWithoutPartialApplication)
{
	const auto firstAddress = ProfileAddress("profile-a", "first");
	const auto secondAddress = ProfileAddress("profile-a", "second");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{
		.generation = 4,
		.revision = 1,
		.entries = { StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
			.value = "stable", .revision = 1 } },
	});
	ASSERT_EQ(4U, cache.GetGeneration());

	StorageChangeBatch mismatched{
		.generation = 4,
		.baseRevision = 1,
		.revision = 2,
		.changes = { StorageChange{
			.address = secondAddress,
			.target = EStorageTarget::Machine,
			.entry = StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
				.value = "corrupt", .revision = 2 },
		} },
	};
	EXPECT_EQ(EStorageChangeApplyStatus::ResyncRequired, cache.Apply(mismatched));
	EXPECT_EQ(1U, cache.GetRevision());
	ASSERT_TRUE(cache.Find(firstAddress));
	EXPECT_EQ("stable", cache.Find(firstAddress)->value);
	EXPECT_FALSE(cache.Find(secondAddress));

	cache.Replace(StorageSnapshot{
		.generation = 0,
		.revision = 1,
		.entries = { StorageEntry{ .address = firstAddress, .target = EStorageTarget::Machine,
			.value = "invalid", .revision = 1 } },
	});
	EXPECT_EQ(0U, cache.GetGeneration());
	EXPECT_EQ(0U, cache.GetRevision());
	EXPECT_FALSE(cache.Find(firstAddress));
}

TEST(StorageSnapshotCache, ConcurrentApplyAndReadsExposeOnlyCopiedCoherentEntries)
{
	constexpr std::uint64_t kGeneration = 9;
	constexpr std::uint64_t kLastRevision = 128;
	const auto address = ProfileAddress("profile-a", "retry-worker-value");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = 0 });

	std::atomic_bool writerStarted = false;
	std::atomic_bool readerStarted = false;
	std::atomic_bool writerFinished = false;
	std::atomic_bool writerObservedApplyFailure = false;
	std::atomic_bool readerObservedInvalidEntry = false;
	std::thread writer([&] {
		writerStarted.store(true, std::memory_order_release);
		while (!readerStarted.load(std::memory_order_acquire)) {
		}
		for (std::uint64_t revision = 1; revision <= kLastRevision; ++revision) {
			StorageChangeBatch batch{
				.generation = kGeneration,
				.baseRevision = revision - 1,
				.revision = revision,
				.changes = { StorageChange{ .address = address, .target = EStorageTarget::Machine,
					.entry = StorageEntry{ .address = address, .target = EStorageTarget::Machine,
						.value = std::to_string(revision), .revision = revision } } },
			};
			if (cache.Apply(batch) != EStorageChangeApplyStatus::Applied) {
				writerObservedApplyFailure.store(true, std::memory_order_release);
			}
		}
		writerFinished.store(true, std::memory_order_release);
	});
	std::thread reader([&] {
		while (!writerStarted.load(std::memory_order_acquire)) {
		}
		readerStarted.store(true, std::memory_order_release);
		while (!writerFinished.load(std::memory_order_acquire)) {
			const auto entry = cache.Find(address);
			if (entry && (entry->value != std::to_string(entry->revision)
				|| entry->address != address || entry->target != EStorageTarget::Machine)) {
				readerObservedInvalidEntry.store(true, std::memory_order_release);
			}
			if (cache.GetGeneration() != kGeneration || cache.GetRevision() > kLastRevision) {
				readerObservedInvalidEntry.store(true, std::memory_order_release);
			}
		}
	});

	writer.join();
	reader.join();

	EXPECT_FALSE(writerObservedApplyFailure.load(std::memory_order_acquire));
	EXPECT_FALSE(readerObservedInvalidEntry.load(std::memory_order_acquire));
	EXPECT_EQ(kGeneration, cache.GetGeneration());
	EXPECT_EQ(kLastRevision, cache.GetRevision());
	const auto entry = cache.Find(address);
	ASSERT_TRUE(entry);
	EXPECT_EQ(std::to_string(kLastRevision), entry->value);
	EXPECT_EQ(kLastRevision, entry->revision);
}

TEST(StorageSnapshotCache, ConcurrentReplaceAndReadsExposeOnlyCompleteSnapshots)
{
	constexpr std::uint64_t kGeneration = 10;
	constexpr std::uint64_t kReplacementCount = 128;
	const auto address = ProfileAddress("profile-a", "resynchronized-value");
	CStorageSnapshotCache cache;
	cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = 1,
		.entries = { StorageEntry{ .address = address, .target = EStorageTarget::Machine,
			.value = "1", .revision = 1 } } });

	std::atomic_bool writerStarted = false;
	std::atomic_bool readerStarted = false;
	std::atomic_bool writerFinished = false;
	std::atomic_bool readerObservedInvalidSnapshot = false;
	std::thread writer([&] {
		writerStarted.store(true, std::memory_order_release);
		while (!readerStarted.load(std::memory_order_acquire)) {
		}
		for (std::uint64_t revision = 2; revision <= kReplacementCount; ++revision) {
			cache.Replace(StorageSnapshot{ .generation = kGeneration, .revision = revision,
				.entries = { StorageEntry{ .address = address, .target = EStorageTarget::Machine,
					.value = std::to_string(revision), .revision = revision } } });
		}
		writerFinished.store(true, std::memory_order_release);
	});
	std::thread reader([&] {
		while (!writerStarted.load(std::memory_order_acquire)) {
		}
		readerStarted.store(true, std::memory_order_release);
		while (!writerFinished.load(std::memory_order_acquire)) {
			const auto entry = cache.Find(address);
			if (!entry || entry->value != std::to_string(entry->revision)
				|| entry->address != address || entry->target != EStorageTarget::Machine
				|| cache.GetGeneration() != kGeneration || cache.GetRevision() > kReplacementCount) {
				readerObservedInvalidSnapshot.store(true, std::memory_order_release);
			}
		}
	});

	writer.join();
	reader.join();

	EXPECT_FALSE(readerObservedInvalidSnapshot.load(std::memory_order_acquire));
	EXPECT_EQ(kGeneration, cache.GetGeneration());
	EXPECT_EQ(kReplacementCount, cache.GetRevision());
	const auto entry = cache.Find(address);
	ASSERT_TRUE(entry);
	EXPECT_EQ(std::to_string(kReplacementCount), entry->value);
	EXPECT_EQ(kReplacementCount, entry->revision);
}

} // namespace
} // namespace platform::storage
