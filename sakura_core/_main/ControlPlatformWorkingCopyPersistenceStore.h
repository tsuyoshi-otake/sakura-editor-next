/*! @file
 * @brief Control-platform durable adapter for working-copy backups and editor sessions.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/EditorControlPlatformRuntime.h"
#include "workbench/editor/persistence/IWorkingCopyPersistenceStore.h"

#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace workbench::editor::persistence {

/*! @brief Narrow, transport-free seam around the editor control-platform facade. */
struct ControlPlatformWorkingCopyPersistenceStoreDependencies final {
	std::function<platform::controlipc::EditorControlStorageCacheCoordinateResult()> storageCacheCoordinates;
	std::function<std::optional<platform::storage::StorageEntry>(const platform::storage::StorageAddress&)> find;
	std::function<platform::controlipc::EditorControlStorageApplyResult(
		const platform::storage::StorageMutationRequest&)> apply;
	//! Production supplies a cryptographically random factory. Public store operations
	//! nevertheless use their caller-supplied operationId so replay identity is stable.
	std::function<std::string()> operationIdFactory;
};

} // namespace workbench::editor::persistence

/*! 
	@brief Composition-root bridge from working-copy persistence to control-owned storage.

	The one class owns no files, IPC endpoints, HWNDs, paths, or display labels. It
	stores each logical record as one small manifest plus bounded chunks in one atomic
	control-storage mutation. Corrupt records are sticky per manifest address, so this
	adapter never overwrites or deletes bytes it cannot validate.
*/
class CControlPlatformWorkingCopyPersistenceStore final
	: public workbench::editor::persistence::IWorkingCopyPersistenceStore {
public:
	CControlPlatformWorkingCopyPersistenceStore(
		platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId);
	CControlPlatformWorkingCopyPersistenceStore(std::string canonicalProfileId,
		workbench::editor::persistence::ControlPlatformWorkingCopyPersistenceStoreDependencies dependencies);

	[[nodiscard]] workbench::editor::persistence::WorkingCopyBackupLoadResult Load(
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope,
		const workbench::editor::persistence::WorkingCopyPersistenceIdentity& identity) override;
	[[nodiscard]] workbench::editor::persistence::WorkingCopyPersistenceWriteResult Save(
		const workbench::editor::persistence::WorkingCopyBackup& backup,
		std::optional<std::uint64_t> expectedGeneration, const std::string& operationId) override;
	[[nodiscard]] workbench::editor::persistence::WorkingCopyPersistenceWriteResult Delete(
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope,
		const workbench::editor::persistence::WorkingCopyPersistenceIdentity& identity,
		std::uint64_t expectedGeneration, const std::string& operationId) override;

	[[nodiscard]] workbench::editor::persistence::EditorSessionLoadResult Load(
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope) override;
	[[nodiscard]] workbench::editor::persistence::WorkingCopyPersistenceWriteResult Save(
		const workbench::editor::persistence::EditorSessionManifest& manifest,
		std::optional<std::uint64_t> expectedGeneration, const std::string& operationId) override;
	[[nodiscard]] workbench::editor::persistence::WorkingCopyPersistenceWriteResult Delete(
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope,
		std::uint64_t expectedGeneration, const std::string& operationId) override;

private:
	struct CapturedRecord final {
		platform::controlipc::EditorControlStorageCacheCoordinates coordinates;
		std::optional<std::uint64_t> generation;
		std::vector<platform::storage::StorageAddress> chunkAddresses;
		//! Binds an address-derived cache entry to the decoded/requested logical record.
		//! This remains a required second check even though the address digest is collision-resistant.
		workbench::editor::persistence::WorkingCopyPersistenceScope scope;
		std::optional<workbench::editor::persistence::WorkingCopyPersistenceIdentity> identity;
	};
	struct CompletedOperation final {
		std::string fingerprint;
		workbench::editor::persistence::WorkingCopyPersistenceWriteResult result;
	};

	enum class ERecordKind : std::uint8_t { Backup, Session };

	[[nodiscard]] bool HasUsableDependencies() const noexcept;
	[[nodiscard]] bool IsExpectedProfile(
		const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept;
	[[nodiscard]] bool IsExpectedScope(
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope) const noexcept;
	[[nodiscard]] std::optional<platform::storage::StorageAddress> ManifestAddress(ERecordKind kind,
		const workbench::editor::persistence::WorkingCopyPersistenceScope& scope,
		const workbench::editor::persistence::WorkingCopyPersistenceIdentity* identity = nullptr) const;
	[[nodiscard]] std::optional<platform::storage::StorageAddress> ChunkAddress(
		const platform::storage::StorageAddress& manifest, std::size_t index) const;
	[[nodiscard]] bool IsInvalid(const platform::storage::StorageAddress& manifest) const;
	void RememberInvalid(const platform::storage::StorageAddress& manifest);
	[[nodiscard]] std::optional<workbench::editor::persistence::WorkingCopyPersistenceWriteResult> ReplayCompleted(
		const std::string& operationId, const std::string& fingerprint) const;
	void RememberCompleted(std::string operationId, std::string fingerprint,
		workbench::editor::persistence::WorkingCopyPersistenceWriteResult result);

	const std::string m_canonicalProfileId;
	const workbench::editor::persistence::ControlPlatformWorkingCopyPersistenceStoreDependencies m_dependencies;
	mutable std::mutex m_mutex;
	std::map<platform::storage::StorageAddress, CapturedRecord> m_captured;
	std::map<platform::storage::StorageAddress, bool> m_invalidStoredRecords;
	std::map<std::string, CompletedOperation> m_completed;
	std::deque<std::string> m_completedOrder;
};
