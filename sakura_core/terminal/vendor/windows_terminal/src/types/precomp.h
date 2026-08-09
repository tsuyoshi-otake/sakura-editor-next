/*! @file
 * Sakura-owned compatibility shim for the pinned Windows Terminal source.
 * The upstream translation unit intentionally relies on its project PCH for
 * these standard-library declarations.  Keeping the shim beside the source
 * lets us compile the unmodified upstream file with PCH disabled.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "../../sakura_compat/MinGWCompilerCompat.h"

#ifndef LOG_CAUGHT_EXCEPTION
#define LOG_CAUGHT_EXCEPTION() ((void)0)
#endif
