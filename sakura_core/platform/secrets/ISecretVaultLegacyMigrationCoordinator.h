/*! @file
	@brief Control-owned lazy migration gate for the legacy extension SecretStorage vault.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstdint>
#include <string_view>

namespace platform::secrets {

//! Value-free terminal outcomes for one extension namespace migration.
enum class ESecretVaultLegacyMigrationStatus : std::uint8_t {
	Migrated,
	Stopped,
	InvalidArgument,
	ResourceExhausted,
	LegacyIoError,
	LegacyCryptoError,
	LegacyCorruptData,
	ImportFailed,
};

struct SecretVaultLegacyMigrationResult {
	ESecretVaultLegacyMigrationStatus status = ESecretVaultLegacyMigrationStatus::ImportFailed;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ESecretVaultLegacyMigrationStatus::Migrated;
	}
};

enum class ESecretVaultLegacyMigrationStopStatus : std::uint8_t {
	Stopped,
	AlreadyStopped,
};

/*! @brief Narrow gate invoked after capability validation and before vault access.

	The implementation must never reveal legacy paths, extension IDs, or secret
	values through this contract. `Stop()` is terminal: no later request may begin
	or complete a migration through this coordinator.
*/
class ISecretVaultLegacyMigrationCoordinator {
public:
	virtual ~ISecretVaultLegacyMigrationCoordinator() = default;
	[[nodiscard]] virtual SecretVaultLegacyMigrationResult EnsureMigrated(
		std::string_view canonicalExtensionId) = 0;
	[[nodiscard]] virtual ESecretVaultLegacyMigrationStopStatus Stop() noexcept = 0;
};

} // namespace platform::secrets
