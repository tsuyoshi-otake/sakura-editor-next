/*! @file
    @brief Protocol DTO dispatcher for tmux and the bounded Harness broker.
*/
#pragma once

#include <sakura/harnessbridge/HarnessBridgeAuthenticatedSession.h>

#include "terminal/tmux/TmuxCli.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platform::harnessbridge {

inline constexpr std::size_t kHarnessBridgeMaximumTmuxArgc = 64;
inline constexpr std::size_t kHarnessBridgeMaximumTmuxArgBytes = 64u * 1024u;

// The broker payloads use a deliberately small, private-to-the-bridge binary
// codec.  These limits are public so a client can construct requests without
// duplicating the server's wire assumptions.
inline constexpr std::uint8_t kHarnessBridgeBrokerCodecVersion = 1;
inline constexpr std::size_t kHarnessBridgeMaximumEndpointNameBytes = 128;
inline constexpr std::size_t kHarnessBridgeMaximumEndpointScopeBytes = 256;
inline constexpr std::size_t kHarnessBridgeMaximumMessageTypeBytes = 128;

enum class EHarnessBridgePayloadDecodeOutcome : std::uint8_t {
	Decoded,
	Malformed,
	UnsupportedVersion,
	TooManyArguments,
	FieldTooLarge,
	InvalidUtf8,
	TrailingBytes,
};

struct HarnessBridgePayloadDecodeResult final {
	EHarnessBridgePayloadDecodeOutcome outcome = EHarnessBridgePayloadDecodeOutcome::Malformed;
	std::vector<std::string> argv;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeTmuxArgv(
	const std::vector<std::string>& argv);
[[nodiscard]] HarnessBridgePayloadDecodeResult DecodeHarnessBridgeTmuxArgv(
	std::span<const std::uint8_t> bytes);

struct HarnessBridgeTmuxResponse final {
	std::int32_t exitCode = 1;
	std::string stdoutText;
	std::string stderrText;
};

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeTmuxResponse(
	const HarnessBridgeTmuxResponse& response);
[[nodiscard]] std::optional<HarnessBridgeTmuxResponse> DecodeHarnessBridgeTmuxResponse(
	std::span<const std::uint8_t> bytes);

// Bounded broker request/response codecs.  They are intentionally DTO-only;
// no runtime, process, window, or capability ownership crosses this boundary.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeEndpointRegistration(
	const HarnessEndpointRegistration& registration);
[[nodiscard]] bool DecodeHarnessBridgeEndpointRegistration(
	std::span<const std::uint8_t> bytes, HarnessEndpointRegistration& registration) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeMessage(
	const HarnessMessage& message);
[[nodiscard]] bool DecodeHarnessBridgeMessage(
	std::span<const std::uint8_t> bytes, HarnessMessage& message) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeReceiveRequest(
	const HarnessEndpointId& endpoint, std::size_t maximumMessages);
[[nodiscard]] bool DecodeHarnessBridgeReceiveRequest(
	std::span<const std::uint8_t> bytes, HarnessEndpointId& endpoint,
	std::uint16_t& maximumMessages) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeAcknowledgeRequest(
	const HarnessEndpointId& endpoint, const HarnessMessageId& message);
[[nodiscard]] bool DecodeHarnessBridgeAcknowledgeRequest(
	std::span<const std::uint8_t> bytes, HarnessEndpointId& endpoint,
	HarnessMessageId& message) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRunPublish(
	bool begin, const HarnessRunResult& result);
[[nodiscard]] bool DecodeHarnessBridgeRunPublish(
	std::span<const std::uint8_t> bytes, bool& begin, HarnessRunResult& result) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRunRequest(
	const HarnessRunId& run);
[[nodiscard]] bool DecodeHarnessBridgeRunRequest(
	std::span<const std::uint8_t> bytes, HarnessRunId& run) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeEndpointList(
	const std::vector<HarnessEndpointInfo>& endpoints);
[[nodiscard]] bool DecodeHarnessBridgeEndpointList(
	std::span<const std::uint8_t> bytes, std::vector<HarnessEndpointInfo>& endpoints) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeDeliveries(
	const std::vector<HarnessMessageDelivery>& deliveries);
[[nodiscard]] bool DecodeHarnessBridgeDeliveries(
	std::span<const std::uint8_t> bytes, std::vector<HarnessMessageDelivery>& deliveries) noexcept;

[[nodiscard]] std::optional<std::vector<std::uint8_t>> EncodeHarnessBridgeRun(
	const std::optional<HarnessRunResult>& run);
[[nodiscard]] bool DecodeHarnessBridgeRun(
	std::span<const std::uint8_t> bytes, std::optional<HarnessRunResult>& run) noexcept;

struct HarnessBridgeOperationDispatcherOptions final {
	std::size_t maximumResponseBytes = kHarnessBridgeMaximumPayloadBytes;
	terminal::tmux::TmuxCompatibilityProfile tmuxProfile;
	terminal::tmux::TmuxDispatcherLimits tmuxLimits;
};

//! Dispatches only protocol DTOs; runtime/editor types stay behind injected ports.
class CHarnessBridgeOperationDispatcher final : public IHarnessBridgeOperationDispatcher {
public:
	CHarnessBridgeOperationDispatcher(
		CHarnessBridgeBroker* broker,
		terminal::tmux::ITmuxRuntimePort* tmuxRuntime,
		HarnessBridgeOperationDispatcherOptions options = {});

	[[nodiscard]] HarnessBridgeOperationResponseDto Dispatch(
		const HarnessBridgeSessionContext& session,
		const HarnessBridgeOperationRequestDto& request) override;
	void Cancel(std::uint64_t requestId) noexcept override;

private:
	[[nodiscard]] HarnessBridgeOperationResponseDto DispatchTmux(
		const HarnessBridgeOperationRequestDto& request);
	[[nodiscard]] HarnessBridgeOperationResponseDto DispatchBroker(
		const HarnessBridgeTargetDescriptor& authority,
		const HarnessBridgeOperationRequestDto& request);
	[[nodiscard]] static HarnessBridgeOperationResponseDto BrokerFailure(
		EHarnessBrokerStatus status) noexcept;
	[[nodiscard]] static EHarnessTerminalStatus MapTmuxFailure(std::string_view diagnostic) noexcept;
	[[nodiscard]] static EHarnessTerminalStatus MapBrokerStatus(EHarnessBrokerStatus status) noexcept;
	[[nodiscard]] bool Cancelled(std::uint64_t generation) const noexcept;

	CHarnessBridgeBroker* m_broker;
	terminal::tmux::ITmuxRuntimePort* m_tmuxRuntime;
	HarnessBridgeOperationDispatcherOptions m_options;
	mutable std::mutex m_mutex;
	std::uint64_t m_cancelGeneration = 0;
};

} // namespace platform::harnessbridge
