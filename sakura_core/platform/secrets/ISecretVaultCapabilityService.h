/*! @file
	@brief Narrow capability authority in front of a control-owned Secret Vault.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/SecretVaultCapabilityTypes.h"

#include <string_view>

namespace platform::secrets {

/*! 
	@brief Issues and validates bearer capabilities for Secret Vault IPC.

	Only the control process may own an implementation. The future extension-host
	IPC adapter must obtain its `SecretVaultCapabilityBinding` from an authenticated
	host connection and must call Validate before forwarding an address to
	ISecretVaultService. In particular, it must never use an extension ID supplied
	by a different extension as the authority for a capability.
*/
class ISecretVaultCapabilityService {
public:
	virtual ~ISecretVaultCapabilityService() = default;

	[[nodiscard]] virtual std::string_view GetProfileId() const noexcept = 0;
	[[nodiscard]] virtual SecretVaultCapabilityIssueResult Issue(
		const SecretVaultCapabilityIssueRequest& request) = 0;
	[[nodiscard]] virtual SecretVaultCapabilityValidationResult Validate(
		const SecretVaultCapabilityValidationRequest& request) = 0;
	//! Revokes every currently valid grant for exactly one canonical extension
	//! binding. It never widens a case-insensitive or partial identity.
	[[nodiscard]] virtual SecretVaultCapabilityRevokeResult RevokeExtension(
		const SecretVaultCapabilityBinding& binding) = 0;
	//! Revokes every grant bound to exactly one authenticated extension-host
	//! connection, regardless of extension identity.
	[[nodiscard]] virtual SecretVaultCapabilityRevokeResult RevokeSession(
		const SecretVaultCapabilitySessionIdentity& session) = 0;
	//! Control-process teardown only: revokes every editor PID session belonging
	//! to one exact extension-host connection. Callers must first fence new issue
	//! authorization in the grant authority.
	[[nodiscard]] virtual SecretVaultCapabilityRevokeResult RevokeHostSession(
		const SecretVaultCapabilityHostSessionIdentity& session) = 0;
	//! Terminal and idempotent. It invalidates every grant and no later issue or
	//! validation operation may revive the authority.
	[[nodiscard]] virtual ESecretVaultCapabilityStopStatus Stop() noexcept = 0;
};

} // namespace platform::secrets
