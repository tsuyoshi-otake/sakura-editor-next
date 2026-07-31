/*! @file
	@brief Control-owned opaque capability contract for Secret Vault callers.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/SecretVaultTypes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace platform::secrets {

//! A capability is an opaque 256-bit bearer value. It must never be formatted
//! into diagnostics, events, settings, or persistent state.
inline constexpr std::size_t kSecretVaultCapabilityTokenBytes = 32;
inline constexpr std::size_t kSecretVaultCapabilityDigestBytes = 32;
inline constexpr std::size_t kMaximumSecretVaultCapabilitySessionIdBytes = 256;
inline constexpr std::size_t kMaximumSecretVaultCapabilityGrants = 1024;
inline constexpr auto kMaximumSecretVaultCapabilityLifetime = std::chrono::hours(24);

using SecretVaultCapabilityToken = std::array<std::uint8_t, kSecretVaultCapabilityTokenBytes>;
using SecretVaultCapabilityDigest = std::array<std::uint8_t, kSecretVaultCapabilityDigestBytes>;

//! A control/extension-host session identity. The extension host may be shared,
//! so an extension identity intentionally does not belong in this value.
struct SecretVaultCapabilitySessionIdentity {
	std::string profileId;
	std::string extensionHostSessionId;
	std::uint32_t clientProcessId = 0;
	std::uint64_t connectionGeneration = 0;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const SecretVaultCapabilitySessionIdentity&) const noexcept = default;
};

//! PID-independent identity used only by the control-owned extension-host
//! teardown path. It revokes all editor capability sessions belonging to one
//! exact authenticated host generation.
struct SecretVaultCapabilityHostSessionIdentity {
	std::string profileId;
	std::string extensionHostSessionId;
	std::uint64_t connectionGeneration = 0;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const SecretVaultCapabilityHostSessionIdentity&) const noexcept = default;
};

//! The full issuer-side grant binding. `extensionId` must already be the exact
//! canonical lowercase extension identity, not merely a case-insensitive match.
struct SecretVaultCapabilityBinding {
	SecretVaultCapabilitySessionIdentity session;
	std::string extensionId;

	[[nodiscard]] bool IsValid() const noexcept;
	[[nodiscard]] bool operator==(const SecretVaultCapabilityBinding&) const noexcept = default;
};

struct SecretVaultCapabilityIssueRequest {
	SecretVaultCapabilityBinding binding;
	//! Positive and no greater than the service's configured maximum.
	std::chrono::milliseconds lifetime{};
};

enum class ESecretVaultCapabilityIssueStatus : std::uint8_t {
	Issued,
	Stopped,
	InvalidConfiguration,
	InvalidBinding,
	InvalidLifetime,
	CapacityReached,
	EntropyFailure,
	DigestFailure,
	Collision,
	StorageFailure,
};

//! `capability` is present only for Issued. `expiresAt` is suitable for a
//! caller's local scheduling but carries neither the token nor an address.
struct SecretVaultCapabilityIssueResult {
	ESecretVaultCapabilityIssueStatus status = ESecretVaultCapabilityIssueStatus::InvalidConfiguration;
	std::optional<SecretVaultCapabilityToken> capability;
	std::optional<std::chrono::steady_clock::time_point> expiresAt;
};

struct SecretVaultCapabilityValidationRequest {
	SecretVaultCapabilityToken capability{};
	SecretVaultCapabilitySessionIdentity session;
	//! `address.extensionId` must be exact canonical lowercase and equal the
	//! extension identity bound to the bearer capability.
	SecretAddress address;
};

enum class ESecretVaultCapabilityValidationStatus : std::uint8_t {
	Valid,
	Stopped,
	InvalidConfiguration,
	InvalidSession,
	InvalidAddress,
	CapabilityMismatch,
	DigestFailure,
	Expired,
	ProfileMismatch,
	SessionMismatch,
	ClientProcessMismatch,
	ConnectionGenerationMismatch,
	ExtensionMismatch,
};

struct SecretVaultCapabilityValidationResult {
	ESecretVaultCapabilityValidationStatus status = ESecretVaultCapabilityValidationStatus::InvalidConfiguration;
};

enum class ESecretVaultCapabilityRevokeStatus : std::uint8_t {
	Revoked,
	NotFound,
	Stopped,
	InvalidConfiguration,
	InvalidBinding,
	InvalidSession,
};

//! `revokedGrantCount` is intentionally aggregate-only: results expose no
//! token, address, extension, or session values.
struct SecretVaultCapabilityRevokeResult {
	ESecretVaultCapabilityRevokeStatus status = ESecretVaultCapabilityRevokeStatus::InvalidConfiguration;
	std::size_t revokedGrantCount = 0;
};

enum class ESecretVaultCapabilityStopStatus : std::uint8_t {
	Stopped,
	AlreadyStopped,
};

//! Test seam for the control process's secure random source. Implementations
//! must fill every byte or return false without issuing a capability.
class ISecretVaultCapabilityTokenSource {
public:
	virtual ~ISecretVaultCapabilityTokenSource() = default;
	[[nodiscard]] virtual bool Fill(SecretVaultCapabilityToken& token) noexcept = 0;
};

//! Test seam for monotonic time. Wall-clock jumps must never affect expiry.
class ISecretVaultCapabilityClock {
public:
	virtual ~ISecretVaultCapabilityClock() = default;
	[[nodiscard]] virtual std::chrono::steady_clock::time_point Now() const noexcept = 0;
};

//! Production secure-token source backed by BCryptGenRandom.
class CSystemSecretVaultCapabilityTokenSource final : public ISecretVaultCapabilityTokenSource {
public:
	[[nodiscard]] bool Fill(SecretVaultCapabilityToken& token) noexcept override;
};

//! Production monotonic clock adapter.
class CSteadySecretVaultCapabilityClock final : public ISecretVaultCapabilityClock {
public:
	[[nodiscard]] std::chrono::steady_clock::time_point Now() const noexcept override;
};

struct SecretVaultCapabilityServiceConfig {
	std::size_t maximumGrants = 256;
	std::chrono::milliseconds maximumLifetime = std::chrono::hours(1);

	[[nodiscard]] bool IsValid() const noexcept;
};

} // namespace platform::secrets
