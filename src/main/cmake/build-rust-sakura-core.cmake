# Build the dependency-free sakura_rust_core workspace only when one of its
# manifests, lockfile, pinned toolchain, sources, or the selected static
# library requires it. The caller runs this script from the Rust workspace so
# rustup selects rust/rust-toolchain.toml deterministically.

foreach(required_variable IN ITEMS
    SAKURA_RUST_CORE_CARGO
    SAKURA_RUST_CORE_MANIFEST
    SAKURA_RUST_CORE_MEMBER_MANIFEST
    SAKURA_RUST_CORE_LOCK
    SAKURA_RUST_CORE_TOOLCHAIN
    SAKURA_RUST_CORE_SOURCE_DIR
    SAKURA_RUST_CORE_TARGET
    SAKURA_RUST_CORE_TARGET_DIR
    SAKURA_RUST_CORE_WORKING_DIR
    SAKURA_RUST_CORE_PROFILE
    SAKURA_RUST_CORE_OUTPUT)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "Missing sakura_rust_core build variable: ${required_variable}")
  endif()
endforeach()

set(needs_build FALSE)
file(GLOB_RECURSE rust_source_files
  "${SAKURA_RUST_CORE_SOURCE_DIR}/*.rs")
if(NOT rust_source_files)
  message(FATAL_ERROR
    "No sakura_rust_core source files found under: ${SAKURA_RUST_CORE_SOURCE_DIR}")
endif()
if(NOT EXISTS "${SAKURA_RUST_CORE_OUTPUT}")
  set(needs_build TRUE)
else()
  foreach(input_file IN ITEMS
      "${SAKURA_RUST_CORE_MANIFEST}"
      "${SAKURA_RUST_CORE_MEMBER_MANIFEST}"
      "${SAKURA_RUST_CORE_LOCK}"
      "${SAKURA_RUST_CORE_TOOLCHAIN}")
    if(NOT EXISTS "${input_file}")
      message(FATAL_ERROR "sakura_rust_core build input does not exist: ${input_file}")
    endif()
    if("${input_file}" IS_NEWER_THAN "${SAKURA_RUST_CORE_OUTPUT}")
      set(needs_build TRUE)
    endif()
  endforeach()
  foreach(input_file IN LISTS rust_source_files)
    if("${input_file}" IS_NEWER_THAN "${SAKURA_RUST_CORE_OUTPUT}")
      set(needs_build TRUE)
    endif()
  endforeach()
endif()

if(NOT needs_build)
  message(STATUS "sakura_rust_core static library is up to date")
  return()
endif()

file(MAKE_DIRECTORY "${SAKURA_RUST_CORE_TARGET_DIR}")
execute_process(
  COMMAND "${SAKURA_RUST_CORE_CARGO}" build
    --manifest-path "${SAKURA_RUST_CORE_MANIFEST}"
    --workspace
    --package sakura-rust-core
    --locked
    --target "${SAKURA_RUST_CORE_TARGET}"
    --target-dir "${SAKURA_RUST_CORE_TARGET_DIR}"
    --profile "${SAKURA_RUST_CORE_PROFILE}"
  WORKING_DIRECTORY "${SAKURA_RUST_CORE_WORKING_DIR}"
  RESULT_VARIABLE cargo_result
)
if(NOT cargo_result EQUAL 0)
  message(FATAL_ERROR "Cargo failed to build sakura_rust_core: ${cargo_result}")
endif()
if(NOT EXISTS "${SAKURA_RUST_CORE_OUTPUT}")
  message(FATAL_ERROR "Cargo completed without producing: ${SAKURA_RUST_CORE_OUTPUT}")
endif()
