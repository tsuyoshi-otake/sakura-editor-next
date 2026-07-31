/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "platform/controlipc/ControlIpcProtocol.h"
#include "platform/storage/IStorageService.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace platform::controlipc {

//! Conservative payload limits for the synchronous P0 storage RPC slice.
inline constexpr std::size_t kControlStorageRpcMaximumItems = storage::kMaximumStorageItems;
inline constexpr std::size_t kControlStorageRpcMaximumStringBytes = storage::kMaximumStorageStringBytes;

/*!
	@brief Encodes/decodes the P0 payloads carried by ControlIpcFrame.

	The outer payload is the versioned ControlIpc TLV format. `StorageSnapshot`
	and `StorageMutation` values use private little-endian records: fixed numeric
	primitives followed by u32-byte-counted UTF-8 strings. Counts, string sizes,
	and the complete payload are validated before any attacker-controlled vector
	or string allocation. The format deliberately has no platform or transport
	dependencies.
*/
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlStorageHello(std::string_view profileId);
[[nodiscard]] std::optional<std::string> DecodeControlStorageHello(std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlStorageSnapshotResponse(
	const storage::StorageSnapshot& snapshot);
[[nodiscard]] std::optional<storage::StorageSnapshot> DecodeControlStorageSnapshotResponse(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlStorageApplyRequest(
	const storage::StorageMutationRequest& request);
[[nodiscard]] std::optional<storage::StorageMutationRequest> DecodeControlStorageApplyRequest(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlStorageApplyResponse(
	const storage::StorageMutationResult& result);
[[nodiscard]] std::optional<storage::StorageMutationResult> DecodeControlStorageApplyResponse(
	std::span<const std::uint8_t> payload);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlStorageCancelRequest(
	std::uint64_t requestId);
[[nodiscard]] std::optional<std::uint64_t> DecodeControlStorageCancelRequest(
	std::span<const std::uint8_t> payload);

struct ControlStorageRpcSessionIdentity {
	//! The endpoint layer authenticates the canonical profile path/hash; this
	//! payload layer binds the already-canonical, nonempty profile ID only.
	std::string profileId;
	std::uint64_t generation = 0;
};

/*!
	@brief Per-connection synchronous state machine for the P0 storage RPCs.

	Process() always returns one terminal response for every input frame. It owns
	no pending asynchronous operation: therefore CancelRequest is deterministically
	rejected as InvalidRequest rather than pretending that an already-completed
	storage call can be cancelled.
*/
class CControlStorageRpcSession final {
public:
	CControlStorageRpcSession(ControlStorageRpcSessionIdentity identity, storage::IStorageService& storage) noexcept;

	//! Never throws; errors, validation failures, and service exceptions become Error frames.
	[[nodiscard]] ControlIpcFrame Process(const ControlIpcFrame& request) noexcept;
	[[nodiscard]] bool IsHandshaken() const noexcept { return m_handshaken; }
	[[nodiscard]] const ControlStorageRpcSessionIdentity& GetIdentity() const noexcept { return m_identity; }

private:
	[[nodiscard]] ControlIpcFrame ErrorFor(const ControlIpcFrame& request,
		EControlIpcTerminalStatus status, std::string_view diagnostic) const noexcept;
	[[nodiscard]] ControlIpcFrame HandleHello(const ControlIpcFrame& request);
	[[nodiscard]] ControlIpcFrame HandleSnapshot(const ControlIpcFrame& request);
	[[nodiscard]] ControlIpcFrame HandleApply(const ControlIpcFrame& request);
	[[nodiscard]] bool HasValidIdentity() const noexcept;
	[[nodiscard]] std::uint64_t ResponseGeneration() const noexcept;

	ControlStorageRpcSessionIdentity m_identity;
	storage::IStorageService& m_storage;
	bool m_handshaken = false;
};

} // namespace platform::controlipc
