/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "_main/ControlPlatformProjectCatalogStore.h"

#include <bcrypt.h>

#include <array>
#include <utility>

namespace {

constexpr char kOwner[] = "workbench.projects";
constexpr char kKey[] = "state";

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
	static constexpr char hex[] = "0123456789abcdef";
	std::string result = "workbench.projects-";
	result.reserve(result.size() + random.size() * 2);
	for (const auto value : random) {
		result.push_back(hex[value >> 4]);
		result.push_back(hex[value & 0xf]);
	}
	return result;
}

bool IsValidOperationId(const std::string& value) noexcept
{
	return !value.empty() && value.size() <= platform::storage::kMaximumStorageOperationIdBytes
		&& platform::storage::IsValidStorageUtf8(value, false);
}

} // namespace

CControlPlatformProjectCatalogStore::CControlPlatformProjectCatalogStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime,
	std::string canonicalProfileId) :
	CControlPlatformProjectCatalogStore(std::move(canonicalProfileId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.waitForStorageCacheReady = [&runtime](const std::chrono::milliseconds timeout) {
			return runtime.WaitForStorageCacheReady(timeout);
		},
		.find = [&runtime](const platform::storage::StorageAddress& address) {
			return runtime.Find(address);
		},
		.apply = [&runtime](const platform::storage::StorageMutationRequest& request) {
			return runtime.Apply(request);
		},
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformProjectCatalogStore::CControlPlatformProjectCatalogStore(
	std::string canonicalProfileId,
	workbench::projects::ControlPlatformProjectCatalogStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)),
	m_dependencies(std::move(dependencies))
{
}

std::optional<platform::storage::StorageAddress>
CControlPlatformProjectCatalogStore::Address() const noexcept
{
	platform::storage::StorageAddress address{
		platform::storage::EStorageScope::Profile, m_canonicalProfileId, kOwner, kKey };
	return address.IsValid()
		? std::optional<platform::storage::StorageAddress>(std::move(address)) : std::nullopt;
}

bool CControlPlatformProjectCatalogStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find
		&& m_dependencies.apply && m_dependencies.operationIdFactory;
}

bool CControlPlatformProjectCatalogStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId
		&& coordinates.generation != 0;
}

workbench::projects::ProjectCatalogStoreLoadResult
CControlPlatformProjectCatalogStore::Load()
{
	using namespace workbench::projects;
	using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
	std::scoped_lock lock(m_mutex);
	m_coordinates.reset();
	if (!HasUsableDependencies() || !Address()) {
		return { EProjectCatalogStoreLoadStatus::Failed, std::nullopt,
			"project catalog store is not configured" };
	}
	auto before = m_dependencies.storageCacheCoordinates();
	if (before.code == EEditorControlStorageCacheCoordinateCode::Resynchronizing
		&& m_dependencies.waitForStorageCacheReady) {
		const auto waited = m_dependencies.waitForStorageCacheReady(std::chrono::seconds(2));
		if (waited.code == platform::controlipc::EEditorControlStorageCacheWaitCode::Ready
			&& waited.coordinates) {
			before = { EEditorControlStorageCacheCoordinateCode::Ready, waited.state,
				waited.coordinates, waited.diagnostic };
		}
	}
	if (before.code != EEditorControlStorageCacheCoordinateCode::Ready
		|| !before.coordinates || !IsExpectedProfile(*before.coordinates)) {
		return { before.code == EEditorControlStorageCacheCoordinateCode::Failed
			? EProjectCatalogStoreLoadStatus::Failed : EProjectCatalogStoreLoadStatus::Unavailable,
			std::nullopt, "control-platform storage cache is unavailable" };
	}
	const auto address = Address();
	const auto entry = m_dependencies.find(*address);
	const auto after = m_dependencies.storageCacheCoordinates();
	if (after.code != EEditorControlStorageCacheCoordinateCode::Ready
		|| !after.coordinates || !IsExpectedProfile(*after.coordinates)) {
		return { after.code == EEditorControlStorageCacheCoordinateCode::Failed
			? EProjectCatalogStoreLoadStatus::Failed : EProjectCatalogStoreLoadStatus::Unavailable,
			std::nullopt, "control-platform storage cache is unavailable" };
	}
	if (*before.coordinates != *after.coordinates) {
		return { EProjectCatalogStoreLoadStatus::Unavailable, std::nullopt,
			"control-platform storage changed during project catalog load" };
	}
	m_coordinates = *after.coordinates;
	if (!entry) return { EProjectCatalogStoreLoadStatus::NotFound, std::nullopt, {} };
	if (entry->address != *address || entry->target != platform::storage::EStorageTarget::User) {
		return { EProjectCatalogStoreLoadStatus::Failed, std::nullopt,
			"project catalog storage entry has an invalid scope or target" };
	}
	return { EProjectCatalogStoreLoadStatus::Succeeded, entry->value, {} };
}

workbench::projects::ProjectCatalogStoreSaveResult
CControlPlatformProjectCatalogStore::Save(std::string payload)
{
	using namespace workbench::projects;
	using platform::controlipc::EEditorControlStorageApplyCode;
	std::scoped_lock lock(m_mutex);
	const auto address = Address();
	if (!HasUsableDependencies() || !address || !m_coordinates) {
		return { EProjectCatalogStoreSaveStatus::Failed,
			"project catalog save requires a coherent load" };
	}
	if (payload.empty() || payload.size() > platform::storage::kMaximumStorageStringBytes
		|| !platform::storage::IsValidStorageUtf8(payload, false)) {
		return { EProjectCatalogStoreSaveStatus::Failed, "project catalog payload is invalid" };
	}
	const auto operationId = m_dependencies.operationIdFactory();
	if (!IsValidOperationId(operationId)) {
		return { EProjectCatalogStoreSaveStatus::Failed,
			"project catalog operation identity is invalid" };
	}
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_coordinates->storageRevision,
		.mutations = { { *address, platform::storage::EStorageTarget::User, std::move(payload) } },
	};
	auto result = m_dependencies.apply(request);
	if (result.code == EEditorControlStorageApplyCode::RetryWithSameOperationId) {
		result = m_dependencies.apply(request);
	}
	if (result.code == EEditorControlStorageApplyCode::Succeeded
		|| result.code == EEditorControlStorageApplyCode::NotApplicable) {
		if (result.storageResult
			&& result.storageResult->revision > m_coordinates->storageRevision) {
			m_coordinates->storageRevision = result.storageResult->revision;
		}
		return { EProjectCatalogStoreSaveStatus::Succeeded, {} };
	}
	if (result.code == EEditorControlStorageApplyCode::ConflictResnapshotScheduled
		|| result.code == EEditorControlStorageApplyCode::ResnapshotScheduled) {
		return { EProjectCatalogStoreSaveStatus::Conflict,
			"project catalog storage conflict requires resnapshot" };
	}
	if (result.code == EEditorControlStorageApplyCode::NotReady
		|| result.code == EEditorControlStorageApplyCode::OperationInFlight
		|| result.code == EEditorControlStorageApplyCode::Stopped) {
		return { EProjectCatalogStoreSaveStatus::Unavailable,
			"control-platform storage writer is unavailable" };
	}
	return { EProjectCatalogStoreSaveStatus::Failed,
		"control-platform storage write failed" };
}
