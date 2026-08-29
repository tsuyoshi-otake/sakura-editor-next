/*! @file @brief Process entry for Sakura-scoped terminal orchestration tools. */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/cli/SakuraHarnessProcessEntry.h"
#include "terminal/cli/SakuraTmuxProcessEntry.h"

#include <cstddef>
#include <string_view>

namespace {

[[nodiscard]] bool IsHarnessExecutable(const wchar_t* const path) noexcept
{
	if (path == nullptr) return false;
	constexpr std::size_t kMaximumPathWideChars = 32u * 1024u;
	std::size_t length = 0;
	while (length <= kMaximumPathWideChars && path[length] != L'\0') ++length;
	if (length > kMaximumPathWideChars) return false;
	std::wstring_view baseName(path, length);
	const auto separator = baseName.find_last_of(L"\\/");
	if (separator != std::wstring_view::npos) baseName.remove_prefix(separator + 1);
	constexpr std::wstring_view expected = L"sakura-harness.exe";
	if (baseName.size() != expected.size()) return false;
	for (std::size_t index = 0; index < expected.size(); ++index) {
		const auto lower = [](const wchar_t value) noexcept {
			return value >= L'A' && value <= L'Z'
				? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
		};
		if (lower(baseName[index]) != expected[index]) return false;
	}
	return true;
}

} // namespace

int wmain(const int argc, wchar_t* const* argv) noexcept
{
	if (argc > 0 && argv != nullptr && IsHarnessExecutable(argv[0])) {
		return terminal::cli::SakuraHarnessCliMain(argc, argv);
	}
	return terminal::cli::SakuraTmuxCliMain(argc, argv);
}
