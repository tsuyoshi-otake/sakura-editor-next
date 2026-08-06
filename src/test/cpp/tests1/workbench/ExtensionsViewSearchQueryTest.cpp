/*! @file
	@brief Tests for workbench::extension::ParseExtensionSearchQuery (Issue #23 gap 2: @filter search syntax)
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/extension/ExtensionSearchQuery.h"

namespace {
using workbench::extension::ExtensionSearchFilter;
using workbench::extension::ExtensionSearchSortKey;
using workbench::extension::ParseExtensionSearchQuery;
} // namespace

TEST(ExtensionsViewSearchQuery, EmptyQueryHasNoFiltersAndNoText)
{
	const auto parsed = ParseExtensionSearchQuery(L"");
	EXPECT_TRUE(parsed.filters.empty());
	EXPECT_EQ(parsed.sortKey, ExtensionSearchSortKey::Relevance);
	EXPECT_TRUE(parsed.searchText.empty());
	EXPECT_TRUE(parsed.unknownTokens.empty());
}

TEST(ExtensionsViewSearchQuery, PlainTextWithNoAtTokenIsFreeTextOnly)
{
	const auto parsed = ParseExtensionSearchQuery(L"python formatter");
	EXPECT_TRUE(parsed.filters.empty());
	EXPECT_EQ(parsed.searchText, L"python formatter");
}

TEST(ExtensionsViewSearchQuery, RecognizesInstalledFilter)
{
	const auto parsed = ParseExtensionSearchQuery(L"@installed");
	ASSERT_EQ(parsed.filters.size(), 1u);
	EXPECT_EQ(parsed.filters[0], ExtensionSearchFilter::Installed);
	EXPECT_TRUE(parsed.searchText.empty());
}

TEST(ExtensionsViewSearchQuery, MatchingIsCaseInsensitive)
{
	const auto parsed = ParseExtensionSearchQuery(L"@INSTALLED");
	ASSERT_EQ(parsed.filters.size(), 1u);
	EXPECT_EQ(parsed.filters[0], ExtensionSearchFilter::Installed);
}

TEST(ExtensionsViewSearchQuery, UpdatesIsAnAliasForOutdated)
{
	const auto viaOutdated = ParseExtensionSearchQuery(L"@outdated");
	const auto viaUpdates = ParseExtensionSearchQuery(L"@updates");
	ASSERT_EQ(viaOutdated.filters.size(), 1u);
	ASSERT_EQ(viaUpdates.filters.size(), 1u);
	EXPECT_EQ(viaOutdated.filters[0], ExtensionSearchFilter::Outdated);
	EXPECT_EQ(viaUpdates.filters[0], ExtensionSearchFilter::Outdated);
}

TEST(ExtensionsViewSearchQuery, CombinesFilterAndFreeTextInAnyOrder)
{
	const auto parsed = ParseExtensionSearchQuery(L"git @installed lens");
	ASSERT_EQ(parsed.filters.size(), 1u);
	EXPECT_EQ(parsed.filters[0], ExtensionSearchFilter::Installed);
	EXPECT_EQ(parsed.searchText, L"git lens");
}

TEST(ExtensionsViewSearchQuery, DuplicateFilterTokensAreIdempotent)
{
	const auto parsed = ParseExtensionSearchQuery(L"@installed @installed");
	EXPECT_EQ(parsed.filters.size(), 1u);
}

TEST(ExtensionsViewSearchQuery, ParsesSortDirective)
{
	const auto parsed = ParseExtensionSearchQuery(L"@sort:installs");
	EXPECT_EQ(parsed.sortKey, ExtensionSearchSortKey::InstallCount);
	EXPECT_TRUE(parsed.filters.empty());
}

TEST(ExtensionsViewSearchQuery, LastSortDirectiveWins)
{
	const auto parsed = ParseExtensionSearchQuery(L"@sort:installs @sort:name");
	EXPECT_EQ(parsed.sortKey, ExtensionSearchSortKey::Name);
}

TEST(ExtensionsViewSearchQuery, UnsupportedSortKeyIsReportedAsUnknownNotSilentlyAccepted)
{
	// @sort:updateDate is real VS Code syntax, but SOpenVsxExtension has no
	// timestamp field to sort by; it must not be misreported as Relevance.
	const auto parsed = ParseExtensionSearchQuery(L"@sort:updateDate");
	EXPECT_EQ(parsed.sortKey, ExtensionSearchSortKey::Relevance);
	ASSERT_EQ(parsed.unknownTokens.size(), 1u);
	EXPECT_EQ(parsed.unknownTokens[0], L"@sort:updateDate");
}

TEST(ExtensionsViewSearchQuery, TokensThisModelCannotEvaluateAreReportedAsUnknown)
{
	// @category:/@tag:/@builtin/@ext:/@id:/@workspaceUnsupported are real VS
	// Code tokens this DTO cannot back; they must be surfaced, not swallowed.
	const auto parsed = ParseExtensionSearchQuery(L"@category:themes @builtin");
	EXPECT_TRUE(parsed.filters.empty());
	ASSERT_EQ(parsed.unknownTokens.size(), 2u);
	EXPECT_EQ(parsed.unknownTokens[0], L"@category:themes");
	EXPECT_EQ(parsed.unknownTokens[1], L"@builtin");
	EXPECT_TRUE(parsed.searchText.empty());
}

TEST(ExtensionsViewSearchQuery, CollapsesRepeatedWhitespaceBetweenFreeTextWords)
{
	const auto parsed = ParseExtensionSearchQuery(L"  git   lens  ");
	EXPECT_EQ(parsed.searchText, L"git lens");
}

TEST(ExtensionsViewSearchQuery, RecognizesDeprecatedAndRecommendedFilters)
{
	const auto deprecated = ParseExtensionSearchQuery(L"@deprecated");
	const auto recommended = ParseExtensionSearchQuery(L"@recommended");
	ASSERT_EQ(deprecated.filters.size(), 1u);
	ASSERT_EQ(recommended.filters.size(), 1u);
	EXPECT_EQ(deprecated.filters[0], ExtensionSearchFilter::Deprecated);
	EXPECT_EQ(recommended.filters[0], ExtensionSearchFilter::Recommended);
}
