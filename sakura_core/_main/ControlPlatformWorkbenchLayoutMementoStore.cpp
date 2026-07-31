/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "_main/ControlPlatformWorkbenchLayoutMementoStore.h"

#include "workbench/layout/WorkbenchLayoutMementoCodec.h"

#include <bcrypt.h>

#include <array>
#include <utility>

using workbench::layout::EWorkbenchLayoutMementoLoadStatus;
using workbench::layout::EWorkbenchLayoutMementoSaveStatus;
using workbench::layout::CWorkbenchLayoutMementoCodec;
using workbench::layout::WorkbenchLayoutMementoLoadResult;
using workbench::layout::WorkbenchLayoutMementoSaveResult;

namespace {

constexpr char kWorkbenchLayoutOwner[] = "workbench.layout";
constexpr char kWorkbenchLayoutKey[] = "state";

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return {};
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string operationId = "workbench.layout-";
	operationId.reserve(operationId.size() + random.size() * 2);
	for (const auto value : random) {
		operationId.push_back(hex[value >> 4]);
		operationId.push_back(hex[value & 0x0f]);
	}
	return operationId;
}

bool IsValidOperationId(const std::string& operationId) noexcept
{
	return !operationId.empty()
		&& operationId.size() <= platform::storage::kMaximumStorageOperationIdBytes
		&& platform::storage::IsValidStorageUtf8(operationId, false);
}

WorkbenchLayoutMementoSaveResult SaveResultFor(
	platform::controlipc::EEditorControlStorageApplyCode code, bool retry)
{
	using platform::controlipc::EEditorControlStorageApplyCode;
	switch (code) {
	case EEditorControlStorageApplyCode::Succeeded:
		return { EWorkbenchLayoutMementoSaveStatus::Persisted, {} };
	case EEditorControlStorageApplyCode::NotApplicable:
		return { EWorkbenchLayoutMementoSaveStatus::NotDirty, {} };
	case EEditorControlStorageApplyCode::ConflictResnapshotScheduled:
	case EEditorControlStorageApplyCode::ResnapshotScheduled:
		return { EWorkbenchLayoutMementoSaveStatus::Conflict, L"workbench layout memento needs resnapshot" };
	case EEditorControlStorageApplyCode::RetryWithSameOperationId:
		return { retry ? EWorkbenchLayoutMementoSaveStatus::RetryExhausted : EWorkbenchLayoutMementoSaveStatus::Failed,
			retry ? std::wstring(L"workbench layout memento retry remained ambiguous") : std::wstring{} };
	case EEditorControlStorageApplyCode::NotReady:
	case EEditorControlStorageApplyCode::OperationInFlight:
		return { EWorkbenchLayoutMementoSaveStatus::Unavailable, L"control-platform storage writer is unavailable" };
	case EEditorControlStorageApplyCode::Stopped:
		return { EWorkbenchLayoutMementoSaveStatus::Stopped, L"control-platform storage writer stopped" };
	case EEditorControlStorageApplyCode::Failed:
		return { EWorkbenchLayoutMementoSaveStatus::Failed, L"control-platform storage write failed" };
	}
	return { EWorkbenchLayoutMementoSaveStatus::Failed, L"control-platform storage write returned an unknown state" };
}

} // namespace

CControlPlatformWorkbenchLayoutMementoStore::CControlPlatformWorkbenchLayoutMementoStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId) :
	CControlPlatformWorkbenchLayoutMementoStore(std::move(canonicalProfileId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.find = [&runtime](const platform::storage::StorageAddress& address) { return runtime.Find(address); },
		.apply = [&runtime](const platform::storage::StorageMutationRequest& request) { return runtime.Apply(request); },
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformWorkbenchLayoutMementoStore::CControlPlatformWorkbenchLayoutMementoStore(
	std::string canonicalProfileId, workbench::layout::ControlPlatformWorkbenchLayoutMementoStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)),
	m_dependencies(std::move(dependencies))
{
}

std::optional<platform::storage::StorageAddress> CControlPlatformWorkbenchLayoutMementoStore::Address() const noexcept
{
	platform::storage::StorageAddress address{ platform::storage::EStorageScope::Profile, m_canonicalProfileId,
		kWorkbenchLayoutOwner, kWorkbenchLayoutKey };
	if (!address.IsValid()) return std::nullopt;
	return address;
}

bool CControlPlatformWorkbenchLayoutMementoStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find && m_dependencies.apply
		&& m_dependencies.operationIdFactory;
}

bool CControlPlatformWorkbenchLayoutMementoStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId
		&& coordinates.generation != 0;
}

WorkbenchLayoutMementoLoadResult CControlPlatformWorkbenchLayoutMementoStore::CoordinateFailure(
	const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const
{
	using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
	switch (result.code) {
	case EEditorControlStorageCacheCoordinateCode::Resynchronizing:
	case EEditorControlStorageCacheCoordinateCode::DegradedUnavailable:
	case EEditorControlStorageCacheCoordinateCode::Stopping:
	case EEditorControlStorageCacheCoordinateCode::Stopped:
		return { EWorkbenchLayoutMementoLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache is unavailable" };
	case EEditorControlStorageCacheCoordinateCode::Failed:
		return { EWorkbenchLayoutMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache failed" };
	case EEditorControlStorageCacheCoordinateCode::Ready:
		break;
	}
	return { EWorkbenchLayoutMementoLoadStatus::Failed, std::nullopt,
		L"control-platform storage cache returned an unknown state" };
}

WorkbenchLayoutMementoLoadResult CControlPlatformWorkbenchLayoutMementoStore::Load()
{
	std::scoped_lock lock(m_mutex);
	// A later incoherent read must never leave a prior coordinate eligible for CAS.
	m_captured.reset();
	if (!HasUsableDependencies() || !Address()) {
		return { EWorkbenchLayoutMementoLoadStatus::Failed, std::nullopt,
			L"workbench layout memento store is not configured" };
	}

	const auto before = m_dependencies.storageCacheCoordinates();
	if (before.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready || !before.coordinates) {
		return CoordinateFailure(before);
	}
	if (!IsExpectedProfile(*before.coordinates)) {
		return { EWorkbenchLayoutMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}

	const auto entry = m_dependencies.find(*Address());
	const auto after = m_dependencies.storageCacheCoordinates();
	if (after.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready || !after.coordinates) {
		return CoordinateFailure(after);
	}
	if (!IsExpectedProfile(*after.coordinates)) {
		return { EWorkbenchLayoutMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}
	if (*before.coordinates != *after.coordinates) {
		return { EWorkbenchLayoutMementoLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache changed during memento load" };
	}

	m_captured = CapturedState{ *after.coordinates, std::nullopt };
	if (!entry) return { EWorkbenchLayoutMementoLoadStatus::NotFound, std::nullopt, {} };
	if (entry->address != *Address() || entry->target != platform::storage::EStorageTarget::Machine) {
		m_invalidStoredMemento = true;
		return { EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workbench layout memento has an invalid storage target" };
	}

	const auto decoded = CWorkbenchLayoutMementoCodec::Decode(entry->value);
	if (!decoded.Succeeded()) {
		m_invalidStoredMemento = true;
		return { EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workbench layout memento is invalid" };
	}
	// Compare future snapshots against the codec's deterministic representation,
	// not incidental JSON whitespace in an otherwise valid older payload.
	const auto canonical = CWorkbenchLayoutMementoCodec::Encode(*decoded.snapshot);
	if (!canonical.Succeeded()) {
		m_invalidStoredMemento = true;
		return { EWorkbenchLayoutMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workbench layout memento could not be canonicalized" };
	}
	m_captured->canonicalPayload = canonical.payload;
	return { EWorkbenchLayoutMementoLoadStatus::Loaded, std::move(decoded.snapshot), {} };
}

void CControlPlatformWorkbenchLayoutMementoStore::RememberPersisted(
	const platform::controlipc::EditorControlStorageApplyResult& result, const std::string& canonicalPayload)
{
	if (!m_captured) return;
	if (result.storageResult && result.storageResult->revision > m_captured->coordinates.storageRevision) {
		m_captured->coordinates.storageRevision = result.storageResult->revision;
	}
	m_captured->canonicalPayload = canonicalPayload;
}

WorkbenchLayoutMementoSaveResult CControlPlatformWorkbenchLayoutMementoStore::Save(
	const workbench::layout::WorkbenchLayoutStateSnapshot& snapshot)
{
	std::scoped_lock lock(m_mutex);
	if (!HasUsableDependencies() || !Address() || !m_captured) {
		return { EWorkbenchLayoutMementoSaveStatus::Failed, L"workbench layout memento save requires a coherent load" };
	}
	if (m_invalidStoredMemento) {
		return { EWorkbenchLayoutMementoSaveStatus::Failed, L"invalid stored workbench layout memento must not be overwritten" };
	}

	const auto encoded = CWorkbenchLayoutMementoCodec::Encode(snapshot);
	if (!encoded.Succeeded()) {
		return { EWorkbenchLayoutMementoSaveStatus::Failed, L"workbench layout memento encoding failed" };
	}
	if (m_captured->canonicalPayload && *m_captured->canonicalPayload == encoded.payload) {
		return { EWorkbenchLayoutMementoSaveStatus::NotDirty, {} };
	}

	const auto operationId = m_dependencies.operationIdFactory();
	if (!IsValidOperationId(operationId)) {
		return { EWorkbenchLayoutMementoSaveStatus::Failed, L"workbench layout memento operation identity is invalid" };
	}
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_captured->coordinates.storageRevision,
		.mutations = { { *Address(), platform::storage::EStorageTarget::Machine, encoded.payload } },
	};

	const auto first = m_dependencies.apply(request);
	if (first.code == platform::controlipc::EEditorControlStorageApplyCode::RetryWithSameOperationId) {
		const auto second = m_dependencies.apply(request);
		if (second.code == platform::controlipc::EEditorControlStorageApplyCode::Succeeded
			|| second.code == platform::controlipc::EEditorControlStorageApplyCode::NotApplicable) {
			RememberPersisted(second, encoded.payload);
		}
		return SaveResultFor(second.code, true);
	}
	if (first.code == platform::controlipc::EEditorControlStorageApplyCode::Succeeded
		|| first.code == platform::controlipc::EEditorControlStorageApplyCode::NotApplicable) {
		RememberPersisted(first, encoded.payload);
	}
	return SaveResultFor(first.code, false);
}
