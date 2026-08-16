get_filename_component(REPO_ROOT "${CURRENT_PORT_DIR}/../../../.." ABSOLUTE)
set(ONIGMO_SOURCE_DIR "${REPO_ROOT}/externals/onigmo-next")

if(NOT EXISTS "${ONIGMO_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Onigmo NEXT CMakeLists.txt not found: ${ONIGMO_SOURCE_DIR}")
endif()
if(NOT EXISTS "${ONIGMO_SOURCE_DIR}/onigmo.h")
  message(FATAL_ERROR "Onigmo source not found: ${ONIGMO_SOURCE_DIR}")
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${ONIGMO_SOURCE_DIR}"
  OPTIONS
    -DONIGMO_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME Onigmo CONFIG_PATH share/Onigmo)

configure_file("${CMAKE_CURRENT_LIST_DIR}/usage" "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage" COPYONLY)

file(REMOVE_RECURSE
  "${CURRENT_PACKAGES_DIR}/debug/include"
  "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(INSTALL
  "${ONIGMO_SOURCE_DIR}/COPYING"
  DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
  RENAME copyright
)
