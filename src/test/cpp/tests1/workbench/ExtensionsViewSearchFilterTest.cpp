/*! @file
	@brief Tests for workbench::extension::ApplyExtensionSearchFilters (Issue #23 gap 2: wiring @filter search into the list)
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/extension/ExtensionSearchFilter.h"

namespace {

using workbench::extension::ApplyExtensionSearchFilters;
using workbench::extension::ExtensionSearchCandidate;
using workbench::extension::ExtensionSearchFilter;
using workbench::extension::ExtensionSearchSortKey;
using workbench::extension::ParsedExtensionSearchQuery;

ExtensionSearchCandidate MakeCandidate(
	const wchar_t* uniqueName,
	const wchar_t* displayName,
	const std::wstring& installedVersion = L"",
	bool enabled = false,
	long long downloads = 0,
	double rating = -1.0,
	bool deprecated = false,
	const wchar_t* latestVersion = L"1.0.0" )
{
	ExtensionSearchCandidate candidate;
	candidate.extension.sNamespace = L"pub";
	candidate.extension.sName = uniqueName;
	candidate.extension.sDisplayName = displayName;
	candidate.extension.sDescription = L"a description";
	candidate.extension.sVersion = latestVersion;
	candidate.extension.nDownloadCount = downloads;
	candidate.extension.dAverageRating = rating;
	candidate.extension.bDeprecated = deprecated;
	candidate.installedVersion = installedVersion;
	candidate.enabled = enabled;
	return candidate;
}

} // namespace

TEST(ExtensionsViewSearchFilter, EmptyQueryKeepsEveryCandidateInOrder)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha"),
		MakeCandidate(L"b", L"Beta"),
	};
	const ParsedExtensionSearchQuery query;
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 2u);
	EXPECT_EQ(indices[0], 0u);
	EXPECT_EQ(indices[1], 1u);
}

TEST(ExtensionsViewSearchFilter, InstalledFilterExcludesNotInstalled)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"1.0.0"),
		MakeCandidate(L"b", L"Beta"), // not installed
	};
	ParsedExtensionSearchQuery query;
	query.filters.push_back(ExtensionSearchFilter::Installed);
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 1u);
	EXPECT_EQ(indices[0], 0u);
}

TEST(ExtensionsViewSearchFilter, EnabledAndDisabledFiltersPartitionInstalledSet)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"1.0.0", /*enabled=*/true),
		MakeCandidate(L"b", L"Beta", L"1.0.0", /*enabled=*/false),
		MakeCandidate(L"c", L"Gamma"), // not installed: excluded from both
	};
	ParsedExtensionSearchQuery enabledQuery;
	enabledQuery.filters.push_back(ExtensionSearchFilter::Enabled);
	const auto enabledIndices = ApplyExtensionSearchFilters(enabledQuery, candidates);
	ASSERT_EQ(enabledIndices.size(), 1u);
	EXPECT_EQ(enabledIndices[0], 0u);

	ParsedExtensionSearchQuery disabledQuery;
	disabledQuery.filters.push_back(ExtensionSearchFilter::Disabled);
	const auto disabledIndices = ApplyExtensionSearchFilters(disabledQuery, candidates);
	ASSERT_EQ(disabledIndices.size(), 1u);
	EXPECT_EQ(disabledIndices[0], 1u);
}

TEST(ExtensionsViewSearchFilter, OutdatedRequiresAKnownNewerVersion)
{
	std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"1.0.0", true, 0, -1.0, false, L"2.0.0"), // installed 1.0.0, latest known 2.0.0
		MakeCandidate(L"b", L"Beta", L"1.0.0", true, 0, -1.0, false, L"1.0.0"),  // up to date
	};
	ParsedExtensionSearchQuery query;
	query.filters.push_back(ExtensionSearchFilter::Outdated);
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 1u);
	EXPECT_EQ(indices[0], 0u);
}

TEST(ExtensionsViewSearchFilter, OutdatedYieldsNothingWhenNoLatestVersionIsKnown)
{
	// An installed-only enumeration has no marketplace-known latest version, so
	// installedVersion and extension.sVersion are the same value by
	// construction. Outdated must not fabricate a positive match here.
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"1.0.0", true, 0, -1.0, false, L"1.0.0"),
	};
	ParsedExtensionSearchQuery query;
	query.filters.push_back(ExtensionSearchFilter::Outdated);
	EXPECT_TRUE(ApplyExtensionSearchFilters(query, candidates).empty());
}

TEST(ExtensionsViewSearchFilter, DeprecatedFilterMatchesOnlyDeprecatedExtensions)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"", false, 0, -1.0, /*deprecated=*/true),
		MakeCandidate(L"b", L"Beta"),
	};
	ParsedExtensionSearchQuery query;
	query.filters.push_back(ExtensionSearchFilter::Deprecated);
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 1u);
	EXPECT_EQ(indices[0], 0u);
}

TEST(ExtensionsViewSearchFilter, FreeTextRequiresEveryTermAsASubstringCaseInsensitively)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"formatter", L"Python Formatter"),
		MakeCandidate(L"lens", L"Git Lens"),
	};
	ParsedExtensionSearchQuery query;
	query.searchText = L"PYTHON format";
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 1u);
	EXPECT_EQ(indices[0], 0u);
}

TEST(ExtensionsViewSearchFilter, EmptySearchTextSkipsFreeTextFilteringEntirely)
{
	// Simulates the marketplace-scope refinement pass: the text was already sent
	// to the registry, so it must not be re-applied as a local substring filter.
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Totally Unrelated Name"),
	};
	ParsedExtensionSearchQuery query;
	query.filters.push_back(ExtensionSearchFilter::Deprecated);
	// intentionally left with searchText empty
	EXPECT_TRUE(ApplyExtensionSearchFilters(query, candidates).empty()); // not deprecated
}

TEST(ExtensionsViewSearchFilter, SortByNameIsAscendingCaseInsensitive)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"b", L"beta"),
		MakeCandidate(L"a", L"Alpha"),
	};
	ParsedExtensionSearchQuery query;
	query.sortKey = ExtensionSearchSortKey::Name;
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 2u);
	EXPECT_EQ(indices[0], 1u); // Alpha
	EXPECT_EQ(indices[1], 0u); // beta
}

TEST(ExtensionsViewSearchFilter, SortByInstallCountIsDescending)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"", false, 10),
		MakeCandidate(L"b", L"Beta", L"", false, 1000),
	};
	ParsedExtensionSearchQuery query;
	query.sortKey = ExtensionSearchSortKey::InstallCount;
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 2u);
	EXPECT_EQ(indices[0], 1u);
	EXPECT_EQ(indices[1], 0u);
}

TEST(ExtensionsViewSearchFilter, SortByRatingPutsUnratedCandidatesLast)
{
	const std::vector<ExtensionSearchCandidate> candidates = {
		MakeCandidate(L"a", L"Alpha", L"", false, 0, -1.0), // unrated
		MakeCandidate(L"b", L"Beta", L"", false, 0, 4.5),
	};
	ParsedExtensionSearchQuery query;
	query.sortKey = ExtensionSearchSortKey::Rating;
	const auto indices = ApplyExtensionSearchFilters(query, candidates);
	ASSERT_EQ(indices.size(), 2u);
	EXPECT_EQ(indices[0], 1u); // rated first
	EXPECT_EQ(indices[1], 0u); // unrated last
}
