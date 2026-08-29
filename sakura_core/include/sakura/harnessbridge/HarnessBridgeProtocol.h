/*! @file
    @brief Versioned, bounded wire contract for the local Harness Bridge.
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

namespace platform::harnessbridge {

//! Protocol-leaf copy of a terminal coordinate. The terminal adapter performs
//! the conversion so this type never depends on a model, session, or HWND.
struct HarnessBridgeTargetDescriptor final {
	std::string profileId;
	std::uint64_t profileGeneration{};
	std::array<std::uint8_t, 16> editorId{};
	std::uint64_t bridgeEpoch{};
	std::uint64_t runtimeGeneration{};
	std::uint64_t instanceGeneration{};
	std::uint64_t sessionId{};
	std::uint64_t windowId{};
	std::uint64_t paneId{};
	std::uint64_t instanceId{};

	friend bool operator==(const HarnessBridgeTargetDescriptor&,
		const HarnessBridgeTargetDescriptor&) = default;
};

inline constexpr std::uint32_t kHarnessBridgeMagic = 0x50424853u; // SHBP
inline constexpr std::uint16_t kHarnessBridgeMajorVersion = 1;
inline constexpr std::uint16_t kHarnessBridgeMinorVersion = 0;
inline constexpr std::size_t kHarnessBridgeLengthPrefixBytes = 4;
inline constexpr std::size_t kHarnessBridgeHeaderBytes = 28;
inline constexpr std::size_t kHarnessBridgeMaximumFrameBytes = 1024u * 1024u;
inline constexpr std::size_t kHarnessBridgeMaximumFieldCount = 256;
inline constexpr std::size_t kHarnessBridgeMaximumUtf8Bytes = 64u * 1024u;
inline constexpr std::size_t kHarnessBridgeMaximumPayloadBytes = 256u * 1024u;
inline constexpr std::size_t kHarnessBridgeMaximumDiagnosticBytes = 256;

enum class EHarnessBridgeFrameKind : std::uint16_t {
	Hello = 1,
	Challenge = 2,
	Authenticate = 3,
	Ready = 4,
	OperationRequest = 5,
	OperationResponse = 6,
	CancelRequest = 7,
	CancelAck = 8,
	MessageEvent = 9,
	MessageAck = 10,
	Error = 11,
};

enum class EHarnessBridgeFrameFlags : std::uint16_t {
	None = 0,
	Request = 0x0001,
	Response = 0x0002,
	Event = 0x0004,
	Terminal = 0x0008,
	More = 0x0010,
};

constexpr EHarnessBridgeFrameFlags operator|(EHarnessBridgeFrameFlags left,
	EHarnessBridgeFrameFlags right) noexcept
{
	return static_cast<EHarnessBridgeFrameFlags>(static_cast<std::uint16_t>(left)
		| static_cast<std::uint16_t>(right));
}
constexpr EHarnessBridgeFrameFlags operator&(EHarnessBridgeFrameFlags left,
	EHarnessBridgeFrameFlags right) noexcept
{
	return static_cast<EHarnessBridgeFrameFlags>(static_cast<std::uint16_t>(left)
		& static_cast<std::uint16_t>(right));
}

enum class EHarnessOperationKind : std::uint16_t {
	QueryOperation = 0x0001,
	ListSessions = 0x0101,
	ListWindows = 0x0102,
	ListPanes = 0x0103,
	CreateSession = 0x0110,
	CreateTerminalWindow = 0x0111,
	SplitPane = 0x0112,
	SelectWindow = 0x0113,
	SelectPane = 0x0114,
	ClosePane = 0x0115,
	CloseWindow = 0x0116,
	CloseSession = 0x0117,
	HasSession = 0x0118,
	SendInput = 0x0120,
	Capture = 0x0121,
	Display = 0x0122,
	WaitChannel = 0x0123,
	Resize = 0x0124,
	RegisterEndpoint = 0x0201,
	RenewEndpoint = 0x0202,
	ListEndpoints = 0x0203,
	SendEndpointMessage = 0x0210,
	ReceiveMessages = 0x0211,
	AcknowledgeMessage = 0x0212,
	PublishRun = 0x0220,
	WaitRun = 0x0221,
	CancelRun = 0x0222,
	ExecuteTmux = 0x0301,
};

enum class EHarnessTerminalStatus : std::uint16_t {
	Succeeded,
	InvalidRequest,
	UnsupportedVersion,
	UnsupportedCapability,
	UnsupportedTmuxSurface,
	ProfileMismatch,
	EditorMismatch,
	GenerationMismatch,
	TargetMissing,
	TopologyChanged,
	NotRunning,
	AccessDenied,
	DeadlineExceeded,
	Cancelled,
	ServerStopping,
	ResourceExhausted,
	OperationUnknown,
	Conflict,
	AlreadyTerminal,
	Ambiguous,
	ProtocolError,
	InternalError,
};

enum class EHarnessGrant : std::uint32_t {
	None = 0,
	Message = 1u << 0,
	ConsoleRead = 1u << 1,
	SendInput = 1u << 2,
	ManageTerminal = 1u << 3,
};

constexpr EHarnessGrant operator|(EHarnessGrant left, EHarnessGrant right) noexcept
{
	return static_cast<EHarnessGrant>(static_cast<std::uint32_t>(left)
		| static_cast<std::uint32_t>(right));
}
constexpr EHarnessGrant operator&(EHarnessGrant left, EHarnessGrant right) noexcept
{
	return static_cast<EHarnessGrant>(static_cast<std::uint32_t>(left)
		& static_cast<std::uint32_t>(right));
}
constexpr bool HasGrant(EHarnessGrant grants, EHarnessGrant grant) noexcept
{
	return (grants & grant) == grant;
}

struct HarnessBridgeFrameHeader final {
	std::uint16_t majorVersion = kHarnessBridgeMajorVersion;
	std::uint16_t minorVersion = kHarnessBridgeMinorVersion;
	EHarnessBridgeFrameKind kind = EHarnessBridgeFrameKind::Error;
	EHarnessBridgeFrameFlags flags = EHarnessBridgeFrameFlags::None;
	std::uint64_t requestId = 0;
	std::uint64_t bridgeEpoch = 0;
};

struct HarnessBridgeFrame final {
	HarnessBridgeFrameHeader header;
	std::vector<std::uint8_t> payload;
};

enum class EHarnessBridgeEncodeOutcome : std::uint8_t {
	Encoded,
	InvalidHeader,
	InvalidFlags,
	PayloadTooLarge,
};
struct HarnessBridgeEncodeResult final {
	EHarnessBridgeEncodeOutcome outcome = EHarnessBridgeEncodeOutcome::InvalidHeader;
	std::vector<std::uint8_t> bytes;
};

enum class EHarnessBridgeDecodeOutcome : std::uint8_t {
	NeedMoreData,
	Decoded,
	ZeroLengthFrame,
	OversizeFrame,
	MalformedFrame,
	UnsupportedMajorVersion,
	UnknownKind,
	UnknownRequiredFlags,
	InvalidRequestId,
	InvalidEpoch,
	InvalidDirection,
};
struct HarnessBridgeDecodeResult final {
	EHarnessBridgeDecodeOutcome outcome = EHarnessBridgeDecodeOutcome::NeedMoreData;
	std::vector<HarnessBridgeFrame> frames;
};

HarnessBridgeEncodeResult EncodeHarnessBridgeFrame(const HarnessBridgeFrame& frame);

class CHarnessBridgeFrameDecoder final {
public:
	explicit CHarnessBridgeFrameDecoder(std::size_t maximumFrameBytes = kHarnessBridgeMaximumFrameBytes) noexcept;
	HarnessBridgeDecodeResult Feed(std::span<const std::uint8_t> bytes);
	void Reset() noexcept;
	[[nodiscard]] bool IsFailed() const noexcept { return m_failed; }

private:
	std::size_t m_maximumFrameBytes;
	std::vector<std::uint8_t> m_buffer;
	bool m_failed = false;
};

enum class EHarnessBridgeFieldTag : std::uint16_t {
	TerminalStatus = 1,
	Diagnostic = 2,
	OperationKind = 3,
	OperationId = 4,
	TimeoutMs = 5,
	Target = 6,
	Payload = 7,
	CurrentTarget = 8,
	Grants = 9,
	ClientNonce = 10,
	ServerNonce = 11,
	AuthenticationDigest = 12,
	ConnectionLease = 13,
	MessageId = 14,
	RunId = 15,
	ReplyTo = 16,
	HopCount = 17,
};

struct HarnessBridgeField final {
	std::uint16_t tag = 0;
	std::vector<std::uint8_t> value;
};
using HarnessBridgeFields = std::vector<HarnessBridgeField>;

enum class EHarnessBridgeFieldDecodeOutcome : std::uint8_t {
	Decoded,
	MalformedField,
	TooManyFields,
	InvalidTag,
	DuplicateTag,
	InvalidUtf8,
	FieldTooLarge,
};
struct HarnessBridgeFieldDecodeResult final {
	EHarnessBridgeFieldDecodeOutcome outcome = EHarnessBridgeFieldDecodeOutcome::Decoded;
	HarnessBridgeFields fields;
};

std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeFields(const HarnessBridgeFields& fields);
HarnessBridgeFieldDecodeResult DecodeHarnessBridgeFields(std::span<const std::uint8_t> bytes);
bool AddHarnessBridgeBytesField(HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag,
	std::span<const std::uint8_t> value);
bool AddHarnessBridgeUtf8Field(HarnessBridgeFields& fields, EHarnessBridgeFieldTag tag,
	std::string_view value);
const HarnessBridgeField* FindHarnessBridgeField(const HarnessBridgeFields& fields,
	EHarnessBridgeFieldTag tag) noexcept;
std::optional<std::string> GetHarnessBridgeUtf8Field(const HarnessBridgeFields& fields,
	EHarnessBridgeFieldTag tag, EHarnessBridgeFieldDecodeOutcome* failure = nullptr);
bool IsValidHarnessBridgeUtf8(std::span<const std::uint8_t> bytes) noexcept;

} // namespace platform::harnessbridge
