/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include "terminal/cli/SakuraCliTypes.h"

#include <sakura/harnessbridge/HarnessBridgeClientSession.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace terminal::cli {

inline constexpr std::size_t kSakuraTmuxMaximumArgc = 64;
inline constexpr std::size_t kSakuraTmuxMaximumArgumentWideChars = 64u * 1024u;
inline constexpr std::size_t kSakuraTmuxMaximumEnvironmentWideChars = 4096;
inline constexpr std::chrono::milliseconds kSakuraTmuxDefaultOperationTimeout =
	std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kSakuraTmuxMaximumOperationTimeout =
	std::chrono::seconds(30);

struct SakuraTmuxEnvironment final {
	std::wstring endpoint;
	std::wstring target;
	std::wstring capability;

	[[nodiscard]] bool IsComplete() const noexcept
	{
		return !endpoint.empty() && !target.empty() && !capability.empty();
	}
};

//! Narrow bridge seam used by the pure CLI core and by standalone tests.
class ISakuraTmuxBridgeClient {
public:
	virtual ~ISakuraTmuxBridgeClient() = default;
	[[nodiscard]] virtual platform::harnessbridge::HarnessBridgeClientConnectResult Connect(
		const SakuraTmuxEnvironment&) = 0;
	[[nodiscard]] virtual platform::harnessbridge::HarnessBridgeClientOperationResult ExecuteTmux(
		std::span<const std::uint8_t> payload, std::chrono::milliseconds timeout) = 0;
	virtual void Close() noexcept = 0;
};

//! Parses a bounded UTF-16 environment block and rejects missing or duplicate
//! required variables. The input includes the final empty entry.
[[nodiscard]] std::optional<SakuraTmuxEnvironment> ParseSakuraTmuxEnvironmentBlock(
	std::span<const wchar_t> environmentBlock) noexcept;

//! Converts one UTF-16 argument to strict UTF-8 without replacement.
[[nodiscard]] std::optional<std::string> SakuraWideToUtf8(
	std::wstring_view value, std::size_t maximumWideChars = kSakuraTmuxMaximumArgumentWideChars) noexcept;

//! Core shared by both tmux.exe and sakura-tmux.exe. argv excludes argv[0].
[[nodiscard]] SakuraCliProcessResult RunSakuraTmuxCli(
	std::span<const std::wstring_view> arguments,
	const SakuraTmuxEnvironment& environment,
	ISakuraTmuxBridgeClient& bridge,
	std::chrono::milliseconds timeout = kSakuraTmuxDefaultOperationTimeout) noexcept;

//! Convenience overload for a native wmain argument vector.
[[nodiscard]] SakuraCliProcessResult RunSakuraTmuxCli(
	int argc, wchar_t* const* argv, const SakuraTmuxEnvironment& environment,
	ISakuraTmuxBridgeClient& bridge,
	std::chrono::milliseconds timeout = kSakuraTmuxDefaultOperationTimeout) noexcept;

[[nodiscard]] std::string SakuraTmuxStatusDiagnostic(
	platform::harnessbridge::EHarnessTerminalStatus status);

} // namespace terminal::cli
