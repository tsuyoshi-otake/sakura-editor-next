/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/profiles/DurableUserDataProfileRegistryService.h"

#include <algorithm>
#include <limits>

namespace platform::profiles {
namespace {

constexpr std::string_view kOwner = "platform.profiles";
constexpr std::string_view kKey = "user-data-profile-registry-v1";

DurableUserDataProfileRegistryResult Result(DurableUserDataProfileRegistryStatus status,
	std::uint64_t storageRevision, std::uint64_t registryRevision, bool replayed = false)
{
	return { status, storageRevision, registryRevision, replayed };
}

} // namespace

DurableUserDataProfileRegistryService::DurableUserDataProfileRegistryService(
	UserDataProfileRegistry& registry, ::platform::storage::IStorageService& storage) noexcept
	: m_registry(registry), m_storage(storage)
{
}

::platform::storage::StorageAddress DurableUserDataProfileRegistryService::Address()
{
	return { ::platform::storage::EStorageScope::Application, {}, std::string(kOwner), std::string(kKey) };
}

DurableUserDataProfileRegistryResult DurableUserDataProfileRegistryService::Load()
{
	const auto snapshot = m_storage.Snapshot();
	const auto address = Address();
	const auto entry = std::find_if(snapshot.entries.begin(), snapshot.entries.end(), [&address](const auto& value) {
		return value.address == address && value.target == ::platform::storage::EStorageTarget::Machine;
	});
	if (entry == snapshot.entries.end()) return Result(DurableUserDataProfileRegistryStatus::NotFound, snapshot.revision, m_registry.Revision());
	const auto decoded = DecodeUserDataProfileRegistryDocument(entry->value);
	if (decoded.status == UserDataProfileRegistryCodecStatus::UnsupportedVersion) {
		return Result(DurableUserDataProfileRegistryStatus::UnsupportedPreserved, snapshot.revision, m_registry.Revision());
	}
	if (!decoded.Decoded() || m_registry.ReplaceSnapshot(decoded.snapshot) != UserDataProfileSnapshotStatus::Applied) {
		return Result(DurableUserDataProfileRegistryStatus::CorruptPreserved, snapshot.revision, m_registry.Revision());
	}
	return Result(DurableUserDataProfileRegistryStatus::Loaded, snapshot.revision, m_registry.Revision());
}

DurableUserDataProfileRegistryResult DurableUserDataProfileRegistryService::Persist(
	std::string operationId, std::optional<std::uint64_t> expectedStorageRevision)
{
	const auto encoded = EncodeUserDataProfileRegistryDocument(m_registry.Snapshot());
	if (encoded.empty()) return Result(DurableUserDataProfileRegistryStatus::InvalidArgument, 0, m_registry.Revision());
	::platform::storage::StorageMutationRequest request;
	request.operationId = std::move(operationId);
	request.expectedRevision = expectedStorageRevision;
	request.mutations = { { Address(), ::platform::storage::EStorageTarget::Machine, encoded } };
	const auto result = m_storage.Apply(request);
	if (result.status == ::platform::storage::EStorageMutationStatus::Conflict) {
		return Result(DurableUserDataProfileRegistryStatus::Conflict, result.revision, m_registry.Revision(), result.replayed);
	}
	if (result.status != ::platform::storage::EStorageMutationStatus::Succeeded
		&& result.status != ::platform::storage::EStorageMutationStatus::NotApplicable) {
		return Result(DurableUserDataProfileRegistryStatus::StorageFailure, result.revision, m_registry.Revision());
	}
	return Result(result.replayed ? DurableUserDataProfileRegistryStatus::Replayed : DurableUserDataProfileRegistryStatus::Saved,
		result.revision, m_registry.Revision(), result.replayed);
}

DurableUserDataProfileRegistryResult DurableUserDataProfileRegistryService::Save(
	std::string operationId, std::optional<std::uint64_t> expectedStorageRevision)
{
	return Persist(std::move(operationId), expectedStorageRevision);
}

std::string DurableUserDataProfileRegistryService::ExportPortableDocument() const
{
	return EncodeUserDataProfileRegistryDocument(m_registry.Snapshot());
}

DurableUserDataProfileRegistryResult DurableUserDataProfileRegistryService::ImportPortableDocument(
	std::string_view document, std::string operationId, std::optional<std::uint64_t> expectedStorageRevision)
{
	const auto decoded = DecodeUserDataProfileRegistryDocument(document);
	if (!decoded.Decoded()) return Result(DurableUserDataProfileRegistryStatus::InvalidArgument, 0, m_registry.Revision());
	const auto before = m_registry.Snapshot(true);
	auto merged = before;
	for (const auto& profile : decoded.snapshot.profiles) {
		// The portable default is a schema anchor, not an instruction to replace
		// this installation's protected default descriptor.
		if (profile.kind == UserDataProfileKind::Default) continue;
		if (std::any_of(merged.profiles.begin(), merged.profiles.end(), [&profile](const auto& existing) { return existing.profileId == profile.profileId; }))
			return Result(DurableUserDataProfileRegistryStatus::DuplicateProfileId, 0, m_registry.Revision());
		if (std::any_of(merged.profiles.begin(), merged.profiles.end(), [&profile](const auto& existing) { return existing.displayName == profile.displayName; }))
			return Result(DurableUserDataProfileRegistryStatus::DuplicateDisplayName, 0, m_registry.Revision());
		merged.profiles.push_back(profile);
	}
	for (const auto& association : decoded.snapshot.workspaceAssociations) {
		if (m_registry.FindProfileForWorkspace(association.first)) return Result(DurableUserDataProfileRegistryStatus::AssociationConflict, 0, m_registry.Revision());
		merged.workspaceAssociations.push_back(association);
	}
	for (const auto& association : decoded.snapshot.emptyWindowAssociations) {
		if (m_registry.FindProfileForEmptyWindow(association.first)) return Result(DurableUserDataProfileRegistryStatus::AssociationConflict, 0, m_registry.Revision());
		merged.emptyWindowAssociations.push_back(association);
	}
	if (before.revision == (std::numeric_limits<std::uint64_t>::max)()) {
		return Result(DurableUserDataProfileRegistryStatus::InvalidArgument, 0, before.revision);
	}
	merged.revision = before.revision + 1;
	if (m_registry.ReplaceSnapshot(merged) != UserDataProfileSnapshotStatus::Applied) return Result(DurableUserDataProfileRegistryStatus::InvalidArgument, 0, before.revision);
	const auto persisted = Persist(std::move(operationId), expectedStorageRevision);
	if (!persisted.Succeeded()) (void)m_registry.ReplaceSnapshot(before);
	return persisted;
}

UserDataProfileResolveResult DurableUserDataProfileRegistryService::Resolve(const UserDataProfileResolveRequest& request) const
{
	return m_registry.Resolve(request);
}

} // namespace platform::profiles
