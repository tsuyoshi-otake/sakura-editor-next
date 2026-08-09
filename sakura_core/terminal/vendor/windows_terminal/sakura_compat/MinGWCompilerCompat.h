/*! @file
 * Sakura-owned MinGW compiler compatibility primitives for the pinned source.
 */
#pragma once

#if defined(__MINGW32__) || defined(__MINGW64__)

#include <type_traits>

// The imported Windows Terminal sources use the MSVC-only __assume intrinsic
// for caller contracts. Keep the contract in this Sakura-owned boundary so the
// vendor copy and its provenance hash remain untouched.
#ifndef __assume
#define __assume(condition) \
	do { \
		if (!(condition)) { \
			__builtin_unreachable(); \
		} \
	} while (false)
#endif

// The selected input sources use WIL's small flag-operation macro family.
// Keep these operations typed and local instead of making the whole WIL
// implementation a MinGW dependency. The helper accepts both Win32 integer
// masks and enum-class protocol flags.
namespace wil::sakura_mingw
{
template <typename T, bool = std::is_enum_v<T>>
struct flag_integer
{
	using type = std::remove_cv_t<T>;
};

template <typename T>
struct flag_integer<T, true>
{
	using type = std::underlying_type_t<T>;
};

template <typename T>
using flag_integer_t = typename flag_integer<std::remove_cv_t<T>>::type;

template <typename T>
constexpr flag_integer_t<T> flag_bits(const T value) noexcept
{
	return static_cast<flag_integer_t<T>>(value);
}

template <typename Value, typename Flags>
using flag_common_t = std::common_type_t<flag_integer_t<Value>, flag_integer_t<Flags>>;

template <typename Value, typename Flags>
constexpr bool is_any_flag_set(const Value value, const Flags flags) noexcept
{
	using common_type = flag_common_t<Value, Flags>;
	return (static_cast<common_type>(flag_bits(value)) & static_cast<common_type>(flag_bits(flags))) != 0;
}

template <typename Value, typename Flags>
constexpr bool are_all_flags_set(const Value value, const Flags flags) noexcept
{
	using common_type = flag_common_t<Value, Flags>;
	const auto value_bits = static_cast<common_type>(flag_bits(value));
	const auto flag_bits_value = static_cast<common_type>(flag_bits(flags));
	return (value_bits & flag_bits_value) == flag_bits_value;
}

template <typename Value, typename Flag>
constexpr void set_flag(Value& value, const Flag flag) noexcept
{
	using common_type = flag_common_t<Value, Flag>;
	value = static_cast<Value>(static_cast<common_type>(flag_bits(value)) |
		static_cast<common_type>(flag_bits(flag)));
}

template <typename Value, typename Flag>
constexpr void clear_flag(Value& value, const Flag flag) noexcept
{
	using common_type = flag_common_t<Value, Flag>;
	value = static_cast<Value>(static_cast<common_type>(flag_bits(value)) &
		~static_cast<common_type>(flag_bits(flag)));
}

} // namespace wil::sakura_mingw

#ifndef WI_IsAnyFlagSet
#define WI_IsAnyFlagSet(value, flags) (::wil::sakura_mingw::is_any_flag_set((value), (flags)))
#endif
#ifndef WI_AreAllFlagsSet
#define WI_AreAllFlagsSet(value, flags) (::wil::sakura_mingw::are_all_flags_set((value), (flags)))
#endif
#ifndef WI_IsFlagSet
#define WI_IsFlagSet(value, flag) WI_IsAnyFlagSet((value), (flag))
#endif
#ifndef WI_IsFlagClear
#define WI_IsFlagClear(value, flag) (!WI_IsFlagSet((value), (flag)))
#endif
#ifndef WI_SetFlag
#define WI_SetFlag(value, flag) (::wil::sakura_mingw::set_flag((value), (flag)))
#endif
#ifndef WI_ClearFlag
#define WI_ClearFlag(value, flag) (::wil::sakura_mingw::clear_flag((value), (flag)))
#endif
#ifndef WI_SetFlagIf
#define WI_SetFlagIf(value, flag, condition) \
	do { \
		if (condition) { \
			WI_SetFlag((value), (flag)); \
		} \
	} while (false)
#endif
#ifndef WI_ClearFlagIf
#define WI_ClearFlagIf(value, flag, condition) \
	do { \
		if (condition) { \
			WI_ClearFlag((value), (flag)); \
		} \
	} while (false)
#endif
#ifndef WI_UpdateFlag
#define WI_UpdateFlag(value, flag, condition) \
	do { \
		if (condition) { \
			WI_SetFlag((value), (flag)); \
		} else { \
			WI_ClearFlag((value), (flag)); \
		} \
	} while (false)
#endif

#endif // defined(__MINGW32__) || defined(__MINGW64__)
