/*! @file
	@brief Read-only migration reader for the legacy extension SecretStorage format.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace platform::secrets {

//! Migration limits. These bound work before a durable vault transaction begins.
inline constexpr std::size_t kMaximumLegacyExtensionSecretEntries = 4096;
inline constexpr std::size_t kMaximumLegacyExtensionSecretBytes = 4 * 1024 * 1024;

enum class ELegacyExtensionSecretVaultReadStatus : std::uint8_t {
	//! A non-empty namespace was decoded completely.
	Success,
	//! The legacy root or this extension's hashed namespace does not exist.
	Absent,
	//! The hashed namespace exists but contains no legacy secret files.
	Empty,
	//! The caller did not supply a canonical extension identity or a usable root.
	InvalidArgument,
	//! The legacy files could not be enumerated or read safely.
	IoError,
	//! DPAPI rejected a well-formed legacy ciphertext.
	CryptoError,
	//! A legacy file is malformed, unsafe to import, or exceeds migration bounds.
	CorruptData,
};

//! Secret-bearing entries are returned only to the one-time migration transaction.
struct LegacyExtensionSecretVaultEntry {
	std::string key;
	std::string value;
};

//! Deliberately has no free-form diagnostic: callers must not log secret-bearing context.
struct LegacyExtensionSecretVaultReadResult {
	ELegacyExtensionSecretVaultReadStatus status = ELegacyExtensionSecretVaultReadStatus::InvalidArgument;
	std::uint32_t errorCode = 0;
	std::vector<LegacyExtensionSecretVaultEntry> entries;

	[[nodiscard]] bool HasEntries() const noexcept
	{
		return status == ELegacyExtensionSecretVaultReadStatus::Success;
	}
};

//! Reads, but never creates, writes, deletes, or repairs the legacy DPAPI store.
class CLegacyExtensionSecretVaultReader final {
public:
	explicit CLegacyExtensionSecretVaultReader(std::filesystem::path legacyBasePath);

	//! extensionId must already be canonical lowercase ASCII publisher.name form.
	[[nodiscard]] LegacyExtensionSecretVaultReadResult ReadAll(std::string_view extensionId) const;

	const std::filesystem::path& GetLegacyBasePath() const noexcept { return m_legacyBasePath; }

private:
	std::filesystem::path m_legacyBasePath;
};

} // namespace platform::secrets
