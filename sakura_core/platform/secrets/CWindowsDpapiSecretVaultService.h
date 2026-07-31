/*! @file
	@brief Control-owner DPAPI-backed durable implementation of ISecretVaultService.
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/secrets/ISecretVaultService.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace platform::secrets {

/*! @brief Lifetime lock for the sole durable Secret Vault writer.

	The control process retains this lock from Open() until Stop()/destruction.  It
	is intentionally opaque so editor and extension-host processes cannot turn the
	file backend into an independent writer.
*/
class IWindowsDpapiSecretVaultWriterLock {
public:
	virtual ~IWindowsDpapiSecretVaultWriterLock() = default;
};

enum class EWindowsDpapiSecretVaultWriterLockStatus : std::uint8_t {
	Acquired,
	Busy,
	IoError,
};

struct WindowsDpapiSecretVaultWriterLockResult {
	EWindowsDpapiSecretVaultWriterLockStatus status = EWindowsDpapiSecretVaultWriterLockStatus::IoError;
	std::unique_ptr<IWindowsDpapiSecretVaultWriterLock> lock;
};

/*! @brief Narrow, value-free durable-I/O seam.

	Production uses CreateWin32WindowsDpapiSecretVaultFileOperations().  The seam
	keeps failed-write recovery deterministic: a false WriteFileAtomically result
	must leave the previously committed state at @p path authoritative.
*/
class IWindowsDpapiSecretVaultFileOperations {
public:
	virtual ~IWindowsDpapiSecretVaultFileOperations() = default;
	virtual bool PrepareDirectory(const std::filesystem::path& directory) = 0;
	[[nodiscard]] virtual WindowsDpapiSecretVaultWriterLockResult AcquireWriterLock(
		const std::filesystem::path& lockPath) = 0;
	virtual bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
		bool& found) = 0;
	virtual bool WriteFileAtomically(const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes) = 0;
};

[[nodiscard]] std::shared_ptr<IWindowsDpapiSecretVaultFileOperations>
CreateWin32WindowsDpapiSecretVaultFileOperations();

enum class EWindowsDpapiSecretVaultOpenStatus : std::uint8_t {
	Opened,
	AlreadyOpen,
	InvalidArgument,
	WriterBusy,
	IoError,
	CorruptData,
	CryptoError,
	UnsupportedFormat,
	Stopped,
};

struct WindowsDpapiSecretVaultOpenResult {
	EWindowsDpapiSecretVaultOpenStatus status = EWindowsDpapiSecretVaultOpenStatus::IoError;
	//! Fixed, value-free diagnostic only. It never includes a path, profile, key, or value.
	std::string diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EWindowsDpapiSecretVaultOpenStatus::Opened
			|| status == EWindowsDpapiSecretVaultOpenStatus::AlreadyOpen;
	}
};

class CWindowsDpapiSecretVaultService;

struct WindowsDpapiSecretVaultCreateResult {
	WindowsDpapiSecretVaultOpenResult open;
	std::unique_ptr<CWindowsDpapiSecretVaultService> service;

	[[nodiscard]] bool Succeeded() const noexcept { return open.Succeeded() && static_cast<bool>(service); }
};

/*! @brief One sorted key/value item read from the legacy per-extension vault.

	The caller owns the reader and must provide the canonical extension identity
	separately. Values are accepted only for the one encrypted import transaction;
	they are never included in the typed result or migration events.
*/
struct LegacySecretVaultEntry {
	std::string key;
	std::string value;

	[[nodiscard]] bool operator==(const LegacySecretVaultEntry&) const noexcept = default;
};

struct LegacySecretVaultImportRequest {
	std::string extensionId;
	//! Strictly increasing by key. An empty vector means the legacy store was absent or empty.
	std::vector<LegacySecretVaultEntry> entries;
	std::string operationId;
	std::optional<std::uint64_t> expectedRevision;

	[[nodiscard]] bool operator==(const LegacySecretVaultImportRequest&) const noexcept = default;
};

enum class ELegacySecretVaultImportStatus : std::uint8_t {
	Succeeded,
	AlreadyImported,
	Conflict,
	Stopped,
	Invalid,
	Failed,
};

//! Deliberately has no values, keys, paths, or profile identity.
struct LegacySecretVaultImportResult {
	ELegacySecretVaultImportStatus status = ELegacySecretVaultImportStatus::Invalid;
	std::uint64_t revision = 0;
	bool replayed = false;
	std::string diagnostic;
};

/*! 
	@brief Per-profile encrypted Secret Vault owned by the hidden control process.

	Open acquires an exclusive OS writer lock and either decrypts exactly the
	profile-bound state already present or fails closed. It never replaces an
	existing corrupt, wrong-profile, or undecryptable state with an empty vault.
	Every successful effective Apply means the encrypted full-state envelope,
	revision, entries, and bounded exact-replay ledger were flushed and atomically
	replaced before the mutation becomes observable in memory.
*/
class CWindowsDpapiSecretVaultService final : public ISecretVaultService {
public:
	static constexpr std::size_t kMaximumEntries = 4096;
	static constexpr std::size_t kMaximumPersistedBytes = 8 * 1024 * 1024;
	static constexpr std::size_t kMaximumCompletedOperations = kMaximumSecretVaultCompletedOperations;

	explicit CWindowsDpapiSecretVaultService(std::filesystem::path metadataRoot,
		std::string canonicalProfileId,
		std::size_t maxCompletedOperations = kMaximumCompletedOperations,
		std::size_t maxSubscriptions = kMaximumSecretVaultSubscriptions,
		std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> fileOperations = {});
	~CWindowsDpapiSecretVaultService() override;
	CWindowsDpapiSecretVaultService(const CWindowsDpapiSecretVaultService&) = delete;
	CWindowsDpapiSecretVaultService& operator=(const CWindowsDpapiSecretVaultService&) = delete;

	//! Creates, opens, and returns the sole durable control authority in one typed result.
	[[nodiscard]] static WindowsDpapiSecretVaultCreateResult Create(std::filesystem::path metadataRoot,
		std::string canonicalProfileId,
		std::size_t maxCompletedOperations = kMaximumCompletedOperations,
		std::size_t maxSubscriptions = kMaximumSecretVaultSubscriptions,
		std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> fileOperations = {});

	//! Acquires the writer lock and opens the existing profile-bound encrypted envelope.
	[[nodiscard]] WindowsDpapiSecretVaultOpenResult Open();
	[[nodiscard]] bool IsOpen() const noexcept;

	[[nodiscard]] std::string_view GetProfileId() const noexcept override;
	[[nodiscard]] SecretGetResult Get(std::string_view extensionId, std::string_view key) const override;
	[[nodiscard]] SecretMutationResult Apply(const SecretMutationRequest& request) override;
	/*! 
		@brief Atomically imports one extension's legacy secret set exactly once.

		The encrypted per-extension completion marker commits in the same replacement
		file as all entries, including when @p request.entries is empty.  Once marked,
		future calls for the extension cannot resurrect values after a Delete.
	*/
	[[nodiscard]] LegacySecretVaultImportResult ImportLegacy(
		const LegacySecretVaultImportRequest& request);
	[[nodiscard]] std::unique_ptr<ISecretVaultChangeSubscription> Subscribe(
		SecretChangeCallback callback) override;
	[[nodiscard]] ESecretVaultStopStatus Stop() noexcept override;

	[[nodiscard]] const std::filesystem::path& MetadataRoot() const noexcept { return m_metadataRoot; }
	//! The outer file contains only generic magic/version/ciphertext. Secret/profile plaintext is encrypted.
	[[nodiscard]] std::filesystem::path StatePath() const;

	//! The answer is read from encrypted per-extension migration metadata.
	[[nodiscard]] bool IsLegacyMigrationComplete(std::string_view extensionId) const;

	struct SubscriptionState;

private:
	struct CompletedOperation;
	struct CompletedLegacyImport;

	[[nodiscard]] std::optional<SecretMutationRequest> CanonicalizeRequest(
		const SecretMutationRequest& request) const;
	[[nodiscard]] bool IsAvailableLocked() const noexcept;
	[[nodiscard]] bool RememberCompleted(std::map<std::string, CompletedOperation>& completed,
		std::deque<std::string>& order, const SecretMutationRequest& request,
		const SecretMutationResult& result) const;
	[[nodiscard]] std::optional<LegacySecretVaultImportRequest> CanonicalizeLegacyImportRequest(
		const LegacySecretVaultImportRequest& request) const;
	[[nodiscard]] bool RememberLegacyImport(
		std::map<std::string, CompletedLegacyImport>& completed,
		std::deque<std::string>& order, const LegacySecretVaultImportRequest& request,
		const LegacySecretVaultImportResult& result) const;
	[[nodiscard]] bool PersistCandidate(const std::map<SecretAddress, SecretEntry>& entries,
		const std::map<std::string, CompletedOperation>& completed,
		const std::deque<std::string>& completedOrder,
		const std::map<std::string, std::uint64_t>& migrationCompleteExtensions,
		const std::map<std::string, CompletedLegacyImport>& completedLegacyImports,
		const std::deque<std::string>& completedLegacyImportOrder,
		std::uint64_t revision) const;
	[[nodiscard]] WindowsDpapiSecretVaultOpenResult Decode(
		std::span<const std::uint8_t> encryptedEnvelope);
	[[nodiscard]] bool EnqueueNotification(const SecretChange& change) noexcept;
	void DrainNotifications() noexcept;

	const std::filesystem::path m_metadataRoot;
	const std::string m_profileId;
	const bool m_profileIdValid;
	const std::size_t m_maxCompletedOperations;
	const std::shared_ptr<IWindowsDpapiSecretVaultFileOperations> m_fileOperations;
	mutable std::mutex m_mutex;
	bool m_open = false;
	bool m_stopped = false;
	std::uint64_t m_revision = 0;
	std::map<SecretAddress, SecretEntry> m_entries;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;
	//! Canonical extension ID -> revision at which absence/empty/data import completed.
	std::map<std::string, std::uint64_t> m_migrationCompleteExtensions;
	std::map<std::string, CompletedLegacyImport> m_completedLegacyImports;
	std::deque<std::string> m_completedLegacyImportOrder;
	std::unique_ptr<IWindowsDpapiSecretVaultWriterLock> m_writerLock;
	std::shared_ptr<SubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<SecretChange> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

} // namespace platform::secrets
