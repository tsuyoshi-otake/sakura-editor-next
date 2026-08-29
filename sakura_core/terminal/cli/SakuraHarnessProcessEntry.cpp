/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/cli/SakuraHarnessProcessEntry.h"

#include <sakura/harnessbridge/HarnessBridgeClientSession.h>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace terminal::cli {
namespace {

constexpr std::size_t kMaximumEnvironmentBlockWideChars = 256u * 1024u;
constexpr std::wstring_view kEndpointName = L"SAKURA_HARNESS_ENDPOINT_V1";
constexpr std::wstring_view kTargetName = L"SAKURA_TERMINAL_TARGET_V1";
constexpr std::wstring_view kCapabilityName = L"SAKURA_HARNESS_CAPABILITY_V1";

[[nodiscard]] bool IsRequiredName(const std::wstring_view name, const std::wstring_view expected) noexcept
{
	if (name.size() != expected.size()) return false;
	for (std::size_t index = 0; index < name.size(); ++index) {
		const auto lower = [](const wchar_t value) noexcept {
			return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
		};
		if (lower(name[index]) != lower(expected[index])) return false;
	}
	return true;
}

[[nodiscard]] bool IsSafeEnvironmentValue(const std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > 4096) return false;
	for (const auto character : value) {
		if (character < 0x21 || character > 0x7e) return false;
	}
	return true;
}

[[nodiscard]] std::optional<SakuraHarnessEnvironment> ParseEnvironmentBlock(
	const std::span<const wchar_t> block) noexcept
{
	try {
		if (block.empty() || block.data() == nullptr || block.size() > kMaximumEnvironmentBlockWideChars) return std::nullopt;
		SakuraHarnessEnvironment result;
		std::size_t endpointCount = 0;
		std::size_t targetCount = 0;
		std::size_t capabilityCount = 0;
		std::size_t offset = 0;
		bool terminated = false;
		while (offset < block.size()) {
			const auto begin = offset;
			while (offset < block.size() && block[offset] != L'\0') ++offset;
			if (offset == block.size()) return std::nullopt;
			const std::wstring_view entry(block.data() + begin, offset - begin);
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

class CProductionHarnessBridgeClient final : public ISakuraHarnessBridgeClient {
public:
	[[nodiscard]] platform::harnessbridge::HarnessBridgeClientConnectResult Connect(
		const SakuraHarnessEnvironment& environment) override
	{
		return m_session.Connect(environment.endpoint, environment.target, environment.capability);
	}

	[[nodiscard]] platform::harnessbridge::HarnessBridgeClientOperationResult Execute(
		const platform::harnessbridge::EHarnessOperationKind operation,
		const std::span<const std::uint8_t> payload,
		const std::chrono::milliseconds timeout) override
	{
		return m_session.Execute(operation, payload, timeout);
	}

	void Close() noexcept override { m_session.Close(); }

private:
	platform::harnessbridge::CHarnessBridgeClientSession m_session;
};

class CSystemHarnessIdSource final : public ISakuraHarnessIdSource {
public:
	[[nodiscard]] std::optional<platform::harnessbridge::HarnessOpaqueId> NextId() noexcept override
	{
		for (unsigned attempt = 0; attempt < 4; ++attempt) {
			platform::harnessbridge::HarnessOpaqueId result;
			if (BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, result.value.data(),
				static_cast<ULONG>(result.value.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
				&& result.IsValid()) return result;
		}
		return std::nullopt;
	}
};

class CWindowsCliOutput final : public ISakuraCliOutput {
public:
	[[nodiscard]] bool WriteStdout(const std::string_view bytes) noexcept override
	{
		return Write(GetStdHandle(STD_OUTPUT_HANDLE), bytes);
	}

	[[nodiscard]] bool WriteStderr(const std::string_view bytes) noexcept override
	{
		return Write(GetStdHandle(STD_ERROR_HANDLE), bytes);
	}

private:
	[[nodiscard]] static bool Write(const HANDLE handle, const std::string_view bytes) noexcept
	{
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
		std::size_t offset = 0;
		while (offset < bytes.size()) {
			const auto chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
				static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
			DWORD written = 0;
			if (!::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) return false;
			offset += written;
		}
		return true;
	}
};

[[nodiscard]] bool ArgumentEquals(const wchar_t* argument, const std::wstring_view expected) noexcept
{
	if (argument == nullptr) return false;
	std::size_t length = 0;
	while (length <= kSakuraHarnessMaximumArgumentWideChars && argument[length] != L'\0') ++length;
	return length == expected.size() && std::wstring_view(argument, length) == expected;
}

[[nodiscard]] bool WantsStdinPayload(const int argc, wchar_t* const* argv) noexcept
{
	if (argc <= 1 || argv == nullptr) return false;
	for (int index = 1; index < argc; ++index) {
		if (ArgumentEquals(argv[index], L"--payload-stdin")) return true;
	}
	return false;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> ReadBoundedStdin() noexcept
{
	try {
		const auto handle = GetStdHandle(STD_INPUT_HANDLE);
		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return std::nullopt;
		std::vector<std::uint8_t> result;
		result.reserve(kSakuraHarnessMaximumInputBytes);
		std::array<std::uint8_t, 16u * 1024u> buffer{};
		for (;;) {
			const auto remaining = kSakuraHarnessMaximumInputBytes - result.size();
			const auto request = static_cast<DWORD>((std::min)(buffer.size(), remaining + 1));
			DWORD read = 0;
			if (!::ReadFile(handle, buffer.data(), request, &read, nullptr)) return std::nullopt;
			if (read == 0) break;
			if (read > remaining) return std::nullopt;
			result.insert(result.end(), buffer.begin(), buffer.begin() + read);
		}
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

[[nodiscard]] SakuraCliProcessResult FixedFailure(const int exitCode, const char* code) noexcept
{
	SakuraCliProcessResult result;
	result.exitCode = exitCode;
	result.stderrText = std::string("sakura-harness: ") + code + '\n';
	return result;
}

} // namespace

std::optional<SakuraHarnessEnvironment> ReadSakuraHarnessEnvironment() noexcept
{
	const auto block = ::GetEnvironmentStringsW();
	if (block == nullptr) return std::nullopt;
	std::size_t offset = 0;
	for (;;) {
		if (offset >= kMaximumEnvironmentBlockWideChars) {
			::FreeEnvironmentStringsW(block);
			return std::nullopt;
		}
		std::size_t length = 0;
		while (offset + length < kMaximumEnvironmentBlockWideChars && block[offset + length] != L'\0') ++length;
		if (offset + length >= kMaximumEnvironmentBlockWideChars) {
			::FreeEnvironmentStringsW(block);
			return std::nullopt;
		}
		offset += length + 1;
		if (length == 0) {
			const auto result = ParseEnvironmentBlock(std::span<const wchar_t>(block, offset));
			::FreeEnvironmentStringsW(block);
			return result;
		}
	}
}

int SakuraHarnessCliMain(const int argc, wchar_t* const* argv) noexcept
{
	CProductionHarnessBridgeClient bridge;
	CSystemHarnessIdSource ids;
	SakuraHarnessEnvironment environment;
	const bool version = argc == 2 && (ArgumentEquals(argv == nullptr ? nullptr : argv[1], L"-V")
		|| ArgumentEquals(argv == nullptr ? nullptr : argv[1], L"--version"));
	if (!version && argc > 1) {
		const auto parsed = ReadSakuraHarnessEnvironment();
		if (!parsed) {
			const auto result = FixedFailure(5, "invalid-environment");
			CWindowsCliOutput output;
			return WriteSakuraCliResult(result, output) ? result.exitCode : 10;
		}
		environment = *parsed;
	}
	std::vector<std::uint8_t> stdinPayload;
	if (WantsStdinPayload(argc, argv)) {
		const auto input = ReadBoundedStdin();
		if (!input) {
			const auto result = FixedFailure(7, "resource-exhausted");
			CWindowsCliOutput output;
			return WriteSakuraCliResult(result, output) ? result.exitCode : 10;
		}
		stdinPayload = *input;
	}
	const auto result = RunSakuraHarnessCli(argc, argv, environment, bridge, ids, stdinPayload);
	CWindowsCliOutput output;
	return WriteSakuraCliResult(result, output) ? result.exitCode : 10;
}

} // namespace terminal::cli
