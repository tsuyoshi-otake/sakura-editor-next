/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "_main/ControlPlatformWorkingCopyPersistenceStore.h"

#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace workbench::editor::persistence;
using platform::controlipc::EEditorControlStorageApplyCode;
using platform::controlipc::EEditorControlStorageCacheCoordinateCode;
using platform::storage::EStorageScope;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageMutation;
using platform::storage::StorageMutationRequest;

constexpr char kBackupOwner[] = "workbench.editor.backup";
constexpr char kSessionOwner[] = "workbench.editor.session";
constexpr char kSessionManifestKey[] = "manifest";
constexpr char kEnvelopeMagic[] = "WCP1";
constexpr std::uint32_t kEnvelopeFormat = 1;
constexpr std::size_t kChunkBytes = 24 * 1024;
constexpr std::size_t kRequestReserveBytes = 32 * 1024;

struct Envelope final {
	char kind = 0;
	std::uint64_t generation = 0;
	std::size_t totalBytes = 0;
	std::string checksum;
	std::size_t chunkCount = 0;
};

std::string GenerateOperationId()
{
	std::array<std::uint8_t, 16> random{};
	if (::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
		BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
	static constexpr char hex[] = "0123456789abcdef";
	std::string operationId = "working-copy-";
	operationId.reserve(operationId.size() + random.size() * 2);
	for (const auto byte : random) {
		operationId.push_back(hex[byte >> 4]);
		operationId.push_back(hex[byte & 0x0f]);
	}
	return operationId;
}

class AlgorithmHandle final {
public:
	~AlgorithmHandle()
	{
		if (m_value) ::BCryptCloseAlgorithmProvider(m_value, 0);
	}
	[[nodiscard]] BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	[[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }

private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

bool NtSuccess(NTSTATUS status) noexcept
{
	return status >= 0;
}

bool IsValidOperationId(std::string_view value) noexcept
{
	return !value.empty() && value.size() <= platform::storage::kMaximumStorageOperationIdBytes
		&& platform::storage::IsValidStorageUtf8(value, false);
}

void AppendIdentityAddressComponent(std::string& target, std::string_view value)
{
	// Length framing preserves boundaries and makes the canonical/opaque discriminator unambiguous.
	target += std::to_string(value.size());
	target.push_back(':');
	target.append(value);
}

std::optional<std::string> IdentityAddressDigest(const WorkingCopyPersistenceIdentity& identity)
{
	std::string input;
	input.reserve(identity.typeId.size() + (identity.canonicalResource ? identity.canonicalResource->size() : identity.opaqueId->size()) + 32);
	AppendIdentityAddressComponent(input, identity.typeId);
	AppendIdentityAddressComponent(input, identity.canonicalResource ? "canonical" : "opaque");
	AppendIdentityAddressComponent(input, identity.canonicalResource ? *identity.canonicalResource : *identity.opaqueId);
	if (input.size() > (std::numeric_limits<ULONG>::max)()) return std::nullopt;

	AlgorithmHandle algorithm;
	if (!NtSuccess(::BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return std::nullopt;
	std::array<std::uint8_t, 32> digest{};
	if (!NtSuccess(::BCryptHash(algorithm.Get(), nullptr, 0,
		reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), digest.data(), static_cast<ULONG>(digest.size())))) {
		return std::nullopt;
	}

	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(digest.size() * 2);
	for (const auto byte : digest) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0f]);
	}
	return result;
}

std::optional<std::uint64_t> ParseUnsigned(std::string_view text) noexcept
{
	std::uint64_t value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
	return value;
}

std::vector<std::string_view> Split(std::string_view text)
{
	std::vector<std::string_view> values;
	std::size_t start = 0;
	while (start <= text.size()) {
		const auto end = text.find('|', start);
		values.emplace_back(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
		if (end == std::string_view::npos) break;
		start = end + 1;
	}
	return values;
}

std::optional<Envelope> DecodeEnvelope(std::string_view value, char expectedKind)
{
	if (value.size() > platform::storage::kMaximumStorageStringBytes) return std::nullopt;
	const auto fields = Split(value);
	if (fields.size() != 7 || fields[0] != kEnvelopeMagic || fields[1].size() != 1 || fields[1][0] != expectedKind
		|| fields[2] != "1" || fields[5].size() != 16) return std::nullopt;
	if (!std::all_of(fields[5].begin(), fields[5].end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	})) return std::nullopt;
	const auto generation = ParseUnsigned(fields[3]);
	const auto total = ParseUnsigned(fields[4]);
	const auto chunks = ParseUnsigned(fields[6]);
	if (!generation || !total || !chunks || *generation == 0 || *total == 0 || *chunks == 0
		|| *total > platform::storage::kMaximumStorageMutationPayloadBytes
		|| *chunks > platform::storage::kMaximumStorageItems - 1
		|| *chunks < (*total + kChunkBytes - 1) / kChunkBytes) return std::nullopt;
	return Envelope{ expectedKind, *generation, static_cast<std::size_t>(*total), std::string(fields[5]),
		static_cast<std::size_t>(*chunks) };
}

std::string EncodeEnvelope(char kind, std::uint64_t generation, std::string_view payload, std::size_t chunks)
{
	return std::string(kEnvelopeMagic) + "|" + kind + "|" + std::to_string(kEnvelopeFormat) + "|"
		+ std::to_string(generation) + "|" + std::to_string(payload.size()) + "|"
		+ CWorkingCopyPersistenceCodec::ComputeContentChecksum(payload) + "|" + std::to_string(chunks);
}

bool SameScope(const WorkingCopyPersistenceScope& left, const WorkingCopyPersistenceScope& right) noexcept
{
	return left == right;
}

WorkingCopyPersistenceWriteResult WriteResult(EWorkingCopyPersistenceWriteStatus status, std::uint64_t generation = 0,
	bool replayed = false, std::wstring diagnostic = {})
{
	return { status, generation, replayed, std::move(diagnostic) };
}

EWorkingCopyPersistenceWriteStatus ApplyStatus(EEditorControlStorageApplyCode code) noexcept
{
	switch (code) {
	case EEditorControlStorageApplyCode::ConflictResnapshotScheduled:
	case EEditorControlStorageApplyCode::ResnapshotScheduled:
		return EWorkingCopyPersistenceWriteStatus::Conflict;
	case EEditorControlStorageApplyCode::NotReady:
	case EEditorControlStorageApplyCode::OperationInFlight:
		return EWorkingCopyPersistenceWriteStatus::Unavailable;
	case EEditorControlStorageApplyCode::RetryWithSameOperationId:
		return EWorkingCopyPersistenceWriteStatus::RetryExhausted;
	case EEditorControlStorageApplyCode::Succeeded:
	case EEditorControlStorageApplyCode::NotApplicable:
	case EEditorControlStorageApplyCode::Stopped:
	case EEditorControlStorageApplyCode::Failed:
		return EWorkingCopyPersistenceWriteStatus::Failed;
	}
	return EWorkingCopyPersistenceWriteStatus::Failed;
}

std::wstring DiagnosticFor(EWorkingCopyPersistenceWriteStatus status)
{
	switch (status) {
	case EWorkingCopyPersistenceWriteStatus::Conflict: return L"control-platform storage changed; resnapshot is required";
	case EWorkingCopyPersistenceWriteStatus::RetryExhausted: return L"control-platform storage retry remained ambiguous";
	case EWorkingCopyPersistenceWriteStatus::Unavailable: return L"control-platform storage is unavailable";
	case EWorkingCopyPersistenceWriteStatus::Persisted:
	case EWorkingCopyPersistenceWriteStatus::Deleted:
	case EWorkingCopyPersistenceWriteStatus::Failed: return L"control-platform storage write failed";
	}
	return L"control-platform storage returned an unknown state";
}

std::wstring DiagnosticForApply(EEditorControlStorageApplyCode code, EWorkingCopyPersistenceWriteStatus status)
{
	if (code == EEditorControlStorageApplyCode::Stopped) return L"control-platform storage stopped";
	if (code == EEditorControlStorageApplyCode::Failed) return L"control-platform storage write failed";
	return DiagnosticFor(status);
}

bool IsSuccess(EEditorControlStorageApplyCode code) noexcept
{
	return code == EEditorControlStorageApplyCode::Succeeded || code == EEditorControlStorageApplyCode::NotApplicable;
}

void AppendFramed(std::string& target, std::string_view value)
{
	target += std::to_string(value.size());
	target.push_back(':');
	target.append(value);
}

void AppendScopeFingerprint(std::string& target, const WorkingCopyPersistenceScope& scope)
{
	AppendFramed(target, scope.profileId);
	AppendFramed(target, scope.workspaceId.value_or(""));
}

void AppendIdentityFingerprint(std::string& target, const WorkingCopyPersistenceIdentity& identity)
{
	AppendFramed(target, identity.typeId);
	AppendFramed(target, identity.canonicalResource.value_or(""));
	AppendFramed(target, identity.opaqueId.value_or(""));
}

std::string BackupFingerprint(const WorkingCopyBackup& backup, std::optional<std::uint64_t> expectedGeneration)
{
	std::string result = "backup-save|" + std::to_string(expectedGeneration.value_or(0)) + "|";
	AppendScopeFingerprint(result, backup.scope);
	AppendIdentityFingerprint(result, backup.identity);
	AppendFramed(result, backup.content);
	AppendFramed(result, backup.checksum);
	result += std::to_string(backup.generation) + ":" + std::to_string(backup.contentVersion);
	return result;
}

std::string SessionFingerprint(const EditorSessionManifest& session, std::optional<std::uint64_t> expectedGeneration)
{
	std::string result = "session-save|" + std::to_string(expectedGeneration.value_or(0)) + "|";
	AppendScopeFingerprint(result, session.scope);
	const auto encoded = CWorkingCopyPersistenceCodec::EncodeSession(session);
	if (encoded.Succeeded()) AppendFramed(result, encoded.payload);
	else AppendFramed(result, std::string_view{});
	return result;
}

std::string DeleteFingerprint(char kind, const WorkingCopyPersistenceScope& scope,
	const WorkingCopyPersistenceIdentity* identity, std::uint64_t expectedGeneration)
{
	std::string result = kind == 'B' ? "backup-delete|" : "session-delete|";
	result += std::to_string(expectedGeneration) + "|";
	AppendScopeFingerprint(result, scope);
	if (identity) AppendIdentityFingerprint(result, *identity);
	return result;
}

bool FitsMutation(const StorageMutationRequest& request) noexcept
{
	if (request.mutations.empty() || request.mutations.size() > platform::storage::kMaximumStorageItems) return false;
	std::size_t bytes = sizeof(std::uint32_t) * 2 + request.operationId.size()
		+ (request.expectedRevision ? sizeof(std::uint64_t) : 0);
	for (const auto& mutation : request.mutations) {
		const auto valueBytes = mutation.value ? mutation.value->size() : 0;
		if (valueBytes > platform::storage::kMaximumStorageStringBytes) return false;
		const std::size_t itemBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) * 3 + sizeof(std::uint8_t) * 2
			+ mutation.address.scopeId.size() + mutation.address.owner.size() + mutation.address.key.size() + valueBytes;
		if (bytes > platform::storage::kMaximumStorageMutationPayloadBytes - itemBytes) return false;
		bytes += itemBytes;
	}
	return bytes <= platform::storage::kMaximumStorageMutationPayloadBytes - kRequestReserveBytes;
}

struct EncodedRecord final {
	std::string envelope;
	std::vector<std::string> chunks;
};

std::optional<EncodedRecord> ChunkRecord(char kind, std::uint64_t generation, const std::string& payload)
{
	if (payload.empty() || payload.size() > platform::storage::kMaximumStorageMutationPayloadBytes) return std::nullopt;
	const auto count = (payload.size() + kChunkBytes - 1) / kChunkBytes;
	if (count == 0 || count >= platform::storage::kMaximumStorageItems) return std::nullopt;
	EncodedRecord result;
	result.envelope = EncodeEnvelope(kind, generation, payload, count);
	if (result.envelope.size() > platform::storage::kMaximumStorageStringBytes) return std::nullopt;
	result.chunks.reserve(count);
	for (std::size_t offset = 0; offset < payload.size();) {
		auto end = std::min(offset + kChunkBytes, payload.size());
		// Storage values are independently UTF-8 validated. Do not cut one JSON
		// string's multi-byte UTF-8 scalar in half merely to fill a chunk.
		while (end > offset && end < payload.size()
			&& (static_cast<unsigned char>(payload[end]) & 0xc0U) == 0x80U) --end;
		if (end == offset) return std::nullopt;
		result.chunks.emplace_back(payload.substr(offset, end - offset));
		offset = end;
	}
	result.envelope = EncodeEnvelope(kind, generation, payload, result.chunks.size());
	return result;
}

struct ApplyAttempt final {
	platform::controlipc::EditorControlStorageApplyResult result;
	bool retried = false;
};

ApplyAttempt ApplyAtMostTwice(const ControlPlatformWorkingCopyPersistenceStoreDependencies& dependencies,
	const StorageMutationRequest& request)
{
	auto first = dependencies.apply(request);
	if (first.code != EEditorControlStorageApplyCode::RetryWithSameOperationId) return { std::move(first), false };
	return { dependencies.apply(request), true };
}

bool CoordinatesAreCoherent(const platform::controlipc::EditorControlStorageCacheCoordinateResult& before,
	const platform::controlipc::EditorControlStorageCacheCoordinateResult& after,
	const std::string& profile) noexcept
{
	return before.code == EEditorControlStorageCacheCoordinateCode::Ready && before.coordinates
		&& after.code == EEditorControlStorageCacheCoordinateCode::Ready && after.coordinates
		&& before.coordinates->profileId == profile && after.coordinates->profileId == profile
		&& before.coordinates->generation != 0 && after.coordinates->generation != 0
		&& *before.coordinates == *after.coordinates;
}

} // namespace

using workbench::editor::persistence::ControlPlatformWorkingCopyPersistenceStoreDependencies;
using workbench::editor::persistence::CWorkingCopyPersistenceCodec;
using workbench::editor::persistence::EditorSessionLoadResult;
using workbench::editor::persistence::EditorSessionManifest;
using workbench::editor::persistence::EditorSessionManifestDecodeResult;
using workbench::editor::persistence::EWorkingCopyPersistenceLoadStatus;
using workbench::editor::persistence::EWorkingCopyPersistenceWriteStatus;
using workbench::editor::persistence::WorkingCopyBackup;
using workbench::editor::persistence::WorkingCopyBackupDecodeResult;
using workbench::editor::persistence::WorkingCopyBackupLoadResult;
using workbench::editor::persistence::WorkingCopyPersistenceIdentity;
using workbench::editor::persistence::WorkingCopyPersistenceScope;
using workbench::editor::persistence::WorkingCopyPersistenceWriteResult;
using platform::storage::EStorageScope;
using platform::storage::EStorageTarget;
using platform::storage::StorageAddress;
using platform::storage::StorageMutationRequest;
using platform::controlipc::EEditorControlStorageCacheCoordinateCode;

CControlPlatformWorkingCopyPersistenceStore::CControlPlatformWorkingCopyPersistenceStore(
	platform::controlipc::CEditorControlPlatformRuntime& runtime, std::string canonicalProfileId) :
	CControlPlatformWorkingCopyPersistenceStore(std::move(canonicalProfileId), {
		.storageCacheCoordinates = [&runtime] { return runtime.StorageCacheCoordinates(); },
		.find = [&runtime](const StorageAddress& address) { return runtime.Find(address); },
		.apply = [&runtime](const StorageMutationRequest& request) { return runtime.Apply(request); },
		.operationIdFactory = GenerateOperationId,
	})
{
}

CControlPlatformWorkingCopyPersistenceStore::CControlPlatformWorkingCopyPersistenceStore(std::string canonicalProfileId,
	ControlPlatformWorkingCopyPersistenceStoreDependencies dependencies) :
	m_canonicalProfileId(std::move(canonicalProfileId)), m_dependencies(std::move(dependencies))
{
}

bool CControlPlatformWorkingCopyPersistenceStore::HasUsableDependencies() const noexcept
{
	return m_dependencies.storageCacheCoordinates && m_dependencies.find && m_dependencies.apply;
}

bool CControlPlatformWorkingCopyPersistenceStore::IsExpectedProfile(
	const platform::controlipc::EditorControlStorageCacheCoordinates& coordinates) const noexcept
{
	return !m_canonicalProfileId.empty() && coordinates.profileId == m_canonicalProfileId && coordinates.generation != 0;
}

bool CControlPlatformWorkingCopyPersistenceStore::IsExpectedScope(const WorkingCopyPersistenceScope& scope) const noexcept
{
	return scope.IsValid() && scope.profileId == m_canonicalProfileId;
}

std::optional<StorageAddress> CControlPlatformWorkingCopyPersistenceStore::ManifestAddress(ERecordKind kind,
	const WorkingCopyPersistenceScope& scope, const WorkingCopyPersistenceIdentity* identity) const
{
	if (!IsExpectedScope(scope) || (kind == ERecordKind::Backup && (identity == nullptr || !identity->IsValid()))) return std::nullopt;
	const auto storageScope = scope.workspaceId ? EStorageScope::Workspace : EStorageScope::Profile;
	const std::string& scopeId = scope.workspaceId ? *scope.workspaceId : scope.profileId;
	const std::string owner = kind == ERecordKind::Backup ? kBackupOwner : kSessionOwner;
	const auto identityDigest = kind == ERecordKind::Backup ? IdentityAddressDigest(*identity) : std::optional<std::string>{};
	if (kind == ERecordKind::Backup && !identityDigest) return std::nullopt;
	const std::string key = kind == ERecordKind::Backup ? "backup." + *identityDigest + ".manifest" : kSessionManifestKey;
	StorageAddress address{ storageScope, scopeId, owner, key };
	if (!address.IsValid()) return std::nullopt;
	return address;
}

std::optional<StorageAddress> CControlPlatformWorkingCopyPersistenceStore::ChunkAddress(
	const StorageAddress& manifest, std::size_t index) const
{
	StorageAddress result = manifest;
	result.key += ".chunk." + std::to_string(index);
	if (!result.IsValid()) return std::nullopt;
	return result;
}

bool CControlPlatformWorkingCopyPersistenceStore::IsInvalid(const StorageAddress& manifest) const
{
	return m_invalidStoredRecords.contains(manifest);
}

void CControlPlatformWorkingCopyPersistenceStore::RememberInvalid(const StorageAddress& manifest)
{
	m_captured.erase(manifest);
	m_invalidStoredRecords.emplace(manifest, true);
}

std::optional<WorkingCopyPersistenceWriteResult> CControlPlatformWorkingCopyPersistenceStore::ReplayCompleted(
	const std::string& operationId, const std::string& fingerprint) const
{
	const auto found = m_completed.find(operationId);
	if (found == m_completed.end()) return std::nullopt;
	if (found->second.fingerprint != fingerprint) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"working-copy persistence operation identity was reused with different data");
	}
	auto replay = found->second.result;
	replay.replayed = true;
	return replay;
}

void CControlPlatformWorkingCopyPersistenceStore::RememberCompleted(std::string operationId,
	std::string fingerprint, WorkingCopyPersistenceWriteResult result)
{
	constexpr std::size_t kMaximumCompletedOperations = 128;
	if (m_completed.contains(operationId)) return;
	while (m_completedOrder.size() >= kMaximumCompletedOperations) {
		m_completed.erase(m_completedOrder.front());
		m_completedOrder.pop_front();
	}
	m_completedOrder.push_back(operationId);
	m_completed.emplace(std::move(operationId), CompletedOperation{ std::move(fingerprint), std::move(result) });
}

WorkingCopyBackupLoadResult CControlPlatformWorkingCopyPersistenceStore::Load(
	const WorkingCopyPersistenceScope& scope, const WorkingCopyPersistenceIdentity& identity)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Backup, scope, &identity);
	if (!HasUsableDependencies() || !manifest) {
		return { EWorkingCopyPersistenceLoadStatus::Failed, std::nullopt, L"working-copy backup store is not configured" };
	}
	m_captured.erase(*manifest);
	if (IsInvalid(*manifest)) {
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt,
			L"stored working-copy backup was previously found invalid" };
	}

	const auto before = m_dependencies.storageCacheCoordinates();
	if (before.code != EEditorControlStorageCacheCoordinateCode::Ready || !before.coordinates
		|| !IsExpectedProfile(*before.coordinates)) {
		return { before.code == EEditorControlStorageCacheCoordinateCode::Failed ? EWorkingCopyPersistenceLoadStatus::Failed
				: EWorkingCopyPersistenceLoadStatus::Unavailable,
			std::nullopt, L"control-platform storage cache is unavailable" };
	}
	auto invalid = [&]() -> WorkingCopyBackupLoadResult {
		const auto after = m_dependencies.storageCacheCoordinates();
		if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
			return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt,
				L"control-platform storage changed during backup load" };
		}
		RememberInvalid(*manifest);
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt, L"stored working-copy backup is invalid" };
	};
	const auto storedEnvelope = m_dependencies.find(*manifest);
	if (!storedEnvelope) {
		const auto after = m_dependencies.storageCacheCoordinates();
		if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
			return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt,
				L"control-platform storage changed during backup load" };
		}
		m_captured.emplace(*manifest, CapturedRecord{ *after.coordinates, std::nullopt, {}, scope, identity });
		return { EWorkingCopyPersistenceLoadStatus::NotFound, std::nullopt, {} };
	}
	if (storedEnvelope->address != *manifest || storedEnvelope->target != EStorageTarget::Machine) {
		return invalid();
	}
	const auto envelope = DecodeEnvelope(storedEnvelope->value, 'B');
	if (!envelope) {
		return invalid();
	}
	std::string payload;
	payload.reserve(envelope->totalBytes);
	std::vector<StorageAddress> chunks;
	chunks.reserve(envelope->chunkCount);
	for (std::size_t index = 0; index < envelope->chunkCount; ++index) {
		const auto address = ChunkAddress(*manifest, index);
		const auto entry = address ? m_dependencies.find(*address) : std::nullopt;
		if (!address || !entry || entry->address != *address || entry->target != EStorageTarget::Machine
			|| entry->value.empty() || entry->value.size() > kChunkBytes
			|| !platform::storage::IsValidStorageUtf8(entry->value)) {
			return invalid();
		}
		payload += entry->value;
		chunks.push_back(*address);
	}
	// A valid atomic record has no orphaned next chunk. Treat one as a partial or
	// duplicate durable generation rather than silently retaining ambiguous bytes.
	if (const auto nextAddress = ChunkAddress(*manifest, envelope->chunkCount);
		nextAddress && m_dependencies.find(*nextAddress)) return invalid();
	const auto after = m_dependencies.storageCacheCoordinates();
	if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
		return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt, L"control-platform storage changed during backup load" };
	}
	const auto decoded = payload.size() == envelope->totalBytes
		&& CWorkingCopyPersistenceCodec::ComputeContentChecksum(payload) == envelope->checksum
		? CWorkingCopyPersistenceCodec::DecodeBackup(payload) : WorkingCopyBackupDecodeResult{};
	// The bounded SHA-256 address is only an index. The authoritative logical
	// identity and scope live in the decoded record and must match exactly.
	if (!decoded.Succeeded() || decoded.backup->generation != envelope->generation || !SameScope(decoded.backup->scope, scope)
		|| decoded.backup->identity != identity) {
		RememberInvalid(*manifest);
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt, L"stored working-copy backup is invalid" };
	}
	m_captured.emplace(*manifest, CapturedRecord{ *after.coordinates, envelope->generation, std::move(chunks), scope, identity });
	return { EWorkingCopyPersistenceLoadStatus::Loaded, std::move(decoded.backup), {} };
}

EditorSessionLoadResult CControlPlatformWorkingCopyPersistenceStore::Load(const WorkingCopyPersistenceScope& scope)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Session, scope);
	if (!HasUsableDependencies() || !manifest) {
		return { EWorkingCopyPersistenceLoadStatus::Failed, std::nullopt, L"editor session store is not configured" };
	}
	m_captured.erase(*manifest);
	if (IsInvalid(*manifest)) {
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt,
			L"stored editor session was previously found invalid" };
	}

	const auto before = m_dependencies.storageCacheCoordinates();
	if (before.code != EEditorControlStorageCacheCoordinateCode::Ready || !before.coordinates
		|| !IsExpectedProfile(*before.coordinates)) {
		return { before.code == EEditorControlStorageCacheCoordinateCode::Failed ? EWorkingCopyPersistenceLoadStatus::Failed
				: EWorkingCopyPersistenceLoadStatus::Unavailable,
			std::nullopt, L"control-platform storage cache is unavailable" };
	}
	auto invalid = [&]() -> EditorSessionLoadResult {
		const auto after = m_dependencies.storageCacheCoordinates();
		if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
			return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt,
				L"control-platform storage changed during editor session load" };
		}
		RememberInvalid(*manifest);
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt, L"stored editor session is invalid" };
	};
	const auto storedEnvelope = m_dependencies.find(*manifest);
	if (!storedEnvelope) {
		const auto after = m_dependencies.storageCacheCoordinates();
		if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
			return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt,
				L"control-platform storage changed during editor session load" };
		}
		m_captured.emplace(*manifest, CapturedRecord{ *after.coordinates, std::nullopt, {}, scope, std::nullopt });
		return { EWorkingCopyPersistenceLoadStatus::NotFound, std::nullopt, {} };
	}
	if (storedEnvelope->address != *manifest || storedEnvelope->target != EStorageTarget::Machine) {
		return invalid();
	}
	const auto envelope = DecodeEnvelope(storedEnvelope->value, 'S');
	if (!envelope) {
		return invalid();
	}
	std::string payload;
	payload.reserve(envelope->totalBytes);
	std::vector<StorageAddress> chunks;
	chunks.reserve(envelope->chunkCount);
	for (std::size_t index = 0; index < envelope->chunkCount; ++index) {
		const auto address = ChunkAddress(*manifest, index);
		const auto entry = address ? m_dependencies.find(*address) : std::nullopt;
		if (!address || !entry || entry->address != *address || entry->target != EStorageTarget::Machine
			|| entry->value.empty() || entry->value.size() > kChunkBytes
			|| !platform::storage::IsValidStorageUtf8(entry->value)) {
			return invalid();
		}
		payload += entry->value;
		chunks.push_back(*address);
	}
	if (const auto nextAddress = ChunkAddress(*manifest, envelope->chunkCount);
		nextAddress && m_dependencies.find(*nextAddress)) return invalid();
	const auto after = m_dependencies.storageCacheCoordinates();
	if (!CoordinatesAreCoherent(before, after, m_canonicalProfileId)) {
		return { EWorkingCopyPersistenceLoadStatus::Unavailable, std::nullopt, L"control-platform storage changed during editor session load" };
	}
	const auto decoded = payload.size() == envelope->totalBytes
		&& CWorkingCopyPersistenceCodec::ComputeContentChecksum(payload) == envelope->checksum
		? CWorkingCopyPersistenceCodec::DecodeSession(payload) : EditorSessionManifestDecodeResult{};
	if (!decoded.Succeeded() || decoded.manifest->generation != envelope->generation || !SameScope(decoded.manifest->scope, scope)) {
		RememberInvalid(*manifest);
		return { EWorkingCopyPersistenceLoadStatus::InvalidStoredRecord, std::nullopt, L"stored editor session is invalid" };
	}
	m_captured.emplace(*manifest, CapturedRecord{ *after.coordinates, envelope->generation, std::move(chunks), scope, std::nullopt });
	return { EWorkingCopyPersistenceLoadStatus::Loaded, std::move(decoded.manifest), {} };
}

WorkingCopyPersistenceWriteResult CControlPlatformWorkingCopyPersistenceStore::Save(
	const WorkingCopyBackup& backup, std::optional<std::uint64_t> expectedGeneration, const std::string& operationId)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Backup, backup.scope, &backup.identity);
	if (!HasUsableDependencies() || !manifest || !backup.IsValid() || !IsValidOperationId(operationId)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"working-copy backup save request is invalid");
	}
	const auto fingerprint = BackupFingerprint(backup, expectedGeneration);
	if (const auto completed = ReplayCompleted(operationId, fingerprint)) return *completed;
	if (IsInvalid(*manifest)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"invalid stored working-copy backup must not be overwritten");
	}
	const auto captured = m_captured.find(*manifest);
	if (captured == m_captured.end()) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"working-copy backup save requires a coherent load");
	}
	if (!SameScope(captured->second.scope, backup.scope) || !captured->second.identity
		|| *captured->second.identity != backup.identity) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"working-copy backup address does not match the coherent loaded identity");
	}
	if (captured->second.generation != expectedGeneration
		|| (captured->second.generation && backup.generation <= *captured->second.generation)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Conflict, captured->second.generation.value_or(0));
	}
	const auto encoded = CWorkingCopyPersistenceCodec::EncodeBackup(backup);
	const auto chunked = encoded.Succeeded() ? ChunkRecord('B', backup.generation, encoded.payload) : std::nullopt;
	if (!chunked) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"working-copy backup exceeds the durable storage request limit");

	StorageMutationRequest request;
	request.operationId = operationId;
	request.expectedRevision = captured->second.coordinates.storageRevision;
	request.mutations.reserve(1 + chunked->chunks.size() + captured->second.chunkAddresses.size());
	request.mutations.push_back({ *manifest, EStorageTarget::Machine, chunked->envelope });
	std::vector<StorageAddress> newChunks;
	newChunks.reserve(chunked->chunks.size());
	for (std::size_t index = 0; index < chunked->chunks.size(); ++index) {
		const auto address = ChunkAddress(*manifest, index);
		if (!address) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"working-copy backup key is invalid");
		request.mutations.push_back({ *address, EStorageTarget::Machine, chunked->chunks[index] });
		newChunks.push_back(*address);
	}
	for (std::size_t index = newChunks.size(); index < captured->second.chunkAddresses.size(); ++index) {
		request.mutations.push_back({ captured->second.chunkAddresses[index], EStorageTarget::Machine, std::nullopt });
	}
	if (!FitsMutation(request)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"working-copy backup exceeds the durable storage request limit");

	const auto attempt = ApplyAtMostTwice(m_dependencies, request);
	if (IsSuccess(attempt.result.code)) {
		CapturedRecord updated{ captured->second.coordinates, backup.generation, std::move(newChunks), backup.scope, backup.identity };
		if (attempt.result.storageResult && attempt.result.storageResult->revision > updated.coordinates.storageRevision) {
			updated.coordinates.storageRevision = attempt.result.storageResult->revision;
		}
		captured->second = std::move(updated);
		auto result = WriteResult(EWorkingCopyPersistenceWriteStatus::Persisted, backup.generation,
			attempt.result.storageResult && attempt.result.storageResult->replayed);
		RememberCompleted(operationId, fingerprint, result);
		return result;
	}
	const auto status = ApplyStatus(attempt.result.code);
	auto result = WriteResult(status, captured->second.generation.value_or(0), false, DiagnosticForApply(attempt.result.code, status));
	RememberCompleted(operationId, fingerprint, result);
	return result;
}

WorkingCopyPersistenceWriteResult CControlPlatformWorkingCopyPersistenceStore::Save(
	const EditorSessionManifest& session, std::optional<std::uint64_t> expectedGeneration, const std::string& operationId)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Session, session.scope);
	if (!HasUsableDependencies() || !manifest || !session.IsValid() || !IsValidOperationId(operationId)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"editor session save request is invalid");
	}
	const auto fingerprint = SessionFingerprint(session, expectedGeneration);
	if (const auto completed = ReplayCompleted(operationId, fingerprint)) return *completed;
	if (IsInvalid(*manifest)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"invalid stored editor session must not be overwritten");
	}
	const auto captured = m_captured.find(*manifest);
	if (captured == m_captured.end()) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"editor session save requires a coherent load");
	}
	if (captured->second.generation != expectedGeneration
		|| (captured->second.generation && session.generation <= *captured->second.generation)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Conflict, captured->second.generation.value_or(0));
	}
	const auto encoded = CWorkingCopyPersistenceCodec::EncodeSession(session);
	const auto chunked = encoded.Succeeded() ? ChunkRecord('S', session.generation, encoded.payload) : std::nullopt;
	if (!chunked) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"editor session exceeds the durable storage request limit");

	StorageMutationRequest request;
	request.operationId = operationId;
	request.expectedRevision = captured->second.coordinates.storageRevision;
	request.mutations.reserve(1 + chunked->chunks.size() + captured->second.chunkAddresses.size());
	request.mutations.push_back({ *manifest, EStorageTarget::Machine, chunked->envelope });
	std::vector<StorageAddress> newChunks;
	newChunks.reserve(chunked->chunks.size());
	for (std::size_t index = 0; index < chunked->chunks.size(); ++index) {
		const auto address = ChunkAddress(*manifest, index);
		if (!address) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"editor session key is invalid");
		request.mutations.push_back({ *address, EStorageTarget::Machine, chunked->chunks[index] });
		newChunks.push_back(*address);
	}
	for (std::size_t index = newChunks.size(); index < captured->second.chunkAddresses.size(); ++index) {
		request.mutations.push_back({ captured->second.chunkAddresses[index], EStorageTarget::Machine, std::nullopt });
	}
	if (!FitsMutation(request)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"editor session exceeds the durable storage request limit");

	const auto attempt = ApplyAtMostTwice(m_dependencies, request);
	if (IsSuccess(attempt.result.code)) {
		CapturedRecord updated{ captured->second.coordinates, session.generation, std::move(newChunks), session.scope, std::nullopt };
		if (attempt.result.storageResult && attempt.result.storageResult->revision > updated.coordinates.storageRevision) {
			updated.coordinates.storageRevision = attempt.result.storageResult->revision;
		}
		captured->second = std::move(updated);
		auto result = WriteResult(EWorkingCopyPersistenceWriteStatus::Persisted, session.generation,
			attempt.result.storageResult && attempt.result.storageResult->replayed);
		RememberCompleted(operationId, fingerprint, result);
		return result;
	}
	const auto status = ApplyStatus(attempt.result.code);
	auto result = WriteResult(status, captured->second.generation.value_or(0), false, DiagnosticForApply(attempt.result.code, status));
	RememberCompleted(operationId, fingerprint, result);
	return result;
}

WorkingCopyPersistenceWriteResult CControlPlatformWorkingCopyPersistenceStore::Delete(
	const WorkingCopyPersistenceScope& scope, const WorkingCopyPersistenceIdentity& identity,
	std::uint64_t expectedGeneration, const std::string& operationId)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Backup, scope, &identity);
	if (!HasUsableDependencies() || !manifest || !IsValidOperationId(operationId)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"working-copy backup delete request is invalid");
	}
	const auto fingerprint = DeleteFingerprint('B', scope, &identity, expectedGeneration);
	if (const auto completed = ReplayCompleted(operationId, fingerprint)) return *completed;
	if (IsInvalid(*manifest)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"invalid stored working-copy backup must not be deleted");
	const auto captured = m_captured.find(*manifest);
	if (captured == m_captured.end()) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"working-copy backup delete requires a coherent load");
	if (!SameScope(captured->second.scope, scope) || !captured->second.identity
		|| *captured->second.identity != identity) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
			L"working-copy backup address does not match the coherent loaded identity");
	}
	if (!captured->second.generation || *captured->second.generation != expectedGeneration) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Conflict, captured->second.generation.value_or(0));
	}
	StorageMutationRequest request{ operationId, captured->second.coordinates.storageRevision, {} };
	request.mutations.reserve(1 + captured->second.chunkAddresses.size());
	request.mutations.push_back({ *manifest, EStorageTarget::Machine, std::nullopt });
	for (const auto& address : captured->second.chunkAddresses) request.mutations.push_back({ address, EStorageTarget::Machine, std::nullopt });
	if (!FitsMutation(request)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, expectedGeneration, false,
		L"working-copy backup delete exceeds the durable storage request limit");
	const auto attempt = ApplyAtMostTwice(m_dependencies, request);
	if (IsSuccess(attempt.result.code)) {
		CapturedRecord updated{ captured->second.coordinates, std::nullopt, {}, scope, identity };
		if (attempt.result.storageResult && attempt.result.storageResult->revision > updated.coordinates.storageRevision) {
			updated.coordinates.storageRevision = attempt.result.storageResult->revision;
		}
		captured->second = std::move(updated);
		auto result = WriteResult(EWorkingCopyPersistenceWriteStatus::Deleted, expectedGeneration,
			attempt.result.storageResult && attempt.result.storageResult->replayed);
		RememberCompleted(operationId, fingerprint, result);
		return result;
	}
	const auto status = ApplyStatus(attempt.result.code);
	auto result = WriteResult(status, expectedGeneration, false, DiagnosticForApply(attempt.result.code, status));
	RememberCompleted(operationId, fingerprint, result);
	return result;
}

WorkingCopyPersistenceWriteResult CControlPlatformWorkingCopyPersistenceStore::Delete(
	const WorkingCopyPersistenceScope& scope, std::uint64_t expectedGeneration, const std::string& operationId)
{
	std::scoped_lock lock(m_mutex);
	const auto manifest = ManifestAddress(ERecordKind::Session, scope);
	if (!HasUsableDependencies() || !manifest || !IsValidOperationId(operationId)) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false, L"editor session delete request is invalid");
	}
	const auto fingerprint = DeleteFingerprint('S', scope, nullptr, expectedGeneration);
	if (const auto completed = ReplayCompleted(operationId, fingerprint)) return *completed;
	if (IsInvalid(*manifest)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"invalid stored editor session must not be deleted");
	const auto captured = m_captured.find(*manifest);
	if (captured == m_captured.end()) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, 0, false,
		L"editor session delete requires a coherent load");
	if (!captured->second.generation || *captured->second.generation != expectedGeneration) {
		return WriteResult(EWorkingCopyPersistenceWriteStatus::Conflict, captured->second.generation.value_or(0));
	}
	StorageMutationRequest request{ operationId, captured->second.coordinates.storageRevision, {} };
	request.mutations.reserve(1 + captured->second.chunkAddresses.size());
	request.mutations.push_back({ *manifest, EStorageTarget::Machine, std::nullopt });
	for (const auto& address : captured->second.chunkAddresses) request.mutations.push_back({ address, EStorageTarget::Machine, std::nullopt });
	if (!FitsMutation(request)) return WriteResult(EWorkingCopyPersistenceWriteStatus::Failed, expectedGeneration, false,
		L"editor session delete exceeds the durable storage request limit");
	const auto attempt = ApplyAtMostTwice(m_dependencies, request);
	if (IsSuccess(attempt.result.code)) {
		CapturedRecord updated{ captured->second.coordinates, std::nullopt, {}, scope, std::nullopt };
		if (attempt.result.storageResult && attempt.result.storageResult->revision > updated.coordinates.storageRevision) {
			updated.coordinates.storageRevision = attempt.result.storageResult->revision;
		}
		captured->second = std::move(updated);
		auto result = WriteResult(EWorkingCopyPersistenceWriteStatus::Deleted, expectedGeneration,
			attempt.result.storageResult && attempt.result.storageResult->replayed);
		RememberCompleted(operationId, fingerprint, result);
		return result;
	}
	const auto status = ApplyStatus(attempt.result.code);
	auto result = WriteResult(status, expectedGeneration, false, DiagnosticForApply(attempt.result.code, status));
	RememberCompleted(operationId, fingerprint, result);
	return result;
}
