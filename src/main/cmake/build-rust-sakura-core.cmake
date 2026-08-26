# Build the native Rust workspace only when one of its manifests, lockfile,
# pinned toolchain, sources, or the selected static library requires it. The
# caller runs this script from rust/native so rustup selects that workspace's
# toolchain deterministically. The historical filename is retained because
# existing CMake projections refer to it; the package and artifacts are the
# native FFI contract.

foreach(required_variable IN ITEMS
    SAKURA_NATIVE_FFI_CARGO
    SAKURA_NATIVE_FFI_MANIFEST
    SAKURA_NATIVE_FFI_MEMBER_MANIFEST
    SAKURA_NATIVE_FFI_SIMD_MANIFEST
    SAKURA_NATIVE_FFI_UNICODE_MANIFEST
    SAKURA_NATIVE_FFI_LOCK
    SAKURA_NATIVE_FFI_TOOLCHAIN
    SAKURA_NATIVE_FFI_SOURCE_DIR
    SAKURA_NATIVE_FFI_TARGET
    SAKURA_NATIVE_FFI_TARGET_DIR
    SAKURA_NATIVE_FFI_WORKING_DIR
    SAKURA_NATIVE_FFI_PROFILE
    SAKURA_NATIVE_FFI_OUTPUT)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing sakura_native_ffi build variable: ${required_variable}")
  endif()
endforeach()

set(native_ffi_input_files
  "${SAKURA_NATIVE_FFI_MANIFEST}"
  "${SAKURA_NATIVE_FFI_MEMBER_MANIFEST}"
  "${SAKURA_NATIVE_FFI_SIMD_MANIFEST}"
  "${SAKURA_NATIVE_FFI_UNICODE_MANIFEST}"
  "${SAKURA_NATIVE_FFI_LOCK}"
  "${SAKURA_NATIVE_FFI_TOOLCHAIN}")
foreach(input_file IN LISTS native_ffi_input_files)
  if(NOT EXISTS "${input_file}")
    message(FATAL_ERROR "sakura_native_ffi build input does not exist: ${input_file}")
  endif()
endforeach()

file(GLOB_RECURSE native_ffi_source_files
  "${SAKURA_NATIVE_FFI_SOURCE_DIR}/*.rs")
if(NOT native_ffi_source_files)
  message(FATAL_ERROR
    "No native Rust source files found under: ${SAKURA_NATIVE_FFI_SOURCE_DIR}")
endif()

set(needs_build FALSE)
if(NOT EXISTS "${SAKURA_NATIVE_FFI_OUTPUT}")
  set(needs_build TRUE)
else()
  foreach(input_file IN LISTS native_ffi_input_files)
    if("${input_file}" IS_NEWER_THAN "${SAKURA_NATIVE_FFI_OUTPUT}")
      set(needs_build TRUE)
    endif()
  endforeach()
  foreach(input_file IN LISTS native_ffi_source_files)
    if("${input_file}" IS_NEWER_THAN "${SAKURA_NATIVE_FFI_OUTPUT}")
      set(needs_build TRUE)
    endif()
  endforeach()
endif()

if(NOT needs_build)
  message(STATUS "sakura_native_ffi static library is up to date")
  return()
endif()

file(MAKE_DIRECTORY "${SAKURA_NATIVE_FFI_TARGET_DIR}")
execute_process(
  COMMAND "${SAKURA_NATIVE_FFI_CARGO}" build
    --manifest-path "${SAKURA_NATIVE_FFI_MANIFEST}"
    --package sakura-native-ffi
    --locked
    --target "${SAKURA_NATIVE_FFI_TARGET}"
    --target-dir "${SAKURA_NATIVE_FFI_TARGET_DIR}"
    --profile "${SAKURA_NATIVE_FFI_PROFILE}"
  WORKING_DIRECTORY "${SAKURA_NATIVE_FFI_WORKING_DIR}"
  RESULT_VARIABLE cargo_result
)
if(NOT cargo_result EQUAL 0)
  message(FATAL_ERROR "Cargo failed to build sakura_native_ffi: ${cargo_result}")
endif()
if(NOT EXISTS "${SAKURA_NATIVE_FFI_OUTPUT}")
  message(FATAL_ERROR
    "Cargo completed without producing the required sakura_native_ffi static library: ${SAKURA_NATIVE_FFI_OUTPUT}")
endif()
