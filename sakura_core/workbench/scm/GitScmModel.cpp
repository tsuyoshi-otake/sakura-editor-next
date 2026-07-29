/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "workbench/scm/GitScmModel.h"

#include <charconv>

namespace workbench::scm {
namespace {

std::wstring FromUtf8(std::string_view value)
{
	if (value.empty()) return {};
	const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return std::wstring(value.begin(), value.end());
	std::wstring result(static_cast<std::size_t>(length), L'\0');
	::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
	return result;
}

std::string_view FieldAfterSpaces(std::string_view record, int spaces)
{
	std::size_t position = 0;
	for (int count = 0; count < spaces; ++count) {
		position = record.find(' ', position);
		if (position == std::string_view::npos) return {};
		++position;
	}
	return record.substr(position);
}

int ParseSigned(std::string_view value)
{
	if (!value.empty() && value.front() == '+') value.remove_prefix(1);
	int result{};
	std::from_chars(value.data(), value.data() + value.size(), result);
	return result;
}

} // namespace

GitScmState ParsePorcelainV2(std::string_view bytes)
{
	GitScmState state;
	std::size_t position = 0;
	while (position < bytes.size()) {
		const auto end = bytes.find('\0', position);
		const auto record = bytes.substr(position, end == std::string_view::npos ? bytes.size() - position : end - position);
		position = end == std::string_view::npos ? bytes.size() : end + 1;
		if (record.starts_with("# branch.head ")) {
			state.repository = true;
			state.branch = FromUtf8(record.substr(14));
			if (state.branch == L"(detached)") state.branch = L"detached HEAD";
		} else if (record.starts_with("# branch.upstream ")) {
			state.upstream = FromUtf8(record.substr(18));
		} else if (record.starts_with("# branch.ab ")) {
			const auto values = record.substr(12);
			const auto separator = values.find(' ');
			state.ahead = ParseSigned(values.substr(0, separator));
			if (separator != std::string_view::npos) state.behind = -ParseSigned(values.substr(separator + 1));
		} else if (record.starts_with("? ")) {
			state.changes.push_back({ L'U', FromUtf8(record.substr(2)) });
		} else if (record.starts_with("! ")) {
			continue;
		} else if (!record.empty() && (record[0] == '1' || record[0] == '2' || record[0] == 'u')) {
			const wchar_t x = record.size() > 2 ? static_cast<unsigned char>(record[2]) : L'M';
			const wchar_t y = record.size() > 3 ? static_cast<unsigned char>(record[3]) : L'.';
			wchar_t status = y != L'.' ? y : x;
			if (record[0] == 'u') status = L'!';
			if (status == L'?') status = L'U';
			const int pathField = record[0] == '2' ? 9 : record[0] == 'u' ? 10 : 8;
			const auto path = FieldAfterSpaces(record, pathField);
			if (!path.empty()) state.changes.push_back({ status, FromUtf8(path) });
			if (record[0] == '2' && position < bytes.size()) {
				// -z rename records append the original path as an extra NUL record.
				const auto originalEnd = bytes.find('\0', position);
				position = originalEnd == std::string_view::npos ? bytes.size() : originalEnd + 1;
			}
		}
	}
	return state;
}

std::wstring FormatStatusLine(const GitScmState& state)
{
	if (!state.repository) return {};
	std::wstring text = state.branch.empty() ? L"HEAD" : state.branch;
	if (state.ahead > 0) text += L"  \x2191" + std::to_wstring(state.ahead);
	if (state.behind > 0) text += L"  \x2193" + std::to_wstring(state.behind);
	if (!state.changes.empty()) text += L"  " + std::to_wstring(state.changes.size()) + L" changes";
	return text;
}

} // namespace workbench::scm
