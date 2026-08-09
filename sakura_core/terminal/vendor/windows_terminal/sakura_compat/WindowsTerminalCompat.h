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
#if !defined(__MINGW32__) && !defined(__MINGW64__)
#include <wil/resource.h>
#include <wil/result.h>
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
#include "MinGWCompilerCompat.h"

namespace wil {

// The selected parser/input closure only needs the handle holder's complete
// type and the formatting helper declared by TIL. Do not pretend to port all
// of WIL to GCC; keep this minimal surface local to the vendor boundary.
class unique_hfile final {
public:
	unique_hfile() noexcept = default;
	explicit unique_hfile( HANDLE handle ) noexcept : m_handle(handle) {}
	~unique_hfile() { reset(); }

	unique_hfile( const unique_hfile& ) = delete;
	unique_hfile& operator=( const unique_hfile& ) = delete;
	unique_hfile( unique_hfile&& other ) noexcept : m_handle(other.release()) {}
	unique_hfile& operator=( unique_hfile&& other ) noexcept
	{
		if( this != &other ) reset(other.release());
		return *this;
	}

	[[nodiscard]] HANDLE get() const noexcept { return m_handle; }
	[[nodiscard]] HANDLE release() noexcept
	{
		const auto handle = m_handle;
		m_handle = INVALID_HANDLE_VALUE;
		return handle;
	}
	void reset( HANDLE handle = INVALID_HANDLE_VALUE ) noexcept
	{
		if( m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr ) CloseHandle(m_handle);
		m_handle = handle;
	}
	[[nodiscard]] explicit operator bool() const noexcept
	{
		return m_handle != INVALID_HANDLE_VALUE && m_handle != nullptr;
	}

private:
	HANDLE m_handle{ INVALID_HANDLE_VALUE };
};

template <typename String, typename... Args>
String str_printf( const wchar_t* format, Args&&... args )
{
	static_assert(std::is_same_v<String, std::wstring>);
	return fmt::format(fmt::runtime(std::wstring_view(format)), std::forward<Args>(args)...);
}

} // namespace wil

#ifndef LOG_HR
#define LOG_HR(...) ((void)0)
#endif

#endif // defined(__MINGW32__) || defined(__MINGW64__)

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

namespace Microsoft::Console::VirtualTerminal {
class TerminalInput;
}

namespace til {
class point;
class size;
}

constexpr INPUT_RECORD SynthesizeMouseEvent(til::point, std::uint32_t, std::uint32_t, std::uint32_t);
constexpr INPUT_RECORD SynthesizeWindowBufferSizeEvent(til::size);

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

class point {
public:
	constexpr point() noexcept = default;
	constexpr point( const CoordType x, const CoordType y ) noexcept : x(x), y(y) {}
	constexpr bool operator==( const point& other ) const noexcept
	{
		return x == other.x && y == other.y;
	}

private:
	friend class ::Microsoft::Console::VirtualTerminal::TerminalInput;
	friend constexpr auto ::SynthesizeMouseEvent(point, std::uint32_t, std::uint32_t, std::uint32_t) -> INPUT_RECORD;
	CoordType x{};
	CoordType y{};
};

class size {
public:
	constexpr size() noexcept = default;
	constexpr size( const CoordType width, const CoordType height ) noexcept : width(width), height(height) {}
	constexpr bool operator==( const size& other ) const noexcept
	{
		return width == other.width && height == other.height;
	}

private:
	friend constexpr auto ::SynthesizeWindowBufferSizeEvent(size) -> INPUT_RECORD;
	CoordType width{};
	CoordType height{};
};

// Only declarations from the upstream utility header instantiate this type in
// the selected parser closure. Keep it complete without importing TIL color.
class color {
public:
	constexpr color() noexcept = default;
	constexpr explicit color( const std::uint32_t value ) noexcept : value(value) {}
	constexpr bool operator==( const color& other ) const noexcept { return value == other.value; }

private:
	std::uint32_t value{};
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
