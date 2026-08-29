/*! @file @brief Tests for scoped integrated-terminal child environments. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "StdAfx.h"

#include "terminal/session/TerminalEnvironment.h"

#include <gtest/gtest.h>

#include <cwchar>
#include <span>
#include <string>
#include <vector>

namespace terminal {
namespace {

std::vector<std::wstring> DecodeBlock( const std::vector<wchar_t>& block )
{
	std::vector<std::wstring> entries;
	for( const wchar_t* entry = block.data(); entry < block.data() + block.size() && *entry != L'\0';
		entry += std::wcslen(entry) + 1 ) entries.emplace_back(entry);
	return entries;
}

const std::wstring* FindBlockEntry( const std::vector<std::wstring>& entries, std::wstring_view name )
{
	for( const auto& entry : entries ) {
		const auto separator = entry.find(L'=');
		if( separator == std::wstring::npos ) continue;
		if( entry.substr(0, separator) == name ) return &entry;
	}
	return nullptr;
}

TEST(TerminalEnvironment, AppliesOverridesAndScopedPathWithoutMutatingInputs)
{
	const std::vector<std::wstring> inherited{
		L"NO_COLOR=1", L"PATH=C:\\Windows\\System32", L"TERM=dumb", L"UNCHANGED=value"
	};
	TerminalLaunchOptions options;
	options.environmentOverrides = {
		{ L"SAKURA_HARNESS_PIPE", L"\\\\.\\pipe\\sakura-harness-test" },
		{ L"UNCHANGED", std::nullopt },
	};
	options.prependPathDirectories = { L"C:\\Program Files\\Sakura Editor\\terminal-tools" };

	const auto result = BuildTerminalEnvironmentBlock(options, inherited);
	ASSERT_TRUE(result.Succeeded());
	ASSERT_GE(result.block.size(), 2U);
	EXPECT_EQ(L'\0', result.block[result.block.size() - 1]);
	EXPECT_EQ(L'\0', result.block[result.block.size() - 2]);
	const auto entries = DecodeBlock(result.block);
	EXPECT_EQ(nullptr, FindBlockEntry(entries, L"NO_COLOR"));
	EXPECT_EQ(nullptr, FindBlockEntry(entries, L"UNCHANGED"));
	ASSERT_NE(nullptr, FindBlockEntry(entries, L"PATH"));
	EXPECT_EQ(L"PATH=C:\\Program Files\\Sakura Editor\\terminal-tools;C:\\Windows\\System32",
		*FindBlockEntry(entries, L"PATH"));
	EXPECT_EQ(L"TERM=xterm-256color", *FindBlockEntry(entries, L"TERM"));
	EXPECT_EQ(L"SAKURA_HARNESS_PIPE=\\\\.\\pipe\\sakura-harness-test",
		*FindBlockEntry(entries, L"SAKURA_HARNESS_PIPE"));
	EXPECT_EQ(L"PATH=C:\\Windows\\System32", inherited[1]);
}

TEST(TerminalEnvironment, RejectsDuplicateCaseInsensitiveOverrides)
{
	TerminalLaunchOptions options;
	options.environmentOverrides = { { L"TOKEN", L"one" }, { L"token", L"two" } };
	const auto result = BuildTerminalEnvironmentBlock(options, std::span<const std::wstring>{});
	EXPECT_EQ(TerminalEnvironmentBuildStatus::InvalidOverride, result.status);
	EXPECT_TRUE(result.block.empty());
}

TEST(TerminalEnvironment, RejectsRelativeOrDelimitedPathDirectories)
{
	TerminalLaunchOptions options;
	options.prependPathDirectories = { L"relative\\tools" };
	EXPECT_EQ(TerminalEnvironmentBuildStatus::InvalidPathDirectory,
		BuildTerminalEnvironmentBlock(options, std::span<const std::wstring>{}).status);
	options.prependPathDirectories = { L"C:\\trusted;C:\\other" };
	EXPECT_EQ(TerminalEnvironmentBuildStatus::InvalidPathDirectory,
		BuildTerminalEnvironmentBlock(options, std::span<const std::wstring>{}).status);
}

TEST(TerminalEnvironment, PrependsTrustedDirectoryExactlyOnce)
{
	const std::vector<std::wstring> inherited{
		L"PATH=C:\\Program Files\\Sakura Editor\\terminal-tools\\;C:\\Windows\\System32"
	};
	TerminalLaunchOptions options;
	options.prependPathDirectories = {
		L"c:\\program files\\sakura editor\\terminal-tools",
		L"C:\\Program Files\\Sakura Editor\\terminal-tools\\",
	};

	const auto result = BuildTerminalEnvironmentBlock(options, inherited);
	ASSERT_TRUE(result.Succeeded());
	const auto entries = DecodeBlock(result.block);
	ASSERT_NE(nullptr, FindBlockEntry(entries, L"PATH"));
	EXPECT_EQ(inherited.front(), *FindBlockEntry(entries, L"PATH"));
}

TEST(TerminalEnvironment, RejectsOversizedEnvironmentBeforeCreateProcess)
{
	std::vector<std::wstring> inherited;
	for( int index = 0; index < 4; ++index ) {
		inherited.emplace_back(L"LARGE" + std::to_wstring(index) + L"=" + std::wstring(9000, L'x'));
	}
	const auto result = BuildTerminalEnvironmentBlock(TerminalLaunchOptions{}, inherited);
	EXPECT_EQ(TerminalEnvironmentBuildStatus::TooLarge, result.status);
	EXPECT_TRUE(result.block.empty());
}

} // namespace
} // namespace terminal
