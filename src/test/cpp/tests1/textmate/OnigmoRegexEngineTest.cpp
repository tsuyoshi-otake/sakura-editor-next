/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "textmate/OnigmoRegexEngine.h"
#include "textmate/TextMateUtf8.h"

namespace {

using textmate::EncodeLineForSearch;
using textmate::OnigmoPattern;

TEST(OnigmoRegexEngineTest, Compile_ValidUtf8Pattern_Succeeds)
{
	std::wstring error;
	const auto pattern = OnigmoPattern::Compile("a+b", &error);
	ASSERT_NE(nullptr, pattern);
	EXPECT_TRUE(error.empty());
}

TEST(OnigmoRegexEngineTest, Compile_InvalidPattern_ReturnsNullAndError)
{
	std::wstring error;
	const auto pattern = OnigmoPattern::Compile("(", &error);
	EXPECT_EQ(nullptr, pattern);
	EXPECT_FALSE(error.empty());
}

TEST(OnigmoRegexEngineTest, Search_ReportsWholeMatchAndCaptureOffsets)
{
	const auto pattern = OnigmoPattern::Compile("a(b+)c", nullptr);
	ASSERT_NE(nullptr, pattern);

	const auto line = EncodeLineForSearch(L"xxabbcy");
	const auto match = pattern->Search(line, 0);
	ASSERT_TRUE(match.has_value());
	ASSERT_GE(match->groups.size(), 2u);

	EXPECT_TRUE(match->WholeMatch().participated);
	EXPECT_EQ(2u, match->WholeMatch().utf16Begin);
	EXPECT_EQ(6u, match->WholeMatch().utf16End);

	EXPECT_TRUE(match->groups[1].participated);
	EXPECT_EQ(3u, match->groups[1].utf16Begin);
	EXPECT_EQ(5u, match->groups[1].utf16End);
}

TEST(OnigmoRegexEngineTest, Search_NoMatch_ReturnsNullopt)
{
	const auto pattern = OnigmoPattern::Compile("zzz", nullptr);
	ASSERT_NE(nullptr, pattern);

	const auto line = EncodeLineForSearch(L"abc");
	EXPECT_FALSE(pattern->Search(line, 0).has_value());
}

} // namespace
