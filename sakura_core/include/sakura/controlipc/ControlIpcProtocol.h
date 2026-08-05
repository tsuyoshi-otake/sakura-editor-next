/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform::controlipc {

//! A control IPC frame is little-endian and begins with this 32-bit byte count.
//! The count covers the fixed header and payload, but not the count itself.
inline constexpr std::size_t kControlIpcLengthPrefixBytes = 4;
inline constexpr std::size_t kControlIpcHeaderBytes = 28;
inline constexpr std::size_t kControlIpcMaximumFrameBytes = 1024 * 1024;
inline constexpr std::size_t kControlIpcMaximumFieldCount = 1024;
inline constexpr std::size_t kControlIpcMaximumUtf8FieldBytes = 64 * 1024;
inline constexpr std::uint32_t kControlIpcMagic = 0x50494353; // "SCIP" on the wire.
inline constexpr std::uint16_t kControlIpcMajorVersion = 1;
inline constexpr std::uint16_t kControlIpcMinorVersion = 0;

enum class EControlIpcKind : std::uint16_t {
	Hello = 1,
	HelloAck = 2,
	StorageSnapshotRequest = 3,
	StorageSnapshotResponse = 4,
	StorageApplyRequest = 5,
	StorageApplyResponse = 6,
	CancelRequest = 7,
	CancelAck = 8,
	Error = 9,
	SecretGetRequest = 10,
	SecretGetResponse = 11,
	SecretApplyRequest = 12,
	SecretApplyResponse = 13,
	SecretCapabilityIssueRequest = 14,
	SecretCapabilityIssueResponse = 15,
	SecretCapabilityRevokeSessionRequest = 16,
	SecretCapabilityRevokeSessionResponse = 17,
	//! Versioned, control-owned user-data profile command envelope.
	ProfileRequest = 18,
	ProfileResponse = 19,
};

enum class EControlIpcFlags : std::uint16_t {
	None = 0,
	Request = 1 << 0,
	Response = 1 << 1,
	Terminal = 1 << 2,
};

[[nodiscard]] constexpr EControlIpcFlags operator|(EControlIpcFlags left, EControlIpcFlags right) noexcept
{
	return static_cast<EControlIpcFlags>(static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

[[nodiscard]] constexpr bool HasFlag(EControlIpcFlags value, EControlIpcFlags flag) noexcept
{
	return (static_cast<std::uint16_t>(value) & static_cast<std::uint16_t>(flag)) != 0;
}

//! Terminal statuses are part of the public protocol rather than transport errors.
enum class EControlIpcTerminalStatus : std::uint16_t {
	Succeeded = 0,
	InvalidRequest = 1,
	UnsupportedVersion = 2,
	ProfileMismatch = 3,
	GenerationMismatch = 4,
	DeadlineExceeded = 5,
	Cancelled = 6,
	ServerStopping = 7,
	AccessDenied = 8,
	ResourceExhausted = 9,
	InternalError = 10,
	ProtocolError = 11,
};

enum class EControlIpcDecodeOutcome : std::uint8_t {
	NeedMoreData,
	Decoded,
	ZeroLengthFrame,
	OversizeFrame,
	MalformedFrame,
	UnsupportedMajorVersion,
	UnknownKind,
	UnknownRequiredFlags,
	InvalidFlags,
	InvalidRequestId,
	InvalidGeneration,
};

enum class EControlIpcEncodeOutcome : std::uint8_t {
	Encoded,
	PayloadTooLarge,
	UnknownKind,
	UnknownRequiredFlags,
	InvalidFlags,
	InvalidRequestId,
	InvalidGeneration,
};

enum class EControlIpcFieldDecodeOutcome : std::uint8_t {
	Decoded,
	MalformedField,
	TooManyFields,
	FieldTooLarge,
	InvalidTag,
	InvalidUtf8,
	DuplicateField,
	MissingField,
};

/*! 
	@brief Header fields that every RPC request and response carries.

	`requestId` is nonzero for all currently defined kinds. `generation` is the
	control-process generation returned by the handshake; an initial `Hello`
	request alone uses generation zero. Later messages use it to prevent a client
	from applying a reply produced by a previous authority instance.
*/
struct ControlIpcHeader {
	std::uint16_t majorVersion = kControlIpcMajorVersion;
	std::uint16_t minorVersion = kControlIpcMinorVersion;
	EControlIpcKind kind = EControlIpcKind::Hello;
	EControlIpcFlags flags = EControlIpcFlags::Request;
	std::uint64_t requestId = 0;
	std::uint64_t generation = 0;
};

struct ControlIpcFrame {
	ControlIpcHeader header;
	std::vector<std::uint8_t> payload;
};

struct ControlIpcDecodeResult {
	EControlIpcDecodeOutcome outcome = EControlIpcDecodeOutcome::NeedMoreData;
	std::vector<ControlIpcFrame> frames;
};

struct ControlIpcEncodeResult {
	EControlIpcEncodeOutcome outcome = EControlIpcEncodeOutcome::Encoded;
	std::vector<std::uint8_t> bytes;
};

//! Bounded TLV field used by the P0 payloads. The value is binary unless a
//! caller explicitly uses AddUtf8Field()/GetUtf8Field().
struct ControlIpcField {
	std::uint16_t tag = 0;
	std::vector<std::uint8_t> value;
};

using ControlIpcFields = std::vector<ControlIpcField>;

struct ControlIpcFieldDecodeResult {
	EControlIpcFieldDecodeOutcome outcome = EControlIpcFieldDecodeOutcome::Decoded;
	ControlIpcFields fields;
};

//! Tags shared by the initial request/reply set. Unknown tags are preserved so
//! a newer minor version can add optional fields without changing the frame.
enum class EControlIpcFieldTag : std::uint16_t {
	TerminalStatus = 1,
	Diagnostic = 2,
	ProfileId = 3,
	KnownGeneration = 4,
	ExpectedRevision = 5,
	OperationId = 6,
	CancelRequestId = 7,
	StorageSnapshot = 8,
	StorageMutation = 9,
	//! Fixed-width 256-bit Secret Vault bearer capability.
	Capability = 10,
	//! Authenticated extension-host session ID for a Secret Vault request.
	ExtensionHostSessionId = 11,
	//! Private bounded SecretAddress record.
	SecretAddress = 12,
	//! A private bounded secret UTF-8 value. It is valid only in a Get response
	//! with a Found result, or a Set request.
	SecretValue = 13,
	//! Private bounded SecretGetResult record.
	SecretResult = 14,
	//! Private bounded SecretMutationRequest/SecretMutationResult record.
	SecretMutation = 15,
	//! Exact canonical extension identity for a capability issuance request.
	ExtensionId = 16,
	//! Nonzero extension-host generation for a capability issuance request.
	ExtensionHostGeneration = 17,
	//! Positive bounded capability lifetime in milliseconds.
	CapabilityLifetimeMilliseconds = 18,
	//! Private bounded ControlProfileRpc request/response record.
	ProfilePayload = 19,
};

struct ControlIpcError {
	EControlIpcTerminalStatus status = EControlIpcTerminalStatus::InternalError;
	std::string diagnostic;
};

//! Serializes one complete frame. It validates metadata before allocating the
//! output buffer and never emits a partial frame.
[[nodiscard]] ControlIpcEncodeResult EncodeControlIpcFrame(const ControlIpcFrame& frame);

//! Serializes bounded TLV fields as: u16 tag, u32 value length, value bytes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlIpcFields(
	const ControlIpcFields& fields);
[[nodiscard]] ControlIpcFieldDecodeResult DecodeControlIpcFields(std::span<const std::uint8_t> bytes);

[[nodiscard]] bool AddUtf8Field(ControlIpcFields& fields, EControlIpcFieldTag tag,
	std::string_view value);
[[nodiscard]] std::optional<std::string> GetUtf8Field(const ControlIpcFields& fields,
	EControlIpcFieldTag tag, EControlIpcFieldDecodeOutcome* failure = nullptr);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeControlIpcError(
	const ControlIpcError& error);
[[nodiscard]] std::optional<ControlIpcError> DecodeControlIpcError(
	std::span<const std::uint8_t> payload, EControlIpcFieldDecodeOutcome* failure = nullptr);

/*! 
	@brief Incremental decoder for a byte-stream named pipe.

	The decoder validates the prefix before it allocates a frame buffer. A
	protocol failure is sticky until Reset(), which makes the terminal condition
	visible to the pipe owner rather than attempting to resynchronize hostile
	input by guessing frame boundaries.
*/
class CControlIpcFrameDecoder final {
public:
	explicit CControlIpcFrameDecoder(std::size_t maximumFrameBytes = kControlIpcMaximumFrameBytes) noexcept;

	[[nodiscard]] ControlIpcDecodeResult Feed(std::span<const std::uint8_t> bytes);
	void Reset() noexcept;
	[[nodiscard]] bool IsFailed() const noexcept;
	[[nodiscard]] EControlIpcDecodeOutcome GetFailure() const noexcept;

private:
	[[nodiscard]] EControlIpcDecodeOutcome DecodeCompleteFrame(ControlIpcFrame& output) const noexcept;
	void Fail(EControlIpcDecodeOutcome outcome) noexcept;

	std::size_t m_maximumFrameBytes;
	std::array<std::uint8_t, kControlIpcLengthPrefixBytes> m_lengthPrefix{};
	std::size_t m_lengthPrefixUsed = 0;
	std::size_t m_expectedFrameBytes = 0;
	std::vector<std::uint8_t> m_frameBytes;
	EControlIpcDecodeOutcome m_failure = EControlIpcDecodeOutcome::NeedMoreData;
};

} // namespace platform::controlipc
