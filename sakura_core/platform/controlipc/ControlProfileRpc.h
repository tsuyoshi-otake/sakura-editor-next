/*! @file @brief Versioned editor-to-control user-data profile RPC. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <sakura/controlipc/ControlIpcTransport.h>
#include "platform/controlipc/ControlStorageRpc.h"
#include "platform/profiles/ControlUserDataProfileRegistry.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace platform::controlipc {

class IControlPlatformClientChannel;

//! The one-byte operation discriminator is part of the versioned ProfileRequest payload.
enum class EControlProfileRpcOperation : std::uint8_t {
	Snapshot = 1,
	List = 2,
	Current = 3,
	Resolve = 4,
	CreateNamed = 5,
	CreateTransient = 6,
	Rename = 7,
	Delete = 8,
	AssociateWorkspace = 9,
	AssociateEmptyWindow = 10,
	Import = 11,
	Export = 12,
};

//! Editor command model. Profile and empty-window IDs are opaque values; a workspace is a URI, never a path.
struct ControlProfileRpcRequest {
	EControlProfileRpcOperation operation = EControlProfileRpcOperation::Snapshot;
	profiles::ControlUserDataProfileRegistryMutation mutation;
	profiles::UserDataProfileCreateRequest create;
	profiles::UserDataProfileId profileId;
	std::wstring displayName;
	std::optional<profiles::WorkspaceUri> workspaceUri;
	std::optional<profiles::EmptyWindowId> emptyWindowId;
	std::string document;
};

//! Terminal command response. snapshotDocument uses the durable profile-document codec and is bounded by that codec.
struct ControlProfileRpcResponse {
	EControlIpcTerminalStatus terminalStatus = EControlIpcTerminalStatus::InternalError;
	profiles::ControlUserDataProfileRegistryResult result;
	std::string snapshotDocument;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlProfileRpcRequest(const ControlProfileRpcRequest& request);
[[nodiscard]] std::optional<ControlProfileRpcRequest> DecodeControlProfileRpcRequest(std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlProfileRpcResponse(const ControlProfileRpcResponse& response);
[[nodiscard]] std::optional<ControlProfileRpcResponse> DecodeControlProfileRpcResponse(std::span<const std::uint8_t> payload);

//! Synchronous per-connection server state. Every input gets one terminal frame; it owns no pending work.
class CControlProfileRpcSession final {
public:
	CControlProfileRpcSession(ControlStorageRpcSessionIdentity identity,
		std::shared_ptr<profiles::ControlUserDataProfileRegistry> registry) noexcept;
	[[nodiscard]] ControlIpcFrame Process(const ControlIpcFrame& request) noexcept;

private:
	[[nodiscard]] ControlIpcFrame ErrorFor(const ControlIpcFrame& request, EControlIpcTerminalStatus status) const noexcept;
	ControlStorageRpcSessionIdentity m_identity;
	std::shared_ptr<profiles::ControlUserDataProfileRegistry> m_registry;
};

//! Editor-facing command client for an authenticated, Hello-complete control channel.
//! Cancellation before dispatch and local deadline expiry are terminal client results; the
//! synchronous server intentionally has no deceptive in-flight cancellation state.
class CControlProfileRpcClient final {
public:
	explicit CControlProfileRpcClient(IControlPlatformClientChannel& channel, std::uint64_t generation,
		std::chrono::milliseconds deadline = std::chrono::seconds(5)) noexcept;
	[[nodiscard]] ControlProfileRpcResponse Execute(const ControlProfileRpcRequest& request, bool cancelled = false) noexcept;

private:
	IControlPlatformClientChannel& m_channel;
	std::uint64_t m_generation;
	std::chrono::milliseconds m_deadline;
	std::uint64_t m_nextRequestId = 1;
};

} // namespace platform::controlipc
