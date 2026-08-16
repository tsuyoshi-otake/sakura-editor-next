# CMake script for tests1
#
# requires
#   ${7ZIP_EXECUTABLE}
#   ${ARCH}
#   ${CMAKE_GENERATOR_PLATFORM}
#   ${EXE_ARCH}

# Create a custom command for tests1.exe.manifest generation
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/tests1.exe.manifest"
  COMMAND ${CMAKE_COMMAND} 
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DEXE_NAME="tests1.exe"
    -DEXE_ARCH="${EXE_ARCH}"
    -DOUTPUT_FILE="${CMAKE_BINARY_DIR}/tests1.exe.manifest"
    -P ${CMAKE_SOURCE_DIR}/src/main/cmake/manifest.cmake
  COMMENT "Generating tests1.exe.manifest"
)

# Create a custom target that depends on the generated file
add_custom_target(generate_tests1_exe_manifest
  DEPENDS
    "${CMAKE_BINARY_DIR}/tests1.exe.manifest"
)

# Find GoogleTest's package(required)
find_package(GTest CONFIG REQUIRED)

# Find OpenCppCoverage for coverage test
find_program(OpenCppCoverage_EXECUTABLE OpenCppCoverage
  PATHS
    "$ENV{ProgramFiles}/OpenCppCoverage"
)

if(OpenCppCoverage_EXECUTABLE)
  message(STATUS "Found OpenCppCoverage: ${OpenCppCoverage_EXECUTABLE}")
endif()

find_program(UV_EXECUTABLE
  NAMES uv
  REQUIRED
)

set(MINIZ_SOURCE_DIR "${CMAKE_SOURCE_DIR}/externals/miniz-cpp")
set(MINIZ_INCLUDE_DIR "${CMAKE_BINARY_DIR}/include/miniz-cpp")

add_custom_command(
  OUTPUT "${MINIZ_SOURCE_DIR}/.git"
  COMMAND ${CMAKE_COMMAND}
    -DGIT_EXECUTABLE:FILEPATH=${GIT_EXECUTABLE}
    -DREPO_ROOT:PATH=${CMAKE_SOURCE_DIR}
    -DSUBMODULE_PATH:STRING=externals/miniz-cpp
    -DLOCK_PATH:FILEPATH=${CMAKE_BINARY_DIR}/cmake-submodule-update.lock
    -P ${CMAKE_SOURCE_DIR}/src/main/cmake/git_submodule_update_locked.cmake
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Fetching miniz-cpp's source files"
)

add_custom_command(
  OUTPUT "${MINIZ_INCLUDE_DIR}/zip_file.hpp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIZ_INCLUDE_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MINIZ_SOURCE_DIR}/zip_file.hpp" "${MINIZ_INCLUDE_DIR}/zip_file.hpp"
  DEPENDS "${MINIZ_SOURCE_DIR}/.git"
  COMMENT "Copying miniz-cpp/zip_file.hpp to include directory"
)

add_custom_target(generate_miniz
  DEPENDS
    "${MINIZ_INCLUDE_DIR}/zip_file.hpp"
)

# define precompiled headers
set(TESTS1_PCH_HEADER ${CMAKE_SOURCE_DIR}/src/test/resources/pch.h)

# define header files of tests1
file(GLOB_RECURSE TESTS1_HEADERS
  ${CMAKE_SOURCE_DIR}/src/test/cpp/tests1/*.hpp
  ${CMAKE_SOURCE_DIR}/src/test/cpp/tests1/*.h
  ${CMAKE_SOURCE_DIR}/src/test/cpp/*.hpp
  ${CMAKE_SOURCE_DIR}/src/test/cpp/*.h
  ${CMAKE_SOURCE_DIR}/src/test/resources/*.hpp
  ${CMAKE_SOURCE_DIR}/src/test/resources/*.h
)

# define source files of tests1
file(GLOB_RECURSE TESTS1_SOURCES
  ${CMAKE_SOURCE_DIR}/src/test/cpp/tests1/*.cpp
  ${CMAKE_SOURCE_DIR}/src/test/cpp/*.cpp
  ${CMAKE_SOURCE_DIR}/src/test/resources/*.cpp
)

# Keep the native Markdown code-highlighter suite explicit so the MSBuild and
# CMake registrations remain aligned even though the recursive glob sees it.
set(MARKDOWN_CODE_HIGHLIGHTER_TEST_SOURCE
  ${CMAKE_SOURCE_DIR}/src/test/cpp/tests1/markdown/MarkdownCodeHighlighterTest.cpp
)
list(REMOVE_ITEM TESTS1_SOURCES ${MARKDOWN_CODE_HIGHLIGHTER_TEST_SOURCE})
list(APPEND TESTS1_SOURCES ${MARKDOWN_CODE_HIGHLIGHTER_TEST_SOURCE})

# The terminal renderer focused suites under tests1/terminal/window are picked
# up by the recursive source glob above; keep this explicit note beside the
# discovery contract so the MSBuild and CMake registrations stay intentional.

set(CODE_COVERAGE_SOURCE ${CMAKE_SOURCE_DIR}/src/test/resources/coverage.cpp)
set(CODE_COVERAGE_HEADER ${CMAKE_VS_INSTALL_DIRECTORY}/VC/Auxiliary/VS/include/CodeCoverage/CodeCoverage.h)

if(MSVC AND EXISTS "${CODE_COVERAGE_HEADER}")
  # Code Coverage SDK がある Visual Studio では coverage.cpp のみ C++17 準拠にする
  set_source_files_properties(
    ${CODE_COVERAGE_SOURCE}
    PROPERTIES
      COMPILE_FLAGS "/std:c++17"
      SKIP_PRECOMPILE_HEADERS ON
  )
else()
  # MSBuild プロジェクトと同様、SDK が無い環境では coverage.cpp を含めない
  list(REMOVE_ITEM TESTS1_SOURCES ${CODE_COVERAGE_SOURCE})
endif()

# define resource files of tests1
set(TESTS1_RESOURCE_SCRIPTS ${CMAKE_SOURCE_DIR}/sakura_core/tests1_rc.rc)

set(TEST_DLLPLUGIN_DIR "${CMAKE_SOURCE_DIR}/src/test/resources/tests1/test-dllplugin")
set(TEST_DLLPLUGIN_TARGET dll_plugin1)
set(TESTS1_RESOURCE_STAGE_DIR "${CMAKE_BINARY_DIR}/tests1_resources")

if(MINGW)
  # Convert RC files to UTF-8 for MinGW
  convert_rc_files_to_utf8(TESTS1_RESOURCE_SCRIPTS "ja-JP" ${CMAKE_BINARY_DIR})
endif()

# Create a custom target for test_resource_zip generation
add_custom_target(test_resource_zip
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${TESTS1_RESOURCE_STAGE_DIR}/test-plugin"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${TESTS1_RESOURCE_STAGE_DIR}/test-plugin"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/src/test/resources/tests1/test-plugin
    ${TESTS1_RESOURCE_STAGE_DIR}/test-plugin
  COMMAND ${7ZIP_EXECUTABLE}
    u -tzip -r -mcu=on
    ${CMAKE_BINARY_DIR}/resources.ja-JP.zip
    ${TESTS1_RESOURCE_STAGE_DIR}/test-plugin
    > NUL
  BYPRODUCTS ${CMAKE_BINARY_DIR}/resources.ja-JP.zip
  COMMENT "Generating resources.ja-JP.zip"
)

# Create a custom target for test_dllplugin_zip generation
add_custom_target(test_dllplugin_zip
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${TESTS1_RESOURCE_STAGE_DIR}/test-dllplugin"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${TESTS1_RESOURCE_STAGE_DIR}/test-dllplugin"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${TEST_DLLPLUGIN_DIR}
    ${TESTS1_RESOURCE_STAGE_DIR}/test-dllplugin
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OUTPUT_DIRECTORY}/dll_plugin1.dll"
    ${TESTS1_RESOURCE_STAGE_DIR}/test-dllplugin/dll_plugin1.dll
  COMMAND ${7ZIP_EXECUTABLE}
    u -tzip -r -mcu=on
    ${CMAKE_BINARY_DIR}/resources-dllplugin.zip
    ${TESTS1_RESOURCE_STAGE_DIR}/test-dllplugin
    > NUL
  BYPRODUCTS ${CMAKE_BINARY_DIR}/resources-dllplugin.zip
  DEPENDS ${TEST_DLLPLUGIN_TARGET}
  COMMENT "Generating resources-dllplugin.zip"
)

# define executable
add_executable(tests1
  ${TESTS1_PCH_HEADER}
  ${TESTS1_HEADERS}
  ${TESTS1_SOURCES}
  ${TESTS1_RESOURCE_SCRIPTS}
  ${SAKURA_LEGACY_RESOURCE_SCRIPTS}
)

# Enable precompiled headers
target_precompile_headers(tests1 PRIVATE ${TESTS1_PCH_HEADER})

# add definitions for project
target_compile_definitions(tests1
  PRIVATE
    _CONSOLE
    _SILENCE_TR1_NAMESPACE_DEPRECATION_WARNING
)

# add include directories for project
target_include_directories(tests1
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src/test/cpp/tests1
    ${CMAKE_SOURCE_DIR}/src/test/resources/tests1
    ${CMAKE_SOURCE_DIR}/src/test/cpp
    ${CMAKE_SOURCE_DIR}/src/test/resources
)

# tests1_rc.rc embeds the two dictionary archives by file name.  Make the
# package directory visible to every resource compiler, including MSVC with a
# single-config generator such as Ninja (where the RC working directory does
# not contain the vcpkg payloads).
get_filename_component(CMIGEMO_DICTIONARY_DIR "${cmigemo_DICT_UTF8_ZIP}" DIRECTORY)
target_include_directories(tests1
  PRIVATE
    "$<BUILD_INTERFACE:${CMIGEMO_DICTIONARY_DIR}>"
)

# link libraries
target_link_libraries(tests1
  PRIVATE
    sakura_legacy_test_support
    GTest::gtest
    GTest::gmock
)

set(SAKURA_LEGACY_CONSUMER_COMPILE_TARGET tests1)
set(SAKURA_LEGACY_CONSUMER_LINK_TARGET tests1)
include(
  "${CMAKE_SOURCE_DIR}/src/main/modules/generated/cmake/legacy/consumers/tests1.cmake"
  OPTIONAL
)
unset(SAKURA_LEGACY_CONSUMER_COMPILE_TARGET)
unset(SAKURA_LEGACY_CONSUMER_LINK_TARGET)

if(WIN32)
  target_link_libraries(tests1
    PRIVATE
      gdiplus
      psapi
  )
endif()

set_target_properties(tests1
  PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
    ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIRECTORY}"
)

add_custom_command(TARGET tests1 PRE_LINK
  COMMAND ${CMAKE_COMMAND} -E remove -f $<TARGET_FILE:tests1>
)

set(TESTS1_EXE_MANIFEST "${CMAKE_BINARY_DIR}/tests1.exe.manifest")
set(TESTS1_MANIFEST_RC "${CMAKE_BINARY_DIR}/tests1_manifest.rc")

# Embed the generated application manifest as resource ID 1 on every Windows
# toolchain.  This avoids generator-specific linker/mt.exe behavior while
# preserving the Common Controls v6 activation context.
add_custom_command(
  OUTPUT "${TESTS1_MANIFEST_RC}"
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
    -DOUTPUT_FILE="${TESTS1_MANIFEST_RC}"
    -DMANIFEST_FILE="${TESTS1_EXE_MANIFEST}"
    -P ${CMAKE_SOURCE_DIR}/src/main/cmake/manifest_resource.cmake
  DEPENDS "${TESTS1_EXE_MANIFEST}"
  COMMENT "Generating tests1_manifest.rc"
)

target_sources(tests1
  PRIVATE
    "${TESTS1_MANIFEST_RC}"
)

if(MSVC)
  # The manifest is already resource-compiled above.  Disable CMake's default
  # linker-generated manifest so resource ID 1 is not emitted twice.
  target_link_options(tests1 PRIVATE "/MANIFEST:NO")
endif()

if(MINGW)
  # Add include directories for tests1
  target_include_directories(tests1
    PRIVATE
      "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/tests1_ja-JP>"
  )
  target_link_options(tests1
    PRIVATE
      -mconsole
  )
endif(MINGW)

# Add dependencies
add_dependencies(tests1
  sakura
  sakura_lang_en_US
  sakura_lang_zh_CN
  generate_tests1_exe_manifest
  test_resource_zip
  test_dllplugin_zip
  generate_miniz
  ppa_stub
)
