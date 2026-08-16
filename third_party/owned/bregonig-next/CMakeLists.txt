cmake_minimum_required(VERSION 3.20)
project(bregonig LANGUAGES CXX RC VERSION 4.20.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BREGONIG_BUILD_TESTS "Build ABI layout and Unicode/ANSI smoke tests" ON)
option(BREGONIG_BUILD_FUZZ "Build the in-process libFuzzer harness" OFF)

set(ONIGMO_SOURCE_DIR "" CACHE PATH "Onigmo NEXT source tree used instead of find_package(Onigmo)")

if(ONIGMO_SOURCE_DIR)
  if(NOT EXISTS "${ONIGMO_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "ONIGMO_SOURCE_DIR has no CMakeLists.txt: ${ONIGMO_SOURCE_DIR}")
  endif()
  set(ONIGMO_BUILD_TESTS OFF CACHE BOOL "Onigmo tests are not part of the bregonig product" FORCE)
  add_subdirectory("${ONIGMO_SOURCE_DIR}" _onigmo)
else()
  find_package(Onigmo CONFIG REQUIRED)
endif()

set(BREGONIG_COMMON_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bregonig.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/subst.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsplit.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/btrans.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/sv.cpp"
)

function(bregonig_add_objects target)
  add_library(${target} OBJECT ${BREGONIG_COMMON_SOURCES})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(${target}
    PRIVATE
      _CRT_SECURE_NO_DEPRECATE
      WIN32_LEAN_AND_MEAN
      _BREGEXP_
      USE_ONIGMO_6
      ${ARGN}
  )
  target_link_libraries(${target} PUBLIC Onigmo::onigmo)
  if(MSVC)
    target_compile_options(${target} PRIVATE /EHsc /W3 /wd4996)
  endif()
endfunction()

# Historical bregonig.dll ships both ANSI and Unicode exports from one image.
bregonig_add_objects(bregonig_ansi)
bregonig_add_objects(bregonig_wide UNICODE _UNICODE)

add_library(bregonig SHARED
  $<TARGET_OBJECTS:bregonig_ansi>
  $<TARGET_OBJECTS:bregonig_wide>
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bregonig.rc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bregonig.def"
)
add_library(bregonig::bregonig ALIAS bregonig)

target_include_directories(bregonig
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(bregonig PRIVATE Onigmo::onigmo)
set_target_properties(bregonig PROPERTIES
  OUTPUT_NAME bregonig
  PREFIX ""
)

if(MSVC)
  target_link_options(bregonig PRIVATE /DYNAMICBASE /NXCOMPAT)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_link_options(bregonig PRIVATE /HIGHENTROPYVA)
  endif()
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/bregexp.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(FILES
  "${CMAKE_CURRENT_SOURCE_DIR}/bsd_license.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/perl_license.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/perl_license_jp.txt"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/bregonig"
)

install(TARGETS bregonig EXPORT bregonigTargets
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(EXPORT bregonigTargets
  NAMESPACE bregonig::
  DESTINATION share/bregonig
  FILE bregonigTargets.cmake
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bregonigConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bregonigConfig.cmake"
  INSTALL_DESTINATION share/bregonig
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/bregonigConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY ExactVersion
)

set(BREGONIG_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE BREGONIG_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/provenance.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/provenance.json"
  @ONLY
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/bregonigConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/bregonigConfigVersion.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/provenance.json"
  DESTINATION share/bregonig
)

if(BREGONIG_BUILD_TESTS)
  enable_testing()
  add_executable(bregonig_abi_layout tests/abi_layout.cpp)
  target_include_directories(bregonig_abi_layout PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(bregonig_abi_layout PRIVATE UNICODE _UNICODE)
  add_test(NAME bregonig_abi_layout COMMAND bregonig_abi_layout)

  add_executable(bregonig_smoke tests/smoke_match.cpp)
  target_compile_definitions(bregonig_smoke PRIVATE UNICODE _UNICODE)
  target_link_libraries(bregonig_smoke PRIVATE bregonig)
  add_custom_command(TARGET bregonig_smoke POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:bregonig>" "$<TARGET_FILE_DIR:bregonig_smoke>"
  )
  add_test(NAME bregonig_smoke COMMAND bregonig_smoke)
endif()

if(BREGONIG_BUILD_FUZZ)
  add_executable(bregonig_fuzzer fuzz/bregonig_fuzzer.cpp)
  target_include_directories(bregonig_fuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(bregonig_fuzzer PRIVATE
    UNICODE _UNICODE _BREGEXP_ USE_ONIGMO_6 WIN32_LEAN_AND_MEAN
  )
  target_link_libraries(bregonig_fuzzer PRIVATE bregonig_wide Onigmo::onigmo)
  target_compile_options(bregonig_fuzzer PRIVATE -fsanitize=fuzzer,address)
  target_link_options(bregonig_fuzzer PRIVATE -fsanitize=fuzzer,address)
endif()
