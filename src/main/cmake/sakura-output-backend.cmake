# Early Output authority backend contract validation.
#
# Output authority selection is intentionally independent from UTF-16/SIMD
# dispatch. C++ remains the Output production-package authority until the
# separate adoption gate lands; Rust is available only through an explicit
# comparison build choice.

if(DEFINED SAKURA_OUTPUT_BACKEND)
  set(_sakura_output_backend_value "${SAKURA_OUTPUT_BACKEND}")
elseif(DEFINED ENV{SAKURA_OUTPUT_BACKEND})
  # A defined environment variable is an explicit caller value. In
  # particular, an empty value must remain invalid instead of becoming the
  # rollback-first default below.
  set(_sakura_output_backend_value "$ENV{SAKURA_OUTPUT_BACKEND}")
else()
  # Only an entirely absent selector receives the native C++ default.
  set(_sakura_output_backend_value "cpp")
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

# Output has its own production-package context. Do not infer Output packaging
# from the UTF-16 package contract: the two backends are selected independently.
# CMake's BOOL cache type would otherwise silently classify arbitrary strings as
# true or false. Validate the raw caller value first, then store a normalized
# ON/OFF value. The accepted spellings preserve the existing command-line and
# environment compatibility while keeping garbage and whitespace fail-closed.
if(DEFINED SAKURA_OUTPUT_PRODUCTION_PACKAGE)
  set(_sakura_output_production_input "${SAKURA_OUTPUT_PRODUCTION_PACKAGE}")
elseif(DEFINED ENV{SAKURA_OUTPUT_PRODUCTION_PACKAGE})
  set(_sakura_output_production_input "$ENV{SAKURA_OUTPUT_PRODUCTION_PACKAGE}")
else()
  set(_sakura_output_production_input OFF)
endif()
string(TOUPPER
  "${_sakura_output_production_input}"
  _sakura_output_production_upper
)
if(_sakura_output_production_upper STREQUAL "1"
   OR _sakura_output_production_upper STREQUAL "ON"
   OR _sakura_output_production_upper STREQUAL "TRUE"
   OR _sakura_output_production_upper STREQUAL "YES")
  set(_sakura_output_production_value ON)
elseif(_sakura_output_production_upper STREQUAL "0"
       OR _sakura_output_production_upper STREQUAL "OFF"
       OR _sakura_output_production_upper STREQUAL "FALSE"
       OR _sakura_output_production_upper STREQUAL "NO")
  set(_sakura_output_production_value OFF)
else()
  message(FATAL_ERROR
    "SAKURA_OUTPUT_PRODUCTION_PACKAGE must be one of "
    "ON, OFF, TRUE, FALSE, 1, 0, YES, or NO (case-insensitive); "
    "got '${_sakura_output_production_input}'")
endif()
set(SAKURA_OUTPUT_PRODUCTION_PACKAGE
  "${_sakura_output_production_value}"
  CACHE BOOL "Output production packaging contract; C++ remains the authority"
  FORCE
)
if(SAKURA_OUTPUT_BACKEND STREQUAL "rust" AND SAKURA_OUTPUT_PRODUCTION_PACKAGE)
  message(FATAL_ERROR
    "SAKURA_OUTPUT_PRODUCTION_PACKAGE=true requires SAKURA_OUTPUT_BACKEND=cpp")
endif()
