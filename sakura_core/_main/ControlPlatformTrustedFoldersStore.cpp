/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "_main/ControlPlatformTrustedFoldersStore.h"

#include "config/TrustedFoldersCodec.h"

#include <bcrypt.h>

#include <array>
#include <utility>

using config::ETrustedFoldersLoadStatus;
using config::ETrustedFoldersSaveStatus;
using config::CTrustedFoldersCodec;
using config::TrustedFoldersLoadResult;
using config::TrustedFoldersSaveResult;

namespace {

constexpr char kTrustedFoldersOwner[] = "workbench.trust";
constexpr char kTrustedFoldersKey[] = "trustedFolders";

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return {};
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string operationId = "workbench.trust-";
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

TrustedFoldersSaveResult SaveResultFor(
	platform::controlipc::EEditorControlStorageApplyCode code, bool retry)
{
	using platform::controlipc::EEditorControlStorageApplyCode;
	switch (code) {
	case EEditorControlStorageApplyCode::Succeeded:
		return { ETrustedFoldersSaveStatus::Persisted, {} };
	case EEditorControlStorageApplyCode::NotApplicable:
		return { ETrustedFoldersSaveStatus::NotDirty, {} };
	case EEditorControlStorageApplyCode::ConflictResnapshotScheduled:
	case EEditorControlStorageApplyCode::ResnapshotScheduled:
		return { ETrustedFoldersSaveStatus::Conflict, L"trusted folders list needs resnapshot" };
	case EEditorControlStorageApplyCode::RetryWithSameOperationId:
		return { retry ? ETrustedFoldersSaveStatus::RetryExhausted : ETrustedFoldersSaveStatus::Failed,
			retry ? std::wstring(L"trusted folders list retry remained ambiguous") : std::wstring{} };
	case EEditorControlStorageApplyCode::NotReady:
	case EEditorControlStorageApplyCode::OperationInFlight:
		return { ETrustedFoldersSaveStatus::Unavailable, L"control-platform storage writer is unavailable" };
	case EEditorControlStorageApplyCode::Stopped:
		return { ETrustedFoldersSaveStatus::Stopped, L"control-platform storage writer stopped" };
	case EEditorControlStorageApplyCode::Failed:
		return { ETrustedFoldersSaveStatus::Failed, L"control-platform storage write failed" };
	}
	return { ETrustedFoldersSaveStatus::Failed, L"control-platform storage write returned an unknown state" };
}

} // namespace

CControlPlatformTrustedFoldersStore::CControlPlatformTrustedFoldersStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId) :
	CControlPlatformTrustedFoldersStore(std::move(canonicalProfileId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.find = [&runtime](const platform::storage::StorageAddress& address) { return runtime.Find(address); },
		.apply = [&runtime](const platform::storage::StorageMutationRequest& request) { return runtime.Apply(request); },
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformTrustedFoldersStore::CControlPlatformTrustedFoldersStore(
	std::string canonicalProfileId, config::ControlPlatformTrustedFoldersStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)),
	m_dependencies(std::move(dependencies))
{
}

std::optional<platform::storage::StorageAddress> CControlPlatformTrustedFoldersStore::Address() const noexcept
{
	platform::storage::StorageAddress address{ platform::storage::EStorageScope::Profile, m_canonicalProfileId,
		kTrustedFoldersOwner, kTrustedFoldersKey };
	if (!address.IsValid()) return std::nullopt;
	return address;
}

bool CControlPlatformTrustedFoldersStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find && m_dependencies.apply
		&& m_dependencies.operationIdFactory;
}

bool CControlPlatformTrustedFoldersStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId
		&& coordinates.generation != 0;
}

TrustedFoldersLoadResult CControlPlatformTrustedFoldersStore::CoordinateFailure(
	const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const
{
	using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
	switch (result.code) {
	case EEditorControlStorageCacheCoordinateCode::Resynchronizing:
	case EEditorControlStorageCacheCoordinateCode::DegradedUnavailable:
	case EEditorControlStorageCacheCoordinateCode::Stopping:
	case EEditorControlStorageCacheCoordinateCode::Stopped:
		return { ETrustedFoldersLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache is unavailable" };
	case EEditorControlStorageCacheCoordinateCode::Failed:
		return { ETrustedFoldersLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache failed" };
	case EEditorControlStorageCacheCoordinateCode::Ready:
		break;
	}
	return { ETrustedFoldersLoadStatus::Failed, std::nullopt,
		L"control-platform storage cache returned an unknown state" };
}

TrustedFoldersLoadResult CControlPlatformTrustedFoldersStore::Load()
{
	std::scoped_lock lock(m_mutex);
	// A later incoherent read must never leave a prior coordinate eligible for CAS.
	m_captured.reset();
	if (!HasUsableDependencies() || !Address()) {
		return { ETrustedFoldersLoadStatus::Failed, std::nullopt,
			L"trusted folders store is not configured" };
	}

	const auto before = m_dependencies.storageCacheCoordinates();
	if (before.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready || !before.coordinates) {
		return CoordinateFailure(before);
	}
	if (!IsExpectedProfile(*before.coordinates)) {
		return { ETrustedFoldersLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}

	const auto entry = m_dependencies.find(*Address());
	// Re-read the coordinates after the fetch, not just before it: the fetch itself has
	// no atomicity guarantee against a concurrent control-side storage revision change,
	// so only a coordinate pair that agrees on both sides of the read brackets one
	// coherent global storage revision. A single read could silently pair bytes from one
	// revision with a CAS token from another and corrupt the next save's precondition.
	const auto after = m_dependencies.storageCacheCoordinates();
	if (after.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready || !after.coordinates) {
		return CoordinateFailure(after);
	}
	if (!IsExpectedProfile(*after.coordinates)) {
		return { ETrustedFoldersLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}
	if (*before.coordinates != *after.coordinates) {
		return { ETrustedFoldersLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache changed during trusted folders load" };
	}

	m_captured = CapturedState{ *after.coordinates, std::nullopt };
	if (!entry) return { ETrustedFoldersLoadStatus::NotFound, std::nullopt, {} };
	if (entry->address != *Address() || entry->target != platform::storage::EStorageTarget::Machine) {
		m_invalidStoredList = true;
		return { ETrustedFoldersLoadStatus::InvalidStoredList, std::nullopt,
			L"stored trusted folders list has an invalid storage target" };
	}

	const auto decoded = CTrustedFoldersCodec::Decode(entry->value);
	if (!decoded.Succeeded()) {
		// Every decode failure status (corrupt bytes, an unsupported schema version, or a
		// payload that exceeds the bound) collapses to the same InvalidStoredList outcome.
		// The caller cannot act differently on the distinction, and the port's contract is
		// that none of them may become a replacement write; keeping one sticky flag for all
		// three is what makes that guarantee simple to preserve.
		m_invalidStoredList = true;
		return { ETrustedFoldersLoadStatus::InvalidStoredList, std::nullopt,
			L"stored trusted folders list is invalid" };
	}
	// Compare future snapshots against the codec's deterministic representation,
	// not incidental JSON whitespace in an otherwise valid older payload.
	const auto canonical = CTrustedFoldersCodec::Encode(*decoded.snapshot);
	if (!canonical.Succeeded()) {
		m_invalidStoredList = true;
		return { ETrustedFoldersLoadStatus::InvalidStoredList, std::nullopt,
			L"stored trusted folders list could not be canonicalized" };
	}
	m_captured->canonicalPayload = canonical.payload;
	return { ETrustedFoldersLoadStatus::Loaded, std::move(decoded.snapshot), {} };
}

void CControlPlatformTrustedFoldersStore::RememberPersisted(
	const platform::controlipc::EditorControlStorageApplyResult& result, const std::string& canonicalPayload)
{
	if (!m_captured) return;
	if (result.storageResult && result.storageResult->revision > m_captured->coordinates.storageRevision) {
		m_captured->coordinates.storageRevision = result.storageResult->revision;
	}
	m_captured->canonicalPayload = canonicalPayload;
}

TrustedFoldersSaveResult CControlPlatformTrustedFoldersStore::Save(
	const config::TrustedFoldersSnapshot& snapshot)
{
	std::scoped_lock lock(m_mutex);
	if (!HasUsableDependencies() || !Address() || !m_captured) {
		return { ETrustedFoldersSaveStatus::Failed, L"trusted folders save requires a coherent load" };
	}
	if (m_invalidStoredList) {
		return { ETrustedFoldersSaveStatus::Failed, L"invalid stored trusted folders list must not be overwritten" };
	}

	const auto encoded = CTrustedFoldersCodec::Encode(snapshot);
	if (!encoded.Succeeded()) {
		return { ETrustedFoldersSaveStatus::Failed, L"trusted folders list encoding failed" };
	}
	if (m_captured->canonicalPayload && *m_captured->canonicalPayload == encoded.payload) {
		return { ETrustedFoldersSaveStatus::NotDirty, {} };
	}

	const auto operationId = m_dependencies.operationIdFactory();
	if (!IsValidOperationId(operationId)) {
		return { ETrustedFoldersSaveStatus::Failed, L"trusted folders operation identity is invalid" };
	}
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_captured->coordinates.storageRevision,
		.mutations = { { *Address(), platform::storage::EStorageTarget::Machine, encoded.payload } },
	};

	const auto first = m_dependencies.apply(request);
	if (first.code == platform::controlipc::EEditorControlStorageApplyCode::RetryWithSameOperationId) {
		// The same request object is replayed verbatim, at most once: a fresh operation ID
		// would let an apply that actually succeeded server-side but was lost in transit be
		// re-applied as a second, distinct mutation instead of recognized as the same one.
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
