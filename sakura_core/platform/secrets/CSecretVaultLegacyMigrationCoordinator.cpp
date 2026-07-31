/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "platform/secrets/CSecretVaultLegacyMigrationCoordinator.h"

#include <array>
#include <utility>

#include <windows.h>

namespace platform::secrets {
namespace {

void SecureClear(std::string& value) noexcept
{
	if (!value.empty()) ::SecureZeroMemory(value.data(), value.size());
	value.clear();
}

void SecureClear(std::vector<LegacyExtensionSecretVaultEntry>& entries) noexcept
{
	for (auto& entry : entries) {
		SecureClear(entry.key);
		SecureClear(entry.value);
	}
	entries.clear();
}

void SecureClear(std::vector<LegacySecretVaultEntry>& entries) noexcept
{
	for (auto& entry : entries) {
		SecureClear(entry.key);
		SecureClear(entry.value);
	}
	entries.clear();
}

bool IsExactCanonicalExtensionId(std::string_view extensionId)
{
	std::string canonical;
	return CanonicalizeSecretVaultExtensionId(extensionId, canonical) && canonical == extensionId;
}

} // namespace

struct CSecretVaultLegacyMigrationCoordinator::InFlight final {
	std::condition_variable completed;
	bool isComplete = false;
	SecretVaultLegacyMigrationResult result{};
};

CSecretVaultLegacyMigrationCoordinator::CSecretVaultLegacyMigrationCoordinator(
	CWindowsDpapiSecretVaultService& vault, std::filesystem::path legacyBasePath,
	std::size_t maximumInFlightNamespaces)
	: m_vault(vault), m_reader(std::move(legacyBasePath)),
	  m_maximumInFlightNamespaces(maximumInFlightNamespaces == 0 ? 1 : maximumInFlightNamespaces)
{
}

std::string CSecretVaultLegacyMigrationCoordinator::OperationIdFor(std::string_view canonicalExtensionId)
{
	// Stable and short enough for the persisted bounded replay ledger. A collision
	// causes the vault import to fail closed; it can never select another namespace.
	std::uint64_t hash = 14695981039346656037ull;
	for (const unsigned char value : canonicalExtensionId) {
		hash ^= value;
		hash *= 1099511628211ull;
	}
	static constexpr std::array<char, 16> digits = {
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
	};
	std::string operation = "legacy-migration-v1-0000000000000000";
	for (std::size_t offset = 0; offset != 16; ++offset) {
		operation[operation.size() - 1 - offset] = digits[hash & 0x0f];
		hash >>= 4;
	}
	return operation;
}

SecretVaultLegacyMigrationResult CSecretVaultLegacyMigrationCoordinator::Migrate(
	std::string_view canonicalExtensionId) noexcept
{
	try {
		if (m_vault.IsLegacyMigrationComplete(canonicalExtensionId)) {
			return { ESecretVaultLegacyMigrationStatus::Migrated };
		}

		auto legacy = m_reader.ReadAll(canonicalExtensionId);
		struct LegacyWiper final {
			LegacyExtensionSecretVaultReadResult& value;
			~LegacyWiper() { SecureClear(value.entries); }
		} legacyWiper{ legacy };

		switch (legacy.status) {
		case ELegacyExtensionSecretVaultReadStatus::Success:
		case ELegacyExtensionSecretVaultReadStatus::Absent:
		case ELegacyExtensionSecretVaultReadStatus::Empty:
			break;
		case ELegacyExtensionSecretVaultReadStatus::InvalidArgument:
			return { ESecretVaultLegacyMigrationStatus::InvalidArgument };
		case ELegacyExtensionSecretVaultReadStatus::IoError:
			return { ESecretVaultLegacyMigrationStatus::LegacyIoError };
		case ELegacyExtensionSecretVaultReadStatus::CryptoError:
			return { ESecretVaultLegacyMigrationStatus::LegacyCryptoError };
		case ELegacyExtensionSecretVaultReadStatus::CorruptData:
			return { ESecretVaultLegacyMigrationStatus::LegacyCorruptData };
		}

		LegacySecretVaultImportRequest request;
		request.extensionId.assign(canonicalExtensionId);
		request.operationId = OperationIdFor(canonicalExtensionId);
		for (auto& entry : legacy.entries) {
			request.entries.emplace_back(LegacySecretVaultEntry{ std::move(entry.key), std::move(entry.value) });
		}
		struct RequestWiper final {
			LegacySecretVaultImportRequest& value;
			~RequestWiper() { SecureClear(value.entries); }
		} requestWiper{ request };

		// `Stop()` serializes with the one durable write. Once Stop returns, no
		// in-flight reader can still commit a legacy namespace.
		std::scoped_lock lock(m_mutex);
		if (m_stopped) return { ESecretVaultLegacyMigrationStatus::Stopped };
		const auto imported = m_vault.ImportLegacy(request);
		switch (imported.status) {
		case ELegacySecretVaultImportStatus::Succeeded:
		case ELegacySecretVaultImportStatus::AlreadyImported:
			return { ESecretVaultLegacyMigrationStatus::Migrated };
		case ELegacySecretVaultImportStatus::Stopped:
			return { ESecretVaultLegacyMigrationStatus::Stopped };
		case ELegacySecretVaultImportStatus::Conflict:
		case ELegacySecretVaultImportStatus::Invalid:
		case ELegacySecretVaultImportStatus::Failed:
			return { ESecretVaultLegacyMigrationStatus::ImportFailed };
		}
	} catch (...) {
		// Allocation and platform exceptions have the same value-free, fail-closed result.
	}
	return { ESecretVaultLegacyMigrationStatus::ImportFailed };
}

SecretVaultLegacyMigrationResult CSecretVaultLegacyMigrationCoordinator::EnsureMigrated(
	std::string_view canonicalExtensionId)
{
	try {
		if (!IsExactCanonicalExtensionId(canonicalExtensionId)) {
			return { ESecretVaultLegacyMigrationStatus::InvalidArgument };
		}
		const std::string extensionId(canonicalExtensionId);
		std::shared_ptr<InFlight> inFlight;
		bool leader = false;
		{
			std::unique_lock lock(m_mutex);
			if (m_stopped) return { ESecretVaultLegacyMigrationStatus::Stopped };
			const auto existing = m_inFlight.find(extensionId);
			if (existing != m_inFlight.end()) {
				inFlight = existing->second;
				inFlight->completed.wait(lock, [&] { return inFlight->isComplete || m_stopped; });
				return m_stopped ? SecretVaultLegacyMigrationResult{ ESecretVaultLegacyMigrationStatus::Stopped }
					: inFlight->result;
			}
			if (m_inFlight.size() >= m_maximumInFlightNamespaces) {
				return { ESecretVaultLegacyMigrationStatus::ResourceExhausted };
			}
			inFlight = std::make_shared<InFlight>();
			m_inFlight.emplace(extensionId, inFlight);
			leader = true;
		}

		const auto result = leader ? Migrate(canonicalExtensionId)
			: SecretVaultLegacyMigrationResult{ ESecretVaultLegacyMigrationStatus::ImportFailed };
		std::scoped_lock lock(m_mutex);
		if (m_stopped) {
			inFlight->result = { ESecretVaultLegacyMigrationStatus::Stopped };
		} else {
			inFlight->result = result;
		}
		inFlight->isComplete = true;
		m_inFlight.erase(extensionId);
		inFlight->completed.notify_all();
		return inFlight->result;
	} catch (...) {
		return { ESecretVaultLegacyMigrationStatus::ResourceExhausted };
	}
}

ESecretVaultLegacyMigrationStopStatus CSecretVaultLegacyMigrationCoordinator::Stop() noexcept
{
	std::scoped_lock lock(m_mutex);
	if (m_stopped) return ESecretVaultLegacyMigrationStopStatus::AlreadyStopped;
	m_stopped = true;
	for (const auto& [extensionId, inFlight] : m_inFlight) {
		(void)extensionId;
		inFlight->result = { ESecretVaultLegacyMigrationStatus::Stopped };
		inFlight->isComplete = true;
		inFlight->completed.notify_all();
	}
	m_inFlight.clear();
	return ESecretVaultLegacyMigrationStopStatus::Stopped;
}

} // namespace platform::secrets
