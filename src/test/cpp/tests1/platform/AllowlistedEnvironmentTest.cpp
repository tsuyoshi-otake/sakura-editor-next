/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <gtest/gtest.h>
#include "platform/process/AllowlistedEnvironment.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ScopedEnvironmentVariable final {
	explicit ScopedEnvironmentVariable(const wchar_t* name) : name(name)
	{
		std::array<wchar_t, 32768> buffer{};
		const DWORD length = ::GetEnvironmentVariableW(name, buffer.data(),
			static_cast<DWORD>(buffer.size()));
		if (length != 0 && length < buffer.size()) original = std::wstring(buffer.data(), length);
	}
	~ScopedEnvironmentVariable()
	{
		::SetEnvironmentVariableW(name.c_str(),
			original.has_value() ? original->c_str() : nullptr);
	}
	std::wstring name;
	std::optional<std::wstring> original;
};

std::vector<std::wstring> ParseBlock(const std::vector<wchar_t>& block)
{
	std::vector<std::wstring> entries;
	for (const wchar_t* cursor = block.data(); *cursor != L'\0';) {
		entries.emplace_back(cursor);
		cursor += entries.back().size() + 1;
	}
	return entries;
}

std::wstring_view EntryName(std::wstring_view entry)
{
	const auto equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
	return equals == std::wstring_view::npos ? entry : entry.substr(0, equals);
}

bool EqualName(std::wstring_view left, std::wstring_view right)
{
	return left.size() == right.size()
		&& ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

TEST(AllowlistedEnvironment, BlockCopiesOnlyAllowedNamesSortedAndTerminated)
{
	// Secrets and an unrelated marker must never reach an allowlisted child;
	// they exist in the test process only to prove the block excludes them.
	ScopedEnvironmentVariable ghToken(L"GH_TOKEN");
	ScopedEnvironmentVariable awsSecret(L"AWS_SECRET_ACCESS_KEY");
	ScopedEnvironmentVariable marker(L"B6D_MARKER");
	ScopedEnvironmentVariable mixed(L"b6dMiXeD");
	ASSERT_TRUE(::SetEnvironmentVariableW(L"GH_TOKEN", L"gh-secret"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"AWS_SECRET_ACCESS_KEY", L"aws-secret"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"B6D_MARKER", L"marker-value"));
	ASSERT_TRUE(::SetEnvironmentVariableW(L"b6dMiXeD", L"mixed-value"));

	std::array<wchar_t, 32768> expected{};
	ASSERT_LT(::GetEnvironmentVariableW(L"SystemRoot", expected.data(),
		static_cast<DWORD>(expected.size())), expected.size());
	const std::wstring expectedSystemRoot{ expected.data() };
	expected.fill(L'\0');
	ASSERT_LT(::GetEnvironmentVariableW(L"windir", expected.data(),
		static_cast<DWORD>(expected.size())), expected.size());
	const std::wstring expectedWindir{ expected.data() };

	const std::array<std::wstring_view, 4> names{
		L"SystemRoot", L"windir", L"B6DMIXED", L"B6D_ABSENT",
	};
	const auto block = platform::process::BuildAllowlistedEnvironmentBlock(names);

	// Double-NUL terminated.
	ASSERT_GE(block.size(), 2u);
	EXPECT_EQ(block[block.size() - 1], L'\0');
	EXPECT_EQ(block[block.size() - 2], L'\0');

	const auto entries = ParseBlock(block);
	std::vector<std::wstring> wanted;
	for (const auto& entry : entries) {
		const auto name = EntryName(entry);
		const bool allowed = std::any_of(names.begin(), names.end(),
			[name](std::wstring_view allowedName) { return EqualName(name, allowedName); });
		ASSERT_TRUE(allowed);
		wanted.emplace_back(entry);
	}

	// Names match case-insensitively: the variable stored as "b6dMiXeD" is
	// found through the allowlist entry "B6DMIXED".
	EXPECT_NE(std::find_if(entries.begin(), entries.end(), [](const std::wstring& e) {
		return EqualName(EntryName(e), L"B6DMIXED") && e.substr(e.find(L'=') + 1) == L"mixed-value";
	}), entries.end());
	// A requested name the parent does not define is simply absent.
	EXPECT_EQ(std::find_if(entries.begin(), entries.end(), [](const std::wstring& e) {
		return EqualName(EntryName(e), L"B6D_ABSENT");
	}), entries.end());

	// Exactly the allowed names, with the parent's own values. The entry keeps
	// the parent's own name casing (this box stores SYSTEMROOT/WINDIR), so the
	// name is matched case-insensitively and the value exactly.
	const auto entryValue = [](const std::wstring& e) { return e.substr(e.find(L'=') + 1); };
	EXPECT_NE(std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
		return EqualName(EntryName(e), L"SystemRoot") && entryValue(e) == expectedSystemRoot;
	}), entries.end());
	EXPECT_NE(std::find_if(entries.begin(), entries.end(), [&](const std::wstring& e) {
		return EqualName(EntryName(e), L"windir") && entryValue(e) == expectedWindir;
	}), entries.end());
	EXPECT_EQ(std::find_if(entries.begin(), entries.end(), [](const std::wstring& e) {
		return EqualName(EntryName(e), L"GH_TOKEN")
			|| EqualName(EntryName(e), L"AWS_SECRET_ACCESS_KEY")
			|| EqualName(EntryName(e), L"B6D_MARKER");
	}), entries.end());

	// Sorted case-insensitively as CreateProcessW requires.
	const auto sorted = wanted;
	EXPECT_TRUE(std::is_sorted(sorted.begin(), sorted.end(), [](const std::wstring& l, const std::wstring& r) {
		return std::lexicographical_compare(l.begin(), l.end(), r.begin(), r.end(),
			[](wchar_t a, wchar_t b) { return std::towupper(a) < std::towupper(b); });
	}));
}

TEST(AllowlistedEnvironment, EmptyAllowlistProducesEmptyBlock)
{
	const auto block = platform::process::BuildAllowlistedEnvironmentBlock({});
	// Two terminators, not one: CreateProcessW scans for the double NUL even
	// when the block holds no entry.
	ASSERT_EQ(block.size(), 2u);
	EXPECT_EQ(block[0], L'\0');
	EXPECT_EQ(block[1], L'\0');
}

} // namespace
