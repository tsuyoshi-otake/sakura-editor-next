/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/SecretVaultTypes.h"

#include <memory>
#include <string_view>

namespace platform::secrets {

/*! 
	@brief A per-profile secret authority, separate from general settings/storage.

	The control process owns an implementation. Editors and extension hosts use a
	narrow IPC bridge in a later slice; they must never create another writer.
*/
class ISecretVaultChangeSubscription {
public:
	virtual ~ISecretVaultChangeSubscription() = default;
	virtual void Unsubscribe() noexcept = 0;
	[[nodiscard]] virtual bool IsSubscribed() const noexcept = 0;
};

class ISecretVaultService {
public:
	virtual ~ISecretVaultService() = default;

	//! The immutable canonical profile identity to which this vault is bound.
	[[nodiscard]] virtual std::string_view GetProfileId() const noexcept = 0;
	[[nodiscard]] virtual SecretGetResult Get(std::string_view extensionId, std::string_view key) const = 0;
	//! Applies one revisioned Set or Delete. operationId supports exact replay only.
	[[nodiscard]] virtual SecretMutationResult Apply(const SecretMutationRequest& request) = 0;
	//! Null means an invalid callback, a stopped vault, or the subscription bound.
	[[nodiscard]] virtual std::unique_ptr<ISecretVaultChangeSubscription> Subscribe(
		SecretChangeCallback callback) = 0;
	//! Terminal and idempotent. No later Get/Apply/Subscribe can revive the vault.
	[[nodiscard]] virtual ESecretVaultStopStatus Stop() noexcept = 0;
};

} // namespace platform::secrets
