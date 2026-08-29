/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/cli/SakuraCliTypes.h"

#include <sakura/harnessbridge/HarnessBridgeClientSession.h>
#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terminal::cli {

inline constexpr std::size_t kSakuraHarnessMaximumArgc = 256;
inline constexpr std::size_t kSakuraHarnessMaximumArgumentWideChars = 64u * 1024u;
inline constexpr std::size_t kSakuraHarnessMaximumInputBytes = 256u * 1024u;
inline constexpr std::size_t kSakuraHarnessMaximumJsonLineBytes = 512u * 1024u;
inline constexpr std::chrono::milliseconds kSakuraHarnessDefaultOperationTimeout =
	std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kSakuraHarnessMaximumOperationTimeout =
	std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kSakuraHarnessMaximumWait =
	std::chrono::minutes(5);

struct SakuraHarnessEnvironment final {
	std::wstring endpoint;
	std::wstring target;
	std::wstring capability;
	// The current bridge protocol has no authenticated endpoint-id field yet.
	// Integrators may provide the broker identity here; message receive/ack and
	// send fail closed when it is absent.
	std::optional<platform::harnessbridge::HarnessEndpointId> endpointId;

	[[nodiscard]] bool IsComplete() const noexcept
	{
		return !endpoint.empty() && !target.empty() && !capability.empty();
	}
};

enum class SakuraHarnessCommandKind : std::uint8_t {
	Version,
	EndpointRegister,
	EndpointList,
	MessageSend,
	MessageReceive,
	MessageAck,
	RunPublish,
	RunWait,
};

struct SakuraHarnessCommand final {
	SakuraHarnessCommandKind kind = SakuraHarnessCommandKind::Version;
	std::string endpointName;
	std::string capabilities;
	platform::harnessbridge::EHarnessGrant grants = platform::harnessbridge::EHarnessGrant::None;
	std::optional<platform::harnessbridge::HarnessEndpointId> endpoint;
	std::optional<platform::harnessbridge::HarnessEndpointId> localEndpoint;
	std::optional<platform::harnessbridge::HarnessMessageId> message;
	std::optional<platform::harnessbridge::HarnessMessageId> replyTo;
	std::optional<platform::harnessbridge::HarnessRunId> run;
	std::string messageType;
	std::size_t maximumMessages = 128;
	std::chrono::milliseconds wait{};
	bool waitSpecified = false;
	bool timeoutSpecified = false;
	std::optional<platform::harnessbridge::EHarnessRunTerminalStatus> runStatus;
	bool payloadFromStdin = false;
};

enum class SakuraHarnessParseOutcome : std::uint8_t {
	Parsed,
	InvalidUsage,
	Unsupported,
	ResourceExhausted,
};

struct SakuraHarnessParseResult final {
	SakuraHarnessParseOutcome outcome = SakuraHarnessParseOutcome::InvalidUsage;
	SakuraHarnessCommand command;
};

//! Narrow bridge seam used by the pure CLI and by standalone tests.
class ISakuraHarnessBridgeClient {
public:
	virtual ~ISakuraHarnessBridgeClient() = default;
	[[nodiscard]] virtual platform::harnessbridge::HarnessBridgeClientConnectResult Connect(
		const SakuraHarnessEnvironment&) = 0;
	[[nodiscard]] virtual platform::harnessbridge::HarnessBridgeClientOperationResult Execute(
		platform::harnessbridge::EHarnessOperationKind operation,
		std::span<const std::uint8_t> payload, std::chrono::milliseconds timeout) = 0;
	virtual void Close() noexcept = 0;
};

//! Generates opaque IDs for client-side message/request correlation. Production
//! implementations must use a CSPRNG; tests can inject deterministic IDs.
class ISakuraHarnessIdSource {
public:
	virtual ~ISakuraHarnessIdSource() = default;
	[[nodiscard]] virtual std::optional<platform::harnessbridge::HarnessOpaqueId> NextId() noexcept = 0;
};

[[nodiscard]] SakuraHarnessParseResult ParseSakuraHarnessArguments(
	std::span<const std::wstring_view> arguments) noexcept;

[[nodiscard]] std::optional<platform::harnessbridge::HarnessOpaqueId> ParseSakuraHarnessId(
	std::string_view value) noexcept;
[[nodiscard]] std::string FormatSakuraHarnessId(
	const platform::harnessbridge::HarnessOpaqueId& value);

[[nodiscard]] int SakuraHarnessExitCode(
	platform::harnessbridge::EHarnessTerminalStatus status) noexcept;
[[nodiscard]] std::string SakuraHarnessStatusName(
	platform::harnessbridge::EHarnessTerminalStatus status);

//! Executes one structured command. argv excludes argv[0]. stdinPayload is
//! consumed only by commands that explicitly request --payload-stdin.
[[nodiscard]] SakuraCliProcessResult RunSakuraHarnessCli(
	std::span<const std::wstring_view> arguments,
	const SakuraHarnessEnvironment& environment,
	ISakuraHarnessBridgeClient& bridge,
	ISakuraHarnessIdSource& ids,
	std::span<const std::uint8_t> stdinPayload = {},
	std::chrono::milliseconds timeout = kSakuraHarnessDefaultOperationTimeout) noexcept;

[[nodiscard]] SakuraCliProcessResult RunSakuraHarnessCli(
	int argc, wchar_t* const* argv,
	const SakuraHarnessEnvironment& environment,
	ISakuraHarnessBridgeClient& bridge,
	ISakuraHarnessIdSource& ids,
	std::span<const std::uint8_t> stdinPayload = {},
	std::chrono::milliseconds timeout = kSakuraHarnessDefaultOperationTimeout) noexcept;

} // namespace terminal::cli
