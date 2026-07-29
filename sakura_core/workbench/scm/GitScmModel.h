/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace workbench::scm {

struct GitChange {
	wchar_t status = L'M';
	std::wstring path;
};

struct GitScmState {
	bool repository = false;
	std::wstring branch;
	std::wstring upstream;
	int ahead = 0;
	int behind = 0;
	std::vector<GitChange> changes;
};

[[nodiscard]] GitScmState ParsePorcelainV2(std::string_view bytes);
[[nodiscard]] std::wstring FormatStatusLine(const GitScmState& state);

} // namespace workbench::scm
