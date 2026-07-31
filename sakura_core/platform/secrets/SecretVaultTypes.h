/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace platform::secrets {

//! Bounds shared by the in-process authority and a future control IPC adapter.
inline constexpr std::size_t kMaximumSecretVaultProfileIdBytes = 256;
inline constexpr std::size_t kMaximumSecretVaultExtensionIdBytes = 256;
inline constexpr std::size_t kMaximumSecretVaultKeyBytes = 256;
inline constexpr std::size_t kMaximumSecretVaultOperationIdBytes = 256;
inline constexpr std::size_t kMaximumSecretVaultValueBytes = 64 * 1024;
inline constexpr std::size_t kMaximumSecretVaultCompletedOperations = 4096;
inline constexpr std::size_t kMaximumSecretVaultSubscriptions = 128;

[[nodiscard]] bool IsValidSecretVaultUtf8(std::string_view value, bool allowEmpty = true) noexcept;
[[nodiscard]] bool IsValidSecretVaultIdentifier(
	std::string_view value, std::size_t maximumBytes) noexcept;
//! Converts a VS Code extension identity to its canonical lowercase ASCII form.
[[nodiscard]] bool CanonicalizeSecretVaultExtensionId(
	std::string_view extensionId, std::string& canonicalExtensionId);

struct SecretAddress {
	//! Always canonical lowercase after a successful service call.
	std::string extensionId;
	std::string key;

	[[nodiscard]] bool IsValid() const;
	[[nodiscard]] bool operator==(const SecretAddress&) const noexcept = default;
	[[nodiscard]] bool operator<(const SecretAddress& other) const noexcept;
};

struct SecretEntry {
	SecretAddress address;
	//! Never include this field in events, diagnostics, mementos, or normal logs.
	std::string value;
	std::uint64_t revision = 0;
};

enum class ESecretGetStatus : std::uint8_t {
	Found,
	NotFound,
	Stopped,
	Invalid,
};

struct SecretGetResult {
	ESecretGetStatus status = ESecretGetStatus::Invalid;
	std::uint64_t revision = 0;
	std::optional<std::string> value;
};

enum class ESecretMutationKind : std::uint8_t {
	Set,
	Delete,
};

struct SecretMutationRequest {
	ESecretMutationKind kind = ESecretMutationKind::Set;
	std::string extensionId;
	std::string key;
	//! Required for Set and required to be empty for Delete.
	std::string value;
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;

	[[nodiscard]] bool operator==(const SecretMutationRequest&) const noexcept = default;
};

enum class ESecretMutationStatus : std::uint8_t {
	Succeeded,
	Conflict,
	NotApplicable,
	Stopped,
	Invalid,
	Failed,
};

enum class ESecretChangeKind : std::uint8_t {
	Set,
	Delete,
};

//! Deliberately excludes secret values.
struct SecretChange {
	std::string profileId;
	SecretAddress address;
	ESecretChangeKind kind = ESecretChangeKind::Set;
	std::uint64_t revision = 0;
};

//! Invoked only after an effective mutation commits. Never receives a secret value.
using SecretChangeCallback = std::function<void(const SecretChange&)>;

struct SecretMutationResult {
	ESecretMutationStatus status = ESecretMutationStatus::Invalid;
	std::uint64_t revision = 0;
	bool replayed = false;
	std::optional<SecretChange> change;
	//! Machine-readable status is primary. Diagnostics must never contain secret values.
	std::string diagnostic;
};

enum class ESecretVaultStopStatus : std::uint8_t {
	Stopped,
	AlreadyStopped,
};

} // namespace platform::secrets
