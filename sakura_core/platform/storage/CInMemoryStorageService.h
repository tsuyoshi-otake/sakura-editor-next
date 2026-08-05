/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/storage/IStorageAuthority.h>

#include <cstddef>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace platform::storage {

struct StorageSubscriptionState;

/*!
	@brief Authoritative storage semantics without durable I/O.

	The control-process backend will compose these transaction, revision, and
	replay rules with a durable database. Editor processes must not instantiate
	this class as an independent writer.
*/
class CInMemoryStorageService final : public IStorageAuthority {
public:
	explicit CInMemoryStorageService(
		std::uint64_t generation = 1,
		std::size_t maxCompletedOperations = 4096,
		std::uint64_t initialRevision = 0) noexcept;
	~CInMemoryStorageService() override;

	// The in-memory implementation is a test/legacy authority. It starts in an
	// already-open state so existing service-only tests remain source-compatible;
	// production composition still requires an explicit durable Open().
	[[nodiscard]] StorageAuthorityOpenResult Open() override;
	void Close() noexcept override;
	[[nodiscard]] bool IsOpen() const noexcept override;

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
	bool m_open = true;
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

} // namespace platform::storage
