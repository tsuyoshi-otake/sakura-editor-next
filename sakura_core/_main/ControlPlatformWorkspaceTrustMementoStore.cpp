/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "_main/ControlPlatformWorkspaceTrustMementoStore.h"

#include "config/WorkspaceTrustMementoCodec.h"

#include <bcrypt.h>

#include <array>
#include <utility>

using config::CWorkspaceTrustMementoCodec;
using config::EWorkspaceTrustMementoLoadStatus;
using config::EWorkspaceTrustMementoSaveStatus;
using config::WorkspaceTrustMementoLoadResult;
using config::WorkspaceTrustMementoSaveResult;

namespace {

constexpr char kWorkspaceTrustOwner[] = "workbench.trust";
constexpr char kWorkspaceTrustMementoKey[] = "workspaceMemento";

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
		return {};
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string operationId = "workbench.trust.memento-";
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

WorkspaceTrustMementoSaveResult SaveResultFor(
	platform::controlipc::EEditorControlStorageApplyCode code, bool retry)
{
	using platform::controlipc::EEditorControlStorageApplyCode;
	switch (code) {
	case EEditorControlStorageApplyCode::Succeeded:
		return { EWorkspaceTrustMementoSaveStatus::Persisted, {} };
	case EEditorControlStorageApplyCode::NotApplicable:
		return { EWorkspaceTrustMementoSaveStatus::NotDirty, {} };
	case EEditorControlStorageApplyCode::ConflictResnapshotScheduled:
	case EEditorControlStorageApplyCode::ResnapshotScheduled:
		return { EWorkspaceTrustMementoSaveStatus::Conflict, L"workspace trust memento needs resnapshot" };
	case EEditorControlStorageApplyCode::RetryWithSameOperationId:
		return { retry ? EWorkspaceTrustMementoSaveStatus::RetryExhausted : EWorkspaceTrustMementoSaveStatus::Failed,
			retry ? std::wstring(L"workspace trust memento retry remained ambiguous") : std::wstring{} };
	case EEditorControlStorageApplyCode::NotReady:
	case EEditorControlStorageApplyCode::OperationInFlight:
		return { EWorkspaceTrustMementoSaveStatus::Unavailable, L"control-platform storage writer is unavailable" };
	case EEditorControlStorageApplyCode::Stopped:
		return { EWorkspaceTrustMementoSaveStatus::Stopped, L"control-platform storage writer stopped" };
	case EEditorControlStorageApplyCode::Failed:
		return { EWorkspaceTrustMementoSaveStatus::Failed, L"control-platform storage write failed" };
	}
	return { EWorkspaceTrustMementoSaveStatus::Failed,
		L"control-platform storage write returned an unknown state" };
}

} // namespace

CControlPlatformWorkspaceTrustMementoStore::CControlPlatformWorkspaceTrustMementoStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime,
	std::string canonicalProfileId, std::optional<std::string> workspaceScopeId) :
	CControlPlatformWorkspaceTrustMementoStore(std::move(canonicalProfileId), std::move(workspaceScopeId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.find = [&runtime](const platform::storage::StorageAddress& address) { return runtime.Find(address); },
		.apply = [&runtime](const platform::storage::StorageMutationRequest& request) { return runtime.Apply(request); },
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformWorkspaceTrustMementoStore::CControlPlatformWorkspaceTrustMementoStore(
	std::string canonicalProfileId, std::optional<std::string> workspaceScopeId,
	config::ControlPlatformWorkspaceTrustMementoStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)),
	m_workspaceScopeId(std::move(workspaceScopeId)),
	m_dependencies(std::move(dependencies))
{
}

std::optional<platform::storage::StorageAddress>
CControlPlatformWorkspaceTrustMementoStore::Address() const noexcept
{
	if (!m_workspaceScopeId) return std::nullopt;
	platform::storage::StorageAddress address{ platform::storage::EStorageScope::Workspace,
		*m_workspaceScopeId, kWorkspaceTrustOwner, kWorkspaceTrustMementoKey };
	if (!address.IsValid()) return std::nullopt;
	return address;
}

bool CControlPlatformWorkspaceTrustMementoStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find && m_dependencies.apply
		&& m_dependencies.operationIdFactory;
}

bool CControlPlatformWorkspaceTrustMementoStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId
		&& coordinates.generation != 0;
}

WorkspaceTrustMementoLoadResult CControlPlatformWorkspaceTrustMementoStore::CoordinateFailure(
	const platform::controlipc::EditorControlStorageCacheCoordinateResult& result) const
{
	using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
	switch (result.code) {
	case EEditorControlStorageCacheCoordinateCode::Resynchronizing:
	case EEditorControlStorageCacheCoordinateCode::DegradedUnavailable:
	case EEditorControlStorageCacheCoordinateCode::Stopping:
	case EEditorControlStorageCacheCoordinateCode::Stopped:
		return { EWorkspaceTrustMementoLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache is unavailable" };
	case EEditorControlStorageCacheCoordinateCode::Failed:
		return { EWorkspaceTrustMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache failed" };
	case EEditorControlStorageCacheCoordinateCode::Ready:
		break;
	}
	return { EWorkspaceTrustMementoLoadStatus::Failed, std::nullopt,
		L"control-platform storage cache returned an unknown state" };
}

WorkspaceTrustMementoLoadResult CControlPlatformWorkspaceTrustMementoStore::Load()
{
	std::scoped_lock lock(m_mutex);
	// A later incoherent read must never leave a prior coordinate eligible for CAS.
	m_captured.reset();
	if (!m_workspaceScopeId) {
		// An empty window has nothing to key a per-workspace record by. This is a
		// coherent answer, not a failure, and it is deliberately not Readable(): the
		// caller must fall back to its own policy rather than treat the absent record
		// as "already asked".
		return { EWorkspaceTrustMementoLoadStatus::NoWorkspaceScope, std::nullopt,
			L"window has no workspace scope for a trust memento" };
	}
	if (!HasUsableDependencies() || !Address()) {
		return { EWorkspaceTrustMementoLoadStatus::Failed, std::nullopt,
			L"workspace trust memento store is not configured" };
	}

	const auto before = m_dependencies.storageCacheCoordinates();
	if (before.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready
		|| !before.coordinates) {
		return CoordinateFailure(before);
	}
	if (!IsExpectedProfile(*before.coordinates)) {
		return { EWorkspaceTrustMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}

	const auto entry = m_dependencies.find(*Address());
	// Re-read the coordinates after the fetch for the same reason the Trusted Folders
	// store does: only a coordinate pair that agrees on both sides of the read brackets
	// one coherent global storage revision, so the bytes and the CAS token the next save
	// writes against cannot come from two different revisions.
	const auto after = m_dependencies.storageCacheCoordinates();
	if (after.code != platform::controlipc::EEditorControlStorageCacheCoordinateCode::Ready
		|| !after.coordinates) {
		return CoordinateFailure(after);
	}
	if (!IsExpectedProfile(*after.coordinates)) {
		return { EWorkspaceTrustMementoLoadStatus::Failed, std::nullopt,
			L"control-platform storage cache identity did not match the workbench" };
	}
	if (*before.coordinates != *after.coordinates) {
		return { EWorkspaceTrustMementoLoadStatus::Unavailable, std::nullopt,
			L"control-platform storage cache changed during workspace trust memento load" };
	}

	m_captured = CapturedState{ *after.coordinates, std::nullopt };
	if (!entry) return { EWorkspaceTrustMementoLoadStatus::NotFound, std::nullopt, {} };
	if (entry->address != *Address() || entry->target != platform::storage::EStorageTarget::Machine) {
		m_invalidStoredMemento = true;
		return { EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workspace trust memento has an invalid storage target" };
	}

	const auto decoded = CWorkspaceTrustMementoCodec::Decode(entry->value);
	if (!decoded.Succeeded()) {
		// Corrupt bytes, an unsupported schema, and an oversized payload all collapse to
		// InvalidStoredMemento: the caller cannot act differently on the distinction, and
		// none of them may become a replacement write.
		m_invalidStoredMemento = true;
		return { EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workspace trust memento is invalid" };
	}
	// Compare future saves against the codec's deterministic representation, not
	// incidental JSON whitespace in an otherwise valid older payload.
	const auto canonical = CWorkspaceTrustMementoCodec::Encode(*decoded.memento);
	if (!canonical.Succeeded()) {
		m_invalidStoredMemento = true;
		return { EWorkspaceTrustMementoLoadStatus::InvalidStoredMemento, std::nullopt,
			L"stored workspace trust memento could not be canonicalized" };
	}
	m_captured->canonicalPayload = canonical.payload;
	return { EWorkspaceTrustMementoLoadStatus::Loaded, decoded.memento, {} };
}

void CControlPlatformWorkspaceTrustMementoStore::RememberPersisted(
	const platform::controlipc::EditorControlStorageApplyResult& result, const std::string& canonicalPayload)
{
	if (!m_captured) return;
	if (result.storageResult && result.storageResult->revision > m_captured->coordinates.storageRevision) {
		m_captured->coordinates.storageRevision = result.storageResult->revision;
	}
	m_captured->canonicalPayload = canonicalPayload;
}

WorkspaceTrustMementoSaveResult CControlPlatformWorkspaceTrustMementoStore::Save(
	const config::WorkspaceTrustMemento& memento)
{
	std::scoped_lock lock(m_mutex);
	if (!m_workspaceScopeId) {
		return { EWorkspaceTrustMementoSaveStatus::NoWorkspaceScope,
			L"window has no workspace scope for a trust memento" };
	}
	if (!HasUsableDependencies() || !Address() || !m_captured) {
		return { EWorkspaceTrustMementoSaveStatus::Failed,
			L"workspace trust memento save requires a coherent load" };
	}
	if (m_invalidStoredMemento) {
		return { EWorkspaceTrustMementoSaveStatus::Failed,
			L"invalid stored workspace trust memento must not be overwritten" };
	}

	const auto encoded = CWorkspaceTrustMementoCodec::Encode(memento);
	if (!encoded.Succeeded()) {
		return { EWorkspaceTrustMementoSaveStatus::Failed, L"workspace trust memento encoding failed" };
	}
	if (m_captured->canonicalPayload && *m_captured->canonicalPayload == encoded.payload) {
		return { EWorkspaceTrustMementoSaveStatus::NotDirty, {} };
	}

	const auto operationId = m_dependencies.operationIdFactory();
	if (!IsValidOperationId(operationId)) {
		return { EWorkspaceTrustMementoSaveStatus::Failed,
			L"workspace trust memento operation identity is invalid" };
	}
	const platform::storage::StorageMutationRequest request{
		.operationId = operationId,
		.expectedRevision = m_captured->coordinates.storageRevision,
		.mutations = { { *Address(), platform::storage::EStorageTarget::Machine, encoded.payload } },
	};

	const auto first = m_dependencies.apply(request);
	if (first.code == platform::controlipc::EEditorControlStorageApplyCode::RetryWithSameOperationId) {
		// Replayed verbatim, at most once: a fresh operation ID would let an apply that
		// actually succeeded but was lost in transit be re-applied as a second mutation.
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
