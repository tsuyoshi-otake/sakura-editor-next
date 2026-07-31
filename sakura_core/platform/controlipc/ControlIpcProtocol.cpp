/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlIpcProtocol.h"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {

template<typename T>
void AppendLittleEndian(std::vector<std::uint8_t>& bytes, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

template<typename T>
[[nodiscard]] T ReadLittleEndian(const std::uint8_t* bytes) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	T value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		value |= static_cast<T>(bytes[index]) << (index * 8);
	}
	return value;
}

[[nodiscard]] bool IsKnownKind(EControlIpcKind kind) noexcept
{
	switch (kind) {
	case EControlIpcKind::Hello:
	case EControlIpcKind::HelloAck:
	case EControlIpcKind::StorageSnapshotRequest:
	case EControlIpcKind::StorageSnapshotResponse:
	case EControlIpcKind::StorageApplyRequest:
	case EControlIpcKind::StorageApplyResponse:
	case EControlIpcKind::CancelRequest:
	case EControlIpcKind::SecretGetRequest:
	case EControlIpcKind::SecretApplyRequest:
	case EControlIpcKind::SecretCapabilityIssueRequest:
	case EControlIpcKind::SecretCapabilityRevokeSessionRequest:
	case EControlIpcKind::ProfileRequest:
	case EControlIpcKind::CancelAck:
	case EControlIpcKind::Error:
	case EControlIpcKind::SecretGetResponse:
	case EControlIpcKind::SecretApplyResponse:
	case EControlIpcKind::SecretCapabilityIssueResponse:
	case EControlIpcKind::SecretCapabilityRevokeSessionResponse:
	case EControlIpcKind::ProfileResponse:
		return true;
	}
	return false;
}

[[nodiscard]] bool IsKnownTerminalStatus(EControlIpcTerminalStatus status) noexcept
{
	switch (status) {
	case EControlIpcTerminalStatus::Succeeded:
	case EControlIpcTerminalStatus::InvalidRequest:
	case EControlIpcTerminalStatus::UnsupportedVersion:
	case EControlIpcTerminalStatus::ProfileMismatch:
	case EControlIpcTerminalStatus::GenerationMismatch:
	case EControlIpcTerminalStatus::DeadlineExceeded:
	case EControlIpcTerminalStatus::Cancelled:
	case EControlIpcTerminalStatus::ServerStopping:
	case EControlIpcTerminalStatus::AccessDenied:
	case EControlIpcTerminalStatus::ResourceExhausted:
	case EControlIpcTerminalStatus::InternalError:
	case EControlIpcTerminalStatus::ProtocolError:
		return true;
	}
	return false;
}

[[nodiscard]] EControlIpcFlags ExpectedDirection(EControlIpcKind kind) noexcept
{
	switch (kind) {
	case EControlIpcKind::Hello:
	case EControlIpcKind::StorageSnapshotRequest:
	case EControlIpcKind::StorageApplyRequest:
	case EControlIpcKind::CancelRequest:
	case EControlIpcKind::SecretGetRequest:
	case EControlIpcKind::SecretApplyRequest:
	case EControlIpcKind::SecretCapabilityIssueRequest:
	case EControlIpcKind::SecretCapabilityRevokeSessionRequest:
	case EControlIpcKind::ProfileRequest:
		return EControlIpcFlags::Request;
	case EControlIpcKind::HelloAck:
	case EControlIpcKind::StorageSnapshotResponse:
	case EControlIpcKind::StorageApplyResponse:
	case EControlIpcKind::CancelAck:
	case EControlIpcKind::Error:
	case EControlIpcKind::SecretGetResponse:
	case EControlIpcKind::SecretApplyResponse:
	case EControlIpcKind::SecretCapabilityIssueResponse:
	case EControlIpcKind::SecretCapabilityRevokeSessionResponse:
	case EControlIpcKind::ProfileResponse:
		return EControlIpcFlags::Response;
	}
	return EControlIpcFlags::None;
}

[[nodiscard]] bool IsValidUtf8(std::span<const std::uint8_t> bytes) noexcept
{
	for (std::size_t index = 0; index < bytes.size();) {
		const auto first = bytes[index];
		if (first <= 0x7f) {
			++index;
			continue;
		}
		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		if ((first & 0xe0) == 0xc0) {
			continuationCount = 1;
			codePoint = first & 0x1f;
		} else if ((first & 0xf0) == 0xe0) {
			continuationCount = 2;
			codePoint = first & 0x0f;
		} else if ((first & 0xf8) == 0xf0) {
			continuationCount = 3;
			codePoint = first & 0x07;
		} else {
			return false;
		}
		if (index + continuationCount >= bytes.size()) return false;
		for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
			const auto next = bytes[index + offset];
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80u : continuationCount == 2 ? 0x800u : 0x10000u;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		index += continuationCount + 1;
	}
	return true;
}

[[nodiscard]] EControlIpcDecodeOutcome ValidateHeader(const ControlIpcHeader& header) noexcept
{
	if (header.majorVersion != kControlIpcMajorVersion) return EControlIpcDecodeOutcome::UnsupportedMajorVersion;
	if (!IsKnownKind(header.kind)) return EControlIpcDecodeOutcome::UnknownKind;
	const auto rawFlags = static_cast<std::uint16_t>(header.flags);
	const auto knownFlags = static_cast<std::uint16_t>(EControlIpcFlags::Request | EControlIpcFlags::Response |
		EControlIpcFlags::Terminal);
	if ((rawFlags & ~knownFlags) != 0) return EControlIpcDecodeOutcome::UnknownRequiredFlags;
	const auto expectedDirection = ExpectedDirection(header.kind);
	if (expectedDirection == EControlIpcFlags::Request) {
		// Requests are never terminal and cannot claim the response direction.
		if (header.flags != EControlIpcFlags::Request) return EControlIpcDecodeOutcome::InvalidFlags;
	} else {
		// A request may have bounded non-terminal response frames before its one terminal
		// response. Error frames themselves are always terminal.
		if (!HasFlag(header.flags, EControlIpcFlags::Response) || HasFlag(header.flags, EControlIpcFlags::Request) ||
			(header.kind == EControlIpcKind::Error && !HasFlag(header.flags, EControlIpcFlags::Terminal))) {
			return EControlIpcDecodeOutcome::InvalidFlags;
		}
	}
	if (header.requestId == 0) return EControlIpcDecodeOutcome::InvalidRequestId;
	if (header.generation == 0 && header.kind != EControlIpcKind::Hello) {
		return EControlIpcDecodeOutcome::InvalidGeneration;
	}
	return EControlIpcDecodeOutcome::Decoded;
}

[[nodiscard]] EControlIpcEncodeOutcome ToEncodeOutcome(EControlIpcDecodeOutcome outcome) noexcept
{
	switch (outcome) {
	case EControlIpcDecodeOutcome::UnknownKind: return EControlIpcEncodeOutcome::UnknownKind;
	case EControlIpcDecodeOutcome::UnknownRequiredFlags: return EControlIpcEncodeOutcome::UnknownRequiredFlags;
	case EControlIpcDecodeOutcome::InvalidFlags: return EControlIpcEncodeOutcome::InvalidFlags;
	case EControlIpcDecodeOutcome::InvalidRequestId: return EControlIpcEncodeOutcome::InvalidRequestId;
	case EControlIpcDecodeOutcome::InvalidGeneration: return EControlIpcEncodeOutcome::InvalidGeneration;
	default: return EControlIpcEncodeOutcome::InvalidFlags;
	}
}

void SetFailure(EControlIpcFieldDecodeOutcome* failure, EControlIpcFieldDecodeOutcome value) noexcept
{
	if (failure) *failure = value;
}

} // namespace

ControlIpcEncodeResult EncodeControlIpcFrame(const ControlIpcFrame& frame)
{
	const auto headerStatus = ValidateHeader(frame.header);
	if (headerStatus != EControlIpcDecodeOutcome::Decoded) return { ToEncodeOutcome(headerStatus), {} };
	if (frame.payload.size() > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes) {
		return { EControlIpcEncodeOutcome::PayloadTooLarge, {} };
	}
	const auto frameBytes = kControlIpcHeaderBytes + frame.payload.size();
	std::vector<std::uint8_t> bytes;
	bytes.reserve(kControlIpcLengthPrefixBytes + frameBytes);
	AppendLittleEndian<std::uint32_t>(bytes, static_cast<std::uint32_t>(frameBytes));
	AppendLittleEndian<std::uint32_t>(bytes, kControlIpcMagic);
	AppendLittleEndian<std::uint16_t>(bytes, frame.header.majorVersion);
	AppendLittleEndian<std::uint16_t>(bytes, frame.header.minorVersion);
	AppendLittleEndian<std::uint16_t>(bytes, static_cast<std::uint16_t>(frame.header.kind));
	AppendLittleEndian<std::uint16_t>(bytes, static_cast<std::uint16_t>(frame.header.flags));
	AppendLittleEndian<std::uint64_t>(bytes, frame.header.requestId);
	AppendLittleEndian<std::uint64_t>(bytes, frame.header.generation);
	bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
	return { EControlIpcEncodeOutcome::Encoded, std::move(bytes) };
}

std::optional<std::vector<std::uint8_t>> EncodeControlIpcFields(const ControlIpcFields& fields)
{
	if (fields.size() > kControlIpcMaximumFieldCount) return std::nullopt;
	std::array<bool, std::numeric_limits<std::uint16_t>::max() + 1> usedTags{};
	std::size_t total = 0;
	for (const auto& field : fields) {
		if (field.tag == 0 || usedTags[field.tag] || field.value.size() > std::numeric_limits<std::uint32_t>::max() ||
			field.value.size() > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes ||
			total > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes - 6 - field.value.size()) return std::nullopt;
		usedTags[field.tag] = true;
		total += 6 + field.value.size();
	}
	std::vector<std::uint8_t> bytes;
	bytes.reserve(total);
	for (const auto& field : fields) {
		AppendLittleEndian<std::uint16_t>(bytes, field.tag);
		AppendLittleEndian<std::uint32_t>(bytes, static_cast<std::uint32_t>(field.value.size()));
		bytes.insert(bytes.end(), field.value.begin(), field.value.end());
	}
	return bytes;
}

ControlIpcFieldDecodeResult DecodeControlIpcFields(std::span<const std::uint8_t> bytes)
{
	ControlIpcFieldDecodeResult result;
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		if (bytes.size() - offset < 6) return { EControlIpcFieldDecodeOutcome::MalformedField, {} };
		if (result.fields.size() == kControlIpcMaximumFieldCount) return { EControlIpcFieldDecodeOutcome::TooManyFields, {} };
		const auto tag = ReadLittleEndian<std::uint16_t>(bytes.data() + offset);
		const auto length = ReadLittleEndian<std::uint32_t>(bytes.data() + offset + 2);
		offset += 6;
		if (tag == 0) return { EControlIpcFieldDecodeOutcome::InvalidTag, {} };
		if (length > bytes.size() - offset) return { EControlIpcFieldDecodeOutcome::MalformedField, {} };
		if (length > kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes) {
			return { EControlIpcFieldDecodeOutcome::FieldTooLarge, {} };
		}
		ControlIpcField field;
		field.tag = tag;
		field.value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
		result.fields.push_back(std::move(field));
		offset += length;
	}
	return result;
}

bool AddUtf8Field(ControlIpcFields& fields, EControlIpcFieldTag tag, std::string_view value)
{
	if (fields.size() >= kControlIpcMaximumFieldCount || value.size() > kControlIpcMaximumUtf8FieldBytes ||
		!IsValidUtf8(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()))) {
		return false;
	}
	fields.push_back({ static_cast<std::uint16_t>(tag),
		std::vector<std::uint8_t>(value.begin(), value.end()) });
	return true;
}

std::optional<std::string> GetUtf8Field(const ControlIpcFields& fields, EControlIpcFieldTag tag,
	EControlIpcFieldDecodeOutcome* failure)
{
	SetFailure(failure, EControlIpcFieldDecodeOutcome::Decoded);
	const auto encodedTag = static_cast<std::uint16_t>(tag);
	const ControlIpcField* found = nullptr;
	for (const auto& field : fields) {
		if (field.tag != encodedTag) continue;
		if (found != nullptr) {
			SetFailure(failure, EControlIpcFieldDecodeOutcome::DuplicateField);
			return std::nullopt;
		}
		found = &field;
	}
	if (!found) {
		SetFailure(failure, EControlIpcFieldDecodeOutcome::MissingField);
		return std::nullopt;
	}
	if (found->value.size() > kControlIpcMaximumUtf8FieldBytes || !IsValidUtf8(found->value)) {
		SetFailure(failure, EControlIpcFieldDecodeOutcome::InvalidUtf8);
		return std::nullopt;
	}
	return std::string(found->value.begin(), found->value.end());
}

std::optional<std::vector<std::uint8_t>> EncodeControlIpcError(const ControlIpcError& error)
{
	if (!IsKnownTerminalStatus(error.status)) return std::nullopt;
	ControlIpcFields fields;
	std::vector<std::uint8_t> status;
	status.reserve(sizeof(std::uint16_t));
	AppendLittleEndian<std::uint16_t>(status, static_cast<std::uint16_t>(error.status));
	fields.push_back({ static_cast<std::uint16_t>(EControlIpcFieldTag::TerminalStatus), std::move(status) });
	if (!AddUtf8Field(fields, EControlIpcFieldTag::Diagnostic, error.diagnostic)) return std::nullopt;
	return EncodeControlIpcFields(fields);
}

std::optional<ControlIpcError> DecodeControlIpcError(std::span<const std::uint8_t> payload,
	EControlIpcFieldDecodeOutcome* failure)
{
	const auto fields = DecodeControlIpcFields(payload);
	if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded) {
		SetFailure(failure, fields.outcome);
		return std::nullopt;
	}
	const auto statusTag = static_cast<std::uint16_t>(EControlIpcFieldTag::TerminalStatus);
	const ControlIpcField* statusField = nullptr;
	for (const auto& field : fields.fields) {
		if (field.tag != statusTag) continue;
		if (statusField) {
			SetFailure(failure, EControlIpcFieldDecodeOutcome::DuplicateField);
			return std::nullopt;
		}
		statusField = &field;
	}
	if (!statusField || statusField->value.size() != sizeof(std::uint16_t)) {
		SetFailure(failure, EControlIpcFieldDecodeOutcome::MalformedField);
		return std::nullopt;
	}
	const auto status = static_cast<EControlIpcTerminalStatus>(
		ReadLittleEndian<std::uint16_t>(statusField->value.data()));
	if (!IsKnownTerminalStatus(status)) {
		SetFailure(failure, EControlIpcFieldDecodeOutcome::MalformedField);
		return std::nullopt;
	}
	const auto diagnostic = GetUtf8Field(fields.fields, EControlIpcFieldTag::Diagnostic, failure);
	if (!diagnostic) return std::nullopt;
	return ControlIpcError{ status, std::move(*diagnostic) };
}

CControlIpcFrameDecoder::CControlIpcFrameDecoder(std::size_t maximumFrameBytes) noexcept
	: m_maximumFrameBytes(std::clamp(maximumFrameBytes, kControlIpcHeaderBytes, kControlIpcMaximumFrameBytes))
{
}

ControlIpcDecodeResult CControlIpcFrameDecoder::Feed(std::span<const std::uint8_t> bytes)
{
	ControlIpcDecodeResult result;
	if (IsFailed()) {
		result.outcome = m_failure;
		return result;
	}
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		if (m_lengthPrefixUsed < kControlIpcLengthPrefixBytes) {
			const auto count = std::min(kControlIpcLengthPrefixBytes - m_lengthPrefixUsed, bytes.size() - offset);
			std::copy_n(bytes.data() + offset, count, m_lengthPrefix.data() + m_lengthPrefixUsed);
			m_lengthPrefixUsed += count;
			offset += count;
			if (m_lengthPrefixUsed != kControlIpcLengthPrefixBytes) continue;
			m_expectedFrameBytes = ReadLittleEndian<std::uint32_t>(m_lengthPrefix.data());
			if (m_expectedFrameBytes == 0) {
				Fail(EControlIpcDecodeOutcome::ZeroLengthFrame);
				break;
			}
			if (m_expectedFrameBytes < kControlIpcHeaderBytes) {
				Fail(EControlIpcDecodeOutcome::MalformedFrame);
				break;
			}
			if (m_expectedFrameBytes > m_maximumFrameBytes) {
				Fail(EControlIpcDecodeOutcome::OversizeFrame);
				break;
			}
			m_frameBytes.clear();
			m_frameBytes.reserve(m_expectedFrameBytes);
		}
		const auto count = std::min(m_expectedFrameBytes - m_frameBytes.size(), bytes.size() - offset);
		m_frameBytes.insert(m_frameBytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
		offset += count;
		if (m_frameBytes.size() != m_expectedFrameBytes) continue;
		ControlIpcFrame frame;
		const auto frameOutcome = DecodeCompleteFrame(frame);
		if (frameOutcome != EControlIpcDecodeOutcome::Decoded) {
			Fail(frameOutcome);
			break;
		}
		result.frames.push_back(std::move(frame));
		m_lengthPrefixUsed = 0;
		m_expectedFrameBytes = 0;
		m_frameBytes.clear();
	}
	result.outcome = IsFailed() ? m_failure : (result.frames.empty() ? EControlIpcDecodeOutcome::NeedMoreData : EControlIpcDecodeOutcome::Decoded);
	return result;
}

void CControlIpcFrameDecoder::Reset() noexcept
{
	m_lengthPrefixUsed = 0;
	m_expectedFrameBytes = 0;
	m_frameBytes.clear();
	m_failure = EControlIpcDecodeOutcome::NeedMoreData;
}

bool CControlIpcFrameDecoder::IsFailed() const noexcept
{
	return m_failure != EControlIpcDecodeOutcome::NeedMoreData;
}

EControlIpcDecodeOutcome CControlIpcFrameDecoder::GetFailure() const noexcept
{
	return m_failure;
}

EControlIpcDecodeOutcome CControlIpcFrameDecoder::DecodeCompleteFrame(ControlIpcFrame& output) const noexcept
{
	if (m_frameBytes.size() < kControlIpcHeaderBytes) return EControlIpcDecodeOutcome::MalformedFrame;
	const auto* bytes = m_frameBytes.data();
	if (ReadLittleEndian<std::uint32_t>(bytes) != kControlIpcMagic) return EControlIpcDecodeOutcome::MalformedFrame;
	output.header.majorVersion = ReadLittleEndian<std::uint16_t>(bytes + 4);
	output.header.minorVersion = ReadLittleEndian<std::uint16_t>(bytes + 6);
	output.header.kind = static_cast<EControlIpcKind>(ReadLittleEndian<std::uint16_t>(bytes + 8));
	output.header.flags = static_cast<EControlIpcFlags>(ReadLittleEndian<std::uint16_t>(bytes + 10));
	output.header.requestId = ReadLittleEndian<std::uint64_t>(bytes + 12);
	output.header.generation = ReadLittleEndian<std::uint64_t>(bytes + 20);
	const auto validation = ValidateHeader(output.header);
	if (validation != EControlIpcDecodeOutcome::Decoded) return validation;
	output.payload.assign(m_frameBytes.begin() + static_cast<std::ptrdiff_t>(kControlIpcHeaderBytes), m_frameBytes.end());
	return EControlIpcDecodeOutcome::Decoded;
}

void CControlIpcFrameDecoder::Fail(EControlIpcDecodeOutcome outcome) noexcept
{
	m_failure = outcome;
}

} // namespace platform::controlipc
