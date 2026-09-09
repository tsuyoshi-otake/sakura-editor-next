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
struct ControlSenpRpcOwner {
	std::wstring extensionId;
	std::wstring packageDigest;
	std::int64_t generation = 0;
	std::int64_t workspaceRevision = 0;
	std::int64_t accountGeneration = 0;
	bool operator==(const ControlSenpRpcOwner&) const = default;
};

/*!
	@brief The workspace one editor connection declares it is opened on.

	It names folders and the revision they were observed at, and deliberately no
	repository: which repository those folders resolve to is decided by the
	control side from the git remotes it finds there, never by this declaration.
	A declaration with no folders is a window that can select nothing, which is a
	meaningful thing to say rather than an absent message.
*/
struct ControlSenpRpcWorkspace {
	std::int64_t generation = 0;
	std::int64_t revision = 0;
	//! Folder identities as the workspace context holds them, never a claim
	//! about their contents.
	std::vector<std::wstring> folders;
	bool operator==(const ControlSenpRpcWorkspace&) const = default;
	//! True while nothing has been declared. Every operation but AdoptWorkspace
	//! must leave it that way.
	[[nodiscard]] bool Empty() const noexcept
	{
		return generation == 0 && revision == 0 && folders.empty();
	}
};

//! Editor command model. Unused members for an operation must stay empty/zero;
//! the decoder rejects a payload whose operation does not match its members.
struct ControlSenpRpcRequest {
	EControlSenpRpcOperation operation = EControlSenpRpcOperation::IssueGrant;
	std::wstring profileId;
	ControlSenpRpcOwner owner;
	std::string grantId;
	std::uint32_t capabilities = 0;
	std::wstring readId;
	std::wstring toolId;
	std::wstring toolOperation;
	std::vector<senp::effect::Field> arguments;
	std::wstring resourceHandle;
	std::uint64_t offset = 0;
	std::uint32_t length = 0;
	//! Declared by AdoptWorkspace only, and empty on every other operation.
	ControlSenpRpcWorkspace workspace;
};

//! Terminal broker response. `completion` is present only when `hasCompletion`
//! is set; a poll with no drained terminal is a successful empty answer.
struct ControlSenpRpcResponse {
	EControlSenpRpcStatus status = EControlSenpRpcStatus::InvalidRequest;
	std::string grantId;
	std::uint64_t expiresAtMilliseconds = 0;
	bool hasCompletion = false;
	senp::effect::ToolCompleted completion;
	//! One text-resource chunk, whole. The editor rebuilds the chunk its text
	//! surface validates from exactly these members, so every member that
	//! validation reads has to travel: a length or an end the editor filled in
	//! itself would be the editor asserting something about a resource the
	//! control side owns.
	std::wstring resourceHandle;
	std::uint64_t resourceOffset = 0;
	std::string resourceBytes;
	//! senp::TextResourceState and senp::TextResourceEnd as raw discriminators.
	//! This header describes the wire rather than the store, so it names their
	//! values without depending on the type that defines them.
	std::uint8_t resourceState = 0;
	std::uint8_t resourceEnd = 0;
	//! Bytes the whole resource holds, and the revision the store answers for.
	//! Whether a chunk is the last one is derived from these rather than sent:
	//! a sent answer could contradict the members it is derived from, and the
	//! editor would have no way to tell which of the two to believe.
	std::uint64_t resourceLength = 0;
	std::int64_t resourceRevision = 0;
	//! Answered by QueryAccount only. Zero means the profile has adopted no
	//! account at all, which `accountState` separates from a signed-out one.
	std::int64_t accountGeneration = 0;
	EControlSenpAccountState accountState = EControlSenpAccountState::Unknown;
};

//! Bounds applied by both encoder and decoder. They are smaller than the frame
//! limit so an aggregate record cannot be used to reach the transport bound.
inline constexpr std::size_t kControlSenpRpcMaximumArguments = 32;
//! A multi-root workspace declares at most this many folders. The control side
//! inspects every one of them, so this bound is what keeps one declaration from
//! turning into an unbounded amount of work on the refresh worker.
inline constexpr std::size_t kControlSenpRpcMaximumWorkspaceFolders = 8;
inline constexpr std::size_t kControlSenpRpcMaximumToolDataBytes = 64 * 1024;
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
