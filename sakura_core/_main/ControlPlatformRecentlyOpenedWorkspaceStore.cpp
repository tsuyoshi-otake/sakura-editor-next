/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "_main/ControlPlatformRecentlyOpenedWorkspaceStore.h"

#include <bcrypt.h>

#include <array>
#include <utility>

namespace {

constexpr char kOwner[] = "workbench.recentlyOpened";
constexpr char kKey[] = "state";

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
	static constexpr char hex[] = "0123456789abcdef";
	std::string result = "workbench.recentlyOpened-";
	result.reserve(result.size() + random.size() * 2);
	for (const auto value : random) { result.push_back(hex[value >> 4]); result.push_back(hex[value & 0xf]); }
	return result;
}

bool IsValidOperationId(const std::string& value) noexcept
{
	return !value.empty() && value.size() <= platform::storage::kMaximumStorageOperationIdBytes
		&& platform::storage::IsValidStorageUtf8(value, false);
}

} // namespace

CControlPlatformRecentlyOpenedWorkspaceStore::CControlPlatformRecentlyOpenedWorkspaceStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId) :
	CControlPlatformRecentlyOpenedWorkspaceStore(std::move(canonicalProfileId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.waitForStorageCacheReady = [&runtime](std::chrono::milliseconds timeout) {
			return runtime.WaitForStorageCacheReady(timeout);
		},
		.find = [&runtime](const platform::storage::StorageAddress& address) { return runtime.Find(address); },
		.apply = [&runtime](const platform::storage::StorageMutationRequest& request) { return runtime.Apply(request); },
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformRecentlyOpenedWorkspaceStore::CControlPlatformRecentlyOpenedWorkspaceStore(
	std::string canonicalProfileId, workbench::recent::ControlPlatformRecentlyOpenedWorkspaceStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)), m_dependencies(std::move(dependencies))
{
}

std::optional<platform::storage::StorageAddress> CControlPlatformRecentlyOpenedWorkspaceStore::Address() const noexcept
{
	platform::storage::StorageAddress address{ platform::storage::EStorageScope::Profile, m_canonicalProfileId, kOwner, kKey };
	return address.IsValid() ? std::optional<platform::storage::StorageAddress>(std::move(address)) : std::nullopt;
}

bool CControlPlatformRecentlyOpenedWorkspaceStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find && m_dependencies.apply && m_dependencies.operationIdFactory;
}

bool CControlPlatformRecentlyOpenedWorkspaceStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId && coordinates.generation != 0;
}

workbench::recent::RecentlyOpenedWorkspaceStoreLoadResult CControlPlatformRecentlyOpenedWorkspaceStore::Load()
{
	using namespace workbench::recent;
	using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
	std::scoped_lock lock(m_mutex);
	m_coordinates.reset();
	if (!HasUsableDependencies() || !Address()) return { ERecentlyOpenedWorkspaceStoreLoadStatus::Failed, std::nullopt, "recent workspace store is not configured" };
	auto before = m_dependencies.storageCacheCoordinates();
	if (before.code == EEditorControlStorageCacheCoordinateCode::Resynchronizing
		&& m_dependencies.waitForStorageCacheReady) {
		const auto waited = m_dependencies.waitForStorageCacheReady(std::chrono::seconds(2));
		if (waited.code == platform::controlipc::EEditorControlStorageCacheWaitCode::Ready && waited.coordinates) {
			before = { EEditorControlStorageCacheCoordinateCode::Ready, waited.state, waited.coordinates, waited.diagnostic };
		}
	}
	if (before.code != EEditorControlStorageCacheCoordinateCode::Ready || !before.coordinates || !IsExpectedProfile(*before.coordinates)) {
		return { before.code == EEditorControlStorageCacheCoordinateCode::Failed ? ERecentlyOpenedWorkspaceStoreLoadStatus::Failed : ERecentlyOpenedWorkspaceStoreLoadStatus::Unavailable,
			std::nullopt, "control-platform storage cache is unavailable" };
	}
	const auto entry = m_dependencies.find(*Address());
	const auto after = m_dependencies.storageCacheCoordinates();
	if (after.code != EEditorControlStorageCacheCoordinateCode::Ready || !after.coordinates || !IsExpectedProfile(*after.coordinates)) {
		return { after.code == EEditorControlStorageCacheCoordinateCode::Failed ? ERecentlyOpenedWorkspaceStoreLoadStatus::Failed : ERecentlyOpenedWorkspaceStoreLoadStatus::Unavailable,
			std::nullopt, "control-platform storage cache is unavailable" };
	}
	if (*before.coordinates != *after.coordinates) return { ERecentlyOpenedWorkspaceStoreLoadStatus::Unavailable, std::nullopt, "control-platform storage changed during recent history load" };
	m_coordinates = *after.coordinates;
	if (!entry) return { ERecentlyOpenedWorkspaceStoreLoadStatus::NotFound, std::nullopt, {} };
	if (entry->address != *Address() || entry->target != platform::storage::EStorageTarget::User) {
		return { ERecentlyOpenedWorkspaceStoreLoadStatus::Failed, std::nullopt, "recent workspace storage entry has an invalid scope or target" };
	}
	return { ERecentlyOpenedWorkspaceStoreLoadStatus::Succeeded, entry->value, {} };
}

workbench::recent::RecentlyOpenedWorkspaceStoreSaveResult CControlPlatformRecentlyOpenedWorkspaceStore::Save(std::string payload)
{
	using namespace workbench::recent;
	using platform::controlipc::EEditorControlStorageApplyCode;
	std::scoped_lock lock(m_mutex);
	if (!HasUsableDependencies() || !Address() || !m_coordinates) return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "recent workspace save requires a coherent load" };
	if (payload.empty() || payload.size() > platform::storage::kMaximumStorageStringBytes || !platform::storage::IsValidStorageUtf8(payload, false)) {
		return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "recent workspace payload is invalid" };
	}
	const auto operationId = m_dependencies.operationIdFactory();
	if (!IsValidOperationId(operationId)) return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "recent workspace operation identity is invalid" };
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_coordinates->storageRevision,
		.mutations = { { *Address(), platform::storage::EStorageTarget::User, std::move(payload) } },
	};
	auto result = m_dependencies.apply(request);
	if (result.code == EEditorControlStorageApplyCode::RetryWithSameOperationId) result = m_dependencies.apply(request);
	if (result.code == EEditorControlStorageApplyCode::Succeeded || result.code == EEditorControlStorageApplyCode::NotApplicable) {
		if (result.storageResult && result.storageResult->revision > m_coordinates->storageRevision) m_coordinates->storageRevision = result.storageResult->revision;
		return { ERecentlyOpenedWorkspaceStoreSaveStatus::Succeeded, {} };
	}
	if (result.code == EEditorControlStorageApplyCode::ConflictResnapshotScheduled || result.code == EEditorControlStorageApplyCode::ResnapshotScheduled) {
		return { ERecentlyOpenedWorkspaceStoreSaveStatus::Conflict, "recent workspace storage conflict requires resnapshot" };
	}
	if (result.code == EEditorControlStorageApplyCode::NotReady || result.code == EEditorControlStorageApplyCode::OperationInFlight
		|| result.code == EEditorControlStorageApplyCode::Stopped) {
		return { ERecentlyOpenedWorkspaceStoreSaveStatus::Unavailable, "control-platform storage writer is unavailable" };
	}
	return { ERecentlyOpenedWorkspaceStoreSaveStatus::Failed, "control-platform storage write failed" };
}
