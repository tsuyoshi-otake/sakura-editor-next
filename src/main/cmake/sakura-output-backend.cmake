# Early Output authority backend contract validation.
#
# Output authority selection is intentionally independent from UTF-16/SIMD
# dispatch. C++ remains the production-package authority until the separate
# adoption gate lands; Rust is available only through an explicit build choice.

if(DEFINED SAKURA_OUTPUT_BACKEND)
  set(_sakura_output_backend_value "${SAKURA_OUTPUT_BACKEND}")
else()
  set(_sakura_output_backend_value "$ENV{SAKURA_OUTPUT_BACKEND}")
  if(_sakura_output_backend_value STREQUAL "")
    set(_sakura_output_backend_value "cpp")
  endif()
endif()
set(
  SAKURA_OUTPUT_BACKEND
  "${_sakura_output_backend_value}"
  CACHE STRING "Output authority backend: rust for explicit MSVC comparison, cpp otherwise"
)
set_property(CACHE SAKURA_OUTPUT_BACKEND PROPERTY STRINGS cpp rust)
if(NOT SAKURA_OUTPUT_BACKEND STREQUAL "cpp"
   AND NOT SAKURA_OUTPUT_BACKEND STREQUAL "rust")
  message(FATAL_ERROR
    "SAKURA_OUTPUT_BACKEND must be exactly cpp or rust; "
    "auto and unknown values are rejected")
endif()

# The existing package flag is the early distribution-build fence used by all
# native Rust adoption gates. It is defined by sakura-utf16-backend.cmake before
# this module is included.
if(SAKURA_OUTPUT_BACKEND STREQUAL "rust" AND SAKURA_UTF16_PRODUCTION_PACKAGE)
  message(FATAL_ERROR
    "The Rust Output backend cannot package production until independent adoption")
endif()
