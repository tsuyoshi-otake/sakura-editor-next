/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/CInMemorySecretVaultService.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
#include <vector>

namespace platform::secrets {
namespace {

struct SecretVaultSubscriptionSlot {
	explicit SecretVaultSubscriptionSlot(SecretChangeCallback listener)
		: callback(std::move(listener))
	{
	}

	std::atomic_bool active = true;
	SecretChangeCallback callback;
};

class CSecretVaultChangeSubscription final : public ISecretVaultChangeSubscription {
public:
	CSecretVaultChangeSubscription(
		std::weak_ptr<SecretVaultSubscriptionState> state, std::uint64_t subscriptionId) noexcept
		: m_state(std::move(state))
		, m_subscriptionId(subscriptionId)
	{
	}

	~CSecretVaultChangeSubscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;

private:
	std::weak_ptr<SecretVaultSubscriptionState> m_state;
	std::uint64_t m_subscriptionId = 0;
};

} // namespace

struct SecretVaultSubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::size_t maximumSubscriptions = kMaximumSecretVaultSubscriptions;
	std::uint64_t nextSubscriptionId = 1;
	std::map<std::uint64_t, std::shared_ptr<SecretVaultSubscriptionSlot>> slots;
};

namespace {

void CSecretVaultChangeSubscription::Unsubscribe() noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			m_subscriptionId = 0;
			return;
		}
		std::scoped_lock lock(state->mutex);
		if (const auto found = state->slots.find(m_subscriptionId); found != state->slots.end()) {
			found->second->active.store(false, std::memory_order_release);
			state->slots.erase(found);
		}
		m_subscriptionId = 0;
	} catch (...) {
		m_subscriptionId = 0;
	}
}

bool CSecretVaultChangeSubscription::IsSubscribed() const noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			return false;
		}
		std::scoped_lock lock(state->mutex);
		const auto found = state->slots.find(m_subscriptionId);
		return !state->closed && found != state->slots.end()
			&& found->second->active.load(std::memory_order_acquire);
	} catch (...) {
		return false;
	}
}

void DeliverNotification(const std::shared_ptr<SecretVaultSubscriptionState>& state,
	const SecretChange& change) noexcept
{
	std::vector<std::shared_ptr<SecretVaultSubscriptionSlot>> listeners;
	try {
		std::scoped_lock lock(state->mutex);
		if (state->closed) {
			return;
		}
		listeners.reserve(state->slots.size());
		for (const auto& [subscriptionId, slot] : state->slots) {
			(void)subscriptionId;
			listeners.push_back(slot);
		}
	} catch (...) {
		return;
	}

	for (const auto& listener : listeners) {
		if (!listener->active.load(std::memory_order_acquire)) {
			continue;
		}
		try {
			listener->callback(change);
		} catch (...) {
			// A consumer cannot prevent the remaining committed-event deliveries.
		}
	}
}

} // namespace

CInMemorySecretVaultService::CInMemorySecretVaultService(std::string canonicalProfileId,
	std::size_t maxCompletedOperations, std::size_t maxSubscriptions)
	: m_profileId(std::move(canonicalProfileId))
	, m_profileIdValid(profiles::IsCanonicalProfileAuthorityId(m_profileId))
	, m_maxCompletedOperations(std::clamp(maxCompletedOperations,
		std::size_t{ 1 }, kMaximumSecretVaultCompletedOperations))
	, m_subscriptionState(std::make_shared<SecretVaultSubscriptionState>())
{
	m_subscriptionState->maximumSubscriptions = std::clamp(maxSubscriptions,
		std::size_t{ 1 }, kMaximumSecretVaultSubscriptions);
}

CInMemorySecretVaultService::~CInMemorySecretVaultService()
{
	(void)Stop();
}

std::string_view CInMemorySecretVaultService::GetProfileId() const noexcept
{
	return m_profileId;
}

bool CInMemorySecretVaultService::IsAvailableLocked() const noexcept
{
	return m_profileIdValid && !m_stopped;
}

SecretGetResult CInMemorySecretVaultService::Get(std::string_view extensionId, std::string_view key) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(extensionId, canonicalExtensionId)
		|| !IsValidSecretVaultIdentifier(key, kMaximumSecretVaultKeyBytes)) {
		return { .status = ESecretGetStatus::Invalid };
	}
	std::scoped_lock lock(m_mutex);
	if (m_stopped) {
		return { .status = ESecretGetStatus::Stopped, .revision = m_revision };
	}
	if (!m_profileIdValid) {
		return { .status = ESecretGetStatus::Invalid, .revision = m_revision };
	}
	const SecretAddress address{ .extensionId = std::move(canonicalExtensionId), .key = std::string(key) };
	const auto found = m_entries.find(address);
	if (found == m_entries.end()) {
		return { .status = ESecretGetStatus::NotFound, .revision = m_revision };
	}
	return { .status = ESecretGetStatus::Found, .revision = m_revision, .value = found->second.value };
}

std::optional<SecretMutationRequest> CInMemorySecretVaultService::CanonicalizeRequest(
	const SecretMutationRequest& request) const
{
	std::string canonicalExtensionId;
	if (!CanonicalizeSecretVaultExtensionId(request.extensionId, canonicalExtensionId)
		|| !IsValidSecretVaultIdentifier(request.key, kMaximumSecretVaultKeyBytes)
		|| !IsValidSecretVaultIdentifier(request.operationId, kMaximumSecretVaultOperationIdBytes)
		|| (request.kind != ESecretMutationKind::Set && request.kind != ESecretMutationKind::Delete)
		|| (request.kind == ESecretMutationKind::Set
			&& (!IsValidSecretVaultUtf8(request.value) || request.value.size() > kMaximumSecretVaultValueBytes))
		|| (request.kind == ESecretMutationKind::Delete && !request.value.empty())) {
		return std::nullopt;
	}
	SecretMutationRequest canonical = request;
	canonical.extensionId = std::move(canonicalExtensionId);
	return canonical;
}

void CInMemorySecretVaultService::RememberCompletedLocked(const SecretMutationRequest& request,
	const SecretMutationResult& result)
{
	if (m_completedOperations.contains(request.operationId)) {
		return;
	}
	m_completedOperations.emplace(request.operationId, CompletedOperation{ request, result });
	m_completedOperationOrder.push_back(request.operationId);
	while (m_completedOperationOrder.size() > m_maxCompletedOperations) {
		m_completedOperations.erase(m_completedOperationOrder.front());
		m_completedOperationOrder.pop_front();
	}
}

bool CInMemorySecretVaultService::QueueNotificationLocked(const SecretChange& change) noexcept
{
	try {
		std::scoped_lock lock(m_notificationMutex);
		m_notificationQueue.push_back(change);
		if (m_dispatchingNotifications) {
			return false;
		}
		m_dispatchingNotifications = true;
		return true;
	} catch (...) {
		// State already committed. A later durable implementation will persist an
		// event ledger; this reference authority must still retain the mutation.
		return false;
	}
}

void CInMemorySecretVaultService::DrainNotifications() noexcept
{
	for (;;) {
		SecretChange next;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) {
				m_dispatchingNotifications = false;
				return;
			}
			next = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		DeliverNotification(m_subscriptionState, next);
	}
}

SecretMutationResult CInMemorySecretVaultService::Apply(const SecretMutationRequest& request)
{
	const auto canonicalRequest = CanonicalizeRequest(request);
	std::optional<SecretChange> change;
	bool shouldDrainNotifications = false;
	SecretMutationResult result;
	{
		std::scoped_lock lock(m_mutex);
		if (m_stopped) {
			return { .status = ESecretMutationStatus::Stopped, .revision = m_revision };
		}
		if (!m_profileIdValid || !canonicalRequest) {
			return { .status = ESecretMutationStatus::Invalid, .revision = m_revision,
				.diagnostic = "secret vault request is invalid" };
		}
		if (const auto completed = m_completedOperations.find(canonicalRequest->operationId);
			completed != m_completedOperations.end()) {
			if (completed->second.request != *canonicalRequest) {
				return { .status = ESecretMutationStatus::Invalid, .revision = m_revision,
					.diagnostic = "secret vault operationId payload collision" };
			}
			result = completed->second.result;
			result.replayed = true;
			result.change.reset();
			return result;
		}
		if (canonicalRequest->expectedRevision && *canonicalRequest->expectedRevision != m_revision) {
			result = { .status = ESecretMutationStatus::Conflict, .revision = m_revision };
			RememberCompletedLocked(*canonicalRequest, result);
			return result;
		}

		const SecretAddress address{ .extensionId = canonicalRequest->extensionId, .key = canonicalRequest->key };
		const auto found = m_entries.find(address);
		const bool present = found != m_entries.end();
		const bool effective = canonicalRequest->kind == ESecretMutationKind::Set
			? !present || found->second.value != canonicalRequest->value
			: present;
		if (!effective) {
			result = { .status = ESecretMutationStatus::NotApplicable, .revision = m_revision };
			RememberCompletedLocked(*canonicalRequest, result);
			return result;
		}
		if (m_revision == (std::numeric_limits<std::uint64_t>::max)()) {
			return { .status = ESecretMutationStatus::Failed, .revision = m_revision,
				.diagnostic = "secret vault revision is exhausted" };
		}
		++m_revision;
		if (canonicalRequest->kind == ESecretMutationKind::Set) {
			m_entries.insert_or_assign(address, SecretEntry{ .address = address,
				.value = canonicalRequest->value, .revision = m_revision });
		} else {
			m_entries.erase(found);
		}
		change = SecretChange{ .profileId = m_profileId, .address = address,
			.kind = canonicalRequest->kind == ESecretMutationKind::Set
				? ESecretChangeKind::Set : ESecretChangeKind::Delete,
			.revision = m_revision };
		result = { .status = ESecretMutationStatus::Succeeded, .revision = m_revision, .change = change };
		RememberCompletedLocked(*canonicalRequest, result);
		shouldDrainNotifications = QueueNotificationLocked(*change);
	}
	if (shouldDrainNotifications) {
		DrainNotifications();
	}
	return result;
}

std::unique_ptr<ISecretVaultChangeSubscription> CInMemorySecretVaultService::Subscribe(
	SecretChangeCallback callback)
{
	if (!callback) {
		return nullptr;
	}
	std::scoped_lock vaultLock(m_mutex);
	if (!IsAvailableLocked()) {
		return nullptr;
	}
	std::scoped_lock subscriptionLock(m_subscriptionState->mutex);
	if (m_subscriptionState->closed
		|| m_subscriptionState->slots.size() >= m_subscriptionState->maximumSubscriptions
		|| m_subscriptionState->nextSubscriptionId == 0) {
		return nullptr;
	}
	const auto subscriptionId = m_subscriptionState->nextSubscriptionId++;
	m_subscriptionState->slots.emplace(subscriptionId,
		std::make_shared<SecretVaultSubscriptionSlot>(std::move(callback)));
	return std::make_unique<CSecretVaultChangeSubscription>(m_subscriptionState, subscriptionId);
}

ESecretVaultStopStatus CInMemorySecretVaultService::Stop() noexcept
{
	try {
		std::scoped_lock vaultLock(m_mutex);
		if (m_stopped) {
			return ESecretVaultStopStatus::AlreadyStopped;
		}
		m_stopped = true;
		{
			std::scoped_lock subscriptionLock(m_subscriptionState->mutex);
			m_subscriptionState->closed = true;
			for (const auto& [subscriptionId, slot] : m_subscriptionState->slots) {
				(void)subscriptionId;
				slot->active.store(false, std::memory_order_release);
			}
			m_subscriptionState->slots.clear();
		}
		{
			std::scoped_lock notificationLock(m_notificationMutex);
			m_notificationQueue.clear();
		}
		// The reference authority does not retain plaintext after its terminal
		// stop. A durable backend must additionally zero transient plaintext
		// buffers after DPAPI/IPC operations.
		m_entries.clear();
		m_completedOperations.clear();
		m_completedOperationOrder.clear();
		return ESecretVaultStopStatus::Stopped;
	} catch (...) {
		// noexcept terminal path: preserve the externally safe terminal state.
		m_stopped = true;
		return ESecretVaultStopStatus::Stopped;
	}
}

} // namespace platform::secrets
