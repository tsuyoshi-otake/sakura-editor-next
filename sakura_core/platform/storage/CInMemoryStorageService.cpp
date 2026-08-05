/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/storage/CInMemoryStorageService.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <set>
#include <utility>

namespace platform::storage {
namespace {

constexpr std::size_t kEncodedSnapshotHeaderBytes = sizeof(std::uint64_t) * 2 + sizeof(std::uint32_t);
constexpr std::size_t kEncodedAddressFixedBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) * 3;
constexpr std::size_t kEncodedEntryFixedBytes = kEncodedAddressFixedBytes + sizeof(std::uint8_t)
	+ sizeof(std::uint32_t) + sizeof(std::uint64_t);
constexpr std::size_t kEncodedMutationFixedBytes = kEncodedAddressFixedBytes + sizeof(std::uint8_t) * 2;

[[nodiscard]] bool CheckedAdd(std::size_t& value, std::size_t addend, std::size_t maximum) noexcept
{
	if (value > maximum || addend > maximum - value) {
		return false;
	}
	value += addend;
	return true;
}

[[nodiscard]] std::size_t EncodedEntryBytes(const StorageEntry& entry) noexcept
{
	return kEncodedEntryFixedBytes + entry.address.scopeId.size() + entry.address.owner.size()
		+ entry.address.key.size() + entry.value.size();
}

struct StorageSubscriptionSlot {
	explicit StorageSubscriptionSlot(StorageChangeCallback listener)
		: callback(std::move(listener))
	{
	}

	std::atomic_bool active = true;
	StorageChangeCallback callback;
};

class CStorageChangeSubscription final : public IStorageChangeSubscription {
public:
	CStorageChangeSubscription(std::weak_ptr<StorageSubscriptionState> state, std::uint64_t subscriptionId) noexcept
		: m_state(std::move(state))
		, m_subscriptionId(subscriptionId)
	{
	}

	~CStorageChangeSubscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;

private:
	std::weak_ptr<StorageSubscriptionState> m_state;
	std::uint64_t m_subscriptionId = 0;
};

} // namespace

struct StorageSubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::uint64_t nextSubscriptionId = 1;
	std::map<std::uint64_t, std::shared_ptr<StorageSubscriptionSlot>> slots;
};

namespace {

void CStorageChangeSubscription::Unsubscribe() noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			m_subscriptionId = 0;
			return;
		}
		std::scoped_lock lock(state->mutex);
		if (const auto subscription = state->slots.find(m_subscriptionId);
			subscription != state->slots.end()) {
			subscription->second->active.store(false, std::memory_order_release);
			state->slots.erase(subscription);
		}
		m_subscriptionId = 0;
	} catch (...) {
		m_subscriptionId = 0;
	}
}

bool CStorageChangeSubscription::IsSubscribed() const noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			return false;
		}
		std::scoped_lock lock(state->mutex);
		if (state->closed) {
			return false;
		}
		const auto subscription = state->slots.find(m_subscriptionId);
		return subscription != state->slots.end()
			&& subscription->second->active.load(std::memory_order_acquire);
	} catch (...) {
		return false;
	}
}

void DeliverNotification(const std::shared_ptr<StorageSubscriptionState>& state,
	const StorageChangeBatch& batch)
{
	std::vector<std::shared_ptr<StorageSubscriptionSlot>> listeners;
	{
		std::scoped_lock lock(state->mutex);
		if (state->closed) {
			return;
		}
		listeners.reserve(state->slots.size());
		for (const auto& [subscriptionId, slot] : state->slots) {
			(void)subscriptionId;
			listeners.push_back(slot);
		}
	}

	for (const auto& listener : listeners) {
		if (!listener->active.load(std::memory_order_acquire)) {
			continue;
		}
		try {
			listener->callback(batch);
		} catch (...) {
			// One client callback cannot block the remaining listeners.
		}
	}
}

} // namespace

CInMemoryStorageService::CInMemoryStorageService(
	std::uint64_t generation,
	std::size_t maxCompletedOperations,
	std::uint64_t initialRevision) noexcept
	: m_generation(generation == 0 ? 1 : generation)
	, m_revision(initialRevision)
	, m_snapshotPayloadBytes(kEncodedSnapshotHeaderBytes)
	, m_maxCompletedOperations(std::max<std::size_t>(1, maxCompletedOperations))
	, m_subscriptionState(std::make_shared<StorageSubscriptionState>())
{
}

CInMemoryStorageService::~CInMemoryStorageService()
{
	{
		std::scoped_lock lock(m_subscriptionState->mutex);
		m_subscriptionState->closed = true;
		for (const auto& [subscriptionId, slot] : m_subscriptionState->slots) {
			(void)subscriptionId;
			slot->active.store(false, std::memory_order_release);
		}
		m_subscriptionState->slots.clear();
	}
	std::scoped_lock notificationLock(m_notificationMutex);
	m_notificationQueue.clear();
}

StorageAuthorityOpenResult CInMemoryStorageService::Open()
{
	std::lock_guard lock(m_mutex);
	if (m_open) return { EStorageAuthorityOpenStatus::AlreadyOpen, "in-memory storage is already open" };
	m_open = true;
	return { EStorageAuthorityOpenStatus::Opened, {} };
}

void CInMemoryStorageService::Close() noexcept
{
	std::lock_guard lock(m_mutex);
	m_open = false;
}

bool CInMemoryStorageService::IsOpen() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_open;
}

StorageMutationResult CInMemoryStorageService::ValidateRequest(const StorageMutationRequest& request) const
{
	if (request.operationId.empty() || request.operationId.size() > kMaximumStorageOperationIdBytes
		|| !IsValidStorageUtf8(request.operationId, false)) {
		return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
			.diagnostic = "storage operationId is invalid" };
	}
	if (request.mutations.empty()) {
		return { .status = EStorageMutationStatus::NotApplicable, .revision = m_revision,
			.diagnostic = "storage mutation batch is empty" };
	}
	if (request.mutations.size() > kMaximumStorageItems) {
		return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
			.diagnostic = "storage mutation batch exceeds the item limit" };
	}

	std::set<StorageAddress> addresses;
	std::size_t encodedBytes = sizeof(std::uint32_t) + sizeof(std::uint32_t) + request.operationId.size()
		+ (request.expectedRevision ? sizeof(std::uint64_t) : 0);
	for (const auto& mutation : request.mutations) {
		if (!mutation.address.IsValid()) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage address is invalid" };
		}
		if (!addresses.emplace(mutation.address).second) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage batch contains a duplicate address" };
		}
		if (!IsValidStorageTarget(mutation.target)) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage target is invalid" };
		}
		if (mutation.value && (mutation.value->size() > kMaximumStorageStringBytes
			|| !IsValidStorageUtf8(*mutation.value))) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage value is invalid" };
		}
		const std::size_t stringBytes = mutation.address.scopeId.size() + mutation.address.owner.size()
			+ mutation.address.key.size() + (mutation.value ? mutation.value->size() : 0);
		if (!CheckedAdd(encodedBytes, kEncodedMutationFixedBytes, kMaximumStorageMutationPayloadBytes)
			|| !CheckedAdd(encodedBytes, stringBytes, kMaximumStorageMutationPayloadBytes)) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage mutation batch exceeds the byte limit" };
		}
	}

	return { .status = EStorageMutationStatus::Succeeded, .revision = m_revision };
}

StorageMutationResult CInMemoryStorageService::Apply(const StorageMutationRequest& request)
{
	std::unique_lock lock(m_mutex);

	if (const auto completed = m_completedOperations.find(request.operationId);
		completed != m_completedOperations.end()) {
		if (!(completed->second.request == request)) {
			return { .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage operationId was reused with a different request" };
		}
		auto replay = completed->second.result;
		replay.replayed = true;
		return replay;
	}

	auto validation = ValidateRequest(request);
	if (validation.status != EStorageMutationStatus::Succeeded) {
		if (validation.status == EStorageMutationStatus::NotApplicable) {
			RememberCompleted(request, validation);
		}
		return validation;
	}

	if (request.expectedRevision && *request.expectedRevision != m_revision) {
		StorageMutationResult conflict{ .status = EStorageMutationStatus::Conflict, .revision = m_revision,
			.diagnostic = "storage revision conflict" };
		RememberCompleted(request, conflict);
		return conflict;
	}

	std::vector<StorageChange> changes;
	changes.reserve(request.mutations.size());
	for (const auto& mutation : request.mutations) {
		const auto existing = m_entries.find(mutation.address);
		if (!mutation.value) {
			if (existing != m_entries.end()) {
				changes.push_back({ .address = mutation.address,
					.target = existing->second.target, .entry = std::nullopt });
			}
			continue;
		}

		if (existing != m_entries.end()
			&& existing->second.target == mutation.target
			&& existing->second.value == *mutation.value) {
			continue;
		}
		changes.push_back({ .address = mutation.address, .target = mutation.target,
			.entry = StorageEntry{ .address = mutation.address, .target = mutation.target,
				.value = *mutation.value } });
	}

	if (changes.empty()) {
		StorageMutationResult noChange{ .status = EStorageMutationStatus::NotApplicable, .revision = m_revision,
			.diagnostic = "storage mutation does not change state" };
		RememberCompleted(request, noChange);
		return noChange;
	}
	if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
		StorageMutationResult exhausted{ .status = EStorageMutationStatus::Failed, .revision = m_revision,
			.diagnostic = "storage revision is exhausted" };
		RememberCompleted(request, exhausted);
		return exhausted;
	}

	std::size_t projectedEntryCount = m_entries.size();
	std::size_t projectedPayloadBytes = m_snapshotPayloadBytes;
	for (const auto& change : changes) {
		const auto existing = m_entries.find(change.address);
		if (existing != m_entries.end()) {
			const auto oldBytes = EncodedEntryBytes(existing->second);
			if (oldBytes > projectedPayloadBytes) {
				StorageMutationResult invalidState{ .status = EStorageMutationStatus::Failed, .revision = m_revision,
					.diagnostic = "storage state accounting is invalid" };
				RememberCompleted(request, invalidState);
				return invalidState;
			}
			projectedPayloadBytes -= oldBytes;
		} else if (change.entry) {
			++projectedEntryCount;
		}
		if (!change.entry) {
			if (existing != m_entries.end()) {
				--projectedEntryCount;
			}
			continue;
		}
		if (!CheckedAdd(projectedPayloadBytes, EncodedEntryBytes(*change.entry),
			kMaximumStorageSnapshotPayloadBytes)) {
			StorageMutationResult capacity{ .status = EStorageMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "storage snapshot exceeds the byte limit" };
			RememberCompleted(request, capacity);
			return capacity;
		}
	}
	if (projectedEntryCount > kMaximumStorageItems) {
		StorageMutationResult capacity{ .status = EStorageMutationStatus::Failed, .revision = m_revision,
			.diagnostic = "storage snapshot exceeds the item limit" };
		RememberCompleted(request, capacity);
		return capacity;
	}

	const std::uint64_t baseRevision = m_revision;
	++m_revision;
	m_snapshotPayloadBytes = projectedPayloadBytes;
	for (auto& change : changes) {
		if (!change.entry) {
			m_entries.erase(change.address);
			continue;
		}
		change.entry->revision = m_revision;
		m_entries.insert_or_assign(change.address, *change.entry);
	}

	StorageChangeBatch batch{ .generation = m_generation, .baseRevision = baseRevision,
		.revision = m_revision, .changes = std::move(changes) };
	StorageMutationResult success{ .status = EStorageMutationStatus::Succeeded, .revision = m_revision,
		.changeBatch = std::move(batch) };
	RememberCompleted(request, success);
	const bool startsNotificationDrain = EnqueueNotification(*success.changeBatch);
	lock.unlock();
	if (startsNotificationDrain) {
		DrainNotifications();
	}
	return success;
}

std::unique_ptr<IStorageChangeSubscription> CInMemoryStorageService::Subscribe(
	StorageChangeCallback callback)
{
	if (!callback) {
		return nullptr;
	}

	std::scoped_lock lock(m_subscriptionState->mutex);
	if (m_subscriptionState->closed) {
		return nullptr;
	}
	if (m_subscriptionState->nextSubscriptionId == 0) {
		return nullptr;
	}
	const std::uint64_t subscriptionId = m_subscriptionState->nextSubscriptionId++;
	m_subscriptionState->slots.emplace(subscriptionId,
		std::make_shared<StorageSubscriptionSlot>(std::move(callback)));
	return std::make_unique<CStorageChangeSubscription>(m_subscriptionState, subscriptionId);
}

bool CInMemoryStorageService::EnqueueNotification(const StorageChangeBatch& batch)
{
	std::scoped_lock lock(m_notificationMutex);
	m_notificationQueue.push_back(batch);
	if (m_dispatchingNotifications) {
		return false;
	}
	m_dispatchingNotifications = true;
	return true;
}

void CInMemoryStorageService::DrainNotifications()
{
	for (;;) {
		StorageChangeBatch batch;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) {
				m_dispatchingNotifications = false;
				return;
			}
			batch = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		DeliverNotification(m_subscriptionState, batch);
	}
}

void CInMemoryStorageService::RememberCompleted(
	const StorageMutationRequest& request,
	const StorageMutationResult& result)
{
	const auto [entry, inserted] = m_completedOperations.emplace(
		request.operationId, CompletedOperation{ request, result });
	(void)entry;
	if (!inserted) {
		return;
	}

	m_completedOperationOrder.push_back(request.operationId);
	while (m_completedOperationOrder.size() > m_maxCompletedOperations) {
		m_completedOperations.erase(m_completedOperationOrder.front());
		m_completedOperationOrder.pop_front();
	}
}

StorageSnapshot CInMemoryStorageService::Snapshot() const
{
	std::scoped_lock lock(m_mutex);
	StorageSnapshot snapshot{ .generation = m_generation, .revision = m_revision };
	snapshot.entries.reserve(m_entries.size());
	for (const auto& [address, entry] : m_entries) {
		(void)address;
		snapshot.entries.push_back(entry);
	}
	return snapshot;
}

std::optional<StorageEntry> CInMemoryStorageService::Find(const StorageAddress& address) const
{
	std::scoped_lock lock(m_mutex);
	const auto entry = m_entries.find(address);
	return entry == m_entries.end() ? std::nullopt : std::optional<StorageEntry>{ entry->second };
}

std::uint64_t CInMemoryStorageService::RestartGeneration() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_generation == std::numeric_limits<std::uint64_t>::max()) {
		return 0;
	}
	return ++m_generation;
}

} // namespace platform::storage
