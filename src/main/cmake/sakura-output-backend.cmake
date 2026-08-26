# Early Output authority backend contract validation.
#
# Output authority selection is intentionally independent from UTF-16/SIMD
# dispatch. C++ remains the Output production-package authority until the
# separate adoption gate lands; Rust is available only through an explicit
# comparison build choice.

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

# Output has its own production-package context. Do not infer Output packaging
# from the UTF-16 package contract: the two backends are selected independently.
string(TOUPPER
  "$ENV{SAKURA_OUTPUT_PRODUCTION_PACKAGE}"
  _sakura_output_production_environment_value
)
if(DEFINED SAKURA_OUTPUT_PRODUCTION_PACKAGE)
  set(_sakura_output_production_value "${SAKURA_OUTPUT_PRODUCTION_PACKAGE}")
elseif(_sakura_output_production_environment_value MATCHES "^(1|ON|TRUE|YES)$")
  set(_sakura_output_production_value ON)
else()
  set(_sakura_output_production_value OFF)
endif()
set(SAKURA_OUTPUT_PRODUCTION_PACKAGE
  "${_sakura_output_production_value}"
  CACHE BOOL "Output production packaging contract; C++ remains the authority"
)
if(SAKURA_OUTPUT_BACKEND STREQUAL "rust" AND SAKURA_OUTPUT_PRODUCTION_PACKAGE)
  message(FATAL_ERROR
    "SAKURA_OUTPUT_PRODUCTION_PACKAGE=true requires SAKURA_OUTPUT_BACKEND=cpp")
endif()
