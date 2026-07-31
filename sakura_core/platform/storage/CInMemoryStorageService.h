/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/storage/IStorageService.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

namespace platform::storage {

struct StorageSubscriptionState;

/*!
	@brief Authoritative storage semantics without durable I/O.

	The control-process backend will compose these transaction, revision, and
	replay rules with a durable database. Editor processes must not instantiate
	this class as an independent writer.
*/
class CInMemoryStorageService final : public IStorageService {
public:
	explicit CInMemoryStorageService(
		std::uint64_t generation = 1,
		std::size_t maxCompletedOperations = 4096,
		std::uint64_t initialRevision = 0) noexcept;
	~CInMemoryStorageService() override;

	[[nodiscard]] StorageMutationResult Apply(const StorageMutationRequest& request) override;
	[[nodiscard]] StorageSnapshot Snapshot() const override;
	[[nodiscard]] std::unique_ptr<IStorageChangeSubscription> Subscribe(
		StorageChangeCallback callback) override;
	[[nodiscard]] std::optional<StorageEntry> Find(const StorageAddress& address) const;

	//! Models a new authoritative connection generation while retaining state. Zero means exhausted.
	[[nodiscard]] std::uint64_t RestartGeneration() noexcept;

private:
	struct CompletedOperation {
		StorageMutationRequest request;
		StorageMutationResult result;
	};

	[[nodiscard]] StorageMutationResult ValidateRequest(const StorageMutationRequest& request) const;
	void RememberCompleted(const StorageMutationRequest& request, const StorageMutationResult& result);
	//! Called with m_mutex held; establishes revision-ordered callback delivery.
	[[nodiscard]] bool EnqueueNotification(const StorageChangeBatch& batch);
	void DrainNotifications();

	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 1;
	std::uint64_t m_revision = 0;
	std::size_t m_snapshotPayloadBytes = 20;
	std::size_t m_maxCompletedOperations = 4096;
	std::map<StorageAddress, StorageEntry> m_entries;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;

	std::shared_ptr<StorageSubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<StorageChangeBatch> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

//! Read cache used by editor/extension clients after snapshot synchronization.
class CStorageSnapshotCache final {
public:
	void Replace(StorageSnapshot snapshot);
	[[nodiscard]] EStorageChangeApplyStatus Apply(const StorageChangeBatch& batch);
	[[nodiscard]] std::optional<StorageEntry> Find(const StorageAddress& address) const;
	[[nodiscard]] std::uint64_t GetGeneration() const;
	[[nodiscard]] std::uint64_t GetRevision() const;
	//! Tests both snapshot coordinates while holding one cache-state lock.
	[[nodiscard]] bool Matches(std::uint64_t generation, std::uint64_t revision) const;

private:
	// All cache state is observed and replaced atomically at an operation boundary.
	// Entries leave this class only as value copies, so readers cannot retain mutable
	// access while a retry worker applies a later revision.
	mutable std::shared_mutex m_mutex;
	std::uint64_t m_generation = 0;
	std::uint64_t m_revision = 0;
	std::map<StorageAddress, StorageEntry> m_entries;
};

} // namespace platform::storage
