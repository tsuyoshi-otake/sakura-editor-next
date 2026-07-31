/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/problems/MarkerService.h"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace workbench::problems {
namespace {

constexpr std::size_t kMaximumCollections = 256U;
constexpr std::size_t kMaximumResourcesPerCollection = 1'024U;
constexpr std::size_t kMaximumMarkersPerResource = 1'024U;
constexpr std::size_t kMaximumMarkerMessageBytes = 4'096U;
constexpr std::size_t kMaximumMarkerMetadataBytes = 256U;
constexpr std::size_t kMaximumPayloadBytes = 64U * 1'024U;
constexpr std::size_t kMaximumSubscriptions = 256U;
constexpr std::size_t kMaximumPendingNotifications = 1'024U;
constexpr std::size_t kMaximumResourceUriCharacters = 8'192U;

bool IsValidUtf8Text(const std::string_view value, const bool permitSpace, const std::size_t maximumBytes) noexcept
{
	if (value.empty() || value.size() > maximumBytes) return false;
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if (first < (permitSpace ? 0x20 : 0x21) || first == 0x7f) return false;
			++index;
			continue;
		}
		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) {
			continuationCount = 1;
			codePoint = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			continuationCount = 2;
			codePoint = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			continuationCount = 3;
			codePoint = first & 0x07;
		} else {
			return false;
		}
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)
			|| (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidSeverity(const EMarkerSeverity severity) noexcept
{
	return severity == EMarkerSeverity::Error || severity == EMarkerSeverity::Warning
		|| severity == EMarkerSeverity::Information || severity == EMarkerSeverity::Hint;
}

bool IsValidResource(const platform::uri::Uri& resource)
{
	const auto stableResource = resource.ToString();
	return !stableResource.empty() && stableResource.size() <= kMaximumResourceUriCharacters;
}

bool IsMarkerOrderBefore(const ProblemMarker& left, const ProblemMarker& right)
{
	return std::tie(left.range.startLine, left.range.startColumn, left.range.endLine, left.range.endColumn, left.severity,
		left.source, left.code, left.message)
		< std::tie(right.range.startLine, right.range.startColumn, right.range.endLine, right.range.endColumn, right.severity,
			right.source, right.code, right.message);
}

bool IsResourceMatch(const platform::uri::Uri& left, const platform::uri::Uri& right)
{
	return platform::uri::UriIdentityService::IsEqual(left, right);
}

} // namespace

MarkerService::MarkerService(MarkerServiceLimits limits)
	: m_limits(std::move(limits))
{
	// A zero host-provided limit must not accidentally make owner fencing unbounded or unusable.
	if (m_limits.maximumOwners == 0U) m_limits.maximumOwners = 1U;
}

MarkerService::~MarkerService()
{
	(void)Stop();
}

bool MarkerOwner::IsValid() const noexcept
{
	return generation != 0 && MarkerService::IsValidStableId(id);
}

bool MarkerCollectionIdentity::IsValid() const noexcept
{
	return owner.IsValid() && MarkerService::IsValidStableId(id);
}

bool MarkerRange::IsValid() const noexcept
{
	return endLine > startLine || (endLine == startLine && endColumn >= startColumn);
}

bool MarkerOperationResult::Succeeded() const noexcept
{
	return status == EMarkerOperationStatus::Replaced || status == EMarkerOperationStatus::Deleted
		|| status == EMarkerOperationStatus::CollectionCleared || status == EMarkerOperationStatus::OwnerDisposed
		|| status == EMarkerOperationStatus::Stopped;
}

bool MarkerService::CollectionKey::operator<(const CollectionKey& right) const noexcept
{
	return std::tie(ownerId, generation, collectionId) < std::tie(right.ownerId, right.generation, right.collectionId);
}

bool MarkerService::IsValidStableId(const std::string_view value) noexcept
{
	return IsValidUtf8Text(value, false, 160U);
}

MarkerOperationResult MarkerService::ResultLocked(const EMarkerOperationStatus status) const noexcept
{
	return { status, m_revision };
}

bool MarkerService::IsExpectedRevisionCurrentLocked(const std::optional<std::uint64_t>& expectedRevision) const noexcept
{
	return !expectedRevision || *expectedRevision == m_revision;
}

bool MarkerService::CanAdvanceRevisionLocked() const noexcept
{
	return m_revision != std::numeric_limits<std::uint64_t>::max();
}

bool MarkerService::IsStopped() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_stopped;
}

void MarkerService::AdvanceRevisionLocked() noexcept
{
	++m_revision;
}

void MarkerService::RemoveOwnerDataLocked(const MarkerOwner& owner) noexcept
{
	for (auto collection = m_collections.begin(); collection != m_collections.end();) {
		if (collection->second.identity.owner == owner) {
			collection = m_collections.erase(collection);
		} else {
			++collection;
		}
	}
}

void MarkerService::SaturatingIncrement(std::uint64_t& value) noexcept
{
	if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

bool MarkerService::EnqueueNotificationLocked(MarkerChange change) noexcept
{
	try {
		std::lock_guard notificationLock(m_notificationMutex);
		if (m_notificationQueue.size() >= kMaximumPendingNotifications) {
			// Notifications are advisory. The bounded model mutation remains
			// observable through Snapshot even when a reentrant listener floods
			// the delivery queue.
			SaturatingIncrement(m_droppedNotificationCount);
			return false;
		}
		m_notificationQueue.push_back(std::move(change));
		if (m_dispatchingNotifications) return false;
		m_dispatchingNotifications = true;
		return true;
	} catch (...) {
		// Notification allocation is advisory; the already-committed model result remains observable by Snapshot().
		SaturatingIncrement(m_droppedNotificationCount);
		return false;
	}
}

bool MarkerService::IsNotificationDispatchThreadLocked() const noexcept
{
	return m_dispatchingNotifications && m_notificationDispatchThreadId == std::this_thread::get_id();
}

bool MarkerService::WaitForNotificationDrain() noexcept
{
	std::unique_lock lock(m_notificationMutex);
	if (IsNotificationDispatchThreadLocked()) return true;
	try {
		m_notificationDrained.wait(lock, [this] { return !m_dispatchingNotifications; });
	} catch (...) {
		return m_dispatchingNotifications;
	}
	return false;
}

void MarkerService::DrainNotifications() noexcept
{
	{
		std::lock_guard notificationLock(m_notificationMutex);
		if (!m_dispatchingNotifications) return;
		if (m_notificationDispatchThreadId != std::thread::id{}) return;
		m_notificationDispatchThreadId = std::this_thread::get_id();
	}
	for (;;) {
		MarkerChange change;
		{
			std::lock_guard notificationLock(m_notificationMutex);
			if (m_notificationQueue.empty()) {
				m_dispatchingNotifications = false;
				m_notificationDispatchThreadId = {};
				m_notificationDrained.notify_all();
				return;
			}
			change = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}

		std::vector<MarkerChangeListener> listeners;
		try {
			std::lock_guard lock(m_mutex);
			if (m_stopped) continue;
			listeners.reserve(m_listeners.size());
			for (const auto& listener : m_listeners) listeners.push_back(listener.second);
		} catch (...) {
			// A listener-copy allocation failure cannot unwind through a mutating caller.
			continue;
		}
		for (const auto& listener : listeners) {
			try {
				listener(change);
			} catch (...) {
				// Observers are advisory and cannot affect committed marker state.
			}
		}
	}
}

MarkerOperationResult MarkerService::Replace(const ReplaceMarkersRequest& request)
{
	if (IsStopped()) return { EMarkerOperationStatus::Stopped, Revision() };
	if (!request.collection.owner.IsValid()) return { EMarkerOperationStatus::InvalidOwner, Revision() };
	if (!request.collection.IsValid()) return { EMarkerOperationStatus::InvalidCollection, Revision() };
	if (!IsValidResource(request.resource)) return { EMarkerOperationStatus::InvalidResource, Revision() };
	if (request.markers.size() > kMaximumMarkersPerResource) return { EMarkerOperationStatus::MaximumMarkersExceeded, Revision() };

	std::size_t payloadBytes{};
	for (const auto& marker : request.markers) {
		if (marker.message.size() > kMaximumMarkerMessageBytes
			|| (marker.code && marker.code->size() > kMaximumMarkerMetadataBytes)
			|| (marker.source && marker.source->size() > kMaximumMarkerMetadataBytes)) {
			return { EMarkerOperationStatus::MaximumPayloadExceeded, Revision() };
		}
		if (!marker.range.IsValid() || !IsValidSeverity(marker.severity)
			|| !IsValidUtf8Text(marker.message, true, kMaximumMarkerMessageBytes)
			|| (marker.code && !IsValidUtf8Text(*marker.code, true, kMaximumMarkerMetadataBytes))
			|| (marker.source && !IsValidUtf8Text(*marker.source, true, kMaximumMarkerMetadataBytes))) {
			return { EMarkerOperationStatus::InvalidMarker, Revision() };
		}
		payloadBytes += marker.message.size() + (marker.code ? marker.code->size() : 0U) + (marker.source ? marker.source->size() : 0U);
		if (payloadBytes > kMaximumPayloadBytes) return { EMarkerOperationStatus::MaximumPayloadExceeded, Revision() };
	}

	bool dispatch{};
	MarkerOperationResult result;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return ResultLocked(EMarkerOperationStatus::Stopped);
		const auto knownOwner = m_ownerGenerations.find(request.collection.owner.id);
		if (knownOwner != m_ownerGenerations.end() && (request.collection.owner.generation < knownOwner->second
			|| (request.collection.owner.generation == knownOwner->second
				&& m_disposedOwnerGenerations.find(request.collection.owner.id) != m_disposedOwnerGenerations.end()))) {
			return ResultLocked(EMarkerOperationStatus::StaleGeneration);
		}
		if (!IsExpectedRevisionCurrentLocked(request.expectedRevision)) return ResultLocked(EMarkerOperationStatus::StaleRevision);
		if (!CanAdvanceRevisionLocked()) return ResultLocked(EMarkerOperationStatus::RevisionExhausted);
		const bool adoptsNewGeneration = knownOwner != m_ownerGenerations.end()
			&& request.collection.owner.generation > knownOwner->second;
		const auto oldOwner = knownOwner != m_ownerGenerations.end()
			? MarkerOwner { request.collection.owner.id, knownOwner->second }
			: MarkerOwner {};
		const auto oldOwnerCollectionCount = adoptsNewGeneration
			? static_cast<std::size_t>(std::count_if(m_collections.begin(), m_collections.end(), [&oldOwner](const auto& entry) {
				return entry.second.identity.owner == oldOwner;
			}))
			: 0U;
		const CollectionKey collectionKey { request.collection.owner.id, request.collection.owner.generation, request.collection.id };
		const auto resourceKey = platform::uri::UriIdentityService::MakeComparisonKey(request.resource);
		if (request.markers.empty()) {
			const auto collection = m_collections.find(collectionKey);
			const bool deletedResource = collection != m_collections.end() && collection->second.resources.find(resourceKey) != collection->second.resources.end();
			if (!deletedResource && !adoptsNewGeneration) {
				return ResultLocked(EMarkerOperationStatus::NotApplicable);
			}
			if (adoptsNewGeneration) RemoveOwnerDataLocked(oldOwner);
			if (deletedResource) {
				auto activeCollection = m_collections.find(collectionKey);
				activeCollection->second.resources.erase(resourceKey);
				if (activeCollection->second.resources.empty()) m_collections.erase(activeCollection);
			}
			m_ownerGenerations[request.collection.owner.id] = request.collection.owner.generation;
			m_disposedOwnerGenerations.erase(request.collection.owner.id);
			AdvanceRevisionLocked();
			result = ResultLocked(EMarkerOperationStatus::Deleted);
			dispatch = EnqueueNotificationLocked({ m_revision, EMarkerChangeKind::Deleted, request.collection.owner, request.collection, request.resource });
		} else {
			if (knownOwner == m_ownerGenerations.end() && m_ownerGenerations.size() >= m_limits.maximumOwners) {
				return ResultLocked(EMarkerOperationStatus::MaximumOwnersExceeded);
			}
			auto collection = m_collections.find(collectionKey);
			if (collection == m_collections.end()) {
				const auto effectiveCollectionCount = m_collections.size() - oldOwnerCollectionCount;
				if (effectiveCollectionCount >= kMaximumCollections) return ResultLocked(EMarkerOperationStatus::MaximumCollectionsExceeded);
				if (adoptsNewGeneration) RemoveOwnerDataLocked(oldOwner);
				collection = m_collections.emplace(collectionKey, CollectionState { request.collection, {} }).first;
			}
			if (collection->second.resources.find(resourceKey) == collection->second.resources.end()
				&& collection->second.resources.size() >= kMaximumResourcesPerCollection) {
				return ResultLocked(EMarkerOperationStatus::MaximumResourcesExceeded);
			}
			m_ownerGenerations[request.collection.owner.id] = request.collection.owner.generation;
			m_disposedOwnerGenerations.erase(request.collection.owner.id);
			auto markers = request.markers;
			std::sort(markers.begin(), markers.end(), IsMarkerOrderBefore);
			collection->second.resources.insert_or_assign(resourceKey, StoredResource { request.resource, std::move(markers) });
			AdvanceRevisionLocked();
			result = ResultLocked(EMarkerOperationStatus::Replaced);
			dispatch = EnqueueNotificationLocked({ m_revision, EMarkerChangeKind::Replaced, request.collection.owner, request.collection, request.resource });
		}
	}
	if (dispatch) DrainNotifications();
	return result;
}

MarkerOperationResult MarkerService::Delete(const DeleteMarkersRequest& request)
{
	if (IsStopped()) return { EMarkerOperationStatus::Stopped, Revision() };
	if (!request.collection.owner.IsValid()) return { EMarkerOperationStatus::InvalidOwner, Revision() };
	if (!request.collection.IsValid()) return { EMarkerOperationStatus::InvalidCollection, Revision() };
	if (!IsValidResource(request.resource)) return { EMarkerOperationStatus::InvalidResource, Revision() };
	bool dispatch{};
	MarkerOperationResult result;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return ResultLocked(EMarkerOperationStatus::Stopped);
		const auto owner = m_ownerGenerations.find(request.collection.owner.id);
		if (owner == m_ownerGenerations.end()) return ResultLocked(EMarkerOperationStatus::NotApplicable);
		if (owner->second != request.collection.owner.generation) return ResultLocked(EMarkerOperationStatus::StaleGeneration);
		if (m_disposedOwnerGenerations.find(request.collection.owner.id) != m_disposedOwnerGenerations.end()) {
			return ResultLocked(EMarkerOperationStatus::NotApplicable);
		}
		if (!IsExpectedRevisionCurrentLocked(request.expectedRevision)) return ResultLocked(EMarkerOperationStatus::StaleRevision);
		if (!CanAdvanceRevisionLocked()) return ResultLocked(EMarkerOperationStatus::RevisionExhausted);
		const CollectionKey collectionKey { request.collection.owner.id, request.collection.owner.generation, request.collection.id };
		const auto collection = m_collections.find(collectionKey);
		if (collection == m_collections.end()) return ResultLocked(EMarkerOperationStatus::NotApplicable);
		const auto resourceKey = platform::uri::UriIdentityService::MakeComparisonKey(request.resource);
		if (collection->second.resources.erase(resourceKey) == 0U) return ResultLocked(EMarkerOperationStatus::NotApplicable);
		if (collection->second.resources.empty()) m_collections.erase(collection);
		AdvanceRevisionLocked();
		result = ResultLocked(EMarkerOperationStatus::Deleted);
		dispatch = EnqueueNotificationLocked({ m_revision, EMarkerChangeKind::Deleted, request.collection.owner, request.collection, request.resource });
	}
	if (dispatch) DrainNotifications();
	return result;
}

MarkerOperationResult MarkerService::ClearCollection(const ClearCollectionRequest& request)
{
	if (IsStopped()) return { EMarkerOperationStatus::Stopped, Revision() };
	if (!request.collection.owner.IsValid()) return { EMarkerOperationStatus::InvalidOwner, Revision() };
	if (!request.collection.IsValid()) return { EMarkerOperationStatus::InvalidCollection, Revision() };
	bool dispatch{};
	MarkerOperationResult result;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return ResultLocked(EMarkerOperationStatus::Stopped);
		const auto owner = m_ownerGenerations.find(request.collection.owner.id);
		if (owner == m_ownerGenerations.end()) return ResultLocked(EMarkerOperationStatus::NotApplicable);
		if (owner->second != request.collection.owner.generation) return ResultLocked(EMarkerOperationStatus::StaleGeneration);
		if (m_disposedOwnerGenerations.find(request.collection.owner.id) != m_disposedOwnerGenerations.end()) {
			return ResultLocked(EMarkerOperationStatus::NotApplicable);
		}
		if (!IsExpectedRevisionCurrentLocked(request.expectedRevision)) return ResultLocked(EMarkerOperationStatus::StaleRevision);
		if (!CanAdvanceRevisionLocked()) return ResultLocked(EMarkerOperationStatus::RevisionExhausted);
		const CollectionKey collectionKey { request.collection.owner.id, request.collection.owner.generation, request.collection.id };
		const auto collection = m_collections.find(collectionKey);
		if (collection == m_collections.end() || collection->second.resources.empty()) {
			return ResultLocked(EMarkerOperationStatus::NotApplicable);
		}
		m_collections.erase(collection);
		AdvanceRevisionLocked();
		result = ResultLocked(EMarkerOperationStatus::CollectionCleared);
		dispatch = EnqueueNotificationLocked(
			{ m_revision, EMarkerChangeKind::CollectionCleared, request.collection.owner, request.collection, std::nullopt });
	}
	if (dispatch) DrainNotifications();
	return result;
}

MarkerOperationResult MarkerService::DisposeOwner(const DisposeMarkerOwnerRequest& request)
{
	if (IsStopped()) return { EMarkerOperationStatus::Stopped, Revision() };
	if (!request.owner.IsValid()) return { EMarkerOperationStatus::InvalidOwner, Revision() };
	bool dispatch{};
	MarkerOperationResult result;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return ResultLocked(EMarkerOperationStatus::Stopped);
		const auto knownOwner = m_ownerGenerations.find(request.owner.id);
		if (knownOwner == m_ownerGenerations.end()) return ResultLocked(EMarkerOperationStatus::NotApplicable);
		if (knownOwner->second != request.owner.generation) return ResultLocked(EMarkerOperationStatus::StaleGeneration);
		if (m_disposedOwnerGenerations.find(request.owner.id) != m_disposedOwnerGenerations.end()) {
			return ResultLocked(EMarkerOperationStatus::NotApplicable);
		}
		if (!IsExpectedRevisionCurrentLocked(request.expectedRevision)) return ResultLocked(EMarkerOperationStatus::StaleRevision);
		if (!CanAdvanceRevisionLocked()) return ResultLocked(EMarkerOperationStatus::RevisionExhausted);
		RemoveOwnerDataLocked(request.owner);
		m_disposedOwnerGenerations[request.owner.id] = request.owner.generation;
		AdvanceRevisionLocked();
		result = ResultLocked(EMarkerOperationStatus::OwnerDisposed);
		dispatch = EnqueueNotificationLocked({ m_revision, EMarkerChangeKind::OwnerDisposed, request.owner, std::nullopt, std::nullopt });
	}
	if (dispatch) DrainNotifications();
	return result;
}

MarkerOperationResult MarkerService::Stop() noexcept
{
	MarkerOperationResult result;
	{
		std::scoped_lock lock(m_mutex, m_notificationMutex);
		if (m_stopped) {
			// Repeated Stop shares the terminal result after waiting for the first
			// caller's dispatch cleanup, matching the other workbench authorities.
			result = ResultLocked(EMarkerOperationStatus::Stopped);
		} else {
			if (CanAdvanceRevisionLocked()) AdvanceRevisionLocked();
			m_stopped = true;
			m_ownerGenerations.clear();
			m_disposedOwnerGenerations.clear();
			m_collections.clear();
			m_listeners.clear();
			m_notificationQueue.clear();
			result = ResultLocked(EMarkerOperationStatus::Stopped);
		}
	}
	result.callbackDrainDeferred = WaitForNotificationDrain();
	return result;
}

std::uint64_t MarkerService::Revision() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_revision;
}

ProblemsSnapshot MarkerService::Snapshot(const ProblemsSnapshotQuery& query) const
{
	std::scoped_lock lock(m_mutex, m_notificationMutex);
	ProblemsSnapshot snapshot { .revision = m_revision, .stopped = m_stopped, .droppedNotificationCount = m_droppedNotificationCount };
	if (m_stopped || (query.owner && !query.owner->IsValid()) || (query.collection && !query.collection->IsValid())) return snapshot;
	for (const auto& collection : m_collections) {
		const auto& identity = collection.second.identity;
		if (query.owner && identity.owner != *query.owner) continue;
		if (query.collection && identity != *query.collection) continue;
		for (const auto& resource : collection.second.resources) {
			if (query.resource && !IsResourceMatch(resource.second.resource, *query.resource)) continue;
			ProblemResourceSnapshot entry { resource.second.resource, identity, {} };
			for (const auto& marker : resource.second.markers) {
				if (query.maximumSeverity && static_cast<std::uint8_t>(marker.severity) > static_cast<std::uint8_t>(*query.maximumSeverity)) continue;
				entry.markers.push_back(marker);
			}
			if (!entry.markers.empty()) snapshot.resources.push_back(std::move(entry));
		}
	}
	std::sort(snapshot.resources.begin(), snapshot.resources.end(), [](const auto& left, const auto& right) {
		const auto leftKey = platform::uri::UriIdentityService::MakeComparisonKey(left.resource);
		const auto rightKey = platform::uri::UriIdentityService::MakeComparisonKey(right.resource);
		return std::tie(leftKey, left.collection.owner.id, left.collection.owner.generation, left.collection.id)
			< std::tie(rightKey, right.collection.owner.id, right.collection.owner.generation, right.collection.id);
	});
	return snapshot;
}

MarkerSubscriptionResult MarkerService::Subscribe(MarkerChangeListener listener)
{
	if (!listener) return { EMarkerSubscriptionStatus::InvalidListener, std::nullopt };
	std::lock_guard lock(m_mutex);
	if (m_stopped) return { EMarkerSubscriptionStatus::Stopped, std::nullopt };
	if (m_listeners.size() >= kMaximumSubscriptions) return { EMarkerSubscriptionStatus::SubscriptionLimitExceeded, std::nullopt };
	if (m_nextSubscriptionId == std::numeric_limits<MarkerSubscriptionId>::max()) {
		return { EMarkerSubscriptionStatus::RevisionExhausted, std::nullopt };
	}
	const auto id = m_nextSubscriptionId++;
	m_listeners.emplace(id, std::move(listener));
	return { EMarkerSubscriptionStatus::Subscribed, id };
}

void MarkerService::Unsubscribe(const MarkerSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_mutex);
	m_listeners.erase(subscriptionId);
}

} // namespace workbench::problems
