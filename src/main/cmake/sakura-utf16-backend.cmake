# Early UTF-16 backend contract validation.
#
# C++ is the UTF-16 production backend on MSVC and MinGW (#239).
# The pinned native Rust archive supplies retained comparison kernels on both.
# MSVC can explicitly select Rust for non-production caller comparisons.

if(DEFINED SAKURA_UTF16_BACKEND)
  set(_sakura_utf16_backend_value "${SAKURA_UTF16_BACKEND}")
else()
  set(_sakura_utf16_backend_value "$ENV{SAKURA_UTF16_BACKEND}")
  if(_sakura_utf16_backend_value STREQUAL "")
    # This file is included before project(), so compiler-specific variables
    # such as MINGW are not available yet. The canonical MinGW runner passes
    # cpp explicitly. The other CMake paths default to cpp.
    set(_sakura_utf16_backend_value "cpp")
  endif()
endif()
set(
  SAKURA_UTF16_BACKEND
  "${_sakura_utf16_backend_value}"
  CACHE STRING "UTF-16 backend (cpp by default, rust for MSVC comparison builds only)"
)
set_property(CACHE SAKURA_UTF16_BACKEND PROPERTY STRINGS cpp rust)
if(NOT SAKURA_UTF16_BACKEND STREQUAL "cpp"
   AND NOT SAKURA_UTF16_BACKEND STREQUAL "rust")
  message(FATAL_ERROR
    "SAKURA_UTF16_BACKEND must be exactly cpp or rust; "
    "auto and unknown values are rejected")
endif()
string(TOUPPER
  "$ENV{SAKURA_UTF16_PRODUCTION_PACKAGE}"
  _sakura_utf16_production_environment_value
)
if(DEFINED SAKURA_UTF16_PRODUCTION_PACKAGE)
  set(_sakura_utf16_production_value "${SAKURA_UTF16_PRODUCTION_PACKAGE}")
elseif(_sakura_utf16_production_environment_value MATCHES "^(1|ON|TRUE|YES)$")
  set(_sakura_utf16_production_value ON)
else()
  set(_sakura_utf16_production_value OFF)
endif()
set(SAKURA_UTF16_PRODUCTION_PACKAGE
  "${_sakura_utf16_production_value}"
  CACHE BOOL "Production packaging contract; C++ remains the authority"
)
if(SAKURA_UTF16_BACKEND STREQUAL "rust" AND SAKURA_UTF16_PRODUCTION_PACKAGE)
  message(FATAL_ERROR
    "The Rust UTF-16 backend cannot package production until independent adoption")
endif()

option(
  SAKURA_UTF16_BENCHMARK_TELEMETRY
  "Compile the explicit test-only actual-caller histogram hooks"
  OFF
)
if(SAKURA_UTF16_BENCHMARK_TELEMETRY AND SAKURA_UTF16_PRODUCTION_PACKAGE)
  message(FATAL_ERROR
    "SAKURA_UTF16_BENCHMARK_TELEMETRY is test-only and cannot package production")
endif()
