/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/controlipc/ControlProfileRpc.h"
#include "platform/controlipc/ControlPlatformClient.h"

#include "platform/profiles/UserDataProfileRegistryCodec.h"

#include <Windows.h>

#include <limits>
#include <type_traits>
#include <utility>

namespace platform::controlipc {
namespace {
constexpr std::size_t kMaximumPayload = kControlIpcMaximumFrameBytes - kControlIpcHeaderBytes;
constexpr std::uint8_t kControlProfileRpcPayloadVersion = 1;
constexpr std::uint8_t kHasExpectedRevision = 1 << 0;
constexpr std::uint8_t kHasWorkspace = 1 << 1;
constexpr std::uint8_t kHasEmptyWindow = 1 << 2;
constexpr std::uint8_t kHasExplicitProfile = 1 << 3;

template<class T> void Put(std::vector<std::uint8_t>& bytes, T value)
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
template<class T> bool Get(std::span<const std::uint8_t> bytes, std::size_t& offset, T& value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
	value = 0;
	for (std::size_t index = 0; index < sizeof(T); ++index) value |= static_cast<T>(bytes[offset + index]) << (index * 8);
	offset += sizeof(T); return true;
}
bool ToUtf8(std::wstring_view source, std::string& target)
{
	if (source.empty()) { target.clear(); return true; }
	if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
	if (bytes <= 0) return false;
	target.resize(static_cast<std::size_t>(bytes));
	return ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), target.data(), bytes, nullptr, nullptr) == bytes;
}
bool FromUtf8(std::string_view source, std::wstring& target)
{
	if (source.empty()) { target.clear(); return true; }
	if (source.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
	const auto characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), nullptr, 0);
	if (characters <= 0) return false;
	target.resize(static_cast<std::size_t>(characters));
	return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(), static_cast<int>(source.size()), target.data(), characters) == characters;
}
bool PutUtf8(std::vector<std::uint8_t>& bytes, std::string_view value)
{
	if (value.size() > kControlIpcMaximumUtf8FieldBytes || value.find('\0') != std::string_view::npos || bytes.size() > kMaximumPayload - sizeof(std::uint32_t) - value.size()) return false;
	ControlIpcFields fields; if (!AddUtf8Field(fields, EControlIpcFieldTag::Diagnostic, value)) return false;
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end()); return true;
}
bool GetUtf8(std::span<const std::uint8_t> bytes, std::size_t& offset, std::string& value)
{
	std::uint32_t length = 0;
	if (!Get(bytes, offset, length) || length > kControlIpcMaximumUtf8FieldBytes || length > bytes.size() - offset) return false;
	ControlIpcFields fields; if (!AddUtf8Field(fields, EControlIpcFieldTag::Diagnostic, std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), length))) return false;
	value.assign(reinterpret_cast<const char*>(bytes.data() + offset), length); offset += length; return true;
}
bool PutWide(std::vector<std::uint8_t>& bytes, std::wstring_view value) { std::string utf8; return ToUtf8(value, utf8) && PutUtf8(bytes, utf8); }
bool GetWide(std::span<const std::uint8_t> bytes, std::size_t& offset, std::wstring& value) { std::string utf8; return GetUtf8(bytes, offset, utf8) && FromUtf8(utf8, value); }
bool IsOperation(std::uint8_t operation) noexcept { return operation >= static_cast<std::uint8_t>(EControlProfileRpcOperation::Snapshot) && operation <= static_cast<std::uint8_t>(EControlProfileRpcOperation::Export); }
bool IsKind(std::uint8_t kind) noexcept { return kind <= static_cast<std::uint8_t>(profiles::UserDataProfileKind::Transient); }
bool IsRegistryStatus(std::uint8_t status) noexcept { return status <= static_cast<std::uint8_t>(profiles::ControlUserDataProfileRegistryStatus::ProfileNotFound); }
bool IsTerminal(std::uint16_t status) noexcept { return status <= static_cast<std::uint16_t>(EControlIpcTerminalStatus::ProtocolError); }

ControlProfileRpcResponse Failure(EControlIpcTerminalStatus terminal) noexcept { return { terminal, {} , {} }; }
ControlIpcFrame Frame(const ControlIpcFrame& request, EControlIpcKind kind, std::vector<std::uint8_t> payload) noexcept
{
	return { { kControlIpcMajorVersion, kControlIpcMinorVersion, kind, EControlIpcFlags::Response | EControlIpcFlags::Terminal,
		request.header.requestId, request.header.generation }, std::move(payload) };
}
ControlProfileRpcResponse Invoke(profiles::ControlUserDataProfileRegistry& registry, const ControlProfileRpcRequest& request)
{
	using Status = profiles::ControlUserDataProfileRegistryStatus;
	profiles::ControlUserDataProfileRegistryResult result;
	switch (request.operation) {
	case EControlProfileRpcOperation::Snapshot:
	case EControlProfileRpcOperation::List: result = registry.Snapshot(); break;
	case EControlProfileRpcOperation::Current: result = registry.Resolve({}); break;
	case EControlProfileRpcOperation::Resolve: {
		profiles::UserDataProfileResolveRequest resolve;
		if (!request.profileId.empty()) resolve.explicitProfileId = request.profileId;
		resolve.workspaceUri = request.workspaceUri; resolve.emptyWindowId = request.emptyWindowId;
		result = registry.Resolve(resolve); break;
	}
	case EControlProfileRpcOperation::CreateNamed: result = registry.CreateNamed(request.create, request.mutation); break;
	case EControlProfileRpcOperation::CreateTransient: result = registry.CreateTransient(request.create, request.mutation); break;
	case EControlProfileRpcOperation::Rename: result = registry.Rename(request.profileId, request.displayName, request.mutation); break;
	case EControlProfileRpcOperation::Delete: result = registry.Delete(request.profileId, request.mutation); break;
	case EControlProfileRpcOperation::AssociateWorkspace: result = request.workspaceUri ? registry.AssociateWorkspace(request.profileId, *request.workspaceUri, request.mutation) : profiles::ControlUserDataProfileRegistryResult{ Status::OperationRejected }; break;
	case EControlProfileRpcOperation::AssociateEmptyWindow: result = request.emptyWindowId ? registry.AssociateEmptyWindow(request.profileId, *request.emptyWindowId, request.mutation) : profiles::ControlUserDataProfileRegistryResult{ Status::OperationRejected }; break;
	case EControlProfileRpcOperation::Import: result = registry.ImportPortableDocument(request.document, request.mutation); break;
	case EControlProfileRpcOperation::Export:
		result.status = Status::Resolved; result.storageRevision = registry.StorageRevision();
		return { EControlIpcTerminalStatus::Succeeded, std::move(result), registry.ExportPortableDocument() };
	default:
		return Failure(EControlIpcTerminalStatus::InvalidRequest);
	}
	return { result.Succeeded() ? EControlIpcTerminalStatus::Succeeded : EControlIpcTerminalStatus::InvalidRequest, std::move(result), {} };
}
} // namespace

std::optional<std::vector<std::uint8_t>> EncodeControlProfileRpcRequest(const ControlProfileRpcRequest& request)
{
	if (!IsOperation(static_cast<std::uint8_t>(request.operation))) return std::nullopt;
	std::vector<std::uint8_t> bytes;
	std::uint8_t flags = request.mutation.expectedStorageRevision ? kHasExpectedRevision : 0;
	if (request.workspaceUri) flags |= kHasWorkspace;
	if (request.emptyWindowId) flags |= kHasEmptyWindow;
	if (!request.profileId.empty()) flags |= kHasExplicitProfile;
	Put<std::uint8_t>(bytes, kControlProfileRpcPayloadVersion); Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(request.operation)); Put<std::uint8_t>(bytes, flags);
	if (flags & kHasExpectedRevision) Put<std::uint64_t>(bytes, *request.mutation.expectedStorageRevision);
	if (!PutUtf8(bytes, request.mutation.operationId) || !PutWide(bytes, request.profileId) || !PutWide(bytes, request.displayName)
		|| !PutWide(bytes, request.create.profileId) || !PutWide(bytes, request.create.displayName)
		|| !IsKind(static_cast<std::uint8_t>(request.create.kind))) return std::nullopt;
	Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(request.create.kind));
	std::uint8_t inheritance = (request.create.resourceInheritance.settings ? 1 : 0) | (request.create.resourceInheritance.keybindings ? 2 : 0)
		| (request.create.resourceInheritance.tasks ? 4 : 0) | (request.create.resourceInheritance.snippets ? 8 : 0)
		| (request.create.resourceInheritance.extensions ? 16 : 0) | (request.create.resourceInheritance.globalState ? 32 : 0);
	Put<std::uint8_t>(bytes, inheritance);
	if (request.create.legacyAliases.size() > 256) return std::nullopt;
	Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(request.create.legacyAliases.size()));
	for (const auto& alias : request.create.legacyAliases) if (!PutWide(bytes, alias)) return std::nullopt;
	if ((request.workspaceUri && !PutWide(bytes, request.workspaceUri->ToString())) || (request.emptyWindowId && !PutWide(bytes, *request.emptyWindowId)) || !PutUtf8(bytes, request.document)) return std::nullopt;
	return bytes;
}

std::optional<ControlProfileRpcRequest> DecodeControlProfileRpcRequest(std::span<const std::uint8_t> bytes)
{
	ControlProfileRpcRequest request; std::size_t offset = 0; std::uint8_t version = 0, operation = 0, flags = 0, kind = 0, inheritance = 0; std::uint32_t aliases = 0;
	if (!Get(bytes, offset, version) || version != kControlProfileRpcPayloadVersion || !Get(bytes, offset, operation) || !Get(bytes, offset, flags) || !IsOperation(operation) || (flags & ~(kHasExpectedRevision | kHasWorkspace | kHasEmptyWindow | kHasExplicitProfile))) return std::nullopt;
	request.operation = static_cast<EControlProfileRpcOperation>(operation);
	if (flags & kHasExpectedRevision) {
		std::uint64_t expectedRevision = 0;
		if (!Get(bytes, offset, expectedRevision)) return std::nullopt;
		request.mutation.expectedStorageRevision = expectedRevision;
	}
	if (!GetUtf8(bytes, offset, request.mutation.operationId) || !GetWide(bytes, offset, request.profileId) || !GetWide(bytes, offset, request.displayName)
		|| !GetWide(bytes, offset, request.create.profileId) || !GetWide(bytes, offset, request.create.displayName) || !Get(bytes, offset, kind) || !IsKind(kind) || !Get(bytes, offset, inheritance) || inheritance > 63 || !Get(bytes, offset, aliases) || aliases > 256) return std::nullopt;
	request.create.kind = static_cast<profiles::UserDataProfileKind>(kind);
	request.create.resourceInheritance = { (inheritance & 1) != 0, (inheritance & 2) != 0, (inheritance & 4) != 0, (inheritance & 8) != 0, (inheritance & 16) != 0, (inheritance & 32) != 0 };
	for (std::uint32_t index = 0; index < aliases; ++index) { std::wstring alias; if (!GetWide(bytes, offset, alias)) return std::nullopt; request.create.legacyAliases.push_back(std::move(alias)); }
	if (flags & kHasWorkspace) { std::wstring text; auto uri = GetWide(bytes, offset, text) ? profiles::WorkspaceUri::Parse(text) : ::platform::uri::UriParseResult{}; if (!uri) return std::nullopt; request.workspaceUri = std::move(*uri.value); }
	if (flags & kHasEmptyWindow) { profiles::EmptyWindowId id; if (!GetWide(bytes, offset, id)) return std::nullopt; request.emptyWindowId = std::move(id); }
	if (!GetUtf8(bytes, offset, request.document) || offset != bytes.size()) return std::nullopt;
	return request;
}

std::optional<std::vector<std::uint8_t>> EncodeControlProfileRpcResponse(const ControlProfileRpcResponse& response)
{
	if (!IsTerminal(static_cast<std::uint16_t>(response.terminalStatus)) || !IsRegistryStatus(static_cast<std::uint8_t>(response.result.status))) return std::nullopt;
	std::vector<std::uint8_t> bytes; Put<std::uint16_t>(bytes, static_cast<std::uint16_t>(response.terminalStatus)); Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(response.result.status)); Put<std::uint64_t>(bytes, response.result.storageRevision);
	const auto profile = response.result.resolved && response.result.resolved->profile ? response.result.resolved->profile : std::optional<profiles::UserDataProfileDescriptor>{};
	Put<std::uint8_t>(bytes, profile ? 1 : 0);
	if (profile && (!PutWide(bytes, profile->profileId) || !PutWide(bytes, profile->displayName) || !IsKind(static_cast<std::uint8_t>(profile->kind)) || profile->legacyAliases.size() > 256)) return std::nullopt;
	if (profile) {
		Put<std::uint8_t>(bytes, static_cast<std::uint8_t>(profile->kind));
		const auto inheritance = static_cast<std::uint8_t>((profile->resourceInheritance.settings ? 1 : 0)
			| (profile->resourceInheritance.keybindings ? 2 : 0) | (profile->resourceInheritance.tasks ? 4 : 0)
			| (profile->resourceInheritance.snippets ? 8 : 0) | (profile->resourceInheritance.extensions ? 16 : 0)
			| (profile->resourceInheritance.globalState ? 32 : 0));
		Put<std::uint8_t>(bytes, inheritance); Put<std::uint32_t>(bytes, static_cast<std::uint32_t>(profile->legacyAliases.size()));
		for (const auto& alias : profile->legacyAliases) if (!PutWide(bytes, alias)) return std::nullopt;
	}
	if (!PutUtf8(bytes, response.snapshotDocument)) return std::nullopt;
	return bytes;
}
std::optional<ControlProfileRpcResponse> DecodeControlProfileRpcResponse(std::span<const std::uint8_t> bytes)
{
	ControlProfileRpcResponse response; std::size_t offset = 0; std::uint16_t terminal = 0; std::uint8_t status = 0, hasProfile = 0;
	if (!Get(bytes, offset, terminal) || !Get(bytes, offset, status) || !IsTerminal(terminal) || !IsRegistryStatus(status) || !Get(bytes, offset, response.result.storageRevision) || !Get(bytes, offset, hasProfile) || hasProfile > 1) return std::nullopt;
	if (hasProfile) {
		profiles::UserDataProfileDescriptor profile; std::uint8_t kind = 0, inheritance = 0; std::uint32_t aliases = 0;
		if (!GetWide(bytes, offset, profile.profileId) || !GetWide(bytes, offset, profile.displayName) || !Get(bytes, offset, kind) || !IsKind(kind) || !Get(bytes, offset, inheritance) || inheritance > 63 || !Get(bytes, offset, aliases) || aliases > 256) return std::nullopt;
		profile.kind = static_cast<profiles::UserDataProfileKind>(kind);
		profile.resourceInheritance = { (inheritance & 1) != 0, (inheritance & 2) != 0, (inheritance & 4) != 0, (inheritance & 8) != 0, (inheritance & 16) != 0, (inheritance & 32) != 0 };
		for (std::uint32_t index = 0; index < aliases; ++index) { std::wstring alias; if (!GetWide(bytes, offset, alias)) return std::nullopt; profile.legacyAliases.push_back(std::move(alias)); }
		response.result.resolved = { profiles::UserDataProfileResolveStatus::Resolved, 0, profiles::UserDataProfileResolveSource::ExplicitProfile, std::move(profile) };
	}
	if (!GetUtf8(bytes, offset, response.snapshotDocument) || offset != bytes.size()) return std::nullopt;
	response.terminalStatus = static_cast<EControlIpcTerminalStatus>(terminal); response.result.status = static_cast<profiles::ControlUserDataProfileRegistryStatus>(status); return response;
}

CControlProfileRpcSession::CControlProfileRpcSession(ControlStorageRpcSessionIdentity identity, std::shared_ptr<profiles::ControlUserDataProfileRegistry> registry) noexcept : m_identity(std::move(identity)), m_registry(std::move(registry)) {}
ControlIpcFrame CControlProfileRpcSession::ErrorFor(const ControlIpcFrame& request, EControlIpcTerminalStatus status) const noexcept
{
	auto response = Frame(request, EControlIpcKind::Error, EncodeControlIpcError({ status, {} }).value_or(std::vector<std::uint8_t>{}));
	response.header.generation = m_identity.generation;
	return response;
}
ControlIpcFrame CControlProfileRpcSession::Process(const ControlIpcFrame& request) noexcept
{
	try {
		if (!m_registry || request.header.kind != EControlIpcKind::ProfileRequest || request.header.generation != m_identity.generation) return ErrorFor(request, request.header.generation == m_identity.generation ? EControlIpcTerminalStatus::InvalidRequest : EControlIpcTerminalStatus::GenerationMismatch);
		auto fields = DecodeControlIpcFields(request.payload); if (fields.outcome != EControlIpcFieldDecodeOutcome::Decoded || fields.fields.size() != 1 || fields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload)) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
		auto decoded = DecodeControlProfileRpcRequest(fields.fields.front().value); if (!decoded) return ErrorFor(request, EControlIpcTerminalStatus::InvalidRequest);
		auto response = Invoke(*m_registry, *decoded);
		if (response.result.snapshot) response.snapshotDocument = profiles::EncodeUserDataProfileRegistryDocument(*response.result.snapshot);
		auto payload = EncodeControlProfileRpcResponse(response); if (!payload) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
		ControlIpcFields responseFields{ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload), std::move(*payload) } }; auto outer = EncodeControlIpcFields(responseFields); if (!outer) return ErrorFor(request, EControlIpcTerminalStatus::InternalError);
		return Frame(request, EControlIpcKind::ProfileResponse, std::move(*outer));
	} catch (...) { return ErrorFor(request, EControlIpcTerminalStatus::InternalError); }
}

CControlProfileRpcClient::CControlProfileRpcClient(IControlPlatformClientChannel& channel, std::uint64_t generation, std::chrono::milliseconds deadline) noexcept : m_channel(channel), m_generation(generation), m_deadline(deadline) {}
ControlProfileRpcResponse CControlProfileRpcClient::Execute(const ControlProfileRpcRequest& command, bool cancelled) noexcept
{
	try {
		if (cancelled) return Failure(EControlIpcTerminalStatus::Cancelled);
		if (m_generation == 0 || m_deadline <= std::chrono::milliseconds::zero() || m_nextRequestId == 0) return Failure(EControlIpcTerminalStatus::InvalidRequest);
		auto encoded = EncodeControlProfileRpcRequest(command); if (!encoded) return Failure(EControlIpcTerminalStatus::InvalidRequest);
		auto fields = EncodeControlIpcFields({ { static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload), std::move(*encoded) } }); if (!fields) return Failure(EControlIpcTerminalStatus::InvalidRequest);
		ControlIpcFrame request{ { kControlIpcMajorVersion, kControlIpcMinorVersion, EControlIpcKind::ProfileRequest, EControlIpcFlags::Request, m_nextRequestId++, m_generation }, std::move(*fields) };
		std::vector<ControlIpcFrame> responses; auto exchanged = m_channel.Exchange(request, responses, m_deadline);
		if (!exchanged.success) return Failure(exchanged.reason == EControlIpcTransportDisconnectReason::DeadlineExceeded ? EControlIpcTerminalStatus::DeadlineExceeded : EControlIpcTerminalStatus::InternalError);
		if (responses.size() != 1 || responses.front().header.requestId != request.header.requestId || responses.front().header.flags != (EControlIpcFlags::Response | EControlIpcFlags::Terminal)) return Failure(EControlIpcTerminalStatus::ProtocolError);
		if (responses.front().header.kind == EControlIpcKind::Error) { auto error = DecodeControlIpcError(responses.front().payload); return Failure(error ? error->status : EControlIpcTerminalStatus::ProtocolError); }
		if (responses.front().header.kind != EControlIpcKind::ProfileResponse || responses.front().header.generation != m_generation) return Failure(EControlIpcTerminalStatus::GenerationMismatch);
		auto responseFields = DecodeControlIpcFields(responses.front().payload); if (responseFields.outcome != EControlIpcFieldDecodeOutcome::Decoded || responseFields.fields.size() != 1 || responseFields.fields.front().tag != static_cast<std::uint16_t>(EControlIpcFieldTag::ProfilePayload)) return Failure(EControlIpcTerminalStatus::ProtocolError);
		return DecodeControlProfileRpcResponse(responseFields.fields.front().value).value_or(Failure(EControlIpcTerminalStatus::ProtocolError));
	} catch (...) { return Failure(EControlIpcTerminalStatus::InternalError); }
}
} // namespace platform::controlipc
