/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/cli/SakuraTmuxCli.h"

#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>
#include "terminal/tmux/TmuxCommandTypes.h"

#include <algorithm>
#include <array>
#include <limits>

namespace terminal::cli {
namespace {

constexpr std::wstring_view kEndpointName = L"SAKURA_HARNESS_ENDPOINT_V1";
constexpr std::wstring_view kTargetName = L"SAKURA_TERMINAL_TARGET_V1";
constexpr std::wstring_view kCapabilityName = L"SAKURA_HARNESS_CAPABILITY_V1";
constexpr std::string_view kPrefix = "sakura-tmux: ";

[[nodiscard]] std::string Diagnostic(const std::string_view code)
{
	std::string result(kPrefix);
	result.append(code.data(), code.size());
	result.push_back('\n');
	return result;
}

[[nodiscard]] bool IsRequiredName(const std::wstring_view name, const std::wstring_view expected) noexcept
{
	if (name.size() != expected.size()) return false;
	for (std::size_t i = 0; i < name.size(); ++i) {
		const auto lower = [](wchar_t value) noexcept {
			return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
		};
		if (lower(name[i]) != lower(expected[i])) return false;
	}
	return true;
}

[[nodiscard]] bool IsSafeEnvironmentValue(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kSakuraTmuxMaximumEnvironmentWideChars) return false;
	for (const auto character : value) {
		if (character < 0x21 || character > 0x7e) return false;
	}
	return true;
}

[[nodiscard]] bool IsScalar(const char32_t value) noexcept
{
	return value <= 0x10ffff && !(value >= 0xd800 && value <= 0xdfff);
}

void AppendUtf8(std::string& output, const char32_t value)
{
	if (value <= 0x7f) {
		output.push_back(static_cast<char>(value));
	} else if (value <= 0x7ff) {
		output.push_back(static_cast<char>(0xc0 | (value >> 6)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else if (value <= 0xffff) {
		output.push_back(static_cast<char>(0xe0 | (value >> 12)));
		output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	} else {
		output.push_back(static_cast<char>(0xf0 | (value >> 18)));
		output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
		output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
	}
}

[[nodiscard]] bool DecodeResponse(
	const platform::harnessbridge::HarnessBridgeClientOperationResult& operation,
	SakuraCliProcessResult& result)
{
	if (operation.payload.empty()) return false;
	const auto response = platform::harnessbridge::DecodeHarnessBridgeTmuxResponse(operation.payload);
	if (!response) return false;
	result.exitCode = response->exitCode;
	result.stdoutText = response->stdoutText;
	result.stderrText = response->stderrText;
	return true;
}

} // namespace

std::optional<SakuraTmuxEnvironment> ParseSakuraTmuxEnvironmentBlock(
	const std::span<const wchar_t> environmentBlock) noexcept
{
	try {
		if (environmentBlock.empty() || environmentBlock.data() == nullptr
			|| environmentBlock.size() > 256u * 1024u) return std::nullopt;
		SakuraTmuxEnvironment result;
		std::size_t endpointCount = 0;
		std::size_t targetCount = 0;
		std::size_t capabilityCount = 0;
		bool terminated = false;
		std::size_t offset = 0;
		while (offset < environmentBlock.size()) {
			const auto begin = offset;
			while (offset < environmentBlock.size() && environmentBlock[offset] != L'\0') ++offset;
			if (offset == environmentBlock.size()) return std::nullopt;
			const std::wstring_view entry(environmentBlock.data() + begin, offset - begin);
			++offset;
			if (entry.empty()) {
				terminated = true;
				break;
			}
			const auto separator = entry.find(L'=');
			if (separator == std::wstring_view::npos || separator == 0) continue;
			const auto name = entry.substr(0, separator);
			const auto value = entry.substr(separator + 1);
			if (IsRequiredName(name, kEndpointName)) {
				if (++endpointCount != 1 || !IsSafeEnvironmentValue(value)) return std::nullopt;
				result.endpoint.assign(value);
			} else if (IsRequiredName(name, kTargetName)) {
				if (++targetCount != 1 || !IsSafeEnvironmentValue(value)) return std::nullopt;
				result.target.assign(value);
			} else if (IsRequiredName(name, kCapabilityName)) {
				if (++capabilityCount != 1 || !IsSafeEnvironmentValue(value)) return std::nullopt;
				result.capability.assign(value);
			}
		}
		if (!terminated || endpointCount != 1 || targetCount != 1 || capabilityCount != 1
			|| !result.IsComplete()) return std::nullopt;
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<std::string> SakuraWideToUtf8(
	const std::wstring_view value, const std::size_t maximumWideChars) noexcept
{
	try {
		if (value.size() > maximumWideChars) return std::nullopt;
		std::string result;
		result.reserve(value.size());
		for (std::size_t index = 0; index < value.size(); ++index) {
			char32_t scalar = static_cast<char32_t>(value[index]);
			if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
				if (scalar >= 0xd800 && scalar <= 0xdbff) {
					if (index + 1 >= value.size()) return std::nullopt;
					const auto low = static_cast<char32_t>(value[index + 1]);
					if (low < 0xdc00 || low > 0xdfff) return std::nullopt;
					scalar = 0x10000 + ((scalar - 0xd800) << 10) + (low - 0xdc00);
					++index;
				} else if (scalar >= 0xdc00 && scalar <= 0xdfff) {
					return std::nullopt;
				}
			} else if (!IsScalar(scalar)) {
				return std::nullopt;
			}
			if (!IsScalar(scalar)) return std::nullopt;
			AppendUtf8(result, scalar);
		}
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

std::string SakuraTmuxStatusDiagnostic(const platform::harnessbridge::EHarnessTerminalStatus status)
{
	using platform::harnessbridge::EHarnessTerminalStatus;
	switch (status) {
	case EHarnessTerminalStatus::InvalidRequest: return Diagnostic("invalid-usage");
	case EHarnessTerminalStatus::UnsupportedVersion: return Diagnostic("unsupported-version");
	case EHarnessTerminalStatus::UnsupportedCapability: return Diagnostic("bridge-unavailable");
	case EHarnessTerminalStatus::UnsupportedTmuxSurface: return Diagnostic("unsupported-surface");
	case EHarnessTerminalStatus::ProfileMismatch: return Diagnostic("profile-mismatch");
	case EHarnessTerminalStatus::EditorMismatch: return Diagnostic("editor-mismatch");
	case EHarnessTerminalStatus::GenerationMismatch: return Diagnostic("generation-mismatch");
	case EHarnessTerminalStatus::TargetMissing: return Diagnostic("target-missing");
	case EHarnessTerminalStatus::TopologyChanged: return Diagnostic("topology-changed");
	case EHarnessTerminalStatus::NotRunning: return Diagnostic("not-running");
	case EHarnessTerminalStatus::AccessDenied: return Diagnostic("access-denied");
	case EHarnessTerminalStatus::DeadlineExceeded: return Diagnostic("deadline-exceeded");
	case EHarnessTerminalStatus::Cancelled: return Diagnostic("cancelled");
	case EHarnessTerminalStatus::ServerStopping: return Diagnostic("server-stopping");
	case EHarnessTerminalStatus::ResourceExhausted: return Diagnostic("resource-exhausted");
	case EHarnessTerminalStatus::OperationUnknown: return Diagnostic("operation-unknown");
	case EHarnessTerminalStatus::Conflict: return Diagnostic("conflict");
	case EHarnessTerminalStatus::AlreadyTerminal: return Diagnostic("already-terminal");
	case EHarnessTerminalStatus::Ambiguous: return Diagnostic("ambiguous");
	case EHarnessTerminalStatus::ProtocolError: return Diagnostic("protocol-error");
	case EHarnessTerminalStatus::InternalError: return Diagnostic("internal-error");
	case EHarnessTerminalStatus::Succeeded: return {};
	}
	return Diagnostic("internal-error");
}

SakuraCliProcessResult RunSakuraTmuxCli(
	const std::span<const std::wstring_view> arguments,
	const SakuraTmuxEnvironment& environment,
	ISakuraTmuxBridgeClient& bridge,
	const std::chrono::milliseconds timeout) noexcept
{
	SakuraCliProcessResult result;
	try {
		if ((arguments.size() != 0 && arguments.data() == nullptr)
			|| arguments.size() > kSakuraTmuxMaximumArgc) {
			result.stderrText = Diagnostic("resource-exhausted");
			return result;
		}
		if (arguments.size() == 1 && arguments.front() == L"-V") {
			result.exitCode = 0;
			result.stdoutText.assign(terminal::tmux::kTmuxCompatibilityVersion);
			result.stdoutText.push_back('\n');
			return result;
		}
		if (arguments.empty()) {
			result.stderrText = Diagnostic("invalid-usage");
			return result;
		}
		if (arguments.front() == L"-V") {
			result.stderrText = Diagnostic("unsupported-global-option");
			return result;
		}
		if (!environment.IsComplete()
			|| !IsSafeEnvironmentValue(environment.endpoint)
			|| !IsSafeEnvironmentValue(environment.target)
			|| !IsSafeEnvironmentValue(environment.capability)) {
			result.stderrText = Diagnostic("invalid-environment");
			return result;
		}
		if (timeout <= std::chrono::milliseconds::zero()) {
			result.stderrText = Diagnostic("invalid-timeout");
			return result;
		}
		std::vector<std::string> narrowArguments;
		narrowArguments.reserve(arguments.size());
		for (const auto argument : arguments) {
			const auto narrow = SakuraWideToUtf8(argument);
			if (!narrow) {
				result.stderrText = Diagnostic("invalid-argument");
				return result;
			}
			narrowArguments.push_back(*narrow);
		}
		const auto payload = platform::harnessbridge::EncodeHarnessBridgeTmuxArgv(narrowArguments);
		if (!payload) {
			result.stderrText = Diagnostic("resource-exhausted");
			return result;
		}
		struct CloseGuard final {
			ISakuraTmuxBridgeClient& client;
			~CloseGuard() { client.Close(); }
		} closeGuard{ bridge };
		const auto connected = bridge.Connect(environment);
		if (!connected.succeeded) {
			result.stderrText = SakuraTmuxStatusDiagnostic(connected.status);
			return result;
		}
		const auto boundedTimeout = (std::min)(timeout, kSakuraTmuxMaximumOperationTimeout);
		const auto operation = bridge.ExecuteTmux(*payload, boundedTimeout);
		if (DecodeResponse(operation, result)) return result;
		result.exitCode = 1;
		result.stdoutText.clear();
		result.stderrText = operation.status == platform::harnessbridge::EHarnessTerminalStatus::Succeeded
			? Diagnostic("protocol-error") : SakuraTmuxStatusDiagnostic(operation.status);
		return result;
	} catch (...) {
		return SakuraCliProcessResult{ 1, {}, Diagnostic("internal-error") };
	}
}

SakuraCliProcessResult RunSakuraTmuxCli(
	const int argc, wchar_t* const* argv, const SakuraTmuxEnvironment& environment,
	ISakuraTmuxBridgeClient& bridge, const std::chrono::milliseconds timeout) noexcept
{
	if (argc < 1 || argv == nullptr) return SakuraCliProcessResult{ 1, {}, Diagnostic("invalid-usage") };
	try {
		if (argc - 1 > static_cast<int>(kSakuraTmuxMaximumArgc)) {
			return SakuraCliProcessResult{ 1, {}, Diagnostic("resource-exhausted") };
		}
		std::vector<std::wstring_view> arguments;
		arguments.reserve(static_cast<std::size_t>(argc - 1));
		for (int index = 1; index < argc; ++index) {
			if (argv[index] == nullptr) return SakuraCliProcessResult{ 1, {}, Diagnostic("invalid-argument") };
			std::size_t length = 0;
			while (length <= kSakuraTmuxMaximumArgumentWideChars && argv[index][length] != L'\0') ++length;
			if (length > kSakuraTmuxMaximumArgumentWideChars) {
				return SakuraCliProcessResult{ 1, {}, Diagnostic("resource-exhausted") };
			}
			arguments.emplace_back(argv[index], length);
		}
		return RunSakuraTmuxCli(arguments, environment, bridge, timeout);
	} catch (...) {
		return SakuraCliProcessResult{ 1, {}, Diagnostic("internal-error") };
	}
}

} // namespace terminal::cli
