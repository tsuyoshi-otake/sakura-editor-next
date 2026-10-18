/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include <sakura/request/HttpDate.h>

#include <chrono>

namespace {
using platform::request::ParseHttpDate;

std::int64_t UnixSeconds(const std::chrono::system_clock::time_point& value) noexcept
{
	return std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch()).count();
}

} // namespace

TEST(HttpDate, ParsesAnImfFixdateAsTheAbsoluteInstantItNames)
{
	const auto parsed = ParseHttpDate(L"Wed, 21 Oct 2015 07:28:00 GMT");
	ASSERT_TRUE(parsed);
	// 2015-10-21 07:28:00 UTC, derived from the calendar rather than the code
	// under test.
	EXPECT_EQ(1445412480, UnixSeconds(*parsed));

	// The byte-oriented overload parses the same header text the envelope
	// reader holds.
	const auto narrow = ParseHttpDate("Wed, 21 Oct 2015 07:28:00 GMT");
	ASSERT_TRUE(narrow);
	EXPECT_EQ(UnixSeconds(*parsed), UnixSeconds(*narrow));

	// The weekday name is advisory: the field fixes positions, not which day
	// the server claims the date is.
	const auto weekday = ParseHttpDate(L"Mon, 21 Oct 2015 07:28:00 GMT");
	ASSERT_TRUE(weekday);
	EXPECT_EQ(UnixSeconds(*parsed), UnixSeconds(*weekday));
}

TEST(HttpDate, RejectsAnythingThatIsNotOneImfFixdate)
{
	// A wrong length and anything but a GMT zone is not a fixdate, and a
	// fixdate-shaped string with out-of-range fields is not a date.
	EXPECT_FALSE(ParseHttpDate(L"Wednesday, 21 Oct 2015 07:28:00 GMT"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Oct 2015 07:28:00 UTC"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Oct 2015 07:28:00 +0000"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Oct 2015 07:28:00 GMT "));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Oct 2015 07:28:00"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 32 Oct 2015 07:28:00 GMT"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Xxx 2015 07:28:00 GMT"));
	EXPECT_FALSE(ParseHttpDate(L"Wed, 21 Oct 2015 24:00:00 GMT"));
	EXPECT_FALSE(ParseHttpDate(L""));
}
