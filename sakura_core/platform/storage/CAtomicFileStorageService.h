/*! @file
\t@brief Control-process-owned durable implementation of IStorageService.
*/
/*
\tCopyright (C) 2026, Sakura Editor Organization

\tSPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/storage/IStorageAuthority.h>

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

namespace platform::storage {

//! The durable store has no encryption and is never a SecretStorage backend.
//! Callers must store credentials and secret values in the dedicated DPAPI/credential
//! service, not in StorageEntry::value. The implementation rejects the reserved
//! `secret`/`secrets` owner namespaces as a defence-in-depth guard.
class IAtomicFileStorageWriterLock {
public:
	virtual ~IAtomicFileStorageWriterLock() = default;
};

enum class EAtomicFileStorageWriterLockStatus : std::uint8_t {
	Acquired,
	Busy,
	IoError,
};

struct AtomicFileStorageWriterLockResult {
	EAtomicFileStorageWriterLockStatus status = EAtomicFileStorageWriterLockStatus::IoError;
	std::unique_ptr<IAtomicFileStorageWriterLock> lock;
	std::string diagnostic;
};

//! Narrow I/O seam used to deterministically inject write/flush/replace failures.
//! Production code obtains it from CreateWin32AtomicFileStorageFileOperations().
class IAtomicFileStorageFileOperations {
public:
	virtual ~IAtomicFileStorageFileOperations() = default;
	virtual bool PrepareDirectory(const std::filesystem::path& directory, std::string& diagnostic) = 0;
	[[nodiscard]] virtual AtomicFileStorageWriterLockResult AcquireWriterLock(
		const std::filesystem::path& lockPath) = 0;
	virtual bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
		bool& found, std::string& diagnostic) = 0;
	virtual bool WriteFileAtomically(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
		std::string& diagnostic) = 0;
};

[[nodiscard]] std::shared_ptr<IAtomicFileStorageFileOperations> CreateWin32AtomicFileStorageFileOperations();

// Compatibility aliases keep the durable implementation's existing tests and
// callers source-compatible while the Control composition depends only on the
// public lifecycle port.
using EAtomicFileStorageOpenStatus = EStorageAuthorityOpenStatus;
using AtomicFileStorageOpenResult = StorageAuthorityOpenResult;

/*! 
	@brief Versioned, checksummed, atomic-file storage for the single control writer.

	Open() must succeed before use and keeps an OS-owned writer lock for the service
	lifetime.  A successful Apply() means its complete replacement file was flushed
	and atomically published.  The store intentionally caps snapshots/mutations to
	the current control-IPC limits; it has no paging protocol with which to expose
	larger state safely.
*/
class CAtomicFileStorageService final : public IStorageAuthority {
public:
	struct SubscriptionState;

	static constexpr std::size_t kMaximumItems = kMaximumStorageItems;
	static constexpr std::size_t kMaximumStringBytes = kMaximumStorageStringBytes;
	static constexpr std::size_t kMaximumCompletedOperations = kMaximumStorageCompletedOperations;
	static constexpr std::size_t kMaximumPersistedBytes = 8 * 1024 * 1024;
	//! Reserves enough room for the largest transport-visible snapshot and file header.
	static constexpr std::size_t kMaximumCompletedPayloadBytes =
		kMaximumPersistedBytes - kMaximumStorageSnapshotPayloadBytes - 64;

	explicit CAtomicFileStorageService(std::filesystem::path directory,
		std::uint64_t generation = 1,
	std::size_t maxCompletedOperations = kMaximumCompletedOperations,
		std::shared_ptr<IAtomicFileStorageFileOperations> fileOperations = {});
	~CAtomicFileStorageService() override;
	CAtomicFileStorageService(const CAtomicFileStorageService&) = delete;
	CAtomicFileStorageService& operator=(const CAtomicFileStorageService&) = delete;

	[[nodiscard]] AtomicFileStorageOpenResult Open() override;
	void Close() noexcept override;
	[[nodiscard]] bool IsOpen() const noexcept override;

	[[nodiscard]] StorageMutationResult Apply(const StorageMutationRequest& request) override;
	[[nodiscard]] StorageSnapshot Snapshot() const override;
	[[nodiscard]] std::unique_ptr<IStorageChangeSubscription> Subscribe(StorageChangeCallback callback) override;
	[[nodiscard]] std::optional<StorageEntry> Find(const StorageAddress& address) const;

	[[nodiscard]] const std::filesystem::path& Directory() const noexcept { return m_directory; }
	[[nodiscard]] std::filesystem::path StatePath() const;

private:
	struct CompletedOperation;

	[[nodiscard]] StorageMutationResult ValidateRequest(const StorageMutationRequest& request) const;
	[[nodiscard]] bool PersistCandidate(const std::map<StorageAddress, StorageEntry>& entries,
		const std::map<std::string, CompletedOperation>& completed,
		const std::deque<std::string>& completedOrder,
		std::uint64_t revision,
		std::string& diagnostic) const;
	[[nodiscard]] AtomicFileStorageOpenResult Decode(std::span<const std::uint8_t> bytes);
	[[nodiscard]] static bool AppendCompletedOperation(std::vector<std::uint8_t>& output,
		const CompletedOperation& operation);
	[[nodiscard]] bool RememberCompleted(std::map<std::string, CompletedOperation>& completed,
		std::deque<std::string>& completedOrder,
		const StorageMutationRequest& request,
		const StorageMutationResult& result,
		std::string& diagnostic) const;
	[[nodiscard]] bool EnqueueNotification(const StorageChangeBatch& batch);
	void DrainNotifications();

	const std::filesystem::path m_directory;
	const std::uint64_t m_generation;
	const std::size_t m_maxCompletedOperations;
	const std::shared_ptr<IAtomicFileStorageFileOperations> m_fileOperations;
	mutable std::mutex m_mutex;
	bool m_open = false;
	std::uint64_t m_revision = 0;
	std::map<StorageAddress, StorageEntry> m_entries;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;
	std::unique_ptr<IAtomicFileStorageWriterLock> m_writerLock;
	std::shared_ptr<SubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<StorageChangeBatch> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

} // namespace platform::storage
