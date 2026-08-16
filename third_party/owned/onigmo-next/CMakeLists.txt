cmake_minimum_required(VERSION 3.20)
project(Onigmo LANGUAGES C VERSION 6.2.0)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(ONIGMO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

set(ONIGMO_SOURCES
  "${ONIGMO_ROOT}/regcomp.c"
  "${ONIGMO_ROOT}/regenc.c"
  "${ONIGMO_ROOT}/regerror.c"
  "${ONIGMO_ROOT}/regexec.c"
  "${ONIGMO_ROOT}/regext.c"
  "${ONIGMO_ROOT}/reggnu.c"
  "${ONIGMO_ROOT}/regparse.c"
  "${ONIGMO_ROOT}/regposerr.c"
  "${ONIGMO_ROOT}/regposix.c"
  "${ONIGMO_ROOT}/regsyntax.c"
  "${ONIGMO_ROOT}/regtrav.c"
  "${ONIGMO_ROOT}/regversion.c"
  "${ONIGMO_ROOT}/st.c"
  "${ONIGMO_ROOT}/enc/ascii.c"
  "${ONIGMO_ROOT}/enc/big5.c"
  "${ONIGMO_ROOT}/enc/euc_jp.c"
  "${ONIGMO_ROOT}/enc/euc_kr.c"
  "${ONIGMO_ROOT}/enc/euc_tw.c"
  "${ONIGMO_ROOT}/enc/gb18030.c"
  "${ONIGMO_ROOT}/enc/iso_8859_1.c"
  "${ONIGMO_ROOT}/enc/iso_8859_2.c"
  "${ONIGMO_ROOT}/enc/iso_8859_3.c"
  "${ONIGMO_ROOT}/enc/iso_8859_4.c"
  "${ONIGMO_ROOT}/enc/iso_8859_5.c"
  "${ONIGMO_ROOT}/enc/iso_8859_6.c"
  "${ONIGMO_ROOT}/enc/iso_8859_7.c"
  "${ONIGMO_ROOT}/enc/iso_8859_8.c"
  "${ONIGMO_ROOT}/enc/iso_8859_9.c"
  "${ONIGMO_ROOT}/enc/iso_8859_10.c"
  "${ONIGMO_ROOT}/enc/iso_8859_11.c"
  "${ONIGMO_ROOT}/enc/iso_8859_13.c"
  "${ONIGMO_ROOT}/enc/iso_8859_14.c"
  "${ONIGMO_ROOT}/enc/iso_8859_15.c"
  "${ONIGMO_ROOT}/enc/iso_8859_16.c"
  "${ONIGMO_ROOT}/enc/koi8_r.c"
  "${ONIGMO_ROOT}/enc/koi8_u.c"
  "${ONIGMO_ROOT}/enc/shift_jis.c"
  "${ONIGMO_ROOT}/enc/unicode.c"
  "${ONIGMO_ROOT}/enc/utf_8.c"
  "${ONIGMO_ROOT}/enc/utf_16be.c"
  "${ONIGMO_ROOT}/enc/utf_16le.c"
  "${ONIGMO_ROOT}/enc/utf_32be.c"
  "${ONIGMO_ROOT}/enc/utf_32le.c"
  "${ONIGMO_ROOT}/enc/windows_1250.c"
  "${ONIGMO_ROOT}/enc/windows_1251.c"
  "${ONIGMO_ROOT}/enc/windows_1252.c"
  "${ONIGMO_ROOT}/enc/windows_1253.c"
  "${ONIGMO_ROOT}/enc/windows_1254.c"
  "${ONIGMO_ROOT}/enc/windows_1257.c"
  "${ONIGMO_ROOT}/enc/windows_31j.c"
)

add_library(onigmo STATIC ${ONIGMO_SOURCES})
add_library(Onigmo::onigmo ALIAS onigmo)
add_library(onigmo::onigmo ALIAS onigmo)
set_target_properties(onigmo PROPERTIES OUTPUT_NAME onigmo)

target_include_directories(onigmo
  PUBLIC
    "$<BUILD_INTERFACE:${ONIGMO_ROOT}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${ONIGMO_ROOT}/win32"
    "${ONIGMO_ROOT}/enc/unicode"
)

target_compile_definitions(onigmo
  PUBLIC
    ONIG_EXTERN=extern
  PRIVATE
    HAVE_CONFIG_H
)

if(MSVC)
  target_compile_options(onigmo PRIVATE /utf-8)
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  target_compile_options(onigmo PRIVATE -std=gnu17 -Wno-error=incompatible-pointer-types)
endif()

install(FILES
  "${ONIGMO_ROOT}/onigmo.h"
  "${ONIGMO_ROOT}/onigmognu.h"
  "${ONIGMO_ROOT}/onigmoposix.h"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(TARGETS onigmo EXPORT OnigmoTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install(EXPORT OnigmoTargets
  NAMESPACE Onigmo::
  DESTINATION share/Onigmo
  FILE OnigmoTargets.cmake
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OnigmoConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/OnigmoConfig.cmake"
  INSTALL_DESTINATION share/Onigmo
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/OnigmoConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/OnigmoConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/OnigmoConfigVersion.cmake"
  DESTINATION share/Onigmo
)

option(ONIGMO_BUILD_TESTS "Build Onigmo's UTF-8 encoding smoke test" OFF)
if(ONIGMO_BUILD_TESTS)
  add_executable(test_enc_utf8 "${ONIGMO_ROOT}/test_enc_utf8.c")
  target_link_libraries(test_enc_utf8 PRIVATE onigmo)
  target_include_directories(test_enc_utf8 PRIVATE "${ONIGMO_ROOT}/win32")
  target_compile_definitions(test_enc_utf8 PRIVATE HAVE_CONFIG_H ONIG_EXTERN=extern)
endif()
