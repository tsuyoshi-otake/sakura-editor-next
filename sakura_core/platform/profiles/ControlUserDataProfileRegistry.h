/*! @file @brief Control-process-only composition for durable user-data profiles. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/profiles/DurableUserDataProfileRegistryService.h"

#include <functional>
#include <mutex>

namespace platform::profiles {

enum class ControlUserDataProfileRegistryStatus : unsigned char {
	Started,
	AlreadyStarted,
	Stopped,
	AlreadyStopped,
	LoadFailed,
	NotRunning,
	InvalidOperationId,
	Applied,
	AppliedTransient,
	OperationRejected,
	PersistConflict,
	PersistFailed,
	Imported,
	ImportRejected,
	Resolved,
	ProfileNotFound,
};

//! Every control-owned operation has one typed terminal status.  A failed durable
//! mutation has already restored the in-memory registry to its pre-operation state.
struct ControlUserDataProfileRegistryResult {
	ControlUserDataProfileRegistryStatus status = ControlUserDataProfileRegistryStatus::NotRunning;
	std::uint64_t storageRevision = 0;
	std::optional<UserDataProfileOperationResult> operation;
	std::optional<DurableUserDataProfileRegistryResult> durable;
	std::optional<UserDataProfileResolveResult> resolved;
	std::optional<UserDataProfileRegistrySnapshot> snapshot;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == ControlUserDataProfileRegistryStatus::Started
			|| status == ControlUserDataProfileRegistryStatus::AlreadyStarted
			|| status == ControlUserDataProfileRegistryStatus::Stopped
			|| status == ControlUserDataProfileRegistryStatus::AlreadyStopped
			|| status == ControlUserDataProfileRegistryStatus::Applied
			|| status == ControlUserDataProfileRegistryStatus::AppliedTransient
			|| status == ControlUserDataProfileRegistryStatus::Imported
			|| status == ControlUserDataProfileRegistryStatus::Resolved;
	}
};

//! Durable mutations keep caller-provided replay identity and CAS precondition.
struct ControlUserDataProfileRegistryMutation {
	std::string operationId;
	std::optional<std::uint64_t> expectedStorageRevision;
};

/*! 
	@brief The only profile-registry mutator composed in the hidden control process.

	This type intentionally has no IPC, HWND, PID, path-display, or UI dependency.
	Editors may receive a resolved immutable descriptor, but never retain this object
	or use its storage service.  Each persistent mutation is rolled back if its CAS
	write does not commit.  Transient-only mutations are intentionally not written.
*/
class ControlUserDataProfileRegistry final {
public:
	explicit ControlUserDataProfileRegistry(std::shared_ptr<::platform::storage::IStorageService> storage);
	[[nodiscard]] ControlUserDataProfileRegistryResult Start();
	[[nodiscard]] ControlUserDataProfileRegistryResult Stop(ControlUserDataProfileRegistryMutation shutdownSave);

	[[nodiscard]] ControlUserDataProfileRegistryResult CreateNamed(
		UserDataProfileCreateRequest request, ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult CreateTransient(
		UserDataProfileCreateRequest request, ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult Rename(UserDataProfileId profileId, std::wstring displayName,
		ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult Delete(UserDataProfileId profileId,
		ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult AssociateWorkspace(UserDataProfileId profileId, WorkspaceUri workspace,
		ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult AssociateEmptyWindow(UserDataProfileId profileId, EmptyWindowId windowId,
		ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] ControlUserDataProfileRegistryResult ImportPortableDocument(std::string_view document,
		ControlUserDataProfileRegistryMutation mutation);
	[[nodiscard]] std::string ExportPortableDocument() const;
	[[nodiscard]] ControlUserDataProfileRegistryResult SwitchTo(const UserDataProfileId& profileId) const;
	[[nodiscard]] ControlUserDataProfileRegistryResult Resolve(const UserDataProfileResolveRequest& request) const;
	//! Returns an immutable control snapshot.  IPC callers receive copies only and
	//! never gain a mutable registry or storage handle.
	[[nodiscard]] ControlUserDataProfileRegistryResult Snapshot() const;
	[[nodiscard]] std::uint64_t StorageRevision() const;

private:
	[[nodiscard]] ControlUserDataProfileRegistryResult Mutate(ControlUserDataProfileRegistryMutation mutation,
		const std::function<UserDataProfileOperationResult()>& apply);
	[[nodiscard]] static bool IsValidMutation(const ControlUserDataProfileRegistryMutation& mutation) noexcept;
	[[nodiscard]] static ControlUserDataProfileRegistryStatus PersistStatus(const DurableUserDataProfileRegistryResult& result) noexcept;

	const std::shared_ptr<::platform::storage::IStorageService> m_storage;
	mutable std::mutex m_mutex;
	UserDataProfileRegistry m_registry;
	DurableUserDataProfileRegistryService m_durable;
	bool m_started = false;
	std::uint64_t m_storageRevision = 0;
};

} // namespace platform::profiles
