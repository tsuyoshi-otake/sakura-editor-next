/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlSenpRpc.h"

#include "senp/SenpTextResource.h"

#include <Windows.h>

#include <limits>
#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {
constexpr std::size_t kMaximumPayload = kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;
//! Bumped with every payload layout change. Version 2 added the account
//! members a QueryAccount answer carries; version 3 added the workspace an
//! AdoptWorkspace request declares; version 4 replaced the derived
//! `resourceFinal` flag with the end, length and revision that make a
//! ReadResource answer a whole text-resource chunk.
constexpr std::uint8_t kControlSenpRpcPayloadVersion = 4;
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

//! Wide text held to a bound the caller names instead of the generic field one.
//! A field that is allowed to be large says so at its own call site, so it
//! cannot raise the ceiling every other field on this channel is held to.
bool PutWideBounded(std::vector<std::uint8_t>& bytes, std::wstring_view value, std::size_t maximum)
{
	std::string utf8;
	if (!ToUtf8(value, utf8) || utf8.find('\0') != std::string::npos) return false;
	return PutBytes(bytes, utf8, maximum);
}

bool GetWideBounded(std::span<const std::uint8_t> bytes, std::size_t& offset, std::wstring& value,
	std::size_t maximum)
{
	std::string utf8;
	if (!GetBytes(bytes, offset, utf8, maximum)) return false;
	return utf8.find('\0') == std::string::npos && FromUtf8(utf8, value);
}

bool IsOperation(std::uint8_t operation) noexcept
{
	return operation >= static_cast<std::uint8_t>(EControlSenpRpcOperation::IssueGrant)
		&& operation <= static_cast<std::uint8_t>(EControlSenpRpcOperation::AdoptWorkspace);
}

bool IsAccountState(std::uint8_t state) noexcept
{
	return state <= static_cast<std::uint8_t>(EControlSenpAccountState::Unavailable);
}

bool IsStatus(std::uint8_t status) noexcept
{
	return status <= static_cast<std::uint8_t>(EControlSenpRpcStatus::NotConnected);
}

bool IsCompletionStatus(std::uint8_t status) noexcept
{
	return status <= static_cast<std::uint8_t>(senp::effect::CompletionStatus::HostUnavailable);
}

//! A discriminator outside either enum decodes into a value the editor's text
//! surface never tests for, and would be read there as some state it does not
//! handle rather than as the malformed answer it is.
bool IsResourceState(std::uint8_t state) noexcept
{
	return state <= static_cast<std::uint8_t>(senp::TextResourceState::Closed);
}

bool IsResourceEnd(std::uint8_t end) noexcept
{
	return end <= static_cast<std::uint8_t>(senp::TextResourceEnd::Revoked);
}

//! Rejects a payload whose members do not belong to its operation. A decoder
//! that silently ignored stray members would let one operation smuggle another
//! operation's authority-relevant fields past a later switch.
bool IsCoherentRequest(const ControlSenpRpcRequest& request) noexcept
{
	if (request.ProfileId().empty()) return false;
	if (request.Arguments().size() > kControlSenpRpcMaximumArguments) return false;
	const bool hasRead = !request.ReadId().empty();
	const bool hasTool = !request.ToolId().empty() || !request.ToolOperation().empty();
	const bool hasResource = !request.ResourceHandle().empty();
	// A workspace declaration precedes every owner for the same reason as the
	// account query, and is refused on the same terms: there is no owner to
	// scope it by, so it must carry nothing an owner-scoped operation carries.
	if (request.Operation() == EControlSenpRpcOperation::AdoptWorkspace) {
		// A revision of zero has observed nothing. Admitting one would let a
		// staleness check pass against a workspace that was never captured.
		if (request.Workspace().Generation() <= 0 || request.Workspace().Revision() <= 0) return false;
		if (request.Workspace().Folders().size() > kControlSenpRpcMaximumWorkspaceFolders) return false;
		for (const auto& folder : request.Workspace().Folders()) {
			if (folder.empty()) return false;
		}
		return request.Owner() == ControlSenpRpcOwner{} && request.GrantId().empty()
			&& request.Capabilities() == 0 && !hasRead && !hasTool && request.Arguments().empty()
			&& !hasResource && request.Offset() == 0 && request.Length() == 0;
	}
	// Every other operation answers for a workspace the control side already
	// holds. One carrying a declaration would be restating that state where
	// nothing rechecks it, so the declaration is refused rather than ignored.
	if (!request.Workspace().Empty()) return false;
	// The account query precedes every owner: an editor cannot name the account
	// generation it is asking for. An owner here could only be a claim that
	// nothing downstream rechecks, so a populated one is refused rather than
	// carried past the switch below.
	if (request.Operation() == EControlSenpRpcOperation::QueryAccount) {
		return request.Owner() == ControlSenpRpcOwner{} && request.GrantId().empty()
			&& request.Capabilities() == 0 && !hasRead && !hasTool && request.Arguments().empty()
			&& !hasResource && request.Offset() == 0 && request.Length() == 0;
	}
	if (request.Owner().ExtensionId().empty()) return false;
	if (request.Owner().Generation() <= 0) return false;
	if (request.Owner().WorkspaceRevision() < 0 || request.Owner().AccountGeneration() < 0) return false;
	switch (request.Operation()) {
	case EControlSenpRpcOperation::IssueGrant:
		return request.Capabilities() != 0 && request.GrantId().empty() && !hasRead && !hasTool
			&& request.Arguments().empty() && !hasResource && request.Offset() == 0 && request.Length() == 0;
	case EControlSenpRpcOperation::StartRead:
		return !request.GrantId().empty() && request.Capabilities() == 0 && hasRead
			&& !request.ToolId().empty() && !request.ToolOperation().empty() && !hasResource
			&& request.Offset() == 0 && request.Length() == 0;
	case EControlSenpRpcOperation::PollRead:
		return !request.GrantId().empty() && request.Capabilities() == 0 && !hasRead && !hasTool
			&& request.Arguments().empty() && !hasResource && request.Offset() == 0 && request.Length() == 0;
	case EControlSenpRpcOperation::CancelRead:
		return !request.GrantId().empty() && request.Capabilities() == 0 && hasRead && !hasTool
			&& request.Arguments().empty() && !hasResource && request.Offset() == 0 && request.Length() == 0;
	case EControlSenpRpcOperation::ReadResource:
		return !request.GrantId().empty() && request.Capabilities() == 0 && !hasRead && !hasTool
			&& request.Arguments().empty() && hasResource && request.Length() > 0
			&& request.Length() <= kControlSenpRpcMaximumResourceChunkBytes;
	case EControlSenpRpcOperation::ReleaseResource:
		return !request.GrantId().empty() && request.Capabilities() == 0 && !hasRead && !hasTool
			&& request.Arguments().empty() && hasResource && request.Offset() == 0 && request.Length() == 0;
	default:
		return false;
	}
}

bool IsCoherentResponse(const ControlSenpRpcResponse& response) noexcept
{
	if (!response.HasCompletion()
		&& (!response.Completion().readId.empty() || !response.Completion().data.empty()
			|| !response.Completion().message.empty())) {
		return false;
	}
	if (response.HasCompletion() && response.Completion().readId.empty()) return false;
	// Counted in characters against a bound written in UTF-8 bytes, which is the
	// conservative direction: a character never encodes to fewer than one byte,
	// so anything this admits the encoder still measures for real.
	if (response.Completion().data.size() > kControlSenpRpcMaximumToolDataBytes) return false;
	if (!IsResourceState(response.ResourceState()) || !IsResourceEnd(response.ResourceEnd())) return false;
	if (response.ResourceRevision() < 0) return false;
	if (response.ResourceBytes().size() > kControlSenpRpcMaximumResourceChunkBytes) return false;
	if (response.ResourceHandle().empty()) {
		// Nothing here names the resource these members would describe, so a
		// refusal or an unrelated answer must carry none of them: the editor
		// would otherwise be free to read one as a chunk of whichever resource
		// it happens to be filling.
		if (!response.ResourceBytes().empty() || response.ResourceOffset() != 0
			|| response.ResourceState() != 0 || response.ResourceEnd() != 0
			|| response.ResourceLength() != 0 || response.ResourceRevision() != 0) {
			return false;
		}
	} else {
		// A chunk that reaches past the resource it belongs to describes no
		// resource the store could have produced.
		if (response.ResourceLength() > kControlSenpRpcMaximumResourceBytes) return false;
		if (response.ResourceOffset() > response.ResourceLength()) return false;
		if (response.ResourceBytes().size() > response.ResourceLength() - response.ResourceOffset()) return false;
	}
	// A negative generation would encode as an enormous unsigned value and decode
	// back as a different number, so it is refused at the encoder instead.
	if (response.AccountGeneration() < 0) return false;
	return true;
}
} // namespace

senp::ContributionOwnerIdentity ToContributionOwner(const ControlSenpRpcOwner& owner)
{
	return { owner.ExtensionId(), owner.PackageDigest(), owner.Generation(),
		owner.WorkspaceRevision(), owner.AccountGeneration() };
}

ControlSenpRpcOwner FromContributionOwner(const senp::ContributionOwnerIdentity& owner)
{
	return ControlSenpRpcOwner(owner.extensionId, owner.packageDigest, owner.generation,
		owner.workspaceRevision, owner.accountGeneration);
}

std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcRequest(const ControlSenpRpcRequest& request)
{
	if (!IsOperation(static_cast<std::uint8_t>(request.Operation()))) return std::nullopt;
	if (!IsCoherentRequest(request)) return std::nullopt;
	std::vector<std::uint8_t> bytes;
	Put<std::uint8_t>(bytes, kControlSenpRpcPayloadVersion);
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(request.Operation()));
	if (!PutWide(bytes, request.ProfileId())) return std::nullopt;
	if (!PutWide(bytes, request.Owner().ExtensionId())) return std::nullopt;
	if (!PutWide(bytes, request.Owner().PackageDigest())) return std::nullopt;
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.Owner().Generation()));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.Owner().WorkspaceRevision()));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.Owner().AccountGeneration()));
	if (!PutUtf8(bytes, request.GrantId())) return std::nullopt;
	Put<std::uint32_t>(bytes, request.Capabilities());
	if (!PutWide(bytes, request.ReadId())) return std::nullopt;
	if (!PutWide(bytes, request.ToolId())) return std::nullopt;
	if (!PutWide(bytes, request.ToolOperation())) return std::nullopt;
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(request.Arguments().size()));
	for (const auto& argument : request.Arguments()) {
		if (argument.name.empty()) return std::nullopt;
		if (!PutWide(bytes, argument.name) || !PutWide(bytes, argument.value)) return std::nullopt;
	}
	if (!PutWide(bytes, request.ResourceHandle())) return std::nullopt;
	Put<std::uint64_t>(bytes, request.Offset());
	Put<std::uint32_t>(bytes, request.Length());
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.Workspace().Generation()));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(request.Workspace().Revision()));
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(request.Workspace().Folders().size()));
	for (const auto& folder : request.Workspace().Folders()) {
		if (!PutWide(bytes, folder)) return std::nullopt;
	}
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
	request.SetOperation(static_cast<EControlSenpRpcOperation>(operation));
	std::wstring profileId;
	if (!GetWide(payload, offset, profileId)) return std::nullopt;
	request.SetProfileId(std::move(profileId));
	std::wstring extensionId, packageDigest;
	if (!GetWide(payload, offset, extensionId)) return std::nullopt;
	if (!GetWide(payload, offset, packageDigest)) return std::nullopt;
	std::uint64_t generation = 0, workspaceRevision = 0, accountGeneration = 0;
	if (!Get(payload, offset, generation) || generation > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, workspaceRevision) || workspaceRevision > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, accountGeneration) || accountGeneration > kMaximumGeneration) return std::nullopt;
	request.SetOwner(ControlSenpRpcOwner(std::move(extensionId), std::move(packageDigest),
		static_cast<std::int64_t>(generation), static_cast<std::int64_t>(workspaceRevision),
		static_cast<std::int64_t>(accountGeneration)));
	std::string grantId;
	if (!GetUtf8(payload, offset, grantId)) return std::nullopt;
	request.SetGrantId(std::move(grantId));
	std::uint32_t capabilities = 0;
	if (!Get(payload, offset, capabilities)) return std::nullopt;
	request.SetCapabilities(capabilities);
	std::wstring readId, toolId, toolOperation;
	if (!GetWide(payload, offset, readId)) return std::nullopt;
	request.SetReadId(std::move(readId));
	if (!GetWide(payload, offset, toolId)) return std::nullopt;
	request.SetToolId(std::move(toolId));
	if (!GetWide(payload, offset, toolOperation)) return std::nullopt;
	request.SetToolOperation(std::move(toolOperation));
	std::uint32_t arguments = 0;
	if (!Get(payload, offset, arguments) || arguments > kControlSenpRpcMaximumArguments) return std::nullopt;
	request.Arguments().reserve(arguments);
	for (std::uint32_t index = 0; index < arguments; ++index) {
		senp::effect::Field field;
		if (!GetWide(payload, offset, field.name) || field.name.empty()) return std::nullopt;
		if (!GetWide(payload, offset, field.value)) return std::nullopt;
		request.Arguments().push_back(std::move(field));
	}
	std::wstring resourceHandle;
	if (!GetWide(payload, offset, resourceHandle)) return std::nullopt;
	request.SetResourceHandle(std::move(resourceHandle));
	std::uint64_t requestOffset = 0;
	std::uint32_t requestLength = 0;
	if (!Get(payload, offset, requestOffset)) return std::nullopt;
	request.SetOffset(requestOffset);
	if (!Get(payload, offset, requestLength)) return std::nullopt;
	request.SetLength(requestLength);
	std::uint64_t declaredGeneration = 0, declaredRevision = 0;
	if (!Get(payload, offset, declaredGeneration) || declaredGeneration > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, declaredRevision) || declaredRevision > kMaximumGeneration) return std::nullopt;
	request.Workspace().SetGeneration(static_cast<std::int64_t>(declaredGeneration));
	request.Workspace().SetRevision(static_cast<std::int64_t>(declaredRevision));
	std::uint32_t folders = 0;
	if (!Get(payload, offset, folders) || folders > kControlSenpRpcMaximumWorkspaceFolders) return std::nullopt;
	request.Workspace().ReserveFolders(folders);
	for (std::uint32_t index = 0; index < folders; ++index) {
		std::wstring folder;
		if (!GetWide(payload, offset, folder)) return std::nullopt;
		request.Workspace().AddFolder(std::move(folder));
	}
	if (offset != payload.size()) return std::nullopt;
	if (!IsCoherentRequest(request)) return std::nullopt;
	return request;
}

std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcResponse(const ControlSenpRpcResponse& response)
{
	if (!IsStatus(static_cast<std::uint8_t>(response.Status()))) return std::nullopt;
	if (!IsCompletionStatus(static_cast<std::uint8_t>(response.Completion().status))) return std::nullopt;
	if (!IsAccountState(static_cast<std::uint8_t>(response.AccountState()))) return std::nullopt;
	if (!IsCoherentResponse(response)) return std::nullopt;
	std::vector<std::uint8_t> bytes;
	Put<std::uint8_t>(bytes, kControlSenpRpcPayloadVersion);
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.Status()));
	if (!PutUtf8(bytes, response.GrantId())) return std::nullopt;
	Put<std::uint64_t>(bytes, response.ExpiresAtMilliseconds());
	Put<std::uint8_t>(bytes, response.HasCompletion() ? 1U : 0U);
	if (!PutWide(bytes, response.Completion().readId)) return std::nullopt;
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.Completion().status));
	if (!PutWideBounded(bytes, response.Completion().data, kControlSenpRpcMaximumToolDataBytes)) {
		return std::nullopt;
	}
	if (!PutWide(bytes, response.Completion().message)) return std::nullopt;
	if (!PutWide(bytes, response.ResourceHandle())) return std::nullopt;
	Put<std::uint64_t>(bytes, response.ResourceOffset());
	if (!PutBytes(bytes, response.ResourceBytes(), kControlSenpRpcMaximumResourceChunkBytes)) return std::nullopt;
	Put<std::uint8_t>(bytes, response.ResourceState());
	Put<std::uint8_t>(bytes, response.ResourceEnd());
	Put<std::uint64_t>(bytes, response.ResourceLength());
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(response.ResourceRevision()));
	Put<std::uint64_t>(bytes, static_cast<std::uint64_t>(response.AccountGeneration()));
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.AccountState()));
	if (bytes.size() > kMaximumPayload) return std::nullopt;
	return bytes;
}

std::optional<ControlSenpRpcResponse> DecodeControlSenpRpcResponse(std::span<const std::uint8_t> payload)
{
	std::size_t offset = 0;
	std::uint8_t version = 0, status = 0, hasCompletion = 0, completionStatus = 0;
	std::uint8_t accountState = 0;
	std::uint64_t accountGeneration = 0, resourceRevision = 0;
	if (!Get(payload, offset, version) || version != kControlSenpRpcPayloadVersion) return std::nullopt;
	if (!Get(payload, offset, status) || !IsStatus(status)) return std::nullopt;
	ControlSenpRpcResponse response;
	response.SetStatus(static_cast<EControlSenpRpcStatus>(status));
	std::string grantId;
	if (!GetUtf8(payload, offset, grantId)) return std::nullopt;
	response.SetGrantId(std::move(grantId));
	std::uint64_t expiresAtMilliseconds = 0;
	if (!Get(payload, offset, expiresAtMilliseconds)) return std::nullopt;
	response.SetExpiresAtMilliseconds(expiresAtMilliseconds);
	if (!Get(payload, offset, hasCompletion) || hasCompletion > 1) return std::nullopt;
	response.SetHasCompletion(hasCompletion != 0);
	if (!GetWide(payload, offset, response.Completion().readId)) return std::nullopt;
	if (!Get(payload, offset, completionStatus) || !IsCompletionStatus(completionStatus)) return std::nullopt;
	response.Completion().status = static_cast<senp::effect::CompletionStatus>(completionStatus);
	if (!GetWideBounded(payload, offset, response.Completion().data, kControlSenpRpcMaximumToolDataBytes)) {
		return std::nullopt;
	}
	if (!GetWide(payload, offset, response.Completion().message)) return std::nullopt;
	std::wstring resourceHandle;
	if (!GetWide(payload, offset, resourceHandle)) return std::nullopt;
	response.SetResourceHandle(std::move(resourceHandle));
	std::uint64_t resourceOffset = 0;
	if (!Get(payload, offset, resourceOffset)) return std::nullopt;
	response.SetResourceOffset(resourceOffset);
	std::string resourceBytes;
	if (!GetBytes(payload, offset, resourceBytes, kControlSenpRpcMaximumResourceChunkBytes)) return std::nullopt;
	response.SetResourceBytes(std::move(resourceBytes));
	std::uint8_t resourceState = 0, resourceEnd = 0;
	if (!Get(payload, offset, resourceState) || !IsResourceState(resourceState)) {
		return std::nullopt;
	}
	response.SetResourceState(resourceState);
	if (!Get(payload, offset, resourceEnd) || !IsResourceEnd(resourceEnd)) {
		return std::nullopt;
	}
	response.SetResourceEnd(resourceEnd);
	std::uint64_t resourceLength = 0;
	if (!Get(payload, offset, resourceLength)) return std::nullopt;
	response.SetResourceLength(resourceLength);
	// Encoded from a signed member, so a value past the signed maximum decodes
	// back as a different revision than the one that was sent.
	if (!Get(payload, offset, resourceRevision) || resourceRevision > kMaximumGeneration) return std::nullopt;
	response.SetResourceRevision(static_cast<std::int64_t>(resourceRevision));
	if (!Get(payload, offset, accountGeneration) || accountGeneration > kMaximumGeneration) return std::nullopt;
	if (!Get(payload, offset, accountState) || !IsAccountState(accountState)) return std::nullopt;
	response.SetAccountGeneration(static_cast<std::int64_t>(accountGeneration));
	response.SetAccountState(static_cast<EControlSenpAccountState>(accountState));
	if (offset != payload.size()) return std::nullopt;
	if (!IsCoherentResponse(response)) return std::nullopt;
	return response;
}

} // namespace platform::controlipc
