/*! @file
	@brief Production bridge from the extension-host lifecycle to control-owned Secret Vault authorities.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "extension/CExtensionHostSecretVaultGrantLifecycle.h"
#include "platform/secrets/ISecretVaultCapabilityService.h"
#include "platform/secrets/ISecretVaultExtensionGrantAuthority.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/*!
	@brief Keeps one extension-host grant and all of its editor capability sessions
	       on the same control-owned CAS revision.

	The adapter is created only by the control-process composition root. It never
	owns the Vault and never exposes a bearer capability or secret value. The
	authority and capability handles are retained only to make reverse shutdown
	fail closed if the runtime is stopped first accidentally.
*/
class CExtensionHostSecretVaultGrantRuntimeAdapter final
	: public IExtensionHostSecretVaultGrantLifecycle {
public:
	CExtensionHostSecretVaultGrantRuntimeAdapter(
		std::shared_ptr<platform::secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
		std::shared_ptr<platform::secrets::ISecretVaultCapabilityService> capabilities);

	[[nodiscard]] bool IsValid() const noexcept;

	SExtensionHostSecretVaultGrantResult Activate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override;
	SExtensionHostSecretVaultGrantResult RegisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t editorProcessId,
		std::uint64_t expectedRevision) override;
	SExtensionHostSecretVaultGrantResult UnregisterEditorProcess(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint32_t editorProcessId,
		std::uint64_t expectedRevision) override;
	SExtensionHostSecretVaultGrantResult ReplaceInstalledExtensionInventory(
		const SExtensionHostSecretVaultGrantSession& session,
		const std::vector<std::string>& canonicalExtensionIds,
		std::uint64_t expectedRevision) override;
	SExtensionHostSecretVaultGrantResult RevokeIssuedCapabilities(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override;
	SExtensionHostSecretVaultGrantResult Deactivate(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) override;

private:
	[[nodiscard]] bool MatchesActiveLocked(
		const SExtensionHostSecretVaultGrantSession& session,
		std::uint64_t expectedRevision) const noexcept;
	[[nodiscard]] platform::secrets::SecretVaultExtensionGrantAuthoritySessionMutation
		SessionMutationLocked(std::uint64_t expectedRevision) const;
	[[nodiscard]] SExtensionHostSecretVaultGrantResult ApplyAuthorityResultLocked(
		const platform::secrets::SecretVaultExtensionGrantAuthorityResult& result) noexcept;
	void ClearActiveLocked() noexcept;

	std::shared_ptr<platform::secrets::ISecretVaultExtensionGrantAuthority> m_grantAuthority;
	std::shared_ptr<platform::secrets::ISecretVaultCapabilityService> m_capabilities;
	mutable std::mutex m_mutex;
	std::string m_profileId;
	std::uint64_t m_controlConnectionGeneration = 0;
	std::uint64_t m_authorityRevision = 0;
	SExtensionHostSecretVaultGrantSession m_activeSession;
	std::string m_activeHostSessionId;
	std::vector<std::string> m_canonicalExtensionInventory;
	std::vector<std::uint32_t> m_editorProcessIds;
	bool m_valid = false;
};

[[nodiscard]] std::shared_ptr<IExtensionHostSecretVaultGrantLifecycle>
CreateExtensionHostSecretVaultGrantRuntimeAdapter(
	std::shared_ptr<platform::secrets::ISecretVaultExtensionGrantAuthority> grantAuthority,
	std::shared_ptr<platform::secrets::ISecretVaultCapabilityService> capabilities);
