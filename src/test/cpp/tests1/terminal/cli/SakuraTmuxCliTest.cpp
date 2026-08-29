/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "pch.h"

#include <gtest/gtest.h>

#include "terminal/cli/SakuraTmuxCli.h"
#include "terminal/tmux/TmuxCommandTypes.h"

#include <sakura/harnessbridge/HarnessBridgeOperationDispatcher.h>

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace terminal::cli;
using namespace platform::harnessbridge;

class FakeTmuxBridge final : public ISakuraTmuxBridgeClient {
public:
	HarnessBridgeClientConnectResult Connect(const SakuraTmuxEnvironment& value) override
	{
		++connectCalls;
		connectedEnvironment = value;
		return connectResult;
	}

	HarnessBridgeClientOperationResult ExecuteTmux(
		const std::span<const std::uint8_t> value, const std::chrono::milliseconds valueTimeout) override
	{
		++executeCalls;
		lastPayload.assign(value.begin(), value.end());
		lastTimeout = valueTimeout;
		return operationResult;
	}

	void Close() noexcept override { ++closeCalls; }

	HarnessBridgeClientConnectResult connectResult{ true, EHarnessTerminalStatus::Succeeded, 0 };
	HarnessBridgeClientOperationResult operationResult;
	SakuraTmuxEnvironment connectedEnvironment;
	std::vector<std::uint8_t> lastPayload;
	std::chrono::milliseconds lastTimeout{};
	int connectCalls{};
	int executeCalls{};
	int closeCalls{};
};

SakuraTmuxEnvironment CompleteEnvironment()
{
	return { L"she1.0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		L"sht1.valid-target", L"shc1.valid-capability" };
}

std::vector<wchar_t> EnvironmentBlock(std::initializer_list<std::wstring_view> entries)
{
	std::vector<wchar_t> block;
	for (const auto entry : entries) {
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	block.push_back(L'\0');
	return block;
}

TEST(SakuraTmuxCli, VersionDoesNotRequireBridgeOrEnvironment)
{
	FakeTmuxBridge bridge;
	const std::wstring_view arguments[] = { L"-V" };
	const auto result = RunSakuraTmuxCli(arguments, {}, bridge);
	EXPECT_EQ(0, result.exitCode);
	EXPECT_EQ("sakura-tmux 0.1 (tmux 3.7c command subset; not upstream tmux)\n", result.stdoutText);
	EXPECT_TRUE(result.stderrText.empty());
	EXPECT_EQ(0, bridge.connectCalls);
	EXPECT_EQ(0, bridge.executeCalls);
}

TEST(SakuraTmuxCli, ConvertsUtf16StrictlyAndPreservesServerResponse)
{
	FakeTmuxBridge bridge;
	const auto encoded = EncodeHarnessBridgeTmuxResponse({ 0, "data\n", "" });
	ASSERT_TRUE(encoded.has_value());
	bridge.operationResult = { EHarnessTerminalStatus::Succeeded, *encoded, 0 };
	const std::wstring_view arguments[] = { L"display-message", L"--", L"\u65e5\u672c\U0001f600" };
	const auto environment = CompleteEnvironment();
	const auto result = RunSakuraTmuxCli(
		std::span<const std::wstring_view>(arguments),
		environment, bridge, std::chrono::seconds(4));
	EXPECT_EQ(0, result.exitCode);
	EXPECT_EQ("data\n", result.stdoutText);
	EXPECT_TRUE(result.stderrText.empty());
	EXPECT_EQ(1, bridge.connectCalls);
	EXPECT_EQ(1, bridge.executeCalls);
	EXPECT_EQ(1, bridge.closeCalls);
	EXPECT_EQ(std::chrono::seconds(4), bridge.lastTimeout);
	const auto decoded = DecodeHarnessBridgeTmuxArgv(bridge.lastPayload);
	ASSERT_EQ(EHarnessBridgePayloadDecodeOutcome::Decoded, decoded.outcome);
	ASSERT_EQ(3u, decoded.argv.size());
	EXPECT_EQ("display-message", decoded.argv[0]);
	EXPECT_EQ("--", decoded.argv[1]);
	EXPECT_EQ("\xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80", decoded.argv[2]);
}

TEST(SakuraTmuxCli, RejectsMalformedSurrogatesAndEnvironmentDuplicates)
{
	FakeTmuxBridge bridge;
	const std::wstring malformedText(1, static_cast<wchar_t>(0xd800));
	const std::wstring_view malformed[] = { L"list-panes", malformedText };
	const auto malformedResult = RunSakuraTmuxCli(
		std::span<const std::wstring_view>(malformed),
		CompleteEnvironment(), bridge);
	EXPECT_EQ(1, malformedResult.exitCode);
	EXPECT_EQ("sakura-tmux: invalid-argument\n", malformedResult.stderrText);
	EXPECT_EQ(0, bridge.connectCalls);

	const auto duplicate = EnvironmentBlock({
		L"SAKURA_HARNESS_ENDPOINT_V1=one",
		L"sakura_harness_endpoint_v1=two",
		L"SAKURA_TERMINAL_TARGET_V1=target",
		L"SAKURA_HARNESS_CAPABILITY_V1=cap",
	});
	EXPECT_FALSE(ParseSakuraTmuxEnvironmentBlock(duplicate).has_value());
}

TEST(SakuraTmuxCli, RejectsMissingEnvironmentAndKeepsBridgeFailuresContentFree)
{
	FakeTmuxBridge bridge;
	const std::wstring_view arguments[] = { L"list-panes" };
	const auto missing = RunSakuraTmuxCli(std::span<const std::wstring_view>(arguments), {}, bridge);
	EXPECT_EQ(1, missing.exitCode);
	EXPECT_EQ("sakura-tmux: invalid-environment\n", missing.stderrText);
	EXPECT_EQ(0, bridge.connectCalls);

	bridge.connectResult = { false, EHarnessTerminalStatus::AccessDenied, 5 };
	const auto denied = RunSakuraTmuxCli(arguments, CompleteEnvironment(), bridge);
	EXPECT_EQ(1, denied.exitCode);
	EXPECT_EQ("sakura-tmux: access-denied\n", denied.stderrText);
	EXPECT_EQ(1, bridge.closeCalls);
}

} // namespace
