/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "extension/CExtensionHostProcess.h"

#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace {

std::map<std::wstring, std::wstring> ParseEnvironmentBlock(const std::vector<wchar_t>& block)
{
	std::map<std::wstring, std::wstring> entries;
	for (const wchar_t* current = block.data(); current && *current; current += std::wcslen(current) + 1) {
		const std::wstring line(current);
		const auto separator = line.find(L'=');
		if (separator != std::wstring::npos) {
			entries.emplace(line.substr(0, separator), line.substr(separator + 1));
		}
	}
	return entries;
}

} // namespace

TEST(CExtensionHostProcess, SanitizedEnvironmentDropsNodeInjectionAndUnrelatedSecrets)
{
	const std::vector<CExtensionHostProcess::EnvironmentEntry> inherited = {
		{ L"SystemRoot", L"C:\\Windows" },
		{ L"PATH", L"C:\\Windows\\System32" },
		{ L"NODE_OPTIONS", L"--require malicious.js" },
		{ L"NODE_PATH", L"C:\\attacker" },
		{ L"API_SECRET", L"must-not-leak" },
		{ L"SAKURA_BOOT_ID", L"spoofed" },
	};
	const std::vector<CExtensionHostProcess::EnvironmentEntry> trusted = {
		{ L"SAKURA_EXTENSION_HOST", L"1" },
		{ L"SAKURA_BOOT_ID", L"trusted" },
		{ L"NODE_OPTIONS", L"still-forbidden" },
	};

	const auto parsed = ParseEnvironmentBlock(
		CExtensionHostProcess::BuildSanitizedEnvironmentBlock(inherited, trusted));
	EXPECT_EQ(L"C:\\Windows", parsed.at(L"SystemRoot"));
	EXPECT_EQ(L"C:\\Windows\\System32", parsed.at(L"PATH"));
	EXPECT_EQ(L"1", parsed.at(L"SAKURA_EXTENSION_HOST"));
	EXPECT_EQ(L"trusted", parsed.at(L"SAKURA_BOOT_ID"));
	EXPECT_FALSE(parsed.contains(L"NODE_OPTIONS"));
	EXPECT_FALSE(parsed.contains(L"NODE_PATH"));
	EXPECT_FALSE(parsed.contains(L"API_SECRET"));
}

TEST(CExtensionHostProcess, EnvironmentBlockIsDoubleNullTerminatedAndCaseInsensitive)
{
	const std::vector<CExtensionHostProcess::EnvironmentEntry> inherited = {
		{ L"Path", L"first" },
		{ L"PATH", L"second" },
	};
	const auto block = CExtensionHostProcess::BuildSanitizedEnvironmentBlock(inherited, {});
	ASSERT_GE(block.size(), 2u);
	EXPECT_EQ(L'\0', block[block.size() - 1]);
	EXPECT_EQ(L'\0', block[block.size() - 2]);
	const auto parsed = ParseEnvironmentBlock(block);
	ASSERT_EQ(1u, parsed.size());
	EXPECT_EQ(L"second", parsed.at(L"PATH"));
}

TEST(CExtensionHostProcess, EmptyEnvironmentBlockIsDoubleNullTerminated)
{
	const auto block = CExtensionHostProcess::BuildSanitizedEnvironmentBlock({}, {});
	ASSERT_EQ(2u, block.size());
	EXPECT_EQ(L'\0', block[0]);
	EXPECT_EQ(L'\0', block[1]);
}

TEST(CExtensionHostProcess, EnvironmentValuesCannotInjectAdditionalEntries)
{
	const std::wstring injected = std::wstring(L"safe") + L'\0' + L"API_SECRET=leaked";
	const std::vector<CExtensionHostProcess::EnvironmentEntry> inherited = {
		{ L"PATH", injected },
	};
	const auto parsed = ParseEnvironmentBlock(
		CExtensionHostProcess::BuildSanitizedEnvironmentBlock(inherited, {}));
	EXPECT_TRUE(parsed.empty());
}

TEST(CExtensionHostProcess, QuotesWindowsArgumentsWithoutChangingTheirValue)
{
	EXPECT_EQ(L"plain", CExtensionHostProcess::QuoteWindowsArgument(L"plain"));
	EXPECT_EQ(L"\"two words\"", CExtensionHostProcess::QuoteWindowsArgument(L"two words"));
	EXPECT_EQ(L"\"\"", CExtensionHostProcess::QuoteWindowsArgument(L""));
	EXPECT_EQ(L"\"a\\\\\\\"b\"", CExtensionHostProcess::QuoteWindowsArgument(L"a\\\"b"));
	EXPECT_EQ(L"\"trailing \\\\\"", CExtensionHostProcess::QuoteWindowsArgument(L"trailing \\"));
}

TEST(CExtensionHostProcess, RejectsMissingExecutableWithoutCreatingAProcess)
{
	CExtensionHostProcess process;
	SExtensionHostLaunchOptions options;
	options.nodeExecutable = L"Z:\\missing-node.exe";
	options.hostBundle = L"Z:\\missing-host.js";
	options.profileHash = L"profile";
	options.bootId = L"boot";
	options.pipeName = L"\\\\.\\pipe\\sakura-exthost-profile-boot";

	const auto result = process.Start(options);
	EXPECT_FALSE(result.success);
	EXPECT_EQ(ERROR_FILE_NOT_FOUND, result.errorCode);
	EXPECT_EQ(0u, process.GetProcessId());
	EXPECT_FALSE(process.PollExitCode().has_value());
}
