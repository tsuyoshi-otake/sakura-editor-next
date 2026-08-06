/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/extension/ExtensionSearchFilter.h"

#include <algorithm>
#include <cwctype>

namespace workbench::extension {
namespace {

std::wstring ToLowerCopy(std::wstring_view value)
{
	std::wstring lowered(value);
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
	return lowered;
}

std::vector<std::wstring> SplitLowerTerms(std::wstring_view text)
{
	std::vector<std::wstring> terms;
	std::size_t start = 0;
	while (start < text.size()) {
		while (start < text.size() && std::iswspace(text[start])) ++start;
		if (start >= text.size()) break;
		std::size_t end = start;
		while (end < text.size() && !std::iswspace(text[end])) ++end;
		terms.push_back(ToLowerCopy(text.substr(start, end - start)));
		start = end;
	}
	return terms;
}

std::wstring DisplayNameOf(const SOpenVsxExtension& extension)
{
	return extension.sDisplayName.empty() ? extension.sName : extension.sDisplayName;
}

bool MatchesFreeText(const ExtensionSearchCandidate& candidate, const std::vector<std::wstring>& lowerTerms)
{
	if (lowerTerms.empty()) return true;
	const std::wstring haystack = ToLowerCopy(DisplayNameOf(candidate.extension))
		+ L'\n' + ToLowerCopy(candidate.extension.sName)
		+ L'\n' + ToLowerCopy(candidate.extension.sNamespace)
		+ L'\n' + ToLowerCopy(candidate.extension.sDescription);
	for (const std::wstring& term : lowerTerms) {
		if (haystack.find(term) == std::wstring::npos) return false;
	}
	return true;
}

bool PassesItemFilters(const ExtensionSearchFilter filter, const ExtensionSearchCandidate& candidate)
{
	const bool installed = !candidate.installedVersion.empty();
	switch (filter) {
	case ExtensionSearchFilter::Installed:
		return installed;
	case ExtensionSearchFilter::Enabled:
		return installed && candidate.enabled;
	case ExtensionSearchFilter::Disabled:
		return installed && !candidate.enabled;
	case ExtensionSearchFilter::Outdated:
		// Equal by construction whenever both fields came from the same source
		// (e.g. an installed-only enumeration with no marketplace fetch), so this
		// honestly reports "no known outdated extensions" rather than guessing.
		return installed && candidate.installedVersion != candidate.extension.sVersion;
	case ExtensionSearchFilter::Deprecated:
		return candidate.extension.bDeprecated;
	case ExtensionSearchFilter::Recommended:
		// Never reached: callers must reject Recommended before calling this
		// function (see the header). Fail closed defensively if one does not.
		return false;
	}
	return false;
}

} // namespace

std::vector<std::size_t> ApplyExtensionSearchFilters(
	const ParsedExtensionSearchQuery& query,
	std::span<const ExtensionSearchCandidate> candidates)
{
	const std::vector<std::wstring> lowerTerms = SplitLowerTerms(query.searchText);

	std::vector<std::size_t> indices;
	indices.reserve(candidates.size());
	for (std::size_t i = 0; i < candidates.size(); ++i) {
		const ExtensionSearchCandidate& candidate = candidates[i];
		bool passes = true;
		for (const ExtensionSearchFilter filter : query.filters) {
			if (!PassesItemFilters(filter, candidate)) {
				passes = false;
				break;
			}
		}
		if (passes && MatchesFreeText(candidate, lowerTerms)) {
			indices.push_back(i);
		}
	}

	switch (query.sortKey) {
	case ExtensionSearchSortKey::Relevance:
		// No resort: preserve the caller's input order.
		break;
	case ExtensionSearchSortKey::Name:
		std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
			return ToLowerCopy(DisplayNameOf(candidates[a].extension))
				< ToLowerCopy(DisplayNameOf(candidates[b].extension));
		});
		break;
	case ExtensionSearchSortKey::InstallCount:
		std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
			return candidates[a].extension.nDownloadCount > candidates[b].extension.nDownloadCount;
		});
		break;
	case ExtensionSearchSortKey::Rating:
		std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
			const bool ratedA = candidates[a].extension.HasRating();
			const bool ratedB = candidates[b].extension.HasRating();
			if (ratedA != ratedB) return ratedA; // rated candidates sort before unrated ones
			if (!ratedA) return false; // both unrated: keep relative order
			return candidates[a].extension.dAverageRating > candidates[b].extension.dAverageRating;
		});
		break;
	}

	return indices;
}

} // namespace workbench::extension
