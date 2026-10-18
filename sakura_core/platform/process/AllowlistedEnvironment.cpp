/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/process/AllowlistedEnvironment.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace platform::process {
namespace {

bool EqualEnvironmentName(std::wstring_view left, std::wstring_view right) noexcept
{
	return left.size() == right.size()
		&& ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

std::vector<wchar_t> BuildAllowlistedEnvironmentBlock(std::span<const std::wstring_view> names)
{
	std::vector<std::wstring> entries;
	wchar_t* const parent = ::GetEnvironmentStringsW();
	if (parent != nullptr) {
		for (const wchar_t* cursor = parent; *cursor != L'\0';) {
			const std::wstring_view entry{ cursor };
			cursor += entry.size() + 1;
			const auto equals = entry.find(L'=', entry.starts_with(L'=') ? 1 : 0);
			const auto name = equals == std::wstring_view::npos ? entry : entry.substr(0, equals);
			const bool allowed = std::any_of(names.begin(), names.end(),
				[name](std::wstring_view allowedName) { return EqualEnvironmentName(name, allowedName); });
			if (allowed) entries.emplace_back(entry);
		}
		::FreeEnvironmentStringsW(parent);
	}
	std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
		return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
			[](wchar_t a, wchar_t b) { return std::towupper(a) < std::towupper(b); });
	});
	// An empty block still needs both terminators: CreateProcessW reads the
	// environment up to the first double NUL.
	std::size_t length = entries.empty() ? 2 : 1;
	for (const auto& entry : entries) length += entry.size() + 1;
	std::vector<wchar_t> block;
	block.reserve(length);
	for (const auto& entry : entries) {
		block.insert(block.end(), entry.begin(), entry.end());
		block.push_back(L'\0');
	}
	if (entries.empty()) block.push_back(L'\0');
	block.push_back(L'\0');
	return block;
}

} // namespace platform::process
