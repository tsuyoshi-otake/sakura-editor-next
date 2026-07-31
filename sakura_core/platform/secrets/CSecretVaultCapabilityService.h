/*! @file
	@brief Process-local control-owned Secret Vault capability authority.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/ISecretVaultCapabilityService.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace platform::secrets {

/*! 
	@brief Bounded, process-local authority for extension Secret Vault access.

	The service stores a SHA-256 digest only; issued plaintext bearer values leave
	the process-local authority in the Issue result and are not retained. It is a
	control composition component, not an editor or extension-host writer.
*/
class CSecretVaultCapabilityService final : public ISecretVaultCapabilityService {
public:
	explicit CSecretVaultCapabilityService(
		std::string canonicalProfileId,
		SecretVaultCapabilityServiceConfig config = {});
	CSecretVaultCapabilityService(
		std::string canonicalProfileId,
		std::shared_ptr<ISecretVaultCapabilityTokenSource> tokenSource,
		std::shared_ptr<ISecretVaultCapabilityClock> clock,
		SecretVaultCapabilityServiceConfig config = {});
	~CSecretVaultCapabilityService() override;
	CSecretVaultCapabilityService(const CSecretVaultCapabilityService&) = delete;
	CSecretVaultCapabilityService& operator=(const CSecretVaultCapabilityService&) = delete;

	[[nodiscard]] std::string_view GetProfileId() const noexcept override;
	[[nodiscard]] SecretVaultCapabilityIssueResult Issue(
		const SecretVaultCapabilityIssueRequest& request) override;
	[[nodiscard]] SecretVaultCapabilityValidationResult Validate(
		const SecretVaultCapabilityValidationRequest& request) override;
	[[nodiscard]] SecretVaultCapabilityRevokeResult RevokeExtension(
		const SecretVaultCapabilityBinding& binding) override;
	[[nodiscard]] SecretVaultCapabilityRevokeResult RevokeSession(
		const SecretVaultCapabilitySessionIdentity& session) override;
	[[nodiscard]] SecretVaultCapabilityRevokeResult RevokeHostSession(
		const SecretVaultCapabilityHostSessionIdentity& session) override;
	[[nodiscard]] ESecretVaultCapabilityStopStatus Stop() noexcept override;

private:
	struct Grant {
		SecretVaultCapabilityDigest digest{};
		SecretVaultCapabilityBinding binding;
		std::chrono::steady_clock::time_point expiresAt{};
	};

	void PruneExpiredLocked(std::chrono::steady_clock::time_point now) noexcept;
	[[nodiscard]] bool ContainsDigestLocked(const SecretVaultCapabilityDigest& digest) const noexcept;
	[[nodiscard]] static bool ConstantTimeDigestEqual(
		const SecretVaultCapabilityDigest& left,
		const SecretVaultCapabilityDigest& right) noexcept;
	[[nodiscard]] static bool ComputeDigest(
		const SecretVaultCapabilityToken& token,
		SecretVaultCapabilityDigest& digest) noexcept;

	mutable std::mutex m_mutex;
	std::string m_profileId;
	std::shared_ptr<ISecretVaultCapabilityTokenSource> m_tokenSource;
	std::shared_ptr<ISecretVaultCapabilityClock> m_clock;
	SecretVaultCapabilityServiceConfig m_config;
	bool m_configurationValid = false;
	bool m_stopped = false;
	std::vector<Grant> m_grants;
};

} // namespace platform::secrets
