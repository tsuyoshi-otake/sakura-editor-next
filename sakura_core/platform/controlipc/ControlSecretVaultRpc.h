/*! @file
	@brief Bounded, capability-checked Secret Vault control-IPC payloads.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcProtocol.h"
#include "platform/secrets/ISecretVaultCapabilityService.h"
#include "platform/secrets/ISecretVaultExtensionGrantAuthority.h"
#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"
#include "platform/secrets/ISecretVaultService.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace platform::controlipc {

//! The Secret Vault contract's existing bounds apply to all private records.
inline constexpr std::size_t kControlSecretVaultRpcMaximumPayloadBytes =
	kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;

struct ControlSecretVaultGetRequest {
	secrets::SecretVaultCapabilityToken capability{};
	std::string extensionHostSessionId;
	secrets::SecretAddress address;
};

struct ControlSecretVaultApplyRequest {
	secrets::SecretVaultCapabilityToken capability{};
	std::string extensionHostSessionId;
	secrets::SecretMutationRequest mutation;
};

//! The pipe session supplies profile, process, and control generation; callers
//! can name only the active host generation and exact canonical extension.
struct ControlSecretVaultCapabilityIssueRequest {
	std::string hostSessionId;
	std::uint64_t hostGeneration = 0;
	std::string extensionId;
	std::chrono::milliseconds lifetime{};
};

//! Present only after a successful Issue. No binding identity is exposed on wire.
struct ControlSecretVaultCapabilityIssueResponse {
	secrets::SecretVaultCapabilityToken capability{};
	std::chrono::milliseconds lifetime{};
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultGetRequest(
	const ControlSecretVaultGetRequest& request);
[[nodiscard]] std::optional<ControlSecretVaultGetRequest> DecodeControlSecretVaultGetRequest(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultGetResponse(
	const secrets::SecretGetResult& result);
[[nodiscard]] std::optional<secrets::SecretGetResult> DecodeControlSecretVaultGetResponse(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultApplyRequest(
	const ControlSecretVaultApplyRequest& request);
[[nodiscard]] std::optional<ControlSecretVaultApplyRequest> DecodeControlSecretVaultApplyRequest(
	std::span<const std::uint8_t> payload);

//! The response intentionally serializes neither SecretMutationResult::diagnostic
//! nor a secret value. A change, when present, contains address/kind/revision only.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultApplyResponse(
	const secrets::SecretMutationResult& result);
[[nodiscard]] std::optional<secrets::SecretMutationResult> DecodeControlSecretVaultApplyResponse(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityIssueRequest(
	const ControlSecretVaultCapabilityIssueRequest& request);
[[nodiscard]] std::optional<ControlSecretVaultCapabilityIssueRequest> DecodeControlSecretVaultCapabilityIssueRequest(
	std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityIssueResponse(
	const ControlSecretVaultCapabilityIssueResponse& response);
[[nodiscard]] std::optional<ControlSecretVaultCapabilityIssueResponse> DecodeControlSecretVaultCapabilityIssueResponse(
	std::span<const std::uint8_t> payload);
	//! A revoke-session request has an empty payload. Its identity is derived solely
	//! from the authenticated pipe session and active grant-authority lease. It
	//! revokes bearer capabilities only; broker/controller lifecycle remains the
	//! single owner of the editor PID lease.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityRevokeSessionRequest();
[[nodiscard]] bool DecodeControlSecretVaultCapabilityRevokeSessionRequest(std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityRevokeSessionResponse();
[[nodiscard]] bool DecodeControlSecretVaultCapabilityRevokeSessionResponse(std::span<const std::uint8_t> payload);

//! The named-pipe layer supplies these values after authenticating the transport.
//! `extensionHostSessionId` deliberately remains request-scoped: one authenticated
//! extension-host connection can carry grants for multiple extension identities.
struct ControlSecretVaultRpcSessionIdentity {
	std::string profileId;
	std::uint32_t clientProcessId = 0;
	std::uint64_t connectionGeneration = 0;
};

/*! 
	@brief Synchronous, already-authenticated Secret Vault RPC session core.

	This class owns no transport handshake and does not issue capabilities. Every
	call to Process produces exactly one terminal response. It validates the full
	payload and calls capability Validate before it calls the vault service.
*/
class CControlSecretVaultRpcSession final {
public:
	CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
		secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities) noexcept;
	//! Production-capable vault access includes the control-owned lazy migration
	//! gate. The compatibility overload above omits it only for existing tests and
	//! callers that cannot compose the durable DPAPI service yet.
	CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
		secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
		secrets::ISecretVaultLegacyMigrationCoordinator& migration) noexcept;
	//! Production issuance requires this overload; the compatibility overload above
	//! deliberately rejects Issue requests because it has no grant authority.
	CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
		secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
		secrets::ISecretVaultExtensionGrantAuthority& grantAuthority) noexcept;
	//! Full production constructor: grant issuance and legacy migration share the
	//! same authenticated control session, but the migration still runs only after
	//! a capability validates the exact canonical extension identity.
	CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
		secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
		secrets::ISecretVaultExtensionGrantAuthority& grantAuthority,
		secrets::ISecretVaultLegacyMigrationCoordinator& migration) noexcept;

	[[nodiscard]] ControlIpcFrame Process(const ControlIpcFrame& request) noexcept;
	[[nodiscard]] const ControlSecretVaultRpcSessionIdentity& GetIdentity() const noexcept { return m_identity; }

private:
	[[nodiscard]] ControlIpcFrame ErrorFor(const ControlIpcFrame& request,
		EControlIpcTerminalStatus status) const noexcept;
	[[nodiscard]] ControlIpcFrame HandleGet(const ControlIpcFrame& request);
	[[nodiscard]] ControlIpcFrame HandleApply(const ControlIpcFrame& request);
	[[nodiscard]] ControlIpcFrame HandleCapabilityIssue(const ControlIpcFrame& request);
	[[nodiscard]] ControlIpcFrame HandleCapabilityRevokeSession(const ControlIpcFrame& request);
	[[nodiscard]] bool HasValidIdentity() const noexcept;
	[[nodiscard]] bool HasMatchingAuthorities() const noexcept;
	[[nodiscard]] std::uint64_t ResponseGeneration() const noexcept;
	[[nodiscard]] secrets::SecretVaultCapabilityValidationResult Validate(
		const secrets::SecretVaultCapabilityToken& capability, std::string_view extensionHostSessionId,
		const secrets::SecretAddress& address);
	[[nodiscard]] bool EnsureMigratedOrError(const ControlIpcFrame& request, std::string_view extensionId,
		ControlIpcFrame& error);

	ControlSecretVaultRpcSessionIdentity m_identity;
	secrets::ISecretVaultService& m_vault;
	secrets::ISecretVaultCapabilityService& m_capabilities;
	secrets::ISecretVaultExtensionGrantAuthority* m_grantAuthority = nullptr;
	secrets::ISecretVaultLegacyMigrationCoordinator* m_migration = nullptr;
};

} // namespace platform::controlipc
