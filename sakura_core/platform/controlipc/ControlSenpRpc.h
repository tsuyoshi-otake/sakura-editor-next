/*! @file @brief Versioned editor-to-control SENP tool broker RPC. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "senp/SenpEffectProtocol.h"
#include "senp/SenpToolGrants.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace platform::controlipc {

//! The one-byte operation discriminator is part of the versioned SenpRequest payload.
//! Every operation that names a contribution carries the requesting owner so the
//! control side can recheck its own trusted authority instead of trusting the
//! editor's claim. QueryAccount and AdoptWorkspace name none, because they are
//! what the editor asks and declares before an owner carrying an account
//! generation and a workspace revision can exist at all.
enum class EControlSenpRpcOperation : std::uint8_t {
	IssueGrant = 1,
	StartRead = 2,
	PollRead = 3,
	CancelRead = 4,
	ReadResource = 5,
	ReleaseResource = 6,
	QueryAccount = 7,
	AdoptWorkspace = 8,
};

//! Transport-neutral view of the account a profile has adopted. It is deliberately
//! not senp::github::GhConnectionState: the wire contract must not depend on which
//! tool happens to serve a capability. Unknown and Disconnected are distinct
//! answers, so a zero account generation is never read as a signed-out account.
enum class EControlSenpAccountState : std::uint8_t {
	Unknown = 0,
	Checking = 1,
	Disconnected = 2,
	Connected = 3,
	ReauthenticationRequired = 4,
	Unavailable = 5,
};

//! Broker outcome for one SENP operation. It is intentionally distinct from
//! EControlIpcTerminalStatus: a rejected read is a normal terminal answer on a
//! healthy connection, not a transport failure.
enum class EControlSenpRpcStatus : std::uint8_t {
	Succeeded = 0,
	InvalidRequest = 1,
	Unauthorized = 2,
	ResourceExhausted = 3,
	Unavailable = 4,
	Closed = 5,
	Expired = 6,
	NotFound = 7,
	Busy = 8,
	NotConnected = 9,
};

//! Editor-declared owner reference. It selects a record; it never authorizes one.
//! State is private and reached only through the constructor/accessors below so a
//! decoder that fills it field-by-field cannot leave a caller holding a reference
//! to a partially-built value: every read goes through the same named accessor a
//! writer used.
class ControlSenpRpcOwner {
public:
	ControlSenpRpcOwner() = default;
	ControlSenpRpcOwner(std::wstring extensionId, std::wstring packageDigest, std::int64_t generation,
		std::int64_t workspaceRevision, std::int64_t accountGeneration) :
		m_extensionId(std::move(extensionId)), m_packageDigest(std::move(packageDigest)),
		m_generation(generation), m_workspaceRevision(workspaceRevision),
		m_accountGeneration(accountGeneration)
	{
	}

	[[nodiscard]] const std::wstring& ExtensionId() const noexcept { return m_extensionId; }
	[[nodiscard]] const std::wstring& PackageDigest() const noexcept { return m_packageDigest; }
	[[nodiscard]] std::int64_t Generation() const noexcept { return m_generation; }
	[[nodiscard]] std::int64_t WorkspaceRevision() const noexcept { return m_workspaceRevision; }
	[[nodiscard]] std::int64_t AccountGeneration() const noexcept { return m_accountGeneration; }

	void SetExtensionId(std::wstring value) { m_extensionId = std::move(value); }
	void SetPackageDigest(std::wstring value) { m_packageDigest = std::move(value); }
	void SetGeneration(std::int64_t value) noexcept { m_generation = value; }
	void SetWorkspaceRevision(std::int64_t value) noexcept { m_workspaceRevision = value; }
	void SetAccountGeneration(std::int64_t value) noexcept { m_accountGeneration = value; }

private:
	std::wstring m_extensionId;
	std::wstring m_packageDigest;
	std::int64_t m_generation = 0;
	std::int64_t m_workspaceRevision = 0;
	std::int64_t m_accountGeneration = 0;
};

//! Free function rather than a member: it reads every field through the same
//! public accessors any other caller uses, so the comparison stays exactly
//! member-wise without a member declaration of its own.
[[nodiscard]] inline bool operator==(const ControlSenpRpcOwner& lhs, const ControlSenpRpcOwner& rhs) noexcept
{
	return lhs.ExtensionId() == rhs.ExtensionId() && lhs.PackageDigest() == rhs.PackageDigest()
		&& lhs.Generation() == rhs.Generation() && lhs.WorkspaceRevision() == rhs.WorkspaceRevision()
		&& lhs.AccountGeneration() == rhs.AccountGeneration();
}

/*!
	@brief The workspace one editor connection declares it is opened on.

	It names folders and the revision they were observed at, and deliberately no
	repository: which repository those folders resolve to is decided by the
	control side from the git remotes it finds there, never by this declaration.
	A declaration with no folders is a window that can select nothing, which is a
	meaningful thing to say rather than an absent message.
*/
class ControlSenpRpcWorkspace {
public:
	ControlSenpRpcWorkspace() = default;
	ControlSenpRpcWorkspace(std::int64_t generation, std::int64_t revision, std::vector<std::wstring> folders) :
		m_generation(generation), m_revision(revision), m_folders(std::move(folders))
	{
	}

	[[nodiscard]] std::int64_t Generation() const noexcept { return m_generation; }
	[[nodiscard]] std::int64_t Revision() const noexcept { return m_revision; }
	//! Folder identities as the workspace context holds them, never a claim
	//! about their contents.
	[[nodiscard]] const std::vector<std::wstring>& Folders() const noexcept { return m_folders; }

	void SetGeneration(std::int64_t value) noexcept { m_generation = value; }
	void SetRevision(std::int64_t value) noexcept { m_revision = value; }
	void SetFolders(std::vector<std::wstring> value) { m_folders = std::move(value); }
	void ReserveFolders(std::size_t count) { m_folders.reserve(count); }
	void AddFolder(std::wstring folder) { m_folders.push_back(std::move(folder)); }

	//! True while nothing has been declared. Every operation but AdoptWorkspace
	//! must leave it that way.
	[[nodiscard]] bool Empty() const noexcept
	{
		return m_generation == 0 && m_revision == 0 && m_folders.empty();
	}

private:
	std::int64_t m_generation = 0;
	std::int64_t m_revision = 0;
	std::vector<std::wstring> m_folders;
};

//! Free function rather than a member: it reads every field through the same
//! public accessors any other caller uses, so the comparison stays exactly
//! member-wise without a member declaration of its own.
[[nodiscard]] inline bool operator==(const ControlSenpRpcWorkspace& lhs, const ControlSenpRpcWorkspace& rhs) noexcept
{
	return lhs.Generation() == rhs.Generation() && lhs.Revision() == rhs.Revision()
		&& lhs.Folders() == rhs.Folders();
}

//! Editor command model. Unused members for an operation must stay empty/zero;
//! the decoder rejects a payload whose operation does not match its members.
//! The wire codec fills this field-by-field, so every member keeps both a const
//! accessor and a setter (or a mutable accessor for the composite members);
//! there is no positional constructor covering every field, because none of the
//! call sites that build one ever have every field available at once.
class ControlSenpRpcRequest {
public:
	ControlSenpRpcRequest() = default;

	[[nodiscard]] EControlSenpRpcOperation Operation() const noexcept { return m_operation; }
	[[nodiscard]] const std::wstring& ProfileId() const noexcept { return m_profileId; }
	[[nodiscard]] const ControlSenpRpcOwner& Owner() const noexcept { return m_owner; }
	[[nodiscard]] ControlSenpRpcOwner& Owner() noexcept { return m_owner; }
	[[nodiscard]] const std::string& GrantId() const noexcept { return m_grantId; }
	[[nodiscard]] std::uint32_t Capabilities() const noexcept { return m_capabilities; }
	[[nodiscard]] const std::wstring& ReadId() const noexcept { return m_readId; }
	[[nodiscard]] const std::wstring& ToolId() const noexcept { return m_toolId; }
	[[nodiscard]] const std::wstring& ToolOperation() const noexcept { return m_toolOperation; }
	[[nodiscard]] const std::vector<senp::effect::Field>& Arguments() const noexcept { return m_arguments; }
	[[nodiscard]] std::vector<senp::effect::Field>& Arguments() noexcept { return m_arguments; }
	[[nodiscard]] const std::wstring& ResourceHandle() const noexcept { return m_resourceHandle; }
	[[nodiscard]] std::uint64_t Offset() const noexcept { return m_offset; }
	[[nodiscard]] std::uint32_t Length() const noexcept { return m_length; }
	//! Declared by AdoptWorkspace only, and empty on every other operation.
	[[nodiscard]] const ControlSenpRpcWorkspace& Workspace() const noexcept { return m_workspace; }
	[[nodiscard]] ControlSenpRpcWorkspace& Workspace() noexcept { return m_workspace; }

	void SetOperation(EControlSenpRpcOperation value) noexcept { m_operation = value; }
	void SetProfileId(std::wstring value) { m_profileId = std::move(value); }
	void SetOwner(ControlSenpRpcOwner value) { m_owner = std::move(value); }
	void SetGrantId(std::string value) { m_grantId = std::move(value); }
	void SetCapabilities(std::uint32_t value) noexcept { m_capabilities = value; }
	void SetReadId(std::wstring value) { m_readId = std::move(value); }
	void SetToolId(std::wstring value) { m_toolId = std::move(value); }
	void SetToolOperation(std::wstring value) { m_toolOperation = std::move(value); }
	void SetArguments(std::vector<senp::effect::Field> value) { m_arguments = std::move(value); }
	void SetResourceHandle(std::wstring value) { m_resourceHandle = std::move(value); }
	void SetOffset(std::uint64_t value) noexcept { m_offset = value; }
	void SetLength(std::uint32_t value) noexcept { m_length = value; }
	void SetWorkspace(ControlSenpRpcWorkspace value) { m_workspace = std::move(value); }

private:
	EControlSenpRpcOperation m_operation = EControlSenpRpcOperation::IssueGrant;
	std::wstring m_profileId;
	ControlSenpRpcOwner m_owner;
	std::string m_grantId;
	std::uint32_t m_capabilities = 0;
	std::wstring m_readId;
	std::wstring m_toolId;
	std::wstring m_toolOperation;
	std::vector<senp::effect::Field> m_arguments;
	std::wstring m_resourceHandle;
	std::uint64_t m_offset = 0;
	std::uint32_t m_length = 0;
	ControlSenpRpcWorkspace m_workspace;
};

//! Terminal broker response. `completion` is present only when `HasCompletion()`
//! is set; a poll with no drained terminal is a successful empty answer. Like the
//! request, this is filled field-by-field by the codec and by handlers that build
//! a partial answer before a status is known, so it keeps setters/mutable
//! accessors rather than a single covering constructor.
class ControlSenpRpcResponse {
public:
	ControlSenpRpcResponse() = default;

	[[nodiscard]] EControlSenpRpcStatus Status() const noexcept { return m_status; }
	[[nodiscard]] const std::string& GrantId() const noexcept { return m_grantId; }
	[[nodiscard]] std::uint64_t ExpiresAtMilliseconds() const noexcept { return m_expiresAtMilliseconds; }
	[[nodiscard]] bool HasCompletion() const noexcept { return m_hasCompletion; }
	[[nodiscard]] const senp::effect::ToolCompleted& Completion() const noexcept { return m_completion; }
	[[nodiscard]] senp::effect::ToolCompleted& Completion() noexcept { return m_completion; }
	//! One text-resource chunk, whole. The editor rebuilds the chunk its text
	//! surface validates from exactly these members, so every member that
	//! validation reads has to travel: a length or an end the editor filled in
	//! itself would be the editor asserting something about a resource the
	//! control side owns.
	[[nodiscard]] const std::wstring& ResourceHandle() const noexcept { return m_resourceHandle; }
	[[nodiscard]] std::uint64_t ResourceOffset() const noexcept { return m_resourceOffset; }
	[[nodiscard]] const std::string& ResourceBytes() const noexcept { return m_resourceBytes; }
	//! senp::TextResourceState and senp::TextResourceEnd as raw discriminators.
	//! This header describes the wire rather than the store, so it names their
	//! values without depending on the type that defines them.
	[[nodiscard]] std::uint8_t ResourceState() const noexcept { return m_resourceState; }
	[[nodiscard]] std::uint8_t ResourceEnd() const noexcept { return m_resourceEnd; }
	//! Bytes the whole resource holds, and the revision the store answers for.
	//! Whether a chunk is the last one is derived from these rather than sent:
	//! a sent answer could contradict the members it is derived from, and the
	//! editor would have no way to tell which of the two to believe.
	[[nodiscard]] std::uint64_t ResourceLength() const noexcept { return m_resourceLength; }
	[[nodiscard]] std::int64_t ResourceRevision() const noexcept { return m_resourceRevision; }
	//! Answered by QueryAccount only. Zero means the profile has adopted no
	//! account at all, which `AccountState()` separates from a signed-out one.
	[[nodiscard]] std::int64_t AccountGeneration() const noexcept { return m_accountGeneration; }
	[[nodiscard]] EControlSenpAccountState AccountState() const noexcept { return m_accountState; }

	void SetStatus(EControlSenpRpcStatus value) noexcept { m_status = value; }
	void SetGrantId(std::string value) { m_grantId = std::move(value); }
	void SetExpiresAtMilliseconds(std::uint64_t value) noexcept { m_expiresAtMilliseconds = value; }
	void SetHasCompletion(bool value) noexcept { m_hasCompletion = value; }
	void SetCompletion(senp::effect::ToolCompleted value) { m_completion = std::move(value); }
	void SetResourceHandle(std::wstring value) { m_resourceHandle = std::move(value); }
	void SetResourceOffset(std::uint64_t value) noexcept { m_resourceOffset = value; }
	void SetResourceBytes(std::string value) { m_resourceBytes = std::move(value); }
	void SetResourceState(std::uint8_t value) noexcept { m_resourceState = value; }
	void SetResourceEnd(std::uint8_t value) noexcept { m_resourceEnd = value; }
	void SetResourceLength(std::uint64_t value) noexcept { m_resourceLength = value; }
	void SetResourceRevision(std::int64_t value) noexcept { m_resourceRevision = value; }
	void SetAccountGeneration(std::int64_t value) noexcept { m_accountGeneration = value; }
	void SetAccountState(EControlSenpAccountState value) noexcept { m_accountState = value; }

private:
	EControlSenpRpcStatus m_status = EControlSenpRpcStatus::InvalidRequest;
	std::string m_grantId;
	std::uint64_t m_expiresAtMilliseconds = 0;
	bool m_hasCompletion = false;
	senp::effect::ToolCompleted m_completion;
	std::wstring m_resourceHandle;
	std::uint64_t m_resourceOffset = 0;
	std::string m_resourceBytes;
	std::uint8_t m_resourceState = 0;
	std::uint8_t m_resourceEnd = 0;
	std::uint64_t m_resourceLength = 0;
	std::int64_t m_resourceRevision = 0;
	std::int64_t m_accountGeneration = 0;
	EControlSenpAccountState m_accountState = EControlSenpAccountState::Unknown;
};

//! Bounds applied by both encoder and decoder. They are smaller than the frame
//! limit so an aggregate record cannot be used to reach the transport bound.
inline constexpr std::size_t kControlSenpRpcMaximumArguments = 32;
//! A multi-root workspace declares at most this many folders. The control side
//! inspects every one of them, so this bound is what keeps one declaration from
//! turning into an unbounded amount of work on the refresh worker.
inline constexpr std::size_t kControlSenpRpcMaximumWorkspaceFolders = 8;
//! Largest body one tool completion may carry across the control channel, in
//! UTF-8 bytes. It is deliberately larger than kControlIpcMaximumUtf8FieldBytes
//! and matches senp::effect::kMaximumToolDataBytes: a completion carries a whole
//! page of an answer, and holding that one field to the bound written for
//! identifiers and messages would refuse pages the effect protocol accepts. The
//! field is encoded against this bound by name, the way a resource chunk is, so
//! naming it here moves no other field's ceiling.
inline constexpr std::size_t kControlSenpRpcMaximumToolDataBytes = 256 * 1024;
inline constexpr std::size_t kControlSenpRpcMaximumResourceChunkBytes = 64 * 1024;
//! Largest resource one chunk may claim to belong to. It mirrors the store's own
//! per-resource ceiling and is restated here so the decoder can refuse an
//! impossible length on its own, before anything reads the chunk as text.
inline constexpr std::uint64_t kControlSenpRpcMaximumResourceBytes = 32ULL * 1024 * 1024;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcRequest(
	const ControlSenpRpcRequest& request);
[[nodiscard]] std::optional<ControlSenpRpcRequest> DecodeControlSenpRpcRequest(
	std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlSenpRpcResponse(
	const ControlSenpRpcResponse& response);
[[nodiscard]] std::optional<ControlSenpRpcResponse> DecodeControlSenpRpcResponse(
	std::span<const std::uint8_t> payload);

//! Maps a wire owner reference onto the contribution-owner identity used by the
//! control-owned grant registry. It performs no authorization by itself.
[[nodiscard]] senp::ContributionOwnerIdentity ToContributionOwner(const ControlSenpRpcOwner& owner);
[[nodiscard]] ControlSenpRpcOwner FromContributionOwner(const senp::ContributionOwnerIdentity& owner);

} // namespace platform::controlipc
