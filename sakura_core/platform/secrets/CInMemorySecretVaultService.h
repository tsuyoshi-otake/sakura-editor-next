/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/ISecretVaultService.h"

#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace platform::secrets {

struct SecretVaultSubscriptionState;

/*! 
	@brief Deterministic per-profile authority semantics without persistent I/O.

	This class is only a contract/reference implementation. A later DPAPI backend
	must retain its profile binding, revision/CAS, replay, and event rules.
*/
class CInMemorySecretVaultService final : public ISecretVaultService {
public:
	explicit CInMemorySecretVaultService(std::string canonicalProfileId,
		std::size_t maxCompletedOperations = kMaximumSecretVaultCompletedOperations,
		std::size_t maxSubscriptions = kMaximumSecretVaultSubscriptions);
	~CInMemorySecretVaultService() override;

	[[nodiscard]] std::string_view GetProfileId() const noexcept override;
	[[nodiscard]] SecretGetResult Get(std::string_view extensionId, std::string_view key) const override;
	[[nodiscard]] SecretMutationResult Apply(const SecretMutationRequest& request) override;
	[[nodiscard]] std::unique_ptr<ISecretVaultChangeSubscription> Subscribe(
		SecretChangeCallback callback) override;
	[[nodiscard]] ESecretVaultStopStatus Stop() noexcept override;

private:
	struct CompletedOperation {
		SecretMutationRequest request;
		SecretMutationResult result;
	};

	[[nodiscard]] std::optional<SecretMutationRequest> CanonicalizeRequest(
		const SecretMutationRequest& request) const;
	[[nodiscard]] bool IsAvailableLocked() const noexcept;
	void RememberCompletedLocked(const SecretMutationRequest& request,
		const SecretMutationResult& result);
	//! Called while m_mutex is held, before the committer releases the state lock.
	[[nodiscard]] bool QueueNotificationLocked(const SecretChange& change) noexcept;
	void DrainNotifications() noexcept;

	mutable std::mutex m_mutex;
	std::string m_profileId;
	bool m_profileIdValid = false;
	bool m_stopped = false;
	std::uint64_t m_revision = 0;
	std::size_t m_maxCompletedOperations = kMaximumSecretVaultCompletedOperations;
	std::map<SecretAddress, SecretEntry> m_entries;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;

	std::shared_ptr<SecretVaultSubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<SecretChange> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

} // namespace platform::secrets
