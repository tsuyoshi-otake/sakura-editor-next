/*! @file
	@brief DPAPI-backed implementation of the control-owned legacy SecretStorage migration gate.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/ISecretVaultLegacyMigrationCoordinator.h"

#include "platform/secrets/CWindowsDpapiSecretVaultService.h"
#include "platform/secrets/LegacyExtensionSecretVaultReader.h"

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace platform::secrets {

/*! @brief Serializes a bounded set of lazy legacy namespace imports for one vault.

	The concrete DPAPI vault is deliberately an explicit constructor dependency. The
	public RPC remains typed against ISecretVaultService and never downcasts it to
	reach this migration-only API.
*/
class CSecretVaultLegacyMigrationCoordinator final : public ISecretVaultLegacyMigrationCoordinator {
public:
	static constexpr std::size_t kDefaultMaximumInFlightNamespaces = 128;

	CSecretVaultLegacyMigrationCoordinator(CWindowsDpapiSecretVaultService& vault,
		std::filesystem::path legacyBasePath,
		std::size_t maximumInFlightNamespaces = kDefaultMaximumInFlightNamespaces);

	[[nodiscard]] SecretVaultLegacyMigrationResult EnsureMigrated(
		std::string_view canonicalExtensionId) override;
	[[nodiscard]] ESecretVaultLegacyMigrationStopStatus Stop() noexcept override;

private:
	struct InFlight;

	[[nodiscard]] SecretVaultLegacyMigrationResult Migrate(std::string_view canonicalExtensionId) noexcept;
	[[nodiscard]] static std::string OperationIdFor(std::string_view canonicalExtensionId);

	CWindowsDpapiSecretVaultService& m_vault;
	CLegacyExtensionSecretVaultReader m_reader;
	const std::size_t m_maximumInFlightNamespaces;
	std::mutex m_mutex;
	bool m_stopped = false;
	std::map<std::string, std::shared_ptr<InFlight>> m_inFlight;
};

} // namespace platform::secrets
