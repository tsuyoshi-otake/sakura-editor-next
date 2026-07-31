/*! @file
	@brief Bounded implementation of the Secret Vault extension grant authority.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/ISecretVaultExtensionGrantAuthority.h"

#include <mutex>

namespace platform::secrets {

class CSecretVaultExtensionGrantAuthority final : public ISecretVaultExtensionGrantAuthority {
public:
	explicit CSecretVaultExtensionGrantAuthority(std::string canonicalProfileId,
		std::uint64_t controlConnectionGeneration,
		SecretVaultExtensionGrantAuthorityConfig config = {});

	[[nodiscard]] std::string_view GetProfileId() const noexcept override;
	[[nodiscard]] std::uint64_t GetControlConnectionGeneration() const noexcept override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult ActivateOrReplace(
		const SecretVaultExtensionGrantAuthorityActivateRequest& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult RegisterEditorProcess(
		const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult UnregisterEditorProcess(
		const SecretVaultExtensionGrantAuthorityEditorLeaseMutation& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult ReplaceApprovedExtensions(
		const SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult DisableExtension(
		const SecretVaultExtensionGrantAuthorityDisableExtensionMutation& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult Deactivate(
		const SecretVaultExtensionGrantAuthoritySessionMutation& request) override;
	[[nodiscard]] SecretVaultExtensionGrantAuthorizationResult AuthorizeIssue(
		const SecretVaultExtensionGrantAuthorityIssueRequest& request) const override;
	[[nodiscard]] SecretVaultExtensionGrantRevokeAuthorizationResult AuthorizeRevokeSession(
		const SecretVaultExtensionGrantRevokeAuthorizationRequest& request) const override;
	[[nodiscard]] ESecretVaultExtensionGrantAuthorityStatus Stop() noexcept override;

private:
	struct ActiveSession {
		std::string hostSessionId;
		std::uint64_t hostGeneration = 0;
		bool enabled = false;
		std::vector<std::string> approvedExtensionIds;
		std::vector<std::uint32_t> editorProcessIds;
	};

	[[nodiscard]] bool IsConfigured() const noexcept;
	[[nodiscard]] bool MatchesActiveLocked(const SecretVaultExtensionGrantAuthoritySessionMutation& request) const noexcept;
	[[nodiscard]] SecretVaultExtensionGrantAuthorityResult CurrentResultLocked(
		ESecretVaultExtensionGrantAuthorityStatus status) const noexcept;
	void ClearActiveLocked() noexcept;

	mutable std::mutex m_mutex;
	std::string m_profileId;
	std::uint64_t m_controlConnectionGeneration = 0;
	SecretVaultExtensionGrantAuthorityConfig m_config;
	bool m_configurationValid = false;
	bool m_stopped = false;
	std::uint64_t m_revision = 0;
	std::uint64_t m_lastHostGeneration = 0;
	ActiveSession m_active;
};

} // namespace platform::secrets
