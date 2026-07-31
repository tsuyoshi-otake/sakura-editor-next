/*! @file
 * @brief Durable store contracts for working-copy backups and session manifests.
 */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/editor/persistence/WorkingCopyPersistenceTypes.h"

#include <optional>
#include <string>

namespace workbench::editor::persistence {

enum class EWorkingCopyPersistenceLoadStatus : std::uint8_t {
	Loaded,
	NotFound,
	InvalidStoredRecord,
	Unavailable,
	Failed,
};

enum class EWorkingCopyPersistenceWriteStatus : std::uint8_t {
	Persisted,
	Deleted,
	Conflict,
	RetryExhausted,
	Unavailable,
	Failed,
};

struct WorkingCopyBackupLoadResult final {
	EWorkingCopyPersistenceLoadStatus status = EWorkingCopyPersistenceLoadStatus::Failed;
	std::optional<WorkingCopyBackup> backup;
	//! Must not expose logical IDs, canonical resource text, physical paths, or record bytes.
	std::wstring diagnostic;

	[[nodiscard]] bool Loaded() const noexcept
	{
		return status == EWorkingCopyPersistenceLoadStatus::Loaded && backup.has_value();
	}
};

struct EditorSessionLoadResult final {
	EWorkingCopyPersistenceLoadStatus status = EWorkingCopyPersistenceLoadStatus::Failed;
	std::optional<EditorSessionManifest> manifest;
	//! Must not expose logical IDs, physical paths, or record bytes.
	std::wstring diagnostic;

	[[nodiscard]] bool Loaded() const noexcept
	{
		return status == EWorkingCopyPersistenceLoadStatus::Loaded && manifest.has_value();
	}
};

struct WorkingCopyPersistenceWriteResult final {
	EWorkingCopyPersistenceWriteStatus status = EWorkingCopyPersistenceWriteStatus::Failed;
	std::uint64_t generation = 0;
	//! True only for a completed same-operationId, byte-for-byte identical request replay.
	bool replayed = false;
	//! Must not expose logical IDs, canonical resource text, physical paths, or record bytes.
	std::wstring diagnostic;
};

/*! 
	@brief Compare-and-swap store for complete dirty working-copy generations.

	Callers provide a nonempty bounded operationId. Implementations retain enough
	completed-operation history to replay exactly the same immutable request; reusing an
	operationId with changed payload, expected generation, or delete intent is a terminal
	failure. Save and Delete compare expectedGeneration against the currently stored
	generation. Delete must remove only that exact generation and must never delete a
	newer backup that arrived after the caller loaded an older one.
*/
class IWorkingCopyBackupStore {
public:
	virtual ~IWorkingCopyBackupStore() = default;
	[[nodiscard]] virtual WorkingCopyBackupLoadResult Load(
		const WorkingCopyPersistenceScope& scope,
		const WorkingCopyPersistenceIdentity& identity) = 0;
	[[nodiscard]] virtual WorkingCopyPersistenceWriteResult Save(
		const WorkingCopyBackup& backup,
		std::optional<std::uint64_t> expectedGeneration,
		const std::string& operationId) = 0;
	[[nodiscard]] virtual WorkingCopyPersistenceWriteResult Delete(
		const WorkingCopyPersistenceScope& scope,
		const WorkingCopyPersistenceIdentity& identity,
		std::uint64_t expectedGeneration,
		const std::string& operationId) = 0;
};

/*! 
	@brief Compare-and-swap store for the one-group editor restore manifest.

	The same operation-ID replay and immutable-request rule applies. A delete with an old
	expected generation must conflict rather than remove a newer manifest. Corrupt durable
	bytes are reported as InvalidStoredRecord; deciding whether to retain, quarantine, or
	delete those bytes belongs to the composition caller, never this contract.
*/
class IEditorSessionStore {
public:
	virtual ~IEditorSessionStore() = default;
	[[nodiscard]] virtual EditorSessionLoadResult Load(const WorkingCopyPersistenceScope& scope) = 0;
	[[nodiscard]] virtual WorkingCopyPersistenceWriteResult Save(
		const EditorSessionManifest& manifest,
		std::optional<std::uint64_t> expectedGeneration,
		const std::string& operationId) = 0;
	[[nodiscard]] virtual WorkingCopyPersistenceWriteResult Delete(
		const WorkingCopyPersistenceScope& scope,
		std::uint64_t expectedGeneration,
		const std::string& operationId) = 0;
};

//! Composition-owned aggregate. Pure lifecycle code may still consume the two
//! narrower contracts independently, while production owns them as one service.
class IWorkingCopyPersistenceStore : public IWorkingCopyBackupStore, public IEditorSessionStore {
public:
	~IWorkingCopyPersistenceStore() override = default;
};

} // namespace workbench::editor::persistence
