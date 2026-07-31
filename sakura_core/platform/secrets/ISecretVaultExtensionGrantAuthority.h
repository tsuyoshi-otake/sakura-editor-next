/*! @file
	@brief Platform-neutral, control-owned issue authorization contract.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/SecretVaultExtensionGrantAuthorityTypes.h"

#include <string_view>

namespace platform::secrets {

class ISecretVaultExtensionGrantAuthority {
public:
	virtual ~ISecretVaultExtensionGrantAuthority() = default;

	[[nodiscard]] virtual std::string_view GetProfileId() const noexcept = 0;
	[[nodiscard]] virtual std::uint64_t GetControlConnectionGeneration() const noexcept = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult ActivateOrReplace(
		const SecretVaultExtensionGrantAuthorityActivateRequest& request) = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult RegisterEditorProcess(
		const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request) = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult UnregisterEditorProcess(
		const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request) = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult ReplaceApprovedExtensions(
		const SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation& request) = 0;
	//! Removes only this exact canonical extension from the active approval set.
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult DisableExtension(
		const SecretVaultExtensionGrantAuthorityDisableExtensionMutation& request) = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorityResult Deactivate(
		const SecretVaultExtensionGrantAuthoritySessionMutation& request) = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantAuthorizationResult AuthorizeIssue(
		const SecretVaultExtensionGrantAuthorityIssueRequest& request) const = 0;
	[[nodiscard]] virtual SecretVaultExtensionGrantRevokeAuthorizationResult AuthorizeRevokeSession(
		const SecretVaultExtensionGrantRevokeAuthorizationRequest& request) const = 0;
	//! Terminal and idempotent. It clears all ephemeral state and forbids later activation.
	[[nodiscard]] virtual ESecretVaultExtensionGrantAuthorityStatus Stop() noexcept = 0;
};

} // namespace platform::secrets
