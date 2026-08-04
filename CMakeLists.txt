cmake_minimum_required(VERSION 3.16)

# Reject an explicitly selected non-x64 generator platform before project()
# starts compiler identification. This keeps unsupported Visual Studio Win32
# and ARM64 configurations from entering the build at all.
if(CMAKE_GENERATOR_PLATFORM)
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _sakura_generator_platform)
  if(NOT _sakura_generator_platform MATCHES "^(x64|amd64)$")
    message(FATAL_ERROR
      "Sakura Editor NEXT's CMake build supports only x64 targets. "
      "Use an x64 generator/toolchain; got '${_sakura_generator_platform}'.")
  endif()
endif()

# Keep direct CMake/IDE builds on the same read-only semantic-graph trust path.
# This runs before compiler probing, so a stale graph fails before restore/build.
option(SAKURA_SKIP_MODULES_CHECK "Diagnostic-only: skip semantic graph staleness check" OFF)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/main/modules/modules.json"
  "${CMAKE_SOURCE_DIR}/src/main/modules/schema-v3.json"
  "${CMAKE_SOURCE_DIR}/src/main/modules/compile-profiles.json"
  "${CMAKE_SOURCE_DIR}/tools/build/sakura_build.py"
  "${CMAKE_SOURCE_DIR}/tools/build/sakura_build_lib/model.py"
  "${CMAKE_SOURCE_DIR}/tools/build/sakura_build_lib/generator.py"
  "${CMAKE_SOURCE_DIR}/tools/build/sakura_build_lib/runner.py")
if(NOT SAKURA_SKIP_MODULES_CHECK)
  find_program(_sakura_build_python NAMES python3 python py REQUIRED)
  get_filename_component(_sakura_build_python_name "${_sakura_build_python}" NAME_WE)
  set(_sakura_build_python_args)
  if(_sakura_build_python_name STREQUAL "py")
    list(APPEND _sakura_build_python_args -3)
  endif()
  execute_process(
    COMMAND "${_sakura_build_python}" ${_sakura_build_python_args}
      "${CMAKE_SOURCE_DIR}/tools/build/sakura_build.py" generate --check
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _sakura_modules_check_result)
  if(NOT _sakura_modules_check_result EQUAL 0)
    message(FATAL_ERROR
      "Component projections are stale. Run: py -3 tools/build/sakura_build.py generate")
  endif()
endif()

# Define primary project
project(sakura LANGUAGES CXX)

# Sakura Editor NEXT's CMake build is supported only for 64-bit x86 targets.
# This also permits the experimental MinGW64 configuration, where the
# generator platform is normally unset and the compiler reports x86_64.
set(_sakura_target_platform "${CMAKE_VS_PLATFORM_NAME}")
if(NOT _sakura_target_platform)
  set(_sakura_target_platform "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(NOT _sakura_target_platform)
  # Single-config generators such as Ninja do not populate either of the
  # platform variables above when using MSVC.  CMake's compiler probe still
  # records the actual target architecture, so use that without weakening the
  # x64-only guard (in particular, ARM64 must remain rejected).
  set(_sakura_target_platform "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
endif()
if(NOT _sakura_target_platform)
  set(_sakura_target_platform "${MSVC_CXX_ARCHITECTURE_ID}")
endif()
string(TOLOWER "${_sakura_target_platform}" _sakura_target_platform)

if(NOT _sakura_target_platform MATCHES "^(x64|amd64|x86_64)$" OR
   CMAKE_SIZEOF_VOID_P LESS 8)
  message(FATAL_ERROR
    "Sakura Editor NEXT's CMake build supports only x64 targets. "
    "Use an x64 generator/toolchain (including experimental MinGW64); "
    "got '${_sakura_target_platform}'.")
endif()

# Include sakura.cmake for sub-targets
include(${CMAKE_SOURCE_DIR}/src/main/cmake/sakura.cmake)

# Create main executable
add_executable(${PROJECT_NAME} WIN32 ${CMAKE_SOURCE_DIR}/sakura_core/StdAfx.cpp)

# link libraries
target_link_libraries(${PROJECT_NAME}
  PRIVATE
    sakura_core
)

add_custom_command(TARGET ${PROJECT_NAME} PRE_LINK
  COMMAND ${CMAKE_COMMAND} -E remove -f $<TARGET_FILE:${PROJECT_NAME}>
)

add_dependencies(${PROJECT_NAME}
  generate_sakura_exe_manifest
)

if(MINGW)
  target_sources(${PROJECT_NAME}
    PRIVATE
      "${SAKURA_MANIFEST_RC}"
  )
endif(MINGW)

# Create language DLL projects
# en-US: 1033 (0x0409) - English (United States)
# zh-CN: 2052 (0x0804) - Chinese (Simplified, PRC)
create_language_dll(en-US 1033)
create_language_dll(zh-CN 2052)

# Include ctags.cmake for ctags command
include(${CMAKE_SOURCE_DIR}/src/main/cmake/ctags.cmake)

# Include diffutils.cmake for diff command
include(${CMAKE_SOURCE_DIR}/src/main/cmake/diffutils.cmake)

# Enable ctest command
enable_testing()

# Convert paths to native Windows format (backslashes) for OpenCppCoverage
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" CMAKE_SOURCE_DIR_NATIVE)
file(TO_NATIVE_PATH "${OUTPUT_DIRECTORY}" OUTPUT_DIRECTORY_NATIVE)

# Include tests1 definition
include(${CMAKE_SOURCE_DIR}/src/test/cmake/tests1.cmake)

if(OpenCppCoverage_EXECUTABLE)
  add_test(NAME CoverageTest
    COMMAND ${OpenCppCoverage_EXECUTABLE}
      --export_type cobertura:${CMAKE_SOURCE_DIR_NATIVE}\\tests1-coverage.xml
      --modules ${OUTPUT_DIRECTORY_NATIVE}\\tests1.exe
      --sources ${CMAKE_SOURCE_DIR_NATIVE}
      --excluded_sources ${CMAKE_SOURCE_DIR_NATIVE}\\build
      --working_dir ${OUTPUT_DIRECTORY_NATIVE}
      --cover_children
      --
      $<TARGET_FILE:tests1>
      --gtest_output=xml:${CMAKE_SOURCE_DIR}/tests1-googletest.xml
    CONFIGURATIONS Debug
  )
endif()

add_test(NAME UnitTest
  COMMAND $<TARGET_FILE:tests1>
    --gtest_output=xml:${CMAKE_SOURCE_DIR}/tests1-googletest.xml
  WORKING_DIRECTORY ${OUTPUT_DIRECTORY}
  CONFIGURATIONS Release
)

add_test(NAME pytest
  COMMAND
    "${UV_EXECUTABLE}"
    run
    python
    -m pytest
    --ignore=externals
    --ignore=tools/vcpkg
    --cov
    --cov-config=src/test/py/.coveragerc
    --cov-report=xml:pytest-coverage.xml
    --cov-fail-under=80
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
