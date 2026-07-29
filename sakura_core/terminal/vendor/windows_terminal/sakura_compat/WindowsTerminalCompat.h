/*! @file
 * Sakura-owned compatibility boundary for the selected Windows Terminal
 * parser and input translation units. This file is not copied from upstream.
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cassert>
#include <climits>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/compile.h>
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <gsl/gsl>
#include <wil/resource.h>
#include <wil/result.h>

#ifndef SHORT_MAX
#define SHORT_MAX SHRT_MAX
#endif

#ifndef sealed
#define sealed final
#endif

// The selected upstream tracing source expects the Terminal project's private
// ETW keyword. It is local to the parser provider in this executable.
#ifndef TIL_KEYWORD_TRACE
#define TIL_KEYWORD_TRACE 0x1ull
#endif

namespace base {

template<class Target, class Source>
constexpr Target saturated_cast( Source value ) noexcept
{
	using Limits = std::numeric_limits<Target>;
	if( std::cmp_less(value, Limits::lowest()) ) return Limits::lowest();
	if( std::cmp_greater(value, Limits::max()) ) return Limits::max();
	return static_cast<Target>(value);
}

} // namespace base

namespace til {

template<class Container>
constexpr decltype(auto) at( Container&& container, std::size_t index )
{
	return std::forward<Container>(container)[index];
}

template<class T, std::size_t N>
constexpr T& at( T (&array)[N], std::size_t index ) noexcept
{
	return array[index];
}

template<class T, std::size_t N>
using small_vector = std::vector<T>;

using CoordType = std::int32_t;

struct point {
	CoordType x{};
	CoordType y{};
	constexpr bool operator==( const point& ) const noexcept = default;
};

struct size {
	CoordType width{};
	CoordType height{};
	constexpr bool operator==( const size& ) const noexcept = default;
};

// Only declarations from the upstream utility header instantiate this type in
// the selected parser closure. Keep it complete without importing TIL color.
struct color {
	std::uint32_t value{};
	constexpr bool operator==( const color& ) const noexcept = default;
};

constexpr char32_t tolower_ascii( char32_t value ) noexcept
{
	return value >= U'A' && value <= U'Z' ? value + (U'a' - U'A') : value;
}

} // namespace til

#include "../src/inc/til/enumset.h"

// The pinned upstream input implementation gates DEC application-keypad
// handling through a generated feature class. Sakura deliberately enables the
// stable behavior and keeps the policy private to this compatibility layer.
struct Feature_KeypadModeEnabled final {
	static constexpr bool IsEnabled() noexcept { return true; }
};
