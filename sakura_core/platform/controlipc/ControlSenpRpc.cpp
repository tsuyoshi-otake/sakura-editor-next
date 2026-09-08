/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlSenpRpc.h"

#include <Windows.h>

#include <limits>
#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {
constexpr std::size_t kMaximumPayload = kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;
constexpr std::uint8_t kControlSenpRpcPayloadVersion = 1;
constexpr std::uint64_t kMaximumGeneration = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

template<class T> void Put(std::vector<std::uint8_t>& bytes, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
	}
}

template<class T> bool Get(std::span<const std::uint8_t> bytes, std::size_t& offset, T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index) {
		value |= static_cast<T>(bytes[offset + index]) << (index * 8);
	}
	offset += sizeof(T);
	return true;
}

bool ToUtf8(std::wstring_view source, std::string& target)
{
	if (source.empty()) { target.clear(); return true; }
	if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
		static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return false;
	target.resize(static_cast<std::size_t>(bytes));
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()),
		target.data(), bytes, nullptr, nullptr) == bytes;
}

bool FromUtf8(std::string_view source, std::wstring& target)
{
	if (source.empty()) { target.clear(); return true; }
	if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
		static_cast<int>(source.size()), nullptr, 0);
	if (characters <= 0) return false;
	target.resize(static_cast<std::size_t>(characters));
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()),
		target.data(), characters) == characters;
}

bool PutBytes(std::vector<std::uint8_t>& bytes, std::string_view value, std::size_t maximum)
{
	if (value.size() > maximum) return false;
	if (bytes.size() + sizeof(std::uint32_t) + value.size() > kMaximumPayload) return false;
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(value.size()));
	bytes.insert(bytes.end(), value.begin(), value.end());
	return true;
}

bool GetBytes(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& value, std::size_t maximum)
{
	std::uint32_t length = 0;
	if (!Get(bytes, offset, length) || length > maximum || length > bytes.size() - offset) return false;
	value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
	offset += length;
	return true;
}

//! UTF-8 text field. Embedded NUL is rejected so a truncated field cannot be
//! reinterpreted as a shorter value by a native consumer.
bool PutUtf8(std::vector<std::uint8_t>& bytes, std::string_view value)
{
	if (value.find('\0') != std::string_view::npos) return false;
	return PutBytes(bytes, value, kControlIpcMaximumUtf8FieldBytes);
}

bool GetUtf8(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& value)
{
	if (!GetBytes(bytes, offset, value, kControlIpcMaximumUtf8FieldBytes)) return false;
	return value.find('\0') == std::string::npos;
}

bool PutWide(std::vector<std::uint8_t>& bytes, std::wstring_view value)
{
	std::string utf8;
	return ToUtf8(value, utf8) && PutUtf8(bytes, utf8);
}

bool GetWide(std::span<const std::uint8_t> bytes, std::size_t& offset, std::wstring& value)
{
	std::string utf8;
	return GetUtf8(bytes, offset, utf8) && FromUtf8(utf8, value);
}

bool IsOperation(std::uint8_t operation) noexcept
{
	return operation >= static_cast<std::uint8_t>(EControlSenpRpcOperation::IssueGrant)
		&& operation <= static_cast<std::uint8_t>(EControlSenpRpcOperation::ReleaseResource);
}

bool IsStatus(std::uint8_t status) noexcept
{
	return status <= static_cast<std::uint8_t>(EControlSenpRpcStatus::NotConnected);
}

bool IsCompletionStatus(std::uint8_t status) noexcept
{
	return status <= static_cast<std::uint8_t>(senp::effect::CompletionStatus::HostUnavailable);
}

//! Rejects a payload whose members do not belong to its operation. A decoder
//! that silently ignored stray members would let one operation smuggle another
//! operation's authority-relevant fields past a later switch.
bool IsCoherentRequest(const ControlSenpRpcRequest& request) noexcept
{
	if (request.profileId.empty() || request.owner.extensionId.empty()) return false;
	if (request.owner.generation <= 0) return false;
	if (request.owner.workspaceRevision < 0 || request.owner.accountGeneration < 0) return false;
	if (request.arguments.size() > kControlSenpRpcMaximumArguments) return false;
	const bool hasRead = !request.readId.empty();
	const bool hasTool = !request.toolId.empty() || !request.toolOperation.empty();
	const bool hasResource = !request.resourceHandle.empty();
	switch (request.operation) {
	case EControlSenpRpcOperation::IssueGrant:
		return request.capabilities != 0 && request.grantId.empty() && !hasRead && !hasTool
			&& request.arguments.empty() && !hasResource && request.offset == 0 && request.length == 0;
	case EControlSenpRpcOperation::StartRead:
		return !request.grantId.empty() && request.capabilities == 0 && hasRead
			&& !request.toolId.empty() && !request.toolOperation.empty() && !hasResource
			&& request.offset == 0 && request.length == 0;
	case EControlSenpRpcOperation::PollRead:
		return !request.grantId.empty() && request.capabilities == 0 && !hasRead && !hasTool
			&& request.arguments.empty() && !hasResource && request.offset == 0 && request.length == 0;
	case EControlSenpRpcOperation::CancelRead:
		return !request.grantId.empty() && request.capabilities == 0 && hasRead && !hasTool
			&& request.arguments.empty() && !hasResource && request.offset == 0 && request.length == 0;
	case EControlSenpRpcOperation::ReadResource:
		return !request.grantId.empty() && request.capabilities == 0 && !hasRead && !hasTool
			&& request.arguments.empty() && hasResource && request.length > 0
			&& request.length <= kControlSenpRpcMaximumResourceChunkBytes;
	case EControlSenpRpcOperation::ReleaseResource:
		return !request.grantId.empty() && request.capabilities == 0 && !hasRead && !hasTool
			&& request.arguments.empty() && hasResource && request.offset == 0 && request.length == 0;
	default:
		return false;
	}
}

bool IsCoherentResponse(const ControlSenpRpcResponse& response) noexcept
{
	if (!response.hasCompletion
		&& (!response.completion.readId.empty() || !response.completion.data.empty()
			|| !response.completion.message.empty())) {
		return false;
	}
	if (response.hasCompletion && response.completion.readId.empty()) return false;
	if (response.completion.data.size() > kControlSenpRpcMaximumToolDataBytes) return false;
	if (!response.resourceBytes.empty() && response.resourceHandle.empty()) return false;
	if (response.resourceBytes.size() > kControlSenpRpcMaximumResourceChunkBytes) return false;
	return true;
}
} // namespace

senp::ContributionOwnerIdentity ToContributionOwner(const ControlSenpRpcOwner& owner)
{
	return { owner.extensionId, owner.packageDigest, owner.generation,
		owner.workspaceRevision, owner.accountGeneration };
}

ControlSenpRpcOwner FromContributionOwner(const senp::ContributionOwnerIdentity& owner)
{
	return { owner.extensionId, owner.packageDigest, owner.generation,
		owner.workspaceRevision, owner.accountGeneration };
}

std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcRequest(const ControlSenpRpcRequest& request)
{
	if (!IsOperation(static_cast<std::uint8_t>(request.operation))) return std::nullopt;
	if (!IsCoherentRequest(request)) return std::nullopt;
	std::vector<std::uint8_t> bytes;
	Put<std::uint8_t>(bytes, kControlSenpRpcPayloadVersion);
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(request.operation));
	if (!PutWide(bytes, request.profileId)) return std::nullopt;
	if (!PutWide(bytes, request.owner.extensionId)) return std::nullopt;
	if (!PutWide(bytes, request.owner.packageDigest)) return std::nullopt;
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.owner.generation));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.owner.workspaceRevision));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.owner.accountGeneration));
	if (!PutUtf8(bytes, request.grantId)) return std::nullopt;
	Put<std::uint32_t>(bytes, request.capabilities);
	if (!PutWide(bytes, request.readId)) return std::nullopt;
	if (!PutWide(bytes, request.toolId)) return std::nullopt;
	if (!PutWide(bytes, request.toolOperation)) return std::nullopt;
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(request.arguments.size()));
	for (const auto& argument : request.arguments) {
		if (argument.name.empty()) return std::nullopt;
		if (!PutWide(bytes, argument.name) || !PutWide(bytes, argument.value)) return std::nullopt;
	}
	if (!PutWide(bytes, request.resourceHandle)) return std::nullopt;
	Put<std::uint64_t>(bytes, request.offset);
	Put<std::uint32_t>(bytes, request.length);
	if (bytes.size() > kMaximumPayload) return std::nullopt;
	return bytes;
}

std::optional<ControlSenpRpcRequest> DecodeControlSenpRpcRequest(std::span<const std::uint8_t> payload)
{
	std::size_t offset = 0;
	std::uint8_t version = 0, operation = 0;
	if (!Get(payload, offset, version) || version != kControlSenpRpcPayloadVersion) return std::nullopt;
	if (!Get(payload, offset, operation) || !IsOperation(operation)) return std::nullopt;
	ControlSenpRpcRequest request;
	request.operation = static_cast<EControlSenpRpcOperation>(operation);
	if (!GetWide(payload, offset, request.profileId)) return std::nullopt;
	if (!GetWide(payload, offset, request.owner.extensionId)) return std::nullopt;
	if (!GetWide(payload, offset, request.owner.packageDigest)) return std::nullopt;
	std::uint64_t generation = 0, workspaceRevision = 0, accountGeneration = 0;
	if (!Get(payload, offset, generation) || generation > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, workspaceRevision) || workspaceRevision > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, accountGeneration) || accountGeneration > kMaximumGeneration) return std::nullopt;
	request.owner.generation = static_cast<std::int64_t>(generation);
	request.owner.workspaceRevision = static_cast<std::int64_t>(workspaceRevision);
	request.owner.accountGeneration = static_cast<std::int64_t>(accountGeneration);
	if (!GetUtf8(payload, offset, request.grantId)) return std::nullopt;
	if (!Get(payload, offset, request.capabilities)) return std::nullopt;
	if (!GetWide(payload, offset, request.readId)) return std::nullopt;
	if (!GetWide(payload, offset, request.toolId)) return std::nullopt;
	if (!GetWide(payload, offset, request.toolOperation)) return std::nullopt;
	std::uint32_t arguments = 0;
	if (!Get(payload, offset, arguments) || arguments > kControlSenpRpcMaximumArguments) return std::nullopt;
	request.arguments.reserve(arguments);
	for (std::uint32_t index = 0; index < arguments; ++index) {
		senp::effect::Field field;
		if (!GetWide(payload, offset, field.name) || field.name.empty()) return std::nullopt;
		if (!GetWide(payload, offset, field.value)) return std::nullopt;
		request.arguments.push_back(std::move(field));
	}
	if (!GetWide(payload, offset, request.resourceHandle)) return std::nullopt;
	if (!Get(payload, offset, request.offset)) return std::nullopt;
	if (!Get(payload, offset, request.length)) return std::nullopt;
	if (offset != payload.size()) return std::nullopt;
	if (!IsCoherentRequest(request)) return std::nullopt;
	return request;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcResponse(const ControlSenpRpcResponse& response)
{
	if (!IsStatus(static_cast<std::uint8_t>(response.status))) return std::nullopt;
	if (!IsCompletionStatus(static_cast<std::uint8_t>(response.completion.status))) return std::nullopt;
	if (!IsCoherentResponse(response)) return std::nullopt;
	std::vector<std::uint8_t> bytes;
	Put<std::uint8_t>(bytes, kControlSenpRpcPayloadVersion);
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.status));
	if (!PutUtf8(bytes, response.grantId)) return std::nullopt;
	Put<std::uint64_t>(bytes, response.expiresAtMilliseconds);
	Put<std::uint8_t>(bytes, response.hasCompletion ? 1U : 0U);
	if (!PutWide(bytes, response.completion.readId)) return std::nullopt;
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.completion.status));
	if (!PutWide(bytes, response.completion.data)) return std::nullopt;
	if (!PutWide(bytes, response.completion.message)) return std::nullopt;
	if (!PutWide(bytes, response.resourceHandle)) return std::nullopt;
	Put<std::uint64_t>(bytes, response.resourceOffset);
	if (!PutBytes(bytes, response.resourceBytes, kControlSenpRpcMaximumResourceChunkBytes)) return std::nullopt;
	Put<std::uint8_t>(bytes, response.resourceState);
	Put<std::uint8_t>(bytes, response.resourceFinal ? 1U : 0U);
	if (bytes.size() > kMaximumPayload) return std::nullopt;
	return bytes;
}

std::optional<ControlSenpRpcResponse> DecodeControlSenpRpcResponse(std::span<const std::uint8_t> payload)
{
	std::size_t offset = 0;
	std::uint8_t version = 0, status = 0, hasCompletion = 0, completionStatus = 0, resourceFinal = 0;
	if (!Get(payload, offset, version) || version != kControlSenpRpcPayloadVersion) return std::nullopt;
	if (!Get(payload, offset, status) || !IsStatus(status)) return std::nullopt;
	ControlSenpRpcResponse response;
	response.status = static_cast<EControlSenpRpcStatus>(status);
	if (!GetUtf8(payload, offset, response.grantId)) return std::nullopt;
	if (!Get(payload, offset, response.expiresAtMilliseconds)) return std::nullopt;
	if (!Get(payload, offset, hasCompletion) || hasCompletion > 1) return std::nullopt;
	response.hasCompletion = hasCompletion != 0;
	if (!GetWide(payload, offset, response.completion.readId)) return std::nullopt;
	if (!Get(payload, offset, completionStatus) || !IsCompletionStatus(completionStatus)) return std::nullopt;
	response.completion.status = static_cast<senp::effect::CompletionStatus>(completionStatus);
	if (!GetWide(payload, offset, response.completion.data)) return std::nullopt;
	if (!GetWide(payload, offset, response.completion.message)) return std::nullopt;
	if (!GetWide(payload, offset, response.resourceHandle)) return std::nullopt;
	if (!Get(payload, offset, response.resourceOffset)) return std::nullopt;
	if (!GetBytes(payload, offset, response.resourceBytes, kControlSenpRpcMaximumResourceChunkBytes)) return std::nullopt;
	if (!Get(payload, offset, response.resourceState)) return std::nullopt;
	if (!Get(payload, offset, resourceFinal) || resourceFinal > 1) return std::nullopt;
	response.resourceFinal = resourceFinal != 0;
	if (offset != payload.size()) return std::nullopt;
	if (!IsCoherentResponse(response)) return std::nullopt;
	return response;
}

} // namespace platform::controlipc
