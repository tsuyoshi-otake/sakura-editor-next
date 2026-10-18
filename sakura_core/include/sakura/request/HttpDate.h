/*! @file
 * @brief Shared RFC 9110 IMF-fixdate parser for HTTP retry metadata.
 */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#pragma once

#include <chrono>
#include <optional>
#include <string_view>

namespace platform::request {

//! Parses one IMF-fixdate (`Wed, 21 Oct 2015 07:28:00 GMT`); the obsolete
//! RFC 850 and asctime forms are rejected. The weekday name is not validated
//! against the date: it is advisory text the answer is not required to carry.
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> ParseHttpDate(
	std::wstring_view value) noexcept;
[[nodiscard]] std::optional<std::chrono::system_clock::time_point> ParseHttpDate(
	std::string_view value) noexcept;

} // namespace platform::request
