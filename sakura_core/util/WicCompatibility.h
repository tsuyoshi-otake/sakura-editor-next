#pragma once

#include <wincodec.h>

namespace wic_compat {

#if defined(__MINGW32__)
inline constexpr auto kHighQualityCubicInterpolation = WICBitmapInterpolationModeCubic;
#else
inline constexpr auto kHighQualityCubicInterpolation = WICBitmapInterpolationModeHighQualityCubic;
#endif

} // namespace wic_compat
