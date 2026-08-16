get_filename_component(REPO_ROOT "${CURRENT_PORT_DIR}/../../../.." ABSOLUTE)
set(BREGONIG_SOURCE_DIR "${REPO_ROOT}/third_party/owned/bregonig-next")

if(NOT EXISTS "${BREGONIG_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "bregonig-next CMakeLists.txt not found: ${BREGONIG_SOURCE_DIR}")
endif()
if(NOT EXISTS "${BREGONIG_SOURCE_DIR}/src/bregexp.h")
  message(FATAL_ERROR "bregonig source not found: ${BREGONIG_SOURCE_DIR}")
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${BREGONIG_SOURCE_DIR}"
  OPTIONS
    -DBREGONIG_BUILD_TESTS=OFF
    -DBREGONIG_BUILD_FUZZ=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME bregonig CONFIG_PATH share/bregonig)
vcpkg_copy_pdbs()

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)

file(REMOVE_RECURSE
  "${CURRENT_PACKAGES_DIR}/debug/include"
  "${CURRENT_PACKAGES_DIR}/debug/share"
)

if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/include/bregexp.h")
  message(FATAL_ERROR "installed bregexp.h is missing")
endif()
if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/bin/bregonig.dll")
  message(FATAL_ERROR "installed bregonig.dll is missing")
endif()
if(NOT VCPKG_TARGET_IS_MINGW AND NOT EXISTS "${CURRENT_PACKAGES_DIR}/lib/bregonig.lib")
  message(FATAL_ERROR "installed import library is missing")
endif()

file(INSTALL
  "${BREGONIG_SOURCE_DIR}/bsd_license.txt"
  DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
  RENAME copyright
)
