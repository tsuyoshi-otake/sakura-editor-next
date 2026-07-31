/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlStorageRpc.h"

#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {

constexpr std::size_t kMaximumPayloadBytes = kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;

template<typename T>
void Append(std::vector<std::uint8_t>& bytes, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t i = 0; i != sizeof(T); ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

template<typename T>
bool Read(std::span<const std::uint8_t> bytes, std::size_t& offset, T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
	value = 0;
	for (std::size_t i = 0; i != sizeof(T); ++i) value |= static_cast<T>(bytes[offset + i]) << (i * 8);
	offset += sizeof(T);
	return true;
}

bool IsUtf8(std::span<const std::uint8_t> bytes) noexcept
{
	for (std::size_t i = 0; i < bytes.size();) {
		const auto first = bytes[i];
		if (first <= 0x7f) { ++i; continue; }
		std::size_t tails = 0;
		std::uint32_t point = 0;
		if ((first & 0xe0) == 0xc0) { tails = 1; point = first & 0x1f; }
		else if ((first & 0xf0) == 0xe0) { tails = 2; point = first & 0x0f; }
		else if ((first & 0xf8) == 0xf0) { tails = 3; point = first & 0x07; }
		else return false;
		if (i + tails >= bytes.size()) return false;
		for (std::size_t j = 1; j <= tails; ++j) {
			const auto tail = bytes[i + j];
			if ((tail & 0xc0) != 0x80) return false;
			point = (point << 6) | (tail & 0x3f);
		}
		const auto minimum = tails == 1 ? 0x80u : tails == 2 ? 0x800u : 0x10000u;
		if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
		i += tails + 1;
	}
	return true;
}

bool IsValidExternalIdentifier(std::string_view value) noexcept
{
	return !value.empty() && value.size() <= kControlStorageRpcMaximumStringBytes &&
		value.find('\0') == std::string_view::npos &&
		IsUtf8(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}

bool AppendString(std::vector<std::uint8_t>& bytes, std::string_view value)
{
	if (value.size() > kControlStorageRpcMaximumStringBytes ||
		!IsUtf8(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())) ||
		bytes.size() > kMaximumPayloadBytes - sizeof(std::uint32_t) - value.size()) return false;
	Append<std::uint32_t>(bytes, static_cast<std::uint32_t>(value.size()));
	bytes.insert(bytes.end(), value.begin(), value.end());
	return bytes.size() <= kMaximumPayloadBytes;
}

bool ReadString(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& value)
{
	std::uint32_t length = 0;
	if (!Read(bytes, offset, length) || length > kControlStorageRpcMaximumStringBytes || length > bytes.size() - offset) return false;
	const auto input = bytes.subspan(offset, length);
	if (!IsUtf8(input)) return false;
	value.assign(reinterpret_cast<const char*>(input.data()), input.size());
	offset += length;
	return true;
}

bool IsScope(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(storage::EStorageScope::Workspace); }
bool IsTarget(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(storage::EStorageTarget::Machine); }
bool IsStatus(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(storage::EStorageMutationStatus::NotApplicable); }

bool AppendAddress(std::vector<std::uint8_t>& bytes, const storage::StorageAddress& address)
{
	if (!address.IsValid()) return false;
	Append<std::uint8_t>(bytes, static_cast<std::uint8_t>(address.scope));
	return AppendString(bytes, address.scopeId) && AppendString(bytes, address.owner) && AppendString(bytes, address.key);
}

bool ReadAddress(std::span<const std::uint8_t> bytes, std::size_t& offset, storage::StorageAddress& address)
{
	std::uint8_t scope = 0;
	if (!Read(bytes, offset, scope) || !IsScope(scope)) return false;
	address.scope = static_cast<storage::EStorageScope>(scope);
	return ReadString(bytes, offset, address.scopeId) && ReadString(bytes, offset, address.owner) &&
		ReadString(bytes, offset, address.key) && address.IsValid();
}

bool AppendEntry(std::vector<std::uint8_t>& bytes, const storage::StorageEntry& entry)
{
	return AppendAddress(bytes, entry.address) && IsTarget(static_cast<std::uint8_t>(entry.target)) &&
		(Append<std::uint8_t>(bytes, static_cast<std::uint8_t>(entry.target)), true) && AppendString(bytes, entry.value) &&
		(Append<std::uint64_t>(bytes, entry.revision), bytes.size() <= kMaximumPayloadBytes);
}

bool ReadEntry(std::span<const std::uint8_t> bytes, std::size_t& offset, storage::StorageEntry& entry)
{
	std::uint8_t target = 0;
	return ReadAddress(bytes, offset, entry.address) && Read(bytes, offset, target) && IsTarget(target) &&
		ReadString(bytes, offset, entry.value) && Read(bytes, offset, entry.revision) &&
		((entry.target = static_cast<storage::EStorageTarget>(target)), true);
}

bool AppendBatch(std::vector<std::uint8_t>& bytes, const storage::StorageChangeBatch& batch)
{
	if (batch.changes.size() > kControlStorageRpcMaximumItems) return false;
	Append<std::uint64_t>(bytes, batch.generation); Append<std::uint64_t>(bytes, batch.baseRevision); Append<std::uint64_t>(bytes, batch.revision);
	Append<std::uint32_t>(bytes, static_cast<std::uint32_t>(batch.changes.size()));
	for (const auto& change : batch.changes) {
		if (!AppendAddress(bytes, change.address) || !IsTarget(static_cast<std::uint8_t>(change.target))) return false;
		Append<std::uint8_t>(bytes, static_cast<std::uint8_t>(change.target));
		Append<std::uint8_t>(bytes, change.entry ? 1 : 0);
		if (change.entry && !AppendEntry(bytes, *change.entry)) return false;
	}
	return bytes.size() <= kMaximumPayloadBytes;
}

bool ReadBatch(std::span<const std::uint8_t> bytes, std::size_t& offset, storage::StorageChangeBatch& batch)
{
	std::uint32_t count = 0;
	if (!Read(bytes, offset, batch.generation) || !Read(bytes, offset, batch.baseRevision) || !Read(bytes, offset, batch.revision) ||
		!Read(bytes, offset, count) || count > kControlStorageRpcMaximumItems) return false;
	batch.changes.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) {
		storage::StorageChange change;
		std::uint8_t target = 0, hasEntry = 0;
		if (!ReadAddress(bytes, offset, change.address) || !Read(bytes, offset, target) || !IsTarget(target) || !Read(bytes, offset, hasEntry) || hasEntry > 1) return false;
		change.target = static_cast<storage::EStorageTarget>(target);
		if (hasEntry) {
			storage::StorageEntry entry;
			if (!ReadEntry(bytes, offset, entry)) return false;
			change.entry = std::move(entry);
		}
		batch.changes.push_back(std::move(change));
	}
	return true;
}

const ControlIpcField* FindUnique(const ControlIpcFields& fields, EControlIpcFieldTag tag)
{
	const ControlIpcField* found = nullptr;
	for (const auto& field : fields) if (field.tag == static_cast<std::uint16_t>(tag)) {
		if (found) return nullptr;
		found = &field;
	}
	return found;
}

bool HasDuplicate(const ControlIpcFields& fields, EControlIpcFieldTag tag) noexcept
{
	std::size_t count = 0;
	for (const auto& field : fields) if (field.tag == static_cast<std::uint16_t>(tag) && ++count > 1) return true;
	return false;
}

bool HasSucceededTerminalStatus(const ControlIpcFields& fields)
{
	const auto* field = FindUnique(fields, EControlIpcFieldTag::TerminalStatus);
	std::size_t offset = 0;
	std::uint16_t status = 0;
	return field && field->value.size() == sizeof(status) && Read(field->value, offset, status) &&
		status == static_cast<std::uint16_t>(EControlIpcTerminalStatus::Succeeded);
}

ControlIpcField SucceededTerminalStatusField()
{
	std::vector<std::uint8_t> value;
	Append<std::uint16_t>(value, static_cast<std::uint16_t>(EControlIpcTerminalStatus::Succeeded));
	return { static_cast<std::uint16_t>(EControlIpcFieldTag::TerminalStatus), std::move(value) };
}

std::optional<ControlIpcFields> DecodeFields(std::span<const std::uint8_t> payload)
{
	if (payload.size() > kMaximumPayloadBytes) return std::nullopt;
	auto decoded = DecodeControlIpcFields(payload);
	if (decoded.outcome != EControlIpcFieldDecodeOutcome::Decoded) return std::nullopt;
	return std::move(decoded.fields);
}

std::vector<std::uint8_t> U64(std::uint64_t value) { std::vector<std::uint8_t> out; out.reserve(8); Append(out, value); return out; }

ControlIpcFrame Response(const ControlIpcFrame& request, EControlIpcKind kind, std::vector<std::uint8_t> payload, std::uint64_t generation)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal, request.header.requestId, generation }, std::move(payload) };
}

} // namespace

std::optional<std::vector<std::uint8_t>> EncodeControlStorageHello(std::string_view profileId)
{
	if (!IsValidExternalIdentifier(profileId)) return std::nullopt;
	ControlIpcFields fields;
	if (!AddUtf8Field(fields, EControlIpcFieldTag::ProfileId, profileId)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<std::string> DecodeControlStorageHello(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload);
	if (!fields || !FindUnique(*fields, EControlIpcFieldTag::ProfileId)) return std::nullopt;
	EControlIpcFieldDecodeOutcome failure;
	auto profileId = GetUtf8Field(*fields, EControlIpcFieldTag::ProfileId, &failure);
	if (!profileId || !IsValidExternalIdentifier(*profileId)) return std::nullopt;
	return profileId;
}

std::optional<std::vector<std::uint8_t>> EncodeControlStorageSnapshotResponse(const storage::StorageSnapshot& snapshot)
{
	if (snapshot.entries.size() > kControlStorageRpcMaximumItems) return std::nullopt;
	std::vector<std::uint8_t> nested;
	nested.reserve(20);
	Append<std::uint64_t>(nested, snapshot.generation); Append<std::uint64_t>(nested, snapshot.revision);
	Append<std::uint32_t>(nested, static_cast<std::uint32_t>(snapshot.entries.size()));
	for (const auto& entry : snapshot.entries) if (!AppendEntry(nested, entry)) return std::nullopt;
	ControlIpcFields fields{ SucceededTerminalStatusField(),
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::StorageSnapshot), std::move(nested) } };
	return EncodeControlIpcFields(fields);
}

std::optional<storage::StorageSnapshot> DecodeControlStorageSnapshotResponse(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* nested = FindUnique(*fields, EControlIpcFieldTag::StorageSnapshot);
	if (!nested || !HasSucceededTerminalStatus(*fields) || nested->value.size() > kMaximumPayloadBytes) return std::nullopt;
	storage::StorageSnapshot snapshot;
	std::size_t offset = 0; std::uint32_t count = 0;
	if (!Read(nested->value, offset, snapshot.generation) || !Read(nested->value, offset, snapshot.revision) || !Read(nested->value, offset, count) || count > kControlStorageRpcMaximumItems) return std::nullopt;
	snapshot.entries.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) { storage::StorageEntry entry; if (!ReadEntry(nested->value, offset, entry)) return std::nullopt; snapshot.entries.push_back(std::move(entry)); }
	if (offset != nested->value.size()) return std::nullopt;
	return snapshot;
}

std::optional<std::vector<std::uint8_t>> EncodeControlStorageApplyRequest(const storage::StorageMutationRequest& request)
{
	if (!IsValidExternalIdentifier(request.operationId) || request.mutations.empty() ||
		request.mutations.size() > kControlStorageRpcMaximumItems) return std::nullopt;
	std::vector<std::uint8_t> nested; Append<std::uint32_t>(nested, static_cast<std::uint32_t>(request.mutations.size()));
	for (const auto& mutation : request.mutations) {
		if (!AppendAddress(nested, mutation.address) || !IsTarget(static_cast<std::uint8_t>(mutation.target))) return std::nullopt;
		Append<std::uint8_t>(nested, static_cast<std::uint8_t>(mutation.target)); Append<std::uint8_t>(nested, mutation.value ? 1 : 0);
		if (mutation.value && !AppendString(nested, *mutation.value)) return std::nullopt;
	}
	ControlIpcFields fields{ { static_cast<std::uint16_t>(EControlIpcFieldTag::StorageMutation), std::move(nested) } };
	if (!AddUtf8Field(fields, EControlIpcFieldTag::OperationId, request.operationId)) return std::nullopt;
	if (request.expectedRevision) fields.push_back({ static_cast<std::uint16_t>(EControlIpcFieldTag::ExpectedRevision), U64(*request.expectedRevision) });
	return EncodeControlIpcFields(fields);
}

std::optional<storage::StorageMutationRequest> DecodeControlStorageApplyRequest(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* nested = FindUnique(*fields, EControlIpcFieldTag::StorageMutation);
	const auto* operation = FindUnique(*fields, EControlIpcFieldTag::OperationId);
	const auto* expected = FindUnique(*fields, EControlIpcFieldTag::ExpectedRevision);
	if (!nested || !operation || HasDuplicate(*fields, EControlIpcFieldTag::ExpectedRevision) || nested->value.size() > kMaximumPayloadBytes) return std::nullopt;
	EControlIpcFieldDecodeOutcome failure; auto operationId = GetUtf8Field(*fields, EControlIpcFieldTag::OperationId, &failure);
	if (!operationId || !IsValidExternalIdentifier(*operationId)) return std::nullopt;
	storage::StorageMutationRequest request; request.operationId = std::move(*operationId);
	if (expected) { std::size_t expectedOffset = 0; std::uint64_t revision = 0; if (expected->value.size() != 8 || !Read(expected->value, expectedOffset, revision)) return std::nullopt; request.expectedRevision = revision; }
	std::size_t offset = 0; std::uint32_t count = 0;
	if (!Read(nested->value, offset, count) || count == 0 || count > kControlStorageRpcMaximumItems) return std::nullopt;
	request.mutations.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i) {
		storage::StorageMutation mutation; std::uint8_t target = 0, hasValue = 0;
		if (!ReadAddress(nested->value, offset, mutation.address) || !Read(nested->value, offset, target) || !IsTarget(target) || !Read(nested->value, offset, hasValue) || hasValue > 1) return std::nullopt;
		mutation.target = static_cast<storage::EStorageTarget>(target);
		if (hasValue) { std::string value; if (!ReadString(nested->value, offset, value)) return std::nullopt; mutation.value = std::move(value); }
		request.mutations.push_back(std::move(mutation));
	}
	if (offset != nested->value.size()) return std::nullopt;
	return request;
}

std::optional<std::vector<std::uint8_t>> EncodeControlStorageApplyResponse(const storage::StorageMutationResult& result)
{
	if (!IsStatus(static_cast<std::uint8_t>(result.status))) return std::nullopt;
	std::vector<std::uint8_t> nested; Append<std::uint8_t>(nested, static_cast<std::uint8_t>(result.status)); Append<std::uint8_t>(nested, result.replayed ? 1 : 0); Append<std::uint64_t>(nested, result.revision);
	if (!AppendString(nested, result.diagnostic)) return std::nullopt;
	Append<std::uint8_t>(nested, result.changeBatch ? 1 : 0);
	if (result.changeBatch && !AppendBatch(nested, *result.changeBatch)) return std::nullopt;
	ControlIpcFields fields{ SucceededTerminalStatusField(),
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::StorageMutation), std::move(nested) } };
	return EncodeControlIpcFields(fields);
}

std::optional<storage::StorageMutationResult> DecodeControlStorageApplyResponse(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* nested = FindUnique(*fields, EControlIpcFieldTag::StorageMutation);
	if (!nested || !HasSucceededTerminalStatus(*fields) || nested->value.size() > kMaximumPayloadBytes) return std::nullopt;
	storage::StorageMutationResult result; std::size_t offset = 0; std::uint8_t status = 0, replayed = 0, hasBatch = 0;
	if (!Read(nested->value, offset, status) || !IsStatus(status) || !Read(nested->value, offset, replayed) || replayed > 1 || !Read(nested->value, offset, result.revision) || !ReadString(nested->value, offset, result.diagnostic) || !Read(nested->value, offset, hasBatch) || hasBatch > 1) return std::nullopt;
	result.status = static_cast<storage::EStorageMutationStatus>(status); result.replayed = replayed != 0;
	if (hasBatch) { storage::StorageChangeBatch batch; if (!ReadBatch(nested->value, offset, batch)) return std::nullopt; result.changeBatch = std::move(batch); }
	if (offset != nested->value.size()) return std::nullopt;
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlStorageCancelRequest(std::uint64_t requestId)
{
	if (requestId == 0) return std::nullopt;
	return EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::CancelRequestId), U64(requestId) } });
}

std::optional<std::uint64_t> DecodeControlStorageCancelRequest(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* field = FindUnique(*fields, EControlIpcFieldTag::CancelRequestId);
	std::size_t offset = 0; std::uint64_t requestId = 0;
	if (!field || field->value.size() != 8 || !Read(field->value, offset, requestId) || requestId == 0) return std::nullopt;
	return requestId;
}

CControlStorageRpcSession::CControlStorageRpcSession(ControlStorageRpcSessionIdentity identity, storage::IStorageService& storage) noexcept
	: m_identity(std::move(identity)), m_storage(storage)
{
}

bool CControlStorageRpcSession::HasValidIdentity() const noexcept
{
	return m_identity.generation != 0 && IsValidExternalIdentifier(m_identity.profileId);
}

std::uint64_t CControlStorageRpcSession::ResponseGeneration() const noexcept
{
	// An invalid local identity must still produce a syntactically valid terminal frame.
	return m_identity.generation == 0 ? 1 : m_identity.generation;
}

ControlIpcFrame CControlStorageRpcSession::ErrorFor(const ControlIpcFrame& request, EControlIpcTerminalStatus status, std::string_view diagnostic) const noexcept
{
	try {
		auto payload = EncodeControlIpcError({ status, std::string(diagnostic) });
		return Response(request, EControlIpcKind::Error, payload.value_or(std::vector<std::uint8_t>{}), ResponseGeneration());
	} catch (...) {
		return Response(request, EControlIpcKind::Error, {}, ResponseGeneration());
	}
}

ControlIpcFrame CControlStorageRpcSession::HandleHello(const ControlIpcFrame& request)
{
	if (m_handshaken) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "hello already completed");
	if (request.header.generation != 0) return ErrorFor(request, EControlIpcTerminalStatus::GenerationMismatch, "hello generation must be zero");
	const auto profileId = DecodeControlStorageHello(request.payload);
	if (!profileId) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "invalid hello payload");
	if (*profileId != m_identity.profileId) return ErrorFor(request, EControlIpcTerminalStatus::ProfileMismatch, "profile mismatch");
	ControlIpcFields fields;
	fields.push_back(SucceededTerminalStatusField());
	if (!AddUtf8Field(fields, EControlIpcFieldTag::ProfileId, m_identity.profileId)) return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "hello acknowledgement failed");
	auto payload = EncodeControlIpcFields(fields);
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "hello acknowledgement failed");
	m_handshaken = true;
	return Response(request, EControlIpcKind::HelloAck, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlStorageRpcSession::HandleSnapshot(const ControlIpcFrame& request)
{
	if (!request.payload.empty()) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "snapshot request payload must be empty");
	auto payload = EncodeControlStorageSnapshotResponse(m_storage.Snapshot());
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "snapshot response exceeds limits");
	return Response(request, EControlIpcKind::StorageSnapshotResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlStorageRpcSession::HandleApply(const ControlIpcFrame& request)
{
	const auto apply = DecodeControlStorageApplyRequest(request.payload);
	if (!apply) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "invalid storage apply payload");
	auto payload = EncodeControlStorageApplyResponse(m_storage.Apply(*apply));
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "storage response exceeds limits");
	return Response(request, EControlIpcKind::StorageApplyResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlStorageRpcSession::Process(const ControlIpcFrame& request) noexcept
{
	try {
		if (request.header.flags != EControlIpcFlags::Request || request.header.requestId == 0) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "invalid request header");
		if (request.header.majorVersion != kControlIpcMajorVersion) return ErrorFor(request, EControlIpcTerminalStatus::UnsupportedVersion, "unsupported major version");
		if (!HasValidIdentity()) return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "invalid session identity");
		if (request.header.kind == EControlIpcKind::Hello) return HandleHello(request);
		if (!m_handshaken) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "hello required");
		if (request.header.generation != m_identity.generation) return ErrorFor(request, EControlIpcTerminalStatus::GenerationMismatch, "generation mismatch");
		switch (request.header.kind) {
		case EControlIpcKind::StorageSnapshotRequest: return HandleSnapshot(request);
		case EControlIpcKind::StorageApplyRequest: return HandleApply(request);
		case EControlIpcKind::CancelRequest:
			if (!DecodeControlStorageCancelRequest(request.payload)) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "invalid cancel payload");
			return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "synchronous session has no pending request");
		default: return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest, "unexpected request kind");
		}
	} catch (...) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError, "storage service failure");
	}
}

} // namespace platform::controlipc
