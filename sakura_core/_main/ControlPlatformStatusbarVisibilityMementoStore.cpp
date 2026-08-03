/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "_main/ControlPlatformStatusbarVisibilityMementoStore.h"

#include "workbench/statusbar/StatusbarVisibilityMementoCodec.h"

#include <bcrypt.h>

#include <array>

namespace {

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
	static constexpr char hex[] = "0123456789abcdef";
	std::string result = "workbench.statusbar-";
	for (const auto value : random) {
		result.push_back(hex[value >> 4]);
		result.push_back(hex[value & 0x0f]);
	}
	return result;
}

workbench::statusbar::StatusbarMementoSaveResult SaveResultFor(
	platform::controlipc::EEditorControlStorageApplyCode code)
{
	using ApplyCode = platform::controlipc::EEditorControlStorageApplyCode;
	using SaveStatus = workbench::statusbar::EStatusbarMementoSaveStatus;
	switch (code) {
	case ApplyCode::Succeeded: return { SaveStatus::Persisted, {} };
	case ApplyCode::NotApplicable: return { SaveStatus::NotDirty, {} };
	case ApplyCode::ConflictResnapshotScheduled:
	case ApplyCode::ResnapshotScheduled: return { SaveStatus::Conflict, L"status bar visibility needs resnapshot" };
	case ApplyCode::NotReady:
	case ApplyCode::OperationInFlight:
	case ApplyCode::Stopped: return { SaveStatus::Unavailable, L"control-platform storage is unavailable" };
	case ApplyCode::RetryWithSameOperationId:
	case ApplyCode::Failed: return { SaveStatus::Failed, L"status bar visibility write failed" };
	}
	return { SaveStatus::Failed, L"unknown status bar visibility write result" };
}

} // namespace

CControlPlatformStatusbarVisibilityMementoStore::CControlPlatformStatusbarVisibilityMementoStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId) :
	m_runtime(runtime), m_canonicalProfileId(std::move(canonicalProfileId))
{
}

std::optional<platform::storage::StorageAddress>
CControlPlatformStatusbarVisibilityMementoStore::Address() const noexcept
{
	platform::storage::StorageAddress address{ platform::storage::EStorageScope::Profile,
		m_canonicalProfileId, "workbench", "statusbar.hidden" };
	return address.IsValid() ? std::optional(address) : std::nullopt;
}

bool CControlPlatformStatusbarVisibilityMementoStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return coordinates.generation != 0 && coordinates.profileId == m_canonicalProfileId;
}

workbench::statusbar::StatusbarMementoLoadResult
CControlPlatformStatusbarVisibilityMementoStore::Load()
{
	using LoadStatus = workbench::statusbar::EStatusbarMementoLoadStatus;
	std::scoped_lock lock(m_mutex);
	m_coordinates.reset();
	const auto address = Address();
	if (!address) return { LoadStatus::Failed, std::nullopt, L"invalid status bar storage address" };
	const auto before = m_runtime.StorageCacheCoordinates();
	if (before.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready
		|| !before.coordinates || !IsExpectedProfile(*before.coordinates)) {
		return { LoadStatus::Unavailable, std::nullopt, L"status bar storage cache is unavailable" };
	}
	const auto entry = m_runtime.Find(*address);
	const auto after = m_runtime.StorageCacheCoordinates();
	if (after.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready
		|| !after.coordinates || !IsExpectedProfile(*after.coordinates)
		|| *before.coordinates != *after.coordinates) {
		return { LoadStatus::Unavailable, std::nullopt, L"status bar storage cache changed during load" };
	}
	m_coordinates = *after.coordinates;
	m_canonicalPayload.reset();
	if (!entry) return { LoadStatus::NotFound, std::vector<std::string>{}, {} };
	if (entry->address != *address || entry->target != platform::storage::EStorageTarget::User) {
		m_invalidStoredMemento = true;
		return { LoadStatus::Invalid, std::nullopt, L"invalid status bar visibility storage target" };
	}
	auto decoded = workbench::statusbar::StatusbarVisibilityMementoCodec::Decode(entry->value);
	if (!decoded) {
		m_invalidStoredMemento = true;
		return { LoadStatus::Invalid, std::nullopt, L"invalid status bar visibility payload" };
	}
	m_canonicalPayload = workbench::statusbar::StatusbarVisibilityMementoCodec::Encode(*decoded);
	return { LoadStatus::Loaded, std::move(decoded), {} };
}

workbench::statusbar::StatusbarMementoSaveResult
CControlPlatformStatusbarVisibilityMementoStore::Save(const std::vector<std::string>& hiddenIds)
{
	using SaveStatus = workbench::statusbar::EStatusbarMementoSaveStatus;
	std::scoped_lock lock(m_mutex);
	const auto address = Address();
	if (!address || !m_coordinates || m_invalidStoredMemento) {
		return { SaveStatus::Failed, L"status bar visibility save requires a coherent load" };
	}
	const auto payload = workbench::statusbar::StatusbarVisibilityMementoCodec::Encode(hiddenIds);
	if (!payload) return { SaveStatus::Failed, L"status bar visibility encoding failed" };
	if (m_canonicalPayload && *m_canonicalPayload == *payload) return { SaveStatus::NotDirty, {} };
	const std::string operationId = GenerateOperationId();
	if (operationId.empty()) return { SaveStatus::Failed, L"status bar visibility operation identity failed" };
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_coordinates->storageRevision,
		.mutations = { { *address, platform::storage::EStorageTarget::User, *payload } },
	};
	auto result = m_runtime.Apply(request);
	if (result.code == platform::controlipc::EEditorControlStorageApplyCode::RetryWithSameOperationId) {
		result = m_runtime.Apply(request);
	}
	if (result.code == platform::controlipc::EEditorControlStorageApplyCode::Succeeded
		|| result.code == platform::controlipc::EEditorControlStorageApplyCode::NotApplicable) {
		if (result.storageResult && result.storageResult->revision > m_coordinates->storageRevision) {
			m_coordinates->storageRevision = result.storageResult->revision;
		}
		m_canonicalPayload = *payload;
	}
	return SaveResultFor(result.code);
}
