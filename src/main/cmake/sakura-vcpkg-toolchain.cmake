# Explicit Sakura package toolchain.
#
# This wrapper is the only supported CMake entry into vcpkg.  The canonical
# Python driver creates the referenced active configuration after a complete
# content-addressed restore.  Leaving this file without that configuration is
# intentionally an error: ordinary CMake configure must not discover the root
# vcpkg.json and begin an implicit restore.

get_filename_component(SAKURA_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
  message(FATAL_ERROR "SAKURA_PACKAGE_TRIPLET_MISSING: pass VCPKG_TARGET_TRIPLET through sakura-build")
endif()

if(NOT DEFINED SAKURA_PACKAGE_CONFIG OR SAKURA_PACKAGE_CONFIG STREQUAL "")
  set(SAKURA_PACKAGE_CONFIG
    "${SAKURA_REPOSITORY_ROOT}/build/pkg/v/a/${VCPKG_TARGET_TRIPLET}.cmake")
endif()

# CMake reloads the toolchain in compiler ABI try_compile projects. Propagate
# the explicit package identity there as platform state; without this list the
# nested configure falls back to an ambient/default package location.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
  SAKURA_PACKAGE_CONFIG
  VCPKG_TARGET_TRIPLET)

if(NOT EXISTS "${SAKURA_PACKAGE_CONFIG}")
  message(FATAL_ERROR
    "SAKURA_PACKAGE_RESTORE_MISSING: ${SAKURA_PACKAGE_CONFIG}. "
    "Run: py -3 tools/build/sakura_build.py package restore sakura_app --context <context-id>")
endif()

include("${SAKURA_PACKAGE_CONFIG}")
set(VCPKG_MANIFEST_MODE OFF CACHE BOOL "Disable implicit vcpkg manifest restore" FORCE)

set(_sakura_vcpkg_root "${SAKURA_REPOSITORY_ROOT}/tools/vcpkg")
if(NOT EXISTS "${_sakura_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
  message(FATAL_ERROR "SAKURA_VCPKG_TOOLCHAIN_MISSING: ${_sakura_vcpkg_root}")
endif()

include("${_sakura_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
