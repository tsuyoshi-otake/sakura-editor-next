/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlSecretVaultRpc.h"
#include "platform/profiles/ProfileAuthorityIdentity.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {

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

bool AppendString(std::vector<std::uint8_t>& bytes, std::string_view value, std::size_t maximumBytes)
{
	if (value.size() > maximumBytes || !secrets::IsValidSecretVaultUtf8(value)
		|| bytes.size() > kControlSecretVaultRpcMaximumPayloadBytes - sizeof(std::uint32_t) - value.size()) return false;
	Append<std::uint32_t>(bytes, static_cast<std::uint32_t>(value.size()));
	bytes.insert(bytes.end(), value.begin(), value.end());
	return true;
}

bool ReadString(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& value,
	std::size_t maximumBytes, bool allowEmpty = true)
{
	std::uint32_t length = 0;
	if (!Read(bytes, offset, length) || length > maximumBytes || length > bytes.size() - offset) return false;
	const auto input = bytes.subspan(offset, length);
	if (!secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(input.data()), input.size()), allowEmpty)) return false;
	value.assign(reinterpret_cast<const char*>(input.data()), input.size());
	offset += length;
	return true;
}

bool AppendAddress(std::vector<std::uint8_t>& bytes, const secrets::SecretAddress& address)
{
	return address.IsValid()
		&& AppendString(bytes, address.extensionId, secrets::kMaximumSecretVaultExtensionIdBytes)
		&& AppendString(bytes, address.key, secrets::kMaximumSecretVaultKeyBytes);
}

bool ReadAddress(std::span<const std::uint8_t> bytes, std::size_t& offset, secrets::SecretAddress& address)
{
	return ReadString(bytes, offset, address.extensionId, secrets::kMaximumSecretVaultExtensionIdBytes, false)
		&& ReadString(bytes, offset, address.key, secrets::kMaximumSecretVaultKeyBytes, false) && address.IsValid();
}

bool IsGetStatus(std::uint8_t value) noexcept
{
	return value <= static_cast<std::uint8_t>(secrets::ESecretGetStatus::Invalid);
}

bool IsMutationKind(std::uint8_t value) noexcept
{
	return value <= static_cast<std::uint8_t>(secrets::ESecretMutationKind::Delete);
}

bool IsMutationStatus(std::uint8_t value) noexcept
{
	return value <= static_cast<std::uint8_t>(secrets::ESecretMutationStatus::Failed);
}

bool IsChangeKind(std::uint8_t value) noexcept
{
	return value <= static_cast<std::uint8_t>(secrets::ESecretChangeKind::Delete);
}

bool IsKnownSecretTag(std::uint16_t tag) noexcept
{
	switch (static_cast<EControlIpcFieldTag>(tag)) {
	case EControlIpcFieldTag::TerminalStatus:
	case EControlIpcFieldTag::Capability:
	case EControlIpcFieldTag::ExtensionHostSessionId:
	case EControlIpcFieldTag::SecretAddress:
	case EControlIpcFieldTag::SecretValue:
	case EControlIpcFieldTag::SecretResult:
	case EControlIpcFieldTag::SecretMutation:
	case EControlIpcFieldTag::ExtensionId:
	case EControlIpcFieldTag::ExtensionHostGeneration:
	case EControlIpcFieldTag::CapabilityLifetimeMilliseconds:
		return true;
	default:
		return false;
	}
}

const ControlIpcField* FindUnique(const ControlIpcFields& fields, EControlIpcFieldTag tag)
{
	const ControlIpcField* found = nullptr;
	for (const auto& field : fields) {
		if (field.tag != static_cast<std::uint16_t>(tag)) continue;
		if (found) return nullptr;
		found = &field;
	}
	return found;
}

bool HasOnlyKnownUniqueTags(const ControlIpcFields& fields) noexcept
{
	std::array<bool, 19> seen{};
	for (const auto& field : fields) {
		if (!IsKnownSecretTag(field.tag) || field.tag >= seen.size() || seen[field.tag]) return false;
		seen[field.tag] = true;
	}
	return true;
}

std::optional<ControlIpcFields> DecodeFields(std::span<const std::uint8_t> payload)
{
	if (payload.size() > kControlSecretVaultRpcMaximumPayloadBytes) return std::nullopt;
	auto fields = DecodeControlIpcFields(payload);
	if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || !HasOnlyKnownUniqueTags(fields.fields)) return std::nullopt;
	return std::move(fields.fields);
}

ControlIpcField SucceededField()
{
	std::vector<std::uint8_t> status;
	Append<std::uint16_t>(status, static_cast<std::uint16_t>(EControlIpcTerminalStatus::Succeeded));
	return { static_cast<std::uint16_t>(EControlIpcFieldTag::TerminalStatus), std::move(status) };
}

bool HasSucceededStatus(const ControlIpcFields& fields) noexcept
{
	const auto* field = FindUnique(fields, EControlIpcFieldTag::TerminalStatus);
	std::size_t offset = 0;
	std::uint16_t status = 0;
	return field && field->value.size() == sizeof(status) && Read(field->value, offset, status)
		&& status == static_cast<std::uint16_t>(EControlIpcTerminalStatus::Succeeded);
}

bool AppendMutationRequest(std::vector<std::uint8_t>& bytes, const secrets::SecretMutationRequest& request)
{
	const secrets::SecretAddress address{ request.extensionId, request.key };
	if (!IsMutationKind(static_cast<std::uint8_t>(request.kind)) || !address.IsValid()) return false;
	if (!secrets::IsValidSecretVaultIdentifier(request.operationId, secrets::kMaximumSecretVaultOperationIdBytes)
		|| request.value.size() > secrets::kMaximumSecretVaultValueBytes
		|| (request.kind == secrets::ESecretMutationKind::Delete && !request.value.empty())) return false;
	Append<std::uint8_t>(bytes, static_cast<std::uint8_t>(request.kind));
	if (!AppendAddress(bytes, address) || !AppendString(bytes, request.value, secrets::kMaximumSecretVaultValueBytes)
		|| !AppendString(bytes, request.operationId, secrets::kMaximumSecretVaultOperationIdBytes)) return false;
	Append<std::uint8_t>(bytes, request.expectedRevision ? 1 : 0);
	if (request.expectedRevision) Append<std::uint64_t>(bytes, *request.expectedRevision);
	return bytes.size() <= kControlSecretVaultRpcMaximumPayloadBytes;
}

bool ReadMutationRequest(std::span<const std::uint8_t> bytes, secrets::SecretMutationRequest& request)
{
	std::size_t offset = 0;
	std::uint8_t kind = 0, hasExpected = 0;
	secrets::SecretAddress address;
	if (!Read(bytes, offset, kind) || !IsMutationKind(kind) || !ReadAddress(bytes, offset, address)
		|| !ReadString(bytes, offset, request.value, secrets::kMaximumSecretVaultValueBytes)
		|| !ReadString(bytes, offset, request.operationId, secrets::kMaximumSecretVaultOperationIdBytes, false)
		|| !Read(bytes, offset, hasExpected) || hasExpected > 1) return false;
	request.kind = static_cast<secrets::ESecretMutationKind>(kind);
	request.extensionId = std::move(address.extensionId);
	request.key = std::move(address.key);
	if (!secrets::IsValidSecretVaultIdentifier(request.operationId, secrets::kMaximumSecretVaultOperationIdBytes)
		|| (request.kind == secrets::ESecretMutationKind::Delete && !request.value.empty())) return false;
	if (hasExpected) {
		std::uint64_t expected = 0;
		if (!Read(bytes, offset, expected)) return false;
		request.expectedRevision = expected;
	}
	return offset == bytes.size();
}

bool AppendChange(std::vector<std::uint8_t>& bytes, const secrets::SecretChange& change)
{
	if (!change.address.IsValid() || !IsChangeKind(static_cast<std::uint8_t>(change.kind))
		|| change.revision == 0) return false;
	return AppendAddress(bytes, change.address)
		&& (Append<std::uint8_t>(bytes, static_cast<std::uint8_t>(change.kind)), true)
		&& (Append<std::uint64_t>(bytes, change.revision), bytes.size() <= kControlSecretVaultRpcMaximumPayloadBytes);
}

bool ReadChange(std::span<const std::uint8_t> bytes, std::size_t& offset, secrets::SecretChange& change)
{
	std::uint8_t kind = 0;
	if (!ReadAddress(bytes, offset, change.address) || !Read(bytes, offset, kind) || !IsChangeKind(kind)
		|| !Read(bytes, offset, change.revision) || change.revision == 0) return false;
	change.kind = static_cast<secrets::ESecretChangeKind>(kind);
	// Profile identity is deliberately absent from this protocol record.
	change.profileId.clear();
	return true;
}

ControlIpcFrame Response(const ControlIpcFrame& request, EControlIpcKind kind, std::vector<std::uint8_t> payload,
	std::uint64_t generation)
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind,
		EControlIpcFlags::Response | EControlIpcFlags::Terminal,
		request.header.requestId == 0 ? 1 : request.header.requestId, generation }, std::move(payload) };
}

std::string_view FixedDiagnostic(EControlIpcTerminalStatus status) noexcept
{
	switch (status) {
	case EControlIpcTerminalStatus::InvalidRequest: return "invalid request";
	case EControlIpcTerminalStatus::UnsupportedVersion: return "unsupported version";
	case EControlIpcTerminalStatus::GenerationMismatch: return "generation mismatch";
	case EControlIpcTerminalStatus::ServerStopping: return "server stopping";
	case EControlIpcTerminalStatus::AccessDenied: return "access denied";
	case EControlIpcTerminalStatus::ResourceExhausted: return "resource exhausted";
	default: return "internal error";
	}
}

void SecureClear(secrets::SecretVaultCapabilityToken& token) noexcept
{
	volatile std::uint8_t* cursor = token.data();
	for (std::size_t index = 0; index < token.size(); ++index) cursor[index] = 0;
}

[[nodiscard]] bool IsExactCanonicalExtensionId(std::string_view extensionId) noexcept
{
	if (extensionId.empty() || extensionId.size() > secrets::kMaximumSecretVaultExtensionIdBytes) return false;
	bool hasPublisherSeparator = false;
	char previous = '\0';
	for (const unsigned char character : extensionId) {
		const bool alphaNumeric = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
		const bool separator = character == '.';
		const bool hyphen = character == '-';
		if (!alphaNumeric && !separator && !hyphen) return false;
		if (hyphen && (previous == '\0' || previous == '.')) return false;
		if (separator && (hasPublisherSeparator || previous == '\0' || previous == '-')) return false;
		hasPublisherSeparator = hasPublisherSeparator || separator;
		previous = static_cast<char>(character);
	}
	return hasPublisherSeparator && previous != '-' && previous != '.';
}

[[nodiscard]] constexpr std::uint64_t MaximumCapabilityLifetimeMilliseconds() noexcept
{
	return static_cast<std::uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			secrets::kMaximumSecretVaultCapabilityLifetime).count());
}

} // namespace

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultGetRequest(const ControlSecretVaultGetRequest& request)
{
	if (!request.address.IsValid() || !secrets::IsValidSecretVaultIdentifier(request.extensionHostSessionId,
		secrets::kMaximumSecretVaultCapabilitySessionIdBytes)) return std::nullopt;
	std::vector<std::uint8_t> address;
	if (!AppendAddress(address, request.address)) return std::nullopt;
	ControlIpcFields fields{
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::Capability), { request.capability.begin(), request.capability.end() } },
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::SecretAddress), std::move(address) },
	};
	if (!AddUtf8Field(fields, EControlIpcFieldTag::ExtensionHostSessionId, request.extensionHostSessionId)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<ControlSecretVaultGetRequest> DecodeControlSecretVaultGetRequest(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* capability = FindUnique(*fields, EControlIpcFieldTag::Capability);
	const auto* session = FindUnique(*fields, EControlIpcFieldTag::ExtensionHostSessionId);
	const auto* address = FindUnique(*fields, EControlIpcFieldTag::SecretAddress);
	if (!capability || !session || !address || fields->size() != 3
		|| capability->value.size() != secrets::kSecretVaultCapabilityTokenBytes
		|| address->value.size() > kControlSecretVaultRpcMaximumPayloadBytes) return std::nullopt;
	ControlSecretVaultGetRequest result;
	std::copy(capability->value.begin(), capability->value.end(), result.capability.begin());
	if (!secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(session->value.data()), session->value.size()), false)
		|| session->value.size() > secrets::kMaximumSecretVaultCapabilitySessionIdBytes) return std::nullopt;
	result.extensionHostSessionId.assign(reinterpret_cast<const char*>(session->value.data()), session->value.size());
	if (!secrets::IsValidSecretVaultIdentifier(result.extensionHostSessionId, secrets::kMaximumSecretVaultCapabilitySessionIdBytes)) return std::nullopt;
	std::size_t offset = 0;
	if (!ReadAddress(address->value, offset, result.address) || offset != address->value.size()) return std::nullopt;
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultGetResponse(const secrets::SecretGetResult& result)
{
	if (!IsGetStatus(static_cast<std::uint8_t>(result.status))
		|| (result.status == secrets::ESecretGetStatus::Found) != result.value.has_value()
		|| (result.value && (result.value->size() > secrets::kMaximumSecretVaultValueBytes
			|| !secrets::IsValidSecretVaultUtf8(*result.value)))) return std::nullopt;
	std::vector<std::uint8_t> record;
	Append<std::uint8_t>(record, static_cast<std::uint8_t>(result.status));
	Append<std::uint64_t>(record, result.revision);
	ControlIpcFields fields{ SucceededField(), { static_cast<std::uint16_t>(EControlIpcFieldTag::SecretResult), std::move(record) } };
	if (result.value && !AddUtf8Field(fields, EControlIpcFieldTag::SecretValue, *result.value)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<secrets::SecretGetResult> DecodeControlSecretVaultGetResponse(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields || !HasSucceededStatus(*fields)) return std::nullopt;
	const auto* resultField = FindUnique(*fields, EControlIpcFieldTag::SecretResult);
	const auto* valueField = FindUnique(*fields, EControlIpcFieldTag::SecretValue);
	if (!resultField || fields->size() != (valueField ? 3u : 2u) || resultField->value.size() != 9) return std::nullopt;
	secrets::SecretGetResult result;
	std::size_t offset = 0; std::uint8_t status = 0;
	if (!Read(resultField->value, offset, status) || !IsGetStatus(status) || !Read(resultField->value, offset, result.revision)) return std::nullopt;
	result.status = static_cast<secrets::ESecretGetStatus>(status);
	if (result.status == secrets::ESecretGetStatus::Found) {
		if (!valueField || valueField->value.size() > secrets::kMaximumSecretVaultValueBytes
			|| !secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(valueField->value.data()), valueField->value.size()))) return std::nullopt;
		result.value = std::string(reinterpret_cast<const char*>(valueField->value.data()), valueField->value.size());
	} else if (valueField) return std::nullopt;
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultApplyRequest(const ControlSecretVaultApplyRequest& request)
{
	if (!secrets::IsValidSecretVaultIdentifier(request.extensionHostSessionId,
		secrets::kMaximumSecretVaultCapabilitySessionIdBytes)) return std::nullopt;
	std::vector<std::uint8_t> mutation;
	if (!AppendMutationRequest(mutation, request.mutation)) return std::nullopt;
	ControlIpcFields fields{
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::Capability), { request.capability.begin(), request.capability.end() } },
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::SecretMutation), std::move(mutation) },
	};
	if (!AddUtf8Field(fields, EControlIpcFieldTag::ExtensionHostSessionId, request.extensionHostSessionId)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<ControlSecretVaultApplyRequest> DecodeControlSecretVaultApplyRequest(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields) return std::nullopt;
	const auto* capability = FindUnique(*fields, EControlIpcFieldTag::Capability);
	const auto* session = FindUnique(*fields, EControlIpcFieldTag::ExtensionHostSessionId);
	const auto* mutation = FindUnique(*fields, EControlIpcFieldTag::SecretMutation);
	if (!capability || !session || !mutation || fields->size() != 3
		|| capability->value.size() != secrets::kSecretVaultCapabilityTokenBytes
		|| mutation->value.size() > kControlSecretVaultRpcMaximumPayloadBytes) return std::nullopt;
	ControlSecretVaultApplyRequest result;
	std::copy(capability->value.begin(), capability->value.end(), result.capability.begin());
	if (session->value.size() > secrets::kMaximumSecretVaultCapabilitySessionIdBytes
		|| !secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(session->value.data()), session->value.size()), false)) return std::nullopt;
	result.extensionHostSessionId.assign(reinterpret_cast<const char*>(session->value.data()), session->value.size());
	if (!secrets::IsValidSecretVaultIdentifier(result.extensionHostSessionId, secrets::kMaximumSecretVaultCapabilitySessionIdBytes)
		|| !ReadMutationRequest(mutation->value, result.mutation)) return std::nullopt;
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultApplyResponse(const secrets::SecretMutationResult& result)
{
	if (!IsMutationStatus(static_cast<std::uint8_t>(result.status))
		|| (result.status == secrets::ESecretMutationStatus::Succeeded) != result.change.has_value()) return std::nullopt;
	std::vector<std::uint8_t> record;
	Append<std::uint8_t>(record, static_cast<std::uint8_t>(result.status));
	Append<std::uint64_t>(record, result.revision);
	Append<std::uint8_t>(record, result.replayed ? 1 : 0);
	Append<std::uint8_t>(record, result.change ? 1 : 0);
	if (result.change && (!AppendChange(record, *result.change) || result.change->revision != result.revision)) return std::nullopt;
	return EncodeControlIpcFields({ SucceededField(), { static_cast<std::uint16_t>(EControlIpcFieldTag::SecretMutation), std::move(record) } });
}

std::optional<secrets::SecretMutationResult> DecodeControlSecretVaultApplyResponse(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields || !HasSucceededStatus(*fields)) return std::nullopt;
	const auto* field = FindUnique(*fields, EControlIpcFieldTag::SecretMutation);
	if (!field || fields->size() != 2 || field->value.size() < 11) return std::nullopt;
	secrets::SecretMutationResult result;
	std::size_t offset = 0; std::uint8_t status = 0, replayed = 0, hasChange = 0;
	if (!Read(field->value, offset, status) || !IsMutationStatus(status) || !Read(field->value, offset, result.revision)
		|| !Read(field->value, offset, replayed) || replayed > 1 || !Read(field->value, offset, hasChange) || hasChange > 1) return std::nullopt;
	result.status = static_cast<secrets::ESecretMutationStatus>(status); result.replayed = replayed != 0;
	if (hasChange) { secrets::SecretChange change; if (!ReadChange(field->value, offset, change) || change.revision != result.revision) return std::nullopt; result.change = std::move(change); }
	if (offset != field->value.size() || (result.status == secrets::ESecretMutationStatus::Succeeded) != result.change.has_value()) return std::nullopt;
	// The wire deliberately has no diagnostic field. Keep the decoded diagnostic fixed and value-free.
	result.diagnostic.clear();
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityIssueRequest(
	const ControlSecretVaultCapabilityIssueRequest& request)
{
	if (!secrets::IsValidSecretVaultIdentifier(request.hostSessionId, secrets::kMaximumSecretVaultCapabilitySessionIdBytes)
		|| request.hostGeneration == 0 || !IsExactCanonicalExtensionId(request.extensionId)
		|| request.lifetime.count() <= 0 || request.lifetime > secrets::kMaximumSecretVaultCapabilityLifetime) return std::nullopt;
	std::vector<std::uint8_t> hostGeneration;
	std::vector<std::uint8_t> lifetime;
	Append<std::uint64_t>(hostGeneration, request.hostGeneration);
	Append<std::uint64_t>(lifetime, static_cast<std::uint64_t>(request.lifetime.count()));
	ControlIpcFields fields{
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::ExtensionHostGeneration), std::move(hostGeneration) },
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::CapabilityLifetimeMilliseconds), std::move(lifetime) },
	};
	if (!AddUtf8Field(fields, EControlIpcFieldTag::ExtensionHostSessionId, request.hostSessionId)
		|| !AddUtf8Field(fields, EControlIpcFieldTag::ExtensionId, request.extensionId)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<ControlSecretVaultCapabilityIssueRequest> DecodeControlSecretVaultCapabilityIssueRequest(
	std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields || fields->size() != 4) return std::nullopt;
	const auto* session = FindUnique(*fields, EControlIpcFieldTag::ExtensionHostSessionId);
	const auto* generation = FindUnique(*fields, EControlIpcFieldTag::ExtensionHostGeneration);
	const auto* extension = FindUnique(*fields, EControlIpcFieldTag::ExtensionId);
	const auto* lifetime = FindUnique(*fields, EControlIpcFieldTag::CapabilityLifetimeMilliseconds);
	if (!session || !generation || !extension || !lifetime || generation->value.size() != sizeof(std::uint64_t)
		|| lifetime->value.size() != sizeof(std::uint64_t)) return std::nullopt;
	ControlSecretVaultCapabilityIssueRequest result;
	if (session->value.size() > secrets::kMaximumSecretVaultCapabilitySessionIdBytes
		|| extension->value.size() > secrets::kMaximumSecretVaultExtensionIdBytes
		|| !secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(session->value.data()), session->value.size()), false)
		|| !secrets::IsValidSecretVaultUtf8(std::string_view(reinterpret_cast<const char*>(extension->value.data()), extension->value.size()), false)) return std::nullopt;
	result.hostSessionId.assign(reinterpret_cast<const char*>(session->value.data()), session->value.size());
	result.extensionId.assign(reinterpret_cast<const char*>(extension->value.data()), extension->value.size());
	std::size_t offset = 0;
	std::uint64_t lifetimeMilliseconds = 0;
	if (!Read(generation->value, offset, result.hostGeneration) || result.hostGeneration == 0) return std::nullopt;
	offset = 0;
	if (!Read(lifetime->value, offset, lifetimeMilliseconds)
		|| lifetimeMilliseconds == 0 || lifetimeMilliseconds > MaximumCapabilityLifetimeMilliseconds()
		|| lifetimeMilliseconds > static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())) return std::nullopt;
	result.lifetime = std::chrono::milliseconds(lifetimeMilliseconds);
	if (!secrets::IsValidSecretVaultIdentifier(result.hostSessionId, secrets::kMaximumSecretVaultCapabilitySessionIdBytes)
		|| !IsExactCanonicalExtensionId(result.extensionId)) return std::nullopt;
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityIssueResponse(
	const ControlSecretVaultCapabilityIssueResponse& response)
{
	if (response.lifetime.count() <= 0 || response.lifetime > secrets::kMaximumSecretVaultCapabilityLifetime) return std::nullopt;
	std::vector<std::uint8_t> lifetime;
	Append<std::uint64_t>(lifetime, static_cast<std::uint64_t>(response.lifetime.count()));
	return EncodeControlIpcFields({ SucceededField(),
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::Capability), { response.capability.begin(), response.capability.end() } },
		{ static_cast<std::uint16_t>(EControlIpcFieldTag::CapabilityLifetimeMilliseconds), std::move(lifetime) } });
}

std::optional<ControlSecretVaultCapabilityIssueResponse> DecodeControlSecretVaultCapabilityIssueResponse(
	std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload); if (!fields || !HasSucceededStatus(*fields) || fields->size() != 3) return std::nullopt;
	const auto* capability = FindUnique(*fields, EControlIpcFieldTag::Capability);
	const auto* lifetime = FindUnique(*fields, EControlIpcFieldTag::CapabilityLifetimeMilliseconds);
	if (!capability || !lifetime || capability->value.size() != secrets::kSecretVaultCapabilityTokenBytes
		|| lifetime->value.size() != sizeof(std::uint64_t)) return std::nullopt;
	ControlSecretVaultCapabilityIssueResponse result;
	std::copy(capability->value.begin(), capability->value.end(), result.capability.begin());
	std::size_t offset = 0;
	std::uint64_t lifetimeMilliseconds = 0;
	if (!Read(lifetime->value, offset, lifetimeMilliseconds) || lifetimeMilliseconds == 0
		|| lifetimeMilliseconds > MaximumCapabilityLifetimeMilliseconds()
		|| lifetimeMilliseconds > static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())) return std::nullopt;
	result.lifetime = std::chrono::milliseconds(lifetimeMilliseconds);
	return result;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityRevokeSessionRequest()
{
	return EncodeControlIpcFields({});
}

bool DecodeControlSecretVaultCapabilityRevokeSessionRequest(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload);
	return fields && fields->empty();
}

std::optional<std::vector<std::uint8_t>> EncodeControlSecretVaultCapabilityRevokeSessionResponse()
{
	return EncodeControlIpcFields({ SucceededField() });
}

bool DecodeControlSecretVaultCapabilityRevokeSessionResponse(std::span<const std::uint8_t> payload)
{
	auto fields = DecodeFields(payload);
	return fields && fields->size() == 1 && HasSucceededStatus(*fields);
}

CControlSecretVaultRpcSession::CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
	secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities) noexcept
	: m_identity(std::move(identity)), m_vault(vault), m_capabilities(capabilities)
{
}

CControlSecretVaultRpcSession::CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
	secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
	secrets::ISecretVaultLegacyMigrationCoordinator& migration) noexcept
	: m_identity(std::move(identity)), m_vault(vault), m_capabilities(capabilities), m_migration(&migration)
{
}

CControlSecretVaultRpcSession::CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
	secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
	secrets::ISecretVaultExtensionGrantAuthority& grantAuthority) noexcept
	: m_identity(std::move(identity)), m_vault(vault), m_capabilities(capabilities), m_grantAuthority(&grantAuthority)
{
}

CControlSecretVaultRpcSession::CControlSecretVaultRpcSession(ControlSecretVaultRpcSessionIdentity identity,
	secrets::ISecretVaultService& vault, secrets::ISecretVaultCapabilityService& capabilities,
	secrets::ISecretVaultExtensionGrantAuthority& grantAuthority,
	secrets::ISecretVaultLegacyMigrationCoordinator& migration) noexcept
	: m_identity(std::move(identity)), m_vault(vault), m_capabilities(capabilities),
	  m_grantAuthority(&grantAuthority), m_migration(&migration)
{
}

bool CControlSecretVaultRpcSession::HasValidIdentity() const noexcept
{
	return m_identity.clientProcessId != 0 && m_identity.connectionGeneration != 0
		&& profiles::IsCanonicalProfileAuthorityId(m_identity.profileId);
}

bool CControlSecretVaultRpcSession::HasMatchingAuthorities() const noexcept
{
	return m_vault.GetProfileId() == m_identity.profileId && m_capabilities.GetProfileId() == m_identity.profileId;
}

std::uint64_t CControlSecretVaultRpcSession::ResponseGeneration() const noexcept
{
	return m_identity.connectionGeneration == 0 ? 1 : m_identity.connectionGeneration;
}

secrets::SecretVaultCapabilityValidationResult CControlSecretVaultRpcSession::Validate(
	const secrets::SecretVaultCapabilityToken& capability, std::string_view extensionHostSessionId,
	const secrets::SecretAddress& address)
{
	return m_capabilities.Validate({ capability,
		{ m_identity.profileId, std::string(extensionHostSessionId), m_identity.clientProcessId, m_identity.connectionGeneration }, address });
}

bool CControlSecretVaultRpcSession::EnsureMigratedOrError(const ControlIpcFrame& request,
	std::string_view extensionId, ControlIpcFrame& error)
{
	if (!m_migration) return true;
	const auto migration = m_migration->EnsureMigrated(extensionId);
	if (migration.status == secrets::ESecretVaultLegacyMigrationStatus::Migrated) return true;
	error = ErrorFor(request, migration.status == secrets::ESecretVaultLegacyMigrationStatus::Stopped
		? EControlIpcTerminalStatus::ServerStopping : EControlIpcTerminalStatus::InternalError);
	return false;
}

ControlIpcFrame CControlSecretVaultRpcSession::ErrorFor(const ControlIpcFrame& request,
	EControlIpcTerminalStatus status) const noexcept
{
	try {
		auto payload = EncodeControlIpcError({ status, std::string(FixedDiagnostic(status)) });
		return Response(request, EControlIpcKind::Error, payload.value_or(std::vector<std::uint8_t>{}), ResponseGeneration());
	} catch (...) {
		return Response(request, EControlIpcKind::Error, {}, ResponseGeneration());
	}
}

ControlIpcFrame CControlSecretVaultRpcSession::HandleGet(const ControlIpcFrame& request)
{
	const auto decoded = DecodeControlSecretVaultGetRequest(request.payload);
	if (!decoded) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
	if (!HasMatchingAuthorities()) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	const auto validation = Validate(decoded->capability, decoded->extensionHostSessionId, decoded->address);
	if (validation.status == secrets::ESecretVaultCapabilityValidationStatus::Stopped) return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	if (validation.status != secrets::ESecretVaultCapabilityValidationStatus::Valid) return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	ControlIpcFrame migrationError;
	if (!EnsureMigratedOrError(request, decoded->address.extensionId, migrationError)) return migrationError;
	const auto result = m_vault.Get(decoded->address.extensionId, decoded->address.key);
	if (result.status == secrets::ESecretGetStatus::Stopped) return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	if (result.status != secrets::ESecretGetStatus::Found && result.status != secrets::ESecretGetStatus::NotFound) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	auto payload = EncodeControlSecretVaultGetResponse(result);
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	return Response(request, EControlIpcKind::SecretGetResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlSecretVaultRpcSession::HandleApply(const ControlIpcFrame& request)
{
	const auto decoded = DecodeControlSecretVaultApplyRequest(request.payload);
	if (!decoded) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
	if (!HasMatchingAuthorities()) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	const secrets::SecretAddress address{ decoded->mutation.extensionId, decoded->mutation.key };
	const auto validation = Validate(decoded->capability, decoded->extensionHostSessionId, address);
	if (validation.status == secrets::ESecretVaultCapabilityValidationStatus::Stopped) return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	if (validation.status != secrets::ESecretVaultCapabilityValidationStatus::Valid) return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	ControlIpcFrame migrationError;
	if (!EnsureMigratedOrError(request, address.extensionId, migrationError)) return migrationError;
	const auto result = m_vault.Apply(decoded->mutation);
	if (result.status == secrets::ESecretMutationStatus::Stopped) return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	auto payload = EncodeControlSecretVaultApplyResponse(result);
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	return Response(request, EControlIpcKind::SecretApplyResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlSecretVaultRpcSession::HandleCapabilityIssue(const ControlIpcFrame& request)
{
	const auto decoded = DecodeControlSecretVaultCapabilityIssueRequest(request.payload);
	if (!decoded) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
	if (!m_grantAuthority) return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	if (!HasMatchingAuthorities() || m_grantAuthority->GetProfileId() != m_identity.profileId
		|| m_grantAuthority->GetControlConnectionGeneration() != m_identity.connectionGeneration) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	const auto authorization = m_grantAuthority->AuthorizeIssue({ m_identity.profileId, m_identity.connectionGeneration,
		decoded->hostSessionId, decoded->hostGeneration, m_identity.clientProcessId, decoded->extensionId });
	if (authorization.status == secrets::ESecretVaultExtensionGrantAuthorizationStatus::Stopped) {
		return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	}
	if (authorization.status == secrets::ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	if (authorization.status != secrets::ESecretVaultExtensionGrantAuthorizationStatus::Authorized) {
		return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	}
	if (!authorization.binding.IsValid() || authorization.binding.session.profileId != m_identity.profileId
		|| authorization.binding.session.connectionGeneration != m_identity.connectionGeneration
		|| authorization.binding.session.clientProcessId != m_identity.clientProcessId
		|| authorization.binding.session.extensionHostSessionId != decoded->hostSessionId
		|| authorization.binding.extensionId != decoded->extensionId) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}

	auto issued = m_capabilities.Issue({ authorization.binding, decoded->lifetime });
	if (issued.status == secrets::ESecretVaultCapabilityIssueStatus::Stopped) {
		return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	}
	if (issued.status == secrets::ESecretVaultCapabilityIssueStatus::CapacityReached) {
		return ErrorFor(request, EControlIpcTerminalStatus::ResourceExhausted);
	}
	if (issued.status != secrets::ESecretVaultCapabilityIssueStatus::Issued || !issued.capability || !issued.expiresAt) {
		if (issued.capability) SecureClear(*issued.capability);
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	ControlSecretVaultCapabilityIssueResponse response{ *issued.capability, decoded->lifetime };
	SecureClear(*issued.capability);
	std::optional<std::vector<std::uint8_t>> payload;
	try {
		payload = EncodeControlSecretVaultCapabilityIssueResponse(response);
	} catch (...) {
		SecureClear(response.capability);
		throw;
	}
	SecureClear(response.capability);
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	return Response(request, EControlIpcKind::SecretCapabilityIssueResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlSecretVaultRpcSession::HandleCapabilityRevokeSession(const ControlIpcFrame& request)
{
	if (!DecodeControlSecretVaultCapabilityRevokeSessionRequest(request.payload)) {
		return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
	}
	if (!m_grantAuthority) return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	if (!HasMatchingAuthorities() || m_grantAuthority->GetProfileId() != m_identity.profileId
		|| m_grantAuthority->GetControlConnectionGeneration() != m_identity.connectionGeneration) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	const auto authorization = m_grantAuthority->AuthorizeRevokeSession(
		{ m_identity.profileId, m_identity.connectionGeneration, m_identity.clientProcessId });
	if (authorization.status == secrets::ESecretVaultExtensionGrantAuthorizationStatus::Stopped) {
		return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	}
	if (authorization.status == secrets::ESecretVaultExtensionGrantAuthorizationStatus::InvalidConfiguration) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	if (authorization.status != secrets::ESecretVaultExtensionGrantAuthorizationStatus::Authorized) {
		return ErrorFor(request, EControlIpcTerminalStatus::AccessDenied);
	}
	if (!authorization.session.IsValid() || authorization.session.profileId != m_identity.profileId
		|| authorization.session.connectionGeneration != m_identity.connectionGeneration
		|| authorization.session.clientProcessId != m_identity.clientProcessId || authorization.hostGeneration == 0) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	const auto revoked = m_capabilities.RevokeSession(authorization.session);
	if (revoked.status == secrets::ESecretVaultCapabilityRevokeStatus::Stopped) {
		return ErrorFor(request, EControlIpcTerminalStatus::ServerStopping);
	}
	if (revoked.status != secrets::ESecretVaultCapabilityRevokeStatus::Revoked
		&& revoked.status != secrets::ESecretVaultCapabilityRevokeStatus::NotFound) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
	auto payload = EncodeControlSecretVaultCapabilityRevokeSessionResponse();
	if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	return Response(request, EControlIpcKind::SecretCapabilityRevokeSessionResponse, std::move(*payload), ResponseGeneration());
}

ControlIpcFrame CControlSecretVaultRpcSession::Process(const ControlIpcFrame& request) noexcept
{
	try {
		if (request.header.flags != EControlIpcFlags::Request || request.header.requestId == 0) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
		if (request.header.majorVersion != kControlIpcMajorVersion) return ErrorFor(request, EControlIpcTerminalStatus::UnsupportedVersion);
		if (!HasValidIdentity()) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
		if (request.header.generation != m_identity.connectionGeneration) return ErrorFor(request, EControlIpcTerminalStatus::GenerationMismatch);
		switch (request.header.kind) {
		case EControlIpcKind::SecretGetRequest: return HandleGet(request);
		case EControlIpcKind::SecretApplyRequest: return HandleApply(request);
		case EControlIpcKind::SecretCapabilityIssueRequest: return HandleCapabilityIssue(request);
		case EControlIpcKind::SecretCapabilityRevokeSessionRequest: return HandleCapabilityRevokeSession(request);
		default: return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
		}
	} catch (...) {
		return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
	}
}

} // namespace platform::controlipc
