/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"

#include "platform/storage/StorageTypes.h"

namespace platform::profiles {
namespace {

bool SameDurableContent(const UserDataProfileRegistrySnapshot& left, const UserDataProfileRegistrySnapshot& right)
{
	if (left.defaultProfileId != right.defaultProfileId || left.profiles != right.profiles
		|| left.workspaceAssociations.size() != right.workspaceAssociations.size()
		|| left.emptyWindowAssociations != right.emptyWindowAssociations) return false;
	for (std::size_t index = 0; index < left.workspaceAssociations.size(); ++index) {
		const auto& leftAssociation = left.workspaceAssociations[index];
		const auto& rightAssociation = right.workspaceAssociations[index];
		if (leftAssociation.second != rightAssociation.second
			|| !::platform::uri::UriIdentityService::IsEqual(leftAssociation.first, rightAssociation.first)) return false;
	}
	return true;
}

} // namespace

ControlUserDataProfileRegistry::ControlUserDataProfileRegistry(std::shared_ptr<::platform::storage::IStorageService> storage)
	: m_storage(std::move(storage)), m_durable(m_registry, *m_storage)
{
}

bool ControlUserDataProfileRegistry::IsValidMutation(const ControlUserDataProfileRegistryMutation& mutation) noexcept
{
	return !mutation.operationId.empty()
		&& mutation.operationId.size() <= ::platform::storage::kMaximumStorageOperationIdBytes
		&& ::platform::storage::IsValidStorageUtf8(mutation.operationId, false);
}

ControlUserDataProfileRegistryStatus ControlUserDataProfileRegistry::PersistStatus(
	const DurableUserDataProfileRegistryResult& result) noexcept
{
	return result.status == DurableUserDataProfileRegistryStatus::Conflict
		? ControlUserDataProfileRegistryStatus::PersistConflict
		: ControlUserDataProfileRegistryStatus::PersistFailed;
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Start()
{
	std::lock_guard lock(m_mutex);
	if (m_started) return { ControlUserDataProfileRegistryStatus::AlreadyStarted, m_storageRevision };
	const auto loaded = m_durable.Load();
	m_storageRevision = loaded.storageRevision;
	if (loaded.status != DurableUserDataProfileRegistryStatus::Loaded
		&& loaded.status != DurableUserDataProfileRegistryStatus::NotFound) {
		return { ControlUserDataProfileRegistryStatus::LoadFailed, m_storageRevision, std::nullopt, loaded };
	}
	m_started = true;
	return { ControlUserDataProfileRegistryStatus::Started, m_storageRevision, std::nullopt, loaded };
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Stop(ControlUserDataProfileRegistryMutation shutdownSave)
{
	std::lock_guard lock(m_mutex);
	if (!m_started) return { ControlUserDataProfileRegistryStatus::AlreadyStopped, m_storageRevision };
	if (!IsValidMutation(shutdownSave)) {
		m_started = false;
		return { ControlUserDataProfileRegistryStatus::InvalidOperationId, m_storageRevision };
	}
	const auto saved = m_durable.Save(std::move(shutdownSave.operationId), shutdownSave.expectedStorageRevision);
	m_storageRevision = saved.storageRevision;
	m_started = false;
	return { saved.Succeeded() ? ControlUserDataProfileRegistryStatus::Stopped : PersistStatus(saved),
		m_storageRevision, std::nullopt, saved };
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Mutate(ControlUserDataProfileRegistryMutation mutation,
	const std::function<UserDataProfileOperationResult()>& apply)
{
	std::lock_guard lock(m_mutex);
	if (!m_started) return { ControlUserDataProfileRegistryStatus::NotRunning, m_storageRevision };
	if (!IsValidMutation(mutation)) return { ControlUserDataProfileRegistryStatus::InvalidOperationId, m_storageRevision };
	const auto before = m_registry.Snapshot(true);
	const auto durableBefore = m_registry.Snapshot();
	const auto operation = apply();
	if (!operation.Changed()) return { ControlUserDataProfileRegistryStatus::OperationRejected, m_storageRevision, operation };
	if (SameDurableContent(m_registry.Snapshot(), durableBefore)) {
		return { ControlUserDataProfileRegistryStatus::AppliedTransient, m_storageRevision, operation };
	}
	const auto saved = m_durable.Save(std::move(mutation.operationId), mutation.expectedStorageRevision);
	m_storageRevision = saved.storageRevision;
	if (!saved.Succeeded()) {
		(void)m_registry.ReplaceSnapshot(before);
		return { PersistStatus(saved), m_storageRevision, operation, saved };
	}
	return { ControlUserDataProfileRegistryStatus::Applied, m_storageRevision, operation, saved };
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::CreateNamed(
	UserDataProfileCreateRequest request, ControlUserDataProfileRegistryMutation mutation)
{
	request.kind = UserDataProfileKind::Normal;
	return Mutate(std::move(mutation), [this, request = std::move(request)]() mutable { return m_registry.Create(std::move(request)); });
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::CreateTransient(
	UserDataProfileCreateRequest request, ControlUserDataProfileRegistryMutation mutation)
{
	request.kind = UserDataProfileKind::Transient;
	return Mutate(std::move(mutation), [this, request = std::move(request)]() mutable { return m_registry.Create(std::move(request)); });
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Rename(UserDataProfileId profileId, std::wstring displayName,
	ControlUserDataProfileRegistryMutation mutation)
{
	return Mutate(std::move(mutation), [this, profileId = std::move(profileId), displayName = std::move(displayName)]() mutable {
		return m_registry.Rename(profileId, std::move(displayName));
	});
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Delete(UserDataProfileId profileId,
	ControlUserDataProfileRegistryMutation mutation)
{
	return Mutate(std::move(mutation), [this, profileId = std::move(profileId)] { return m_registry.Remove(profileId); });
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::AssociateWorkspace(UserDataProfileId profileId, WorkspaceUri workspace,
	ControlUserDataProfileRegistryMutation mutation)
{
	return Mutate(std::move(mutation), [this, profileId = std::move(profileId), workspace = std::move(workspace)]() mutable {
		return m_registry.AssociateWorkspace(profileId, std::move(workspace));
	});
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::AssociateEmptyWindow(UserDataProfileId profileId, EmptyWindowId windowId,
	ControlUserDataProfileRegistryMutation mutation)
{
	return Mutate(std::move(mutation), [this, profileId = std::move(profileId), windowId = std::move(windowId)]() mutable {
		return m_registry.AssociateEmptyWindow(profileId, std::move(windowId));
	});
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::ImportPortableDocument(std::string_view document,
	ControlUserDataProfileRegistryMutation mutation)
{
	std::lock_guard lock(m_mutex);
	if (!m_started) return { ControlUserDataProfileRegistryStatus::NotRunning, m_storageRevision };
	if (!IsValidMutation(mutation)) return { ControlUserDataProfileRegistryStatus::InvalidOperationId, m_storageRevision };
	const auto imported = m_durable.ImportPortableDocument(document, std::move(mutation.operationId), mutation.expectedStorageRevision);
	m_storageRevision = imported.storageRevision;
	return { imported.Succeeded() ? ControlUserDataProfileRegistryStatus::Imported : ControlUserDataProfileRegistryStatus::ImportRejected,
		m_storageRevision, std::nullopt, imported };
}

std::string ControlUserDataProfileRegistry::ExportPortableDocument() const
{
	std::lock_guard lock(m_mutex);
	return m_started ? m_durable.ExportPortableDocument() : std::string{};
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::SwitchTo(const UserDataProfileId& profileId) const
{
	return Resolve({ profileId, std::nullopt, std::nullopt });
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Resolve(const UserDataProfileResolveRequest& request) const
{
	std::lock_guard lock(m_mutex);
	if (!m_started) return { ControlUserDataProfileRegistryStatus::NotRunning, m_storageRevision };
	const auto resolved = m_durable.Resolve(request);
	return { resolved.Resolved() ? ControlUserDataProfileRegistryStatus::Resolved : ControlUserDataProfileRegistryStatus::ProfileNotFound,
		m_storageRevision, std::nullopt, std::nullopt, resolved };
}

ControlUserDataProfileRegistryResult ControlUserDataProfileRegistry::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	if (!m_started) return { ControlUserDataProfileRegistryStatus::NotRunning, m_storageRevision };
	ControlUserDataProfileRegistryResult result;
	result.status = ControlUserDataProfileRegistryStatus::Resolved;
	result.storageRevision = m_storageRevision;
	// Include live transient profiles here. They are intentionally absent only
	// from durable export, not from a control-authoritative editor snapshot.
	result.snapshot = m_registry.Snapshot(true);
	return result;
}

std::uint64_t ControlUserDataProfileRegistry::StorageRevision() const
{
	std::lock_guard lock(m_mutex);
	return m_storageRevision;
}

} // namespace platform::profiles
