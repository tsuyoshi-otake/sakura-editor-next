/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/cli/SakuraTmuxProcessEntry.h"

#include <sakura/harnessbridge/HarnessBridgeClientSession.h>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>

namespace terminal::cli {
namespace {

constexpr std::size_t kMaximumEnvironmentBlockWideChars = 256u * 1024u;

class CProductionTmuxBridgeClient final : public ISakuraTmuxBridgeClient {
public:
	[[nodiscard]] platform::harnessbridge::HarnessBridgeClientConnectResult Connect(
		const SakuraTmuxEnvironment& environment) override
	{
		return m_session.Connect(environment.endpoint, environment.target, environment.capability);
	}

	[[nodiscard]] platform::harnessbridge::HarnessBridgeClientOperationResult ExecuteTmux(
		const std::span<const std::uint8_t> payload, const std::chrono::milliseconds timeout) override
	{
		return m_session.Execute(platform::harnessbridge::EHarnessOperationKind::ExecuteTmux, payload, timeout);
	}

	void Close() noexcept override { m_session.Close(); }

private:
	platform::harnessbridge::CHarnessBridgeClientSession m_session;
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
			const auto remaining = bytes.size() - offset;
			const auto chunk = static_cast<DWORD>((std::min)(remaining,
				static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
			DWORD written = 0;
			if (!::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr)
				|| written == 0) return false;
			offset += written;
		}
		return true;
	}
};

[[nodiscard]] bool IsVersionInvocation(const int argc, wchar_t* const* argv) noexcept
{
	return argc == 2 && argv != nullptr && argv[1] != nullptr
		&& argv[1][0] == L'-' && argv[1][1] == L'V' && argv[1][2] == L'\0';
}

[[nodiscard]] bool StartsWithVersionOption(const int argc, wchar_t* const* argv) noexcept
{
	return argc >= 2 && argv != nullptr && argv[1] != nullptr
		&& argv[1][0] == L'-' && argv[1][1] == L'V';
}

[[nodiscard]] bool IsNoArgumentInvocation(const int argc) noexcept
{
	return argc <= 1;
}

} // namespace

std::optional<SakuraTmuxEnvironment> ReadSakuraTmuxEnvironment() noexcept
{
	const auto block = ::GetEnvironmentStringsW();
	if (block == nullptr) return std::nullopt;
	const auto release = [&] { ::FreeEnvironmentStringsW(block); };
	std::size_t offset = 0;
	for (;;) {
		if (offset >= kMaximumEnvironmentBlockWideChars) {
			release();
			return std::nullopt;
		}
		std::size_t length = 0;
		while (offset + length < kMaximumEnvironmentBlockWideChars && block[offset + length] != L'\0') ++length;
		if (offset + length >= kMaximumEnvironmentBlockWideChars) {
			release();
			return std::nullopt;
		}
		const auto entryEnd = offset + length;
		offset = entryEnd + 1;
		if (length == 0) {
			const auto environment = ParseSakuraTmuxEnvironmentBlock(
				std::span<const wchar_t>(block, offset));
			release();
			return environment;
		}
	}
}

int SakuraTmuxCliMain(const int argc, wchar_t* const* argv) noexcept
{
	CProductionTmuxBridgeClient bridge;
	SakuraTmuxEnvironment environment;
	if (!StartsWithVersionOption(argc, argv) && !IsNoArgumentInvocation(argc)) {
		const auto parsed = ReadSakuraTmuxEnvironment();
		if (!parsed) {
			SakuraCliProcessResult result{ 1, {}, "sakura-tmux: invalid-environment\n" };
			CWindowsCliOutput output;
			return WriteSakuraCliResult(result, output) ? result.exitCode : 1;
		}
		environment = *parsed;
	}
	const auto result = RunSakuraTmuxCli(argc, argv, environment, bridge);
	CWindowsCliOutput output;
	return WriteSakuraCliResult(result, output) ? result.exitCode : 1;
}

} // namespace terminal::cli
