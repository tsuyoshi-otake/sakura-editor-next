/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extension/ExtensionSearchQuery.h"

#include <algorithm>
#include <cwctype>
#include <optional>

namespace workbench::extension {
namespace {

std::wstring ToLowerCopy(std::wstring_view value)
{
	std::wstring lowered(value);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
	return lowered;
}

//! Splits on ASCII whitespace only. VS Code's own query tokenizer is likewise
//! whitespace-delimited; quoted multi-word filter values (e.g. `@category:"a b"`)
//! are not part of this model's recognized token set (see the header), so no
//! quote-aware splitting is needed.
std::vector<std::wstring_view> SplitTokens(std::wstring_view query)
{
	std::vector<std::wstring_view> tokens;
	std::size_t start = 0;
	while (start < query.size()) {
		while (start < query.size() && std::iswspace(query[start])) ++start;
		if (start >= query.size()) break;
		std::size_t end = start;
		while (end < query.size() && !std::iswspace(query[end])) ++end;
		tokens.push_back(query.substr(start, end - start));
		start = end;
	}
	return tokens;
}

//! Recognized bare `@token` filters. `@updates` is VS Code's own alias for
//! `@outdated` (both appear in its Extensions view), so both map to the same
//! filter here.
[[nodiscard]] std::optional<ExtensionSearchFilter> MatchFilterToken(const std::wstring& lowered)
{
	if (lowered == L"@installed") return ExtensionSearchFilter::Installed;
	if (lowered == L"@disabled") return ExtensionSearchFilter::Disabled;
	if (lowered == L"@enabled") return ExtensionSearchFilter::Enabled;
	if (lowered == L"@outdated" || lowered == L"@updates") return ExtensionSearchFilter::Outdated;
	if (lowered == L"@deprecated") return ExtensionSearchFilter::Deprecated;
	if (lowered == L"@recommended") return ExtensionSearchFilter::Recommended;
	return std::nullopt;
}

[[nodiscard]] std::optional<ExtensionSearchSortKey> MatchSortToken(const std::wstring& lowered)
{
	constexpr std::wstring_view kPrefix = L"@sort:";
	if (lowered.size() <= kPrefix.size() || lowered.compare(0, kPrefix.size(), kPrefix) != 0) return std::nullopt;
	const std::wstring key = lowered.substr(kPrefix.size());
	if (key == L"installs") return ExtensionSearchSortKey::InstallCount;
	if (key == L"rating") return ExtensionSearchSortKey::Rating;
	if (key == L"name") return ExtensionSearchSortKey::Name;
	return std::nullopt;
}

} // namespace

bool ParsedExtensionSearchQuery::HasFilter(ExtensionSearchFilter filter) const noexcept
{
	return std::find(filters.begin(), filters.end(), filter) != filters.end();
}

ParsedExtensionSearchQuery ParseExtensionSearchQuery(std::wstring_view query)
{
	ParsedExtensionSearchQuery result;
	std::wstring freeText;

	for (const std::wstring_view token : SplitTokens(query)) {
		if (token.empty() || token.front() != L'@') {
			if (!freeText.empty()) freeText += L' ';
			freeText += token;
			continue;
		}

		const std::wstring lowered = ToLowerCopy(token);
		if (const auto filter = MatchFilterToken(lowered)) {
			// A repeated filter token is idempotent, matching VS Code's own
			// query box (typing "@installed @installed" is not double-filtered).
			if (!result.HasFilter(*filter)) result.filters.push_back(*filter);
			continue;
		}
		if (const auto sortKey = MatchSortToken(lowered)) {
			// The last @sort: token wins, matching a single-valued directive
			// rather than silently picking the first one seen.
			result.sortKey = *sortKey;
			continue;
		}
		result.unknownTokens.emplace_back(token);
	}

	result.searchText = std::move(freeText);
	return result;
}

} // namespace workbench::extension
