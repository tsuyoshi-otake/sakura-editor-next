/*! @file
 * Sakura-owned narrow compatibility include preserving the pinned upstream
 * source path without importing the unrelated utility dependency graph.
 */
#pragma once

#include <cstddef>

namespace Microsoft::Console::Utils {

template<class T>
constexpr int Sign( T value ) noexcept
{
	return (T{} < value) - (value < T{});
}

const wchar_t* FindActionableControlCharacter( const wchar_t* begin, std::size_t length ) noexcept;

} // namespace Microsoft::Console::Utils
