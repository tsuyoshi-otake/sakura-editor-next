/*! @file
 * @brief Pure, owner-fenced marker and Problems snapshot service.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#pragma once

#include <sakura/uri/UriIdentity.h>

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace workbench::problems {

//! A reloaded extension receives a new generation; an older generation can never replace its markers.
struct MarkerOwner final {
	std::string id;
	std::uint64_t generation{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const MarkerOwner&) const noexcept = default;
};

//! A collection is the VS Code marker-collection identity within one extension generation.
struct MarkerCollectionIdentity final {
	MarkerOwner owner;
	std::string id;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const MarkerCollectionIdentity&) const noexcept = default;
};

//! Zero-based, half-open text coordinates. A marker cannot span backwards.
struct MarkerRange final {
	std::uint32_t startLine{};
	std::uint32_t startColumn{};
	std::uint32_t endLine{};
	std::uint32_t endColumn{};

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const MarkerRange&) const noexcept = default;
};

enum class EMarkerSeverity : std::uint8_t {
	Error = 0,
	Warning = 1,
	Information = 2,
	Hint = 3,
};

//! Value copied at the API boundary; no document, HWND, transport, or I/O handle escapes into the model.
struct ProblemMarker final {
	MarkerRange range;
	EMarkerSeverity severity{ EMarkerSeverity::Error };
	std::string message;
	std::optional<std::string> code;
	std::optional<std::string> source;

	[[nodiscard]] bool operator==(const ProblemMarker&) const noexcept = default;
};

struct ReplaceMarkersRequest final {
	MarkerCollectionIdentity collection;
	platform::uri::Uri resource;
	std::optional<std::uint64_t> expectedRevision;
	std::vector<ProblemMarker> markers;
};

struct DeleteMarkersRequest final {
	MarkerCollectionIdentity collection;
	platform::uri::Uri resource;
	std::optional<std::uint64_t> expectedRevision;
};

//! Value request for the VS Code DiagnosticCollection.clear() equivalent.
struct ClearCollectionRequest final {
	MarkerCollectionIdentity collection;
	std::optional<std::uint64_t> expectedRevision;
};

struct DisposeMarkerOwnerRequest final {
	MarkerOwner owner;
	std::optional<std::uint64_t> expectedRevision;
};

enum class EMarkerOperationStatus : std::uint8_t {
	Replaced,
	Deleted,
	CollectionCleared,
	OwnerDisposed,
	Stopped,
	AlreadyStopped,
	NotApplicable,
	InvalidOwner,
	InvalidCollection,
	InvalidResource,
	InvalidMarker,
	//! The service retains a bounded owner-generation fence for every unique owner ID.
	MaximumOwnersExceeded,
	MaximumCollectionsExceeded,
	MaximumResourcesExceeded,
	MaximumMarkersExceeded,
	MaximumPayloadExceeded,
	StaleGeneration,
	StaleRevision,
	RevisionExhausted,
};

struct MarkerOperationResult final {
	EMarkerOperationStatus status{ EMarkerOperationStatus::NotApplicable };
	std::uint64_t revision{};
	bool callbackDrainDeferred{};

	[[nodiscard]] bool Succeeded() const noexcept;
};

struct ProblemsSnapshotQuery final {
	std::optional<platform::uri::Uri> resource;
	std::optional<MarkerOwner> owner;
	std::optional<MarkerCollectionIdentity> collection;
	//! Includes Error through this severity, using VS Code's severity ordering.
	std::optional<EMarkerSeverity> maximumSeverity;
};

struct ProblemResourceSnapshot final {
	platform::uri::Uri resource;
	MarkerCollectionIdentity collection;
	std::vector<ProblemMarker> markers;
};

//! Resources, collections, and markers are returned in a deterministic Problems ordering.
struct ProblemsSnapshot final {
	std::uint64_t revision{};
	bool stopped{};
	std::uint64_t droppedNotificationCount{};
	std::vector<ProblemResourceSnapshot> resources;
};

enum class EMarkerChangeKind : std::uint8_t {
	Replaced,
	Deleted,
	CollectionCleared,
	OwnerDisposed,
};

struct MarkerChange final {
	std::uint64_t revision{};
	EMarkerChangeKind kind{ EMarkerChangeKind::Replaced };
	MarkerOwner owner;
	std::optional<MarkerCollectionIdentity> collection;
	std::optional<platform::uri::Uri> resource;
};

using MarkerSubscriptionId = std::uint64_t;
using MarkerChangeListener = std::function<void(const MarkerChange&)>;

enum class EMarkerSubscriptionStatus : std::uint8_t {
	Subscribed,
	Stopped,
	InvalidListener,
	SubscriptionLimitExceeded,
	RevisionExhausted,
};

struct MarkerSubscriptionResult final {
	EMarkerSubscriptionStatus status{ EMarkerSubscriptionStatus::InvalidListener };
	std::optional<MarkerSubscriptionId> subscriptionId;
};

//! Lifetime limits for the pure marker authority. Disposed owner IDs remain counted so a late
//! callback can never resurrect their fenced generation; hosts must create a new service after
//! reaching this explicit lifetime capacity.
struct MarkerServiceLimits final {
	std::size_t maximumOwners{ 128U };
};

/*!
	@brief Thread-safe, IMarkerService-shaped storage for diagnostics.

	`Replace` is atomic per collection/resource. An empty marker vector means an
	explicit clear and returns `Deleted`. Invalid, stale, and oversized requests
	leave the last accepted marker state unchanged. The class is intentionally a
	pure model; composition owns any extension RPC, document conversion, Problems
	view, and native-window integration.
*/
class MarkerService final {
public:
	explicit MarkerService(MarkerServiceLimits limits = {});
	~MarkerService();
	MarkerService(const MarkerService&) = delete;
	MarkerService& operator=(const MarkerService&) = delete;

	[[nodiscard]] MarkerOperationResult Replace(const ReplaceMarkersRequest& request);
	[[nodiscard]] MarkerOperationResult Delete(const DeleteMarkersRequest& request);
	//! Atomically removes every resource belonging to exactly one collection.
	[[nodiscard]] MarkerOperationResult ClearCollection(const ClearCollectionRequest& request);
	[[nodiscard]] MarkerOperationResult DisposeOwner(const DisposeMarkerOwnerRequest& request);
	//! External callers wait for an active listener dispatch; a reentrant listener Stop returns deferred.
	//! A listener borrows this service and must not destroy it from inside the callback.
	[[nodiscard]] MarkerOperationResult Stop() noexcept;
	[[nodiscard]] std::uint64_t Revision() const noexcept;
	[[nodiscard]] ProblemsSnapshot Snapshot(const ProblemsSnapshotQuery& query = {}) const;
	[[nodiscard]] MarkerSubscriptionResult Subscribe(MarkerChangeListener listener);
	void Unsubscribe(MarkerSubscriptionId subscriptionId) noexcept;

	[[nodiscard]] static bool IsValidStableId(std::string_view value) noexcept;

private:
	struct CollectionKey final {
		std::string ownerId;
		std::uint64_t generation{};
		std::string collectionId;

		[[nodiscard]] bool operator<(const CollectionKey& right) const noexcept;
	};

	struct StoredResource final {
		platform::uri::Uri resource;
		std::vector<ProblemMarker> markers;
	};

	struct CollectionState final {
		MarkerCollectionIdentity identity;
		std::map<std::wstring, StoredResource, std::less<>> resources;
	};

	[[nodiscard]] MarkerOperationResult ResultLocked(EMarkerOperationStatus status) const noexcept;
	[[nodiscard]] bool IsStopped() const noexcept;
	[[nodiscard]] bool IsExpectedRevisionCurrentLocked(const std::optional<std::uint64_t>& expectedRevision) const noexcept;
	[[nodiscard]] bool CanAdvanceRevisionLocked() const noexcept;
	void AdvanceRevisionLocked() noexcept;
	void RemoveOwnerDataLocked(const MarkerOwner& owner) noexcept;
	[[nodiscard]] bool EnqueueNotificationLocked(MarkerChange change) noexcept;
	void DrainNotifications() noexcept;
	[[nodiscard]] bool WaitForNotificationDrain() noexcept;
	[[nodiscard]] bool IsNotificationDispatchThreadLocked() const noexcept;
	static void SaturatingIncrement(std::uint64_t& value) noexcept;

	mutable std::mutex m_mutex;
	std::uint64_t m_revision{};
	bool m_stopped{};
	MarkerServiceLimits m_limits;
	//! This registry includes disposed IDs. It is intentionally never evicted before Stop().
	std::map<std::string, std::uint64_t, std::less<>> m_ownerGenerations;
	//! A disposed generation remains a fence; it must never be resurrected by a late extension callback.
	std::map<std::string, std::uint64_t, std::less<>> m_disposedOwnerGenerations;
	std::map<CollectionKey, CollectionState> m_collections;
	std::map<MarkerSubscriptionId, MarkerChangeListener> m_listeners;
	MarkerSubscriptionId m_nextSubscriptionId{ 1 };

	mutable std::mutex m_notificationMutex;
	std::condition_variable m_notificationDrained;
	std::deque<MarkerChange> m_notificationQueue;
	bool m_dispatchingNotifications{};
	std::thread::id m_notificationDispatchThreadId;
	std::uint64_t m_droppedNotificationCount{};
};

} // namespace workbench::problems
