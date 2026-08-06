/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include "update/UpdateVersion.h"

using update::IsNewerBuild;
using update::ParseProductVersion;
using update::ParseReleaseTag;
using update::UpdateVersion;

TEST(UpdateVersion, ParsesTheFourFieldProductVersionAndRendersItBack)
{
	const auto parsed = ParseProductVersion(L"3.1.0.7221");
	ASSERT_TRUE(parsed.has_value());
	EXPECT_EQ(3u, parsed->major);
	EXPECT_EQ(1u, parsed->minor);
	EXPECT_EQ(0u, parsed->patch);
	EXPECT_EQ(7221u, parsed->revision);
	EXPECT_EQ(L"3.1.0.7221", parsed->ToProductVersion());
	EXPECT_EQ(L"v3.1.0-build.7221", parsed->ToReleaseTag());
}

TEST(UpdateVersion, ParsesAReleaseTagWithOrWithoutTheLeadingV)
{
	const auto withV = ParseReleaseTag(L"v3.1.0-build.7221");
	const auto withoutV = ParseReleaseTag(L"3.1.0-build.7221");
	const auto upperV = ParseReleaseTag(L"V3.1.0-build.7221");
	ASSERT_TRUE(withV.has_value());
	ASSERT_TRUE(withoutV.has_value());
	ASSERT_TRUE(upperV.has_value());
	EXPECT_EQ(*withV, *withoutV);
	EXPECT_EQ(*withV, *upperV);
	EXPECT_EQ(L"v3.1.0-build.7221", withV->ToReleaseTag());
}

TEST(UpdateVersion, RefusesAPartialVersionInsteadOfZeroFillingIt)
{
	// A zero-filled revision would compare older than every published release and
	// would therefore offer an endless update.
	EXPECT_FALSE(ParseProductVersion(L"3.1.0").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1").has_value());
	EXPECT_FALSE(ParseProductVersion(L"").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1.0.7221.5").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1.0.").has_value());
	EXPECT_FALSE(ParseProductVersion(L".1.0.7221").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1.0.7221 ").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1.0.72a1").has_value());
	EXPECT_FALSE(ParseProductVersion(L"3.1.0.1234567890").has_value());
}

TEST(UpdateVersion, RefusesATagThatIsNotThisProductsReleaseGrammar)
{
	EXPECT_FALSE(ParseReleaseTag(L"v3.1.0").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"3.1.0-build").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"3.1.0-build.").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"3.1.0-rc.7221").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"v3.1.0-build.7221-hotfix").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"").has_value());
	EXPECT_FALSE(ParseReleaseTag(L"vv3.1.0-build.7221").has_value());
}

TEST(UpdateVersion, OrdersByCommitCountRevisionBeforeTheMarketingFields)
{
	const UpdateVersion olderMarketingNewerBuild{ 3, 1, 0, 7300 };
	const UpdateVersion newerMarketingOlderBuild{ 4, 0, 0, 7200 };
	EXPECT_LT(newerMarketingOlderBuild, olderMarketingNewerBuild);

	const UpdateVersion sameRevisionLowPatch{ 3, 1, 0, 7300 };
	const UpdateVersion sameRevisionHighPatch{ 3, 1, 2, 7300 };
	EXPECT_LT(sameRevisionLowPatch, sameRevisionHighPatch);
	EXPECT_EQ(sameRevisionLowPatch, olderMarketingNewerBuild);
}

TEST(UpdateVersion, TreatsOnlyAStrictlyGreaterBuildAsAnUpdate)
{
	const UpdateVersion current{ 3, 1, 0, 7221 };
	EXPECT_TRUE(IsNewerBuild({ 3, 1, 0, 7222 }, current));
	EXPECT_TRUE(IsNewerBuild({ 2, 0, 0, 7222 }, current));
	EXPECT_FALSE(IsNewerBuild(current, current));
	EXPECT_FALSE(IsNewerBuild({ 3, 1, 0, 7220 }, current));
	// A larger marketing version cannot rescue an older commit count.
	EXPECT_FALSE(IsNewerBuild({ 9, 9, 9, 7220 }, current));
	// It does decide the comparison when the commit count ties.
	EXPECT_TRUE(IsNewerBuild({ 3, 1, 1, 7221 }, current));
}
