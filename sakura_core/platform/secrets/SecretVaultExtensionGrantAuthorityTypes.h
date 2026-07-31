/*! @file
	@brief Ephemeral control-owned authorization state for Secret Vault capability issuance.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/SecretVaultCapabilityTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform::secrets {

//! The authority is intentionally bounded independently from bearer-capability storage.
inline constexpr std::size_t kMaximumSecretVaultExtensionGrantEditorLeases = 256;
inline constexpr std::size_t kMaximumSecretVaultExtensionGrantApprovedExtensions = 1024;

struct SecretVaultExtensionGrantAuthorityConfig {
	std::size_t maximumEditorLeases = 64;
	std::size_t maximumApprovedExtensions = 256;

	[[nodiscard]] bool IsValid() const noexcept;
};

enum class ESecretVaultExtensionGrantAuthorityStatus : std::uint8_t {
	Applied,
	Stopped,
	InvalidConfiguration,
	InvalidRequest,
	RevisionMismatch,
	GenerationMismatch,
	CapacityReached,
	NotActive,
	NotFound,
};

//! Contains no profile, host, extension, or process identity for safe diagnostics.
struct SecretVaultExtensionGrantAuthorityResult {
	ESecretVaultExtensionGrantAuthorityStatus status = ESecretVaultExtensionGrantAuthorityStatus::InvalidConfiguration;
	std::uint64_t revision = 0;
};

//! The optimistic revision and host pair fence every mutation against replacement.
struct SecretVaultExtensionGrantAuthoritySessionMutation {
	std::uint64_t expectedRevision = 0;
	std::string hostSessionId;
	std::uint64_t hostGeneration = 0;

	[[nodiscard]] bool IsWellFormed() const noexcept;
};

struct SecretVaultExtensionGrantAuthorityActivateRequest {
	std::uint64_t expectedRevision = 0;
	std::string hostSessionId;
	std::uint64_t hostGeneration = 0;
	std::vector<std::string> approvedExtensionIds;
	std::vector<std::uint32_t> editorProcessIds;

	[[nodiscard]] bool IsWellFormed() const noexcept;
};

struct SecretVaultExtensionGrantAuthorityEditorLeaseMutation {
	SecretVaultExtensionGrantAuthoritySessionMutation session;
	std::uint32_t editorProcessId = 0;
};

struct SecretVaultExtensionGrantAuthorityApprovedExtensionsMutation {
	SecretVaultExtensionGrantAuthoritySessionMutation session;
	std::vector<std::string> approvedExtensionIds;
};

struct SecretVaultExtensionGrantAuthorityDisableExtensionMutation {
	SecretVaultExtensionGrantAuthoritySessionMutation session;
	std::string extensionId;
};

//! Complete caller binding supplied to the issue gate. It is never serialized as a diagnostic.
struct SecretVaultExtensionGrantAuthorityIssueRequest {
	std::string profileId;
	std::uint64_t controlConnectionGeneration = 0;
	std::string hostSessionId;
	std::uint64_t hostGeneration = 0;
	std::uint32_t clientProcessId = 0;
	std::string extensionId;
};

enum class ESecretVaultExtensionGrantAuthorizationStatus : std::uint8_t {
	Authorized,
	Stopped,
	InvalidConfiguration,
	AccessDenied,
};

struct SecretVaultExtensionGrantAuthorizationResult {
	ESecretVaultExtensionGrantAuthorizationStatus status = ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration;
	//! Filled only for Authorized and intended solely as the immediate capability-service input.
	SecretVaultCapabilityBinding binding;
	std::uint64_t revision = 0;
};

//! Revoke derives the host identity from the active lease rather than accepting it from the caller.
struct SecretVaultExtensionGrantRevokeAuthorizationRequest {
	std::string profileId;
	std::uint64_t controlConnectionGeneration = 0;
	std::uint32_t clientProcessId = 0;
};

struct SecretVaultExtensionGrantRevokeAuthorizationResult {
	ESecretVaultExtensionGrantAuthorizationStatus status = ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration;
	//! Filled only for Authorized and intended solely as the immediate capability-service input.
	SecretVaultCapabilitySessionIdentity session;
	std::uint64_t revision = 0;
	std::uint64_t hostGeneration = 0;
};

} // namespace platform::secrets
