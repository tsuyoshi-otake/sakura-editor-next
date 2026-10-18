/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include <sakura/request/HttpDate.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace platform::request {
namespace {

wchar_t AsciiLower(const wchar_t character) noexcept
{
	return character >= L'A' && character <= L'Z' ? character - L'A' + L'a' : character;
}

bool EqualsAsciiInsensitive(const std::wstring_view left, const std::wstring_view right) noexcept
{
	return left.size() == right.size() && std::ranges::equal(left, right,
		[](const wchar_t a, const wchar_t b) { return AsciiLower(a) == AsciiLower(b); });
}

int MonthNumber(std::wstring_view month) noexcept
{
	constexpr std::array<std::wstring_view, 12> months = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
	for (std::size_t index = 0; index < months.size(); ++index) {
		if (EqualsAsciiInsensitive(month, months[index])) {
			return static_cast<int>(index + 1);
		}
	}
	return 0;
}

// Howard Hinnant's civil-date conversion, kept local so HTTP date parsing needs no CRT time zone state.
std::int64_t DaysFromCivil(int year, unsigned month, unsigned day) noexcept
{
	year -= month <= 2;
	const auto era = (year >= 0 ? year : year - 399) / 400;
	const auto yearOfEra = static_cast<unsigned>(year - era * 400);
	const auto dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const auto dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

} // namespace

std::optional<std::chrono::system_clock::time_point> ParseHttpDate(const std::wstring_view value) noexcept
{
	// IMF-fixdate only: Wed, 21 Oct 2015 07:28:00 GMT. Other HTTP-date forms are obsolete.
	if (value.size() != 29 || value[3] != L',' || value[4] != L' ' || value[7] != L' ' || value[11] != L' ' ||
		value[16] != L' ' || value[19] != L':' || value[22] != L':' || value.substr(25) != L" GMT") {
		return std::nullopt;
	}
	auto twoDigits = [&value](std::size_t offset) -> std::optional<int> {
		if (value[offset] < L'0' || value[offset] > L'9' || value[offset + 1] < L'0' || value[offset + 1] > L'9') return std::nullopt;
		return (value[offset] - L'0') * 10 + value[offset + 1] - L'0';
	};
	const auto day = twoDigits(5);
	const auto hour = twoDigits(17);
	const auto minute = twoDigits(20);
	const auto second = twoDigits(23);
	if (!day || !hour || !minute || !second || *day == 0 || *day > 31 || *hour > 23 || *minute > 59 || *second > 59) return std::nullopt;
	int year = 0;
	for (std::size_t index = 12; index < 16; ++index) {
		if (value[index] < L'0' || value[index] > L'9') return std::nullopt;
		year = year * 10 + value[index] - L'0';
	}
	const int month = MonthNumber(value.substr(8, 3));
	if (month == 0) return std::nullopt;
	const auto seconds = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(*day)) * 86400 + *hour * 3600 + *minute * 60 + *second;
	return std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
}

std::optional<std::chrono::system_clock::time_point> ParseHttpDate(const std::string_view value) noexcept
{
	// A fixdate is 29 ASCII code units; anything longer is never one, so a
	// fixed buffer is the whole widening this overload needs.
	wchar_t widened[64];
	if (value.size() > std::size(widened)) return std::nullopt;
	for (std::size_t index = 0; index < value.size(); ++index) {
		widened[index] = static_cast<unsigned char>(value[index]);
	}
	return ParseHttpDate(std::wstring_view(widened, value.size()));
}

} // namespace platform::request
